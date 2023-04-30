/// @file
/// Marlin2 interface implementation for the MMU2
#include "mmu2_marlin.h"

#include "marlin_move.hpp"

#include "../../inc/MarlinConfig.h"
#include "../../gcode/gcode.h"
#include "../../libs/buzzer.h"
#include "../../libs/nozzle.h"
#include "../../module/temperature.h"
#include "../../module/planner.h"
#include "../../module/stepper/indirection.h"
#include "../../Marlin.h"

namespace MMU2 {

void extruder_move(float distance, float feed_rate) {
    marlin::extruder_move(distance, feed_rate);
}

void extruder_schedule_turning(float feed_rate) {
    marlin::extruder_schedule_turning(feed_rate);
}

float raise_z(float delta) {
    // @@TODO
    return 0.0F;
}

void planner_synchronize() {
    planner.synchronize();
}

bool planner_any_moves() {
    return planner.movesplanned();
}

pos3d planner_current_position() {
    return pos3d(current_position.x, current_position.y, current_position.z);
}

void motion_do_blocking_move_to_xy(float rx, float ry, float feedRate_mm_s) {
    do_blocking_move_to_xy(rx, ry, feedRate_mm_s);
}

void motion_do_blocking_move_to_z(float z, float feedRate_mm_s) {
    do_blocking_move_to_z(z, feedRate_mm_s);
}

void nozzle_park() {
    static constexpr xyz_pos_t park_point = NOZZLE_PARK_POINT_M600;
    nozzle.park(2, park_point);
}

bool marlin_printingIsActive() {
    return printingIsActive();
}

void marlin_manage_heater() {
    thermalManager.manage_heater();
}

void marlin_manage_inactivity(bool b) {
    manage_inactivity(b);
}

void marlin_idle(bool b) {
    idle(b);
}

int16_t thermal_degTargetHotend() {
    return thermalManager.degTargetHotend(active_extruder);
}

int16_t thermal_degHotend() {
    return thermalManager.degHotend(active_extruder);
}

void thermal_setExtrudeMintemp(int16_t t) {
    thermalManager.extrude_min_temp = t;
}

void thermal_setTargetHotend(int16_t t) {
    thermalManager.setTargetHotend(t, active_extruder);
}

void safe_delay_keep_alive(uint16_t t) {
    safe_delay(t);
}

void gcode_reset_stepper_timeout() {
    gcode.reset_stepper_timeout();
}

void Enable_E0() {
    enable_E0();
}

void Disable_E0() {
    disable_E0();
}

bool all_axes_homed() {
    return false; // @@TODO
}

bool cutter_enabled() {
    // read eeprom - cutter enabled
    return false; //@@TODO
}

void FullScreenMsg(const char *pgmS, uint8_t slot) {
#ifdef __AVR__
    lcd_update_enable(false);
    lcd_clear();
    lcd_puts_at_P(0, 1, pgmS);
    lcd_print(' ');
    lcd_print(slot + 1);
#else
#endif
}
} // namespace MMU2
