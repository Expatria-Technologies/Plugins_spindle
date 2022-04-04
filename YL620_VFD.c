/*

  YL620_VFD.c - Yalang VFD spindle support

  Part of grblHAL

  Copyright (c) 2020-2021 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

      Manual Configuration required for the YL620
    Parameter number        Description                     Value
    -------------------------------------------------------------------------------
    P00.00                  Main frequency                  400.00Hz (match to your spindle)
    P00.01                  Command source                  3
    
    P03.00                  RS485 Baud rate                 3 (9600)
    P03.01                  RS485 address                   1
    P03.02                  RS485 protocol                  2
    P03.08                  Frequency given lower limit     100.0Hz (match to your spindle cooling-type)
    ===============================================================================================================

    RS485 communication is standard Modbus RTU
    Therefore, the following operation codes are relevant:
    0x03:   read single holding register
    0x06:   write single holding register
    Given a parameter Pnn.mm, the high byte of the register address is nn,
    the low is mm.  The numbers nn and mm in the manual are given in decimal,
    so P13.16 would be register address 0x0d10 when represented in hex.
    Holding register address                Description
    ---------------------------------------------------------------------------
    0x0000                                  main frequency
    0x0308                                  frequency given lower limit
    0x2000                                  command register (further information below)
    0x2001                                  Modbus485 frequency command (x0.1Hz => 2500 = 250.0Hz)
    0x200A                                  Target frequency
    0x200B                                  Output frequency
    0x200C                                  Output current    

    Command register at holding address 0x2000
    --------------------------------------------------------------------------
    bit 1:0             b00: No function
                        b01: shutdown command
                        b10: start command
                        b11: Jog command
    bit 3:2             reserved
    bit 5:4             b00: No function
                        b01: Forward command
                        b10: Reverse command
                        b11: change direction
    bit 7:6             b00: No function
                        b01: reset an error flag
                        b10: reset all error flags
                        b11: reserved    

*/

#ifdef ARDUINO
#include "../driver.h"
#else
#include "driver.h"
#endif

#if VFD_ENABLE

#include <math.h>
#include <string.h>

#ifdef ARDUINO
#include "../grbl/hal.h"
#include "../grbl/protocol.h"
#include "../grbl/state_machine.h"
#include "../grbl/report.h"
#else
#include "grbl/hal.h"
#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/report.h"
#endif

#include "modbus.h"
#include "vfd_spindle.h"

#ifdef SPINDLE_PWM_DIRECT
#error "Uncomment SPINDLE_RPM_CONTROLLED in grbl/config.h to add Huanyang spindle support!"
#endif

#ifndef VFD_ADDRESS
#define VFD_ADDRESS 0x01
#endif

static float rpm_programmed = -1.0f;
static spindle_state_t vfd_state = {0};
static spindle_data_t spindle_data = {0};
static on_report_options_ptr on_report_options;
static on_spindle_select_ptr on_spindle_select;
static driver_reset_ptr driver_reset;
static uint32_t rpm_max = 0;
static uint16_t retry_counter = 0;

static void rx_packet (modbus_message_t *msg);
static void rx_exception (uint8_t code, void *context);

static const modbus_callbacks_t callbacks = {
    .on_rx_packet = rx_packet,
    .on_rx_exception = rx_exception
};

// To-do, this should be a mechanism to read max RPM from the VFD in order to configure RPM/Hz instead of above define.
static void spindleGetMaxRPM (void)
{

}

static void spindleSetRPM (float rpm, bool block)
{
        uint16_t data = ((uint32_t)(rpm)*10) / modbus.vfd_rpm_hz;

        modbus_message_t rpm_cmd = {
            .context = (void *)VFD_SetRPM,
            .crc_check = false,
            .adu[0] = VFD_ADDRESS,
            .adu[1] = ModBus_WriteRegister,
            .adu[2] = 0x20,
            .adu[3] = 0x01,
            .adu[4] = data >> 8,
            .adu[5] = data & 0xFF,
            .tx_length = 8,
            .rx_length = 8
        };        

        vfd_state.at_speed = false;

        modbus_send(&rpm_cmd, &callbacks, block);

        if(settings.spindle.at_speed_tolerance > 0.0f) {
            spindle_data.rpm_low_limit = rpm / (1.0f + settings.spindle.at_speed_tolerance);
            spindle_data.rpm_high_limit = rpm * (1.0f + settings.spindle.at_speed_tolerance);
        }        
        rpm_programmed = rpm;
}

static void spindleUpdateRPM (float rpm)
{
    spindleSetRPM(rpm, false);
}

// Start or stop spindle
static void spindleSetState (spindle_state_t state, float rpm)
{
    uint8_t runstop = 0;
    uint8_t direction = 0;

    if(!state.on || rpm == 0.0f)
        runstop = 0x1;
    else
        runstop = 0x2;

    if(state.ccw)
        direction = 0x20;
    else
        direction = 0x10;

    modbus_message_t mode_cmd = {
        .context = (void *)VFD_SetStatus,
        .crc_check = false,
        .adu[0] = VFD_ADDRESS,
        .adu[1] = ModBus_WriteRegister,
        .adu[2] = 0x20,
        .adu[3] = 0x00,
        .adu[4] = 0x00,
        .adu[5] = direction|runstop,
        .tx_length = 8,
        .rx_length = 8
    };

    if(vfd_state.ccw != state.ccw)
        rpm_programmed = 0.0f;

    vfd_state.on = state.on;
    vfd_state.ccw = state.ccw;

    if(modbus_send(&mode_cmd, &callbacks, true))
        spindleSetRPM(rpm, true);
}

static spindle_data_t *spindleGetData (spindle_data_request_t request)
{
    return &spindle_data;
}

// Returns spindle state in a spindle_state_t variable
static spindle_state_t spindleGetState (void)
{

    modbus_message_t mode_cmd = {
        .context = (void *)VFD_GetRPM,
        .crc_check = false,
        .adu[0] = VFD_ADDRESS,
        .adu[1] = ModBus_ReadHoldingRegisters,
        .adu[2] = 0x20,
        .adu[3] = 0x0B,
        .adu[4] = 0x00,
        .adu[5] = 0x01,
        .tx_length = 8,
        .rx_length = 7
    };


    modbus_send(&mode_cmd, &callbacks, false); // TODO: add flag for not raising alarm?
    

    // Get the actual RPM from spindle encoder input when available.
    if(hal.spindle.get_data && hal.spindle.get_data != spindleGetData) {
        float rpm = hal.spindle.get_data(SpindleData_RPM)->rpm;
        vfd_state.at_speed = settings.spindle.at_speed_tolerance <= 0.0f || (rpm >= spindle_data.rpm_low_limit && rpm <= spindle_data.rpm_high_limit);
    }

    return vfd_state; // return previous state as we do not want to wait for the response
}

static void rx_packet (modbus_message_t *msg)
{
    if(!(msg->adu[0] & 0x80)) {

        switch((vfd_response_t)msg->context) {

            case VFD_GetRPM:
                spindle_data.rpm = (float)((msg->adu[3] << 8) | msg->adu[4])*modbus.vfd_rpm_hz/10;
                vfd_state.at_speed = settings.spindle.at_speed_tolerance <= 0.0f || (spindle_data.rpm >= spindle_data.rpm_low_limit && spindle_data.rpm <= spindle_data.rpm_high_limit);
                retry_counter = 0;
                break;

            case VFD_GetMaxRPM:
                rpm_max = (msg->adu[4] << 8) | msg->adu[5];
                retry_counter = 0;
                break;

            case VFD_SetStatus:
                //add check here to ensure command was successful, retry if not.
                retry_counter = 0;
                break;
            case VFD_SetRPM:
                //add check here to ensure command was successful, retry if not.
                retry_counter = 0;
                break;        
            default:
            retry_counter = 0;
                break;
        }
    }
}

static void raise_alarm (uint_fast16_t state)
{
    system_raise_alarm(Alarm_Spindle);
}

static void rx_exception (uint8_t code, void *context)
{
    // Alarm needs to be raised directly to correctly handle an error during reset (the rt command queue is
    // emptied on a warm reset). Exception is during cold start, where alarms need to be queued.
    if(sys.cold_start){
        protocol_enqueue_rt_command(raise_alarm);
    }
    //if RX exceptions during one of the VFD messages, need to retry.
    else if ((vfd_response_t)context > 0 ){
        retry_counter++;
        if (retry_counter >= VFD_RETRIES){
            system_raise_alarm(Alarm_Spindle);
            retry_counter = 0;
            return;
            }
        switch((vfd_response_t)context) {
            case VFD_SetStatus:    
            case VFD_SetRPM:
            modbus_reset();
            hal.spindle.set_state(hal.spindle.get_state(), sys.spindle_rpm);
            break;
            case VFD_GetRPM:
            modbus_reset();
            hal.spindle.get_state();
            break;
            default:
            break;
        }//close switch statement
    }
    else{
        retry_counter = 0;
        system_raise_alarm(Alarm_Spindle);
    }
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(!newopt) {
        hal.stream.write("[PLUGIN:Yalang VFD YL620A v0.01]" ASCII_EOL);
    }
}

static void yl620_reset (void)
{
    driver_reset();
}

bool yl620_spindle_select (uint_fast8_t spindle_id)
{
    static bool init_ok = false, vfd_active = false;
    static driver_cap_t driver_cap;
    static spindle_ptrs_t spindle_org;

    if(vfd_active && spindle_id != 1 && spindle_org.set_state != NULL) {

        vfd_active = false;

        gc_spindle_off();

        hal.driver_cap = driver_cap;
        memcpy(&hal.spindle, &spindle_org, sizeof(spindle_ptrs_t));
    }

    if(on_spindle_select && on_spindle_select(spindle_id))
        return true;

    if(!modbus_isup())
        return false;

    if((vfd_active = spindle_id == 1)) {

        if(hal.spindle.set_state != spindleSetState) {

            if(spindle_org.set_state == NULL) {
                driver_cap = hal.driver_cap;
                memcpy(&spindle_org, &hal.spindle, sizeof(spindle_ptrs_t));
            }

            if(spindle_org.set_state)
                gc_spindle_off();

            hal.spindle.set_state = spindleSetState;
            hal.spindle.get_state = spindleGetState;
            hal.spindle.update_rpm = spindleUpdateRPM;
            hal.spindle.reset_data = NULL;

            hal.driver_cap.variable_spindle = On;
            hal.driver_cap.spindle_at_speed = On;
            hal.driver_cap.spindle_dir = On;
        }

        if(settings.spindle.ppr == 0)
            hal.spindle.get_data = spindleGetData;

        if(!init_ok) {
            init_ok = true;
            spindleGetMaxRPM();
        }
    }

    return true;
}

void YL620_init (void)
{
    if(modbus_enabled()) {

        on_spindle_select = grbl.on_spindle_select;
        grbl.on_spindle_select = yl620_spindle_select;

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = onReportOptions;

        driver_reset = hal.driver_reset;
        hal.driver_reset = yl620_reset;
    }
}

#endif