/*

  huanyang.c - Huanyang VFD spindle support

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
//#if VFD_ENABLE == 1
static float rpm_max50 = 3000;
//#endif

extern modbus_settings_t modbus;

static void rx_packet (modbus_message_t *msg);
static void rx_exception (uint8_t code, void *context);

static const modbus_callbacks_t callbacks = {
    .on_rx_packet = rx_packet,
    .on_rx_exception = rx_exception
};

// Read maximum configured RPM from spindle, value is used later for calculating current RPM
// In the case of the original Huanyang protocol, the value is the configured RPM at 50Hz
static void spindleGetMaxRPM (void)
{
if (modbus.vfd_type == HUANYANG2) {
    modbus_message_t cmd = {
        .context = (void *)VFD_GetMaxRPM,
        .adu[0] = VFD_ADDRESS,
        .adu[1] = ModBus_ReadHoldingRegisters,
        .adu[2] = 0xB0,
        .adu[3] = 0x05,
        .adu[4] = 0x00,
        .adu[5] = 0x02,
        .tx_length = 8,
        .rx_length = 8
    };
    modbus_send(&cmd, &callbacks, true);
}else{
    modbus_message_t cmd = {
        .context = (void *)VFD_GetMaxRPM50,
        .adu[0] = VFD_ADDRESS,
        .adu[1] = ModBus_ReadCoils,
        .adu[2] = 0x03,
        .adu[3] = 0x90, // PD144
        .adu[4] = 0x00,
        .adu[5] = 0x00,
        .tx_length = 8,
        .rx_length = 8
    };
    modbus_send(&cmd, &callbacks, true);
    }
}

static void spindleSetRPM (float rpm, bool block)
{

modbus_message_t rpm_cmd;

    if (rpm != rpm_programmed) {

    if (modbus.vfd_type == HUANYANG2) {

        uint16_t data = (uint32_t)(rpm) * 10000UL / rpm_max;

            rpm_cmd.context = (void *)VFD_SetRPM;
            rpm_cmd.crc_check = false;
            rpm_cmd.adu[0] = VFD_ADDRESS;
            rpm_cmd.adu[1] = ModBus_WriteRegister;
            rpm_cmd.adu[2] = 0x10;
            rpm_cmd.adu[4] = data >> 8;
            rpm_cmd.adu[5] = data & 0xFF;
            rpm_cmd.tx_length = 8;
            rpm_cmd.rx_length = 8;

    }else{

        uint32_t data = lroundf(rpm * 5000.0f / (float)rpm_max50); // send Hz * 10  (Ex:1500 RPM = 25Hz .... Send 2500)

            rpm_cmd.context = (void *)VFD_SetRPM;
            rpm_cmd.crc_check = false;
            rpm_cmd.adu[0] = VFD_ADDRESS;
            rpm_cmd.adu[1] = ModBus_WriteCoil;
            rpm_cmd.adu[2] = 0x02;
            rpm_cmd.adu[3] = data >> 8;
            rpm_cmd.adu[4] = data & 0xFF;
            rpm_cmd.tx_length = 7;
            rpm_cmd.rx_length = 6;

}

        vfd_state.at_speed = false;

        modbus_send(&rpm_cmd, &callbacks, block);

        if(settings.spindle.at_speed_tolerance > 0.0f) {
            spindle_data.rpm_low_limit = rpm / (1.0f + settings.spindle.at_speed_tolerance);
            spindle_data.rpm_high_limit = rpm * (1.0f + settings.spindle.at_speed_tolerance);
        }
        rpm_programmed = rpm;
    }
}

static void spindleUpdateRPM (float rpm)
{
    spindleSetRPM(rpm, false);
}

// Start or stop spindle
static void spindleSetState (spindle_state_t state, float rpm)
{
modbus_message_t mode_cmd;

if (modbus.vfd_type == HUANYANG2) {

        mode_cmd.context = (void *)VFD_SetStatus;
        mode_cmd.crc_check = false;
        mode_cmd.adu[0] = VFD_ADDRESS;
        mode_cmd.adu[1] = ModBus_WriteRegister;
        mode_cmd.adu[2] = 0x20;
        mode_cmd.adu[5] = (!state.on || rpm == 0.0f) ? 6 : (state.ccw ? 2 : 1);
        mode_cmd.tx_length = 8;
        mode_cmd.rx_length = 8;

}else{
        mode_cmd.context = (void *)VFD_SetStatus;
        mode_cmd.crc_check = false;
        mode_cmd.adu[0] = VFD_ADDRESS;
        mode_cmd.adu[1] = ModBus_ReadHoldingRegisters;
        mode_cmd.adu[2] = 0x01;
        mode_cmd.adu[3] = (!state.on || rpm == 0.0f) ? 0x08 : (state.ccw ? 0x11 : 0x01);
        mode_cmd.tx_length = 6;
        mode_cmd.rx_length = 6;
}

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

modbus_message_t mode_cmd;

if (modbus.vfd_type == HUANYANG2) {

        mode_cmd.context = (void *)VFD_GetRPM;
        mode_cmd.crc_check = false;
        mode_cmd.adu[0] = VFD_ADDRESS;
        mode_cmd.adu[1] = ModBus_ReadHoldingRegisters;
        mode_cmd.adu[2] = 0x70;
        mode_cmd.adu[3] = 0x0C;
        mode_cmd.adu[4] = 0x00;
        mode_cmd.adu[5] = 0x02;
        mode_cmd.tx_length = 8;
        mode_cmd.rx_length = 8;

}else{

        mode_cmd.context = (void *)VFD_GetRPM;
        mode_cmd.crc_check = false;
        mode_cmd.adu[0] = VFD_ADDRESS;
        mode_cmd.adu[1] = ModBus_ReadInputRegisters;
        mode_cmd.adu[2] = 0x03;
        mode_cmd.adu[3] = 0x01;
        mode_cmd.tx_length = 8;
        mode_cmd.rx_length = 8;

}

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
if (modbus.vfd_type == HUANYANG2) {
                spindle_data.rpm = (float)((msg->adu[4] << 8) | msg->adu[5]);
}else{
                spindle_data.rpm = (float)((msg->adu[4] << 8) | msg->adu[5]) * (float)rpm_max50 / 5000.0f;
}
                vfd_state.at_speed = settings.spindle.at_speed_tolerance <= 0.0f || (spindle_data.rpm >= spindle_data.rpm_low_limit && spindle_data.rpm <= spindle_data.rpm_high_limit);
                break;

            case VFD_GetMaxRPM:
                rpm_max = (msg->adu[4] << 8) | msg->adu[5];
                break;

if (modbus.vfd_type == HUANYANG1) {
            case VFD_GetMaxRPM50:
                rpm_max50 = (msg->adu[4] << 8) | msg->adu[5];
                break;
}

            default:
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
    if(sys.cold_start)
        protocol_enqueue_rt_command(raise_alarm);
    else
        system_raise_alarm(Alarm_Spindle);
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(!newopt) {
if (modbus.vfd_type == HUANYANG2) {
        hal.stream.write("[PLUGIN:HUANYANG VFD P2A v0.07]" ASCII_EOL);
}else{
        hal.stream.write("[PLUGIN:HUANYANG VFD v0.07]" ASCII_EOL);
        }
    }
}

static void huanyang_reset (void)
{
    driver_reset();
    spindleGetMaxRPM();
}

bool huanyang_spindle_select (uint_fast8_t spindle_id)
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

void HY_VFD_init (void)
{

    if(modbus_enabled()) {

        on_spindle_select = grbl.on_spindle_select;
        grbl.on_spindle_select = huanyang_spindle_select;

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = onReportOptions;

        driver_reset = hal.driver_reset;
        hal.driver_reset = huanyang_reset;
    }
}

#endif
