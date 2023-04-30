/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2019 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * motion.cpp
 */

#include "motion.h"
#include "bsod_gui.hpp"
#include "endstops.h"
#include "stepper.h"
#include "planner.h"
#include "temperature.h"

#include "../gcode/gcode.h"

#include "../inc/MarlinConfig.h"
#include "../Marlin.h"
#include "../../../../../src/common/eeprom.h"

#include "metric.h"
#include "PersistentStorage.h"

#ifdef MINDA_BROKEN_CABLE_DETECTION
#include "minda_broken_cable_detection.h"
#else
static inline void MINDA_BROKEN_CABLE_DETECTION__POST_ZHOME_0(){}
#endif

#if IS_SCARA
  #include "../libs/buzzer.h"
  #include "../lcd/ultralcd.h"
#endif

#if HAS_BED_PROBE
  #include "probe.h"
#endif

#if HAS_LEVELING
  #include "../feature/bedlevel/bedlevel.h"
#endif

#if ENABLED(BLTOUCH)
  #include "../feature/bltouch.h"
#endif

#if ENABLED(NOZZLE_LOAD_CELL)
  #include "feature/prusa/loadcell.h"
#endif

#if HAS_DISPLAY
  #include "../lcd/ultralcd.h"
#endif

#if ENABLED(SENSORLESS_HOMING)
  #include "../feature/tmc_util.h"

  #if ENABLED(CRASH_RECOVERY)
    #include "../feature/prusa/crash_recovery.h"
  #endif // ENABLED(CRASH_RECOVERY)
#endif

#if ENABLED(FWRETRACT)
  #include "../feature/fwretract.h"
#endif

#if ENABLED(BABYSTEP_DISPLAY_TOTAL)
  #include "../feature/babystep.h"
#endif

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../core/debug_out.h"

#if ENABLED(PRECISE_HOMING)
  #include "precise_homing.h"
#endif

#define XYZ_CONSTS(T, NAME, OPT) const PROGMEM XYZval<T> NAME##_P = { X_##OPT, Y_##OPT, Z_##OPT }

XYZ_CONSTS(float, base_min_pos,   MIN_POS);
XYZ_CONSTS(float, base_max_pos,   MAX_POS);
XYZ_CONSTS(float, base_home_pos,  HOME_POS);
XYZ_CONSTS(float, max_length,     MAX_LENGTH);
XYZ_CONSTS(float, home_bump_mm,   HOME_BUMP_MM);
XYZ_CONSTS(signed char, home_dir, HOME_DIR);

/**
 * axis_homed
 *   Flags that each linear axis was homed.
 *   XYZ on cartesian, ABC on delta, ABZ on SCARA.
 *
 * axis_known_position
 *   Flags that the position is known in each linear axis. Set when homed.
 *   Cleared whenever a stepper powers off, potentially losing its position.
 */
uint8_t axis_homed, axis_known_position; // = 0

// Relative Mode. Enable with G91, disable with G90.
bool relative_mode; // = false;

/**
 * Cartesian Current Position
 *   Planned position. Printer is heading to this position or is at this position.
 *   Used to track the native machine position as moves are queued.
 *   Used by 'line_to_current_position' to do a move after changing it.
 *   Used by 'sync_plan_position' to update 'planner.position'.
 */
xyze_pos_t current_position = { X_HOME_POS, Y_HOME_POS, Z_HOME_POS };

/**
 * Cartesian Destination
 *   The destination for a move, filled in by G-code movement commands,
 *   and expected by functions like 'prepare_move_to_destination'.
 *   G-codes can set destination using 'get_destination_from_command'
 */
xyze_pos_t destination; // {0}

// The active extruder (tool). Set with T<extruder> command.
#if EXTRUDERS > 1
  uint8_t active_extruder; // = 0
#endif

// Extruder offsets
#if HAS_HOTEND_OFFSET
  xyz_pos_t hotend_offset[HOTENDS]; // Initialized by settings.load()
  xyz_pos_t hotend_currently_applied_offset;
  void reset_hotend_offsets() {
    constexpr float tmp[XYZ][HOTENDS] = { HOTEND_OFFSET_X, HOTEND_OFFSET_Y, HOTEND_OFFSET_Z };
    static_assert(
      !tmp[X_AXIS][0] && !tmp[Y_AXIS][0] && !tmp[Z_AXIS][0],
      "Offsets for the first hotend must be 0.0."
    );
    // Transpose from [XYZ][HOTENDS] to [HOTENDS][XYZ]
    HOTEND_LOOP() LOOP_XYZ(a) hotend_offset[e][a] = tmp[a][e];
    #if ENABLED(DUAL_X_CARRIAGE)
      hotend_offset[1].x = _MAX(X2_HOME_POS, X2_MAX_POS);
    #endif
  }
#endif

// The feedrate for the current move, often used as the default if
// no other feedrate is specified. Overridden for special moves.
// Set by the last G0 through G5 command's "F" parameter.
// Functions that override this for custom moves *must always* restore it!
feedRate_t feedrate_mm_s =
  #ifdef DEFAULT_FEEDRATE
    DEFAULT_FEEDRATE;
  #else
    MMM_TO_MMS(1500);
  #endif
int16_t feedrate_percentage = 100;

// Homing feedrate is const progmem - compare to constexpr in the header
const feedRate_t homing_feedrate_mm_s[XYZ] PROGMEM = {
  #if ENABLED(DELTA)
    MMM_TO_MMS(HOMING_FEEDRATE_Z), MMM_TO_MMS(HOMING_FEEDRATE_Z),
  #else
    MMM_TO_MMS(HOMING_FEEDRATE_XY), MMM_TO_MMS(HOMING_FEEDRATE_XY),
  #endif
  MMM_TO_MMS(HOMING_FEEDRATE_Z)
};

#if ENABLED(PRECISE_HOMING)
float homing_bump_divisor[] = { 0, 0, 0 }; // on printers with PRECISE_HOMING, the divisor will be loaded from eeprom
static const float homing_bump_divisor_dflt[] PROGMEM = HOMING_BUMP_DIVISOR;
static const float homing_bump_divisor_max[] PROGMEM = HOMING_BUMP_DIVISOR_MAX;
static const float homing_bump_divisor_min[] PROGMEM = HOMING_BUMP_DIVISOR_MIN;
#else
float homing_bump_divisor[] = HOMING_BUMP_DIVISOR;
#endif

// Cartesian conversion result goes here:
xyz_pos_t cartes;

#if IS_KINEMATIC

  abc_pos_t delta;

  #if HAS_SCARA_OFFSET
    abc_pos_t scara_home_offset;
  #endif

  #if HAS_SOFTWARE_ENDSTOPS
    float delta_max_radius, delta_max_radius_2;
  #elif IS_SCARA
    constexpr float delta_max_radius = SCARA_PRINTABLE_RADIUS,
                    delta_max_radius_2 = sq(SCARA_PRINTABLE_RADIUS);
  #else // DELTA
    constexpr float delta_max_radius = DELTA_PRINTABLE_RADIUS,
                    delta_max_radius_2 = sq(DELTA_PRINTABLE_RADIUS);
  #endif

#endif

/**
 * The workspace can be offset by some commands, or
 * these offsets may be omitted to save on computation.
 */
#if HAS_POSITION_SHIFT
  // The distance that XYZ has been offset by G92. Reset by G28.
  xyz_pos_t position_shift{0};
#endif
#if HAS_HOME_OFFSET
  // This offset is added to the configured home position.
  // Set by M206, M428, or menu item. Saved to EEPROM.
  xyz_pos_t home_offset{0};
#endif
#if HAS_HOME_OFFSET && HAS_POSITION_SHIFT
  // The above two are combined to save on computes
  xyz_pos_t workspace_offset{0};
#endif

#if HAS_ABL_NOT_UBL
  float xy_probe_feedrate_mm_s = MMM_TO_MMS(XY_PROBE_SPEED);
#endif

/**
 * Output the current position to serial
 */
void report_current_position() {
  const xyz_pos_t lpos = current_position.asLogical();
  SERIAL_ECHOPAIR("X:", lpos.x, " Y:", lpos.y, " Z:", lpos.z, " E:", current_position.e);

  stepper.report_positions();

  #if IS_SCARA
    scara_report_positions();
  #endif
}

/**
 * sync_plan_position
 *
 * Set the planner/stepper positions directly from current_position with
 * no kinematic translation. Used for homing axes and cartesian/core syncing.
 */
void sync_plan_position() {
  if (DEBUGGING(LEVELING)) DEBUG_POS("sync_plan_position", current_position);
  planner.set_position_mm(current_position);
}

void sync_plan_position_e() { planner.set_e_position_mm(current_position.e); }

/**
 * Get the stepper positions in the cartes[] array.
 * Forward kinematics are applied for DELTA and SCARA.
 *
 * The result is in the current coordinate space with
 * leveling applied. The coordinates need to be run through
 * unapply_leveling to obtain the "ideal" coordinates
 * suitable for current_position, etc.
 */
void get_cartesian_from_steppers() {
  #if ENABLED(DELTA)
    forward_kinematics_DELTA(
      planner.get_axis_position_mm(A_AXIS),
      planner.get_axis_position_mm(B_AXIS),
      planner.get_axis_position_mm(C_AXIS)
    );
  #else
    #if IS_SCARA
      forward_kinematics_SCARA(
        planner.get_axis_position_degrees(A_AXIS),
        planner.get_axis_position_degrees(B_AXIS)
      );
    #else
      cartes.set(planner.get_axis_position_mm(X_AXIS), planner.get_axis_position_mm(Y_AXIS));
    #endif
    cartes.z = planner.get_axis_position_mm(Z_AXIS);
  #endif
}

/**
 * Set the current_position for an axis based on
 * the stepper positions, removing any leveling that
 * may have been applied.
 *
 * To prevent small shifts in axis position always call
 * sync_plan_position after updating axes with this.
 *
 * To keep hosts in sync, always call report_current_position
 * after updating the current_position.
 */
void set_current_from_steppers_for_axis(const AxisEnum axis) {
  get_cartesian_from_steppers();

  #if HAS_POSITION_MODIFIERS
    xyze_pos_t pos = { cartes.x, cartes.y, cartes.z, current_position.e };
    planner.unapply_modifiers(pos
      #if HAS_LEVELING
        , true
      #endif
    );
    xyze_pos_t &cartes = pos;
  #endif
  if (axis == ALL_AXES_ENUM)
    current_position = cartes;
  else
    current_position[axis] = cartes[axis];
}


/**
 * Set the current_position for all axes based on
 * the stepper positions, removing any leveling that
 * may have been applied.
 */
void set_current_from_steppers() {
  set_current_from_steppers_for_axis(ALL_AXES_ENUM);
}

/**
 * Plans a line movement to the current_position from the last point
 * in the planner's buffer.
 * Suitable for homing, does not apply UBL.
 */
void line_to_current_position(const feedRate_t &fr_mm_s/*=feedrate_mm_s*/) {
  planner.buffer_line(current_position, fr_mm_s, active_extruder);
}

#if IS_KINEMATIC

  /**
   * Buffer a fast move without interpolation. Set current_position to destination
   */
  void prepare_fast_move_to_destination(const feedRate_t &scaled_fr_mm_s/*=MMS_SCALED(feedrate_mm_s)*/) {
    if (DEBUGGING(LEVELING)) DEBUG_POS("prepare_fast_move_to_destination", destination);

    #if UBL_SEGMENTED
      // UBL segmented line will do Z-only moves in single segment
      ubl.line_to_destination_segmented(scaled_fr_mm_s);
    #else
      if (current_position == destination) return;

      planner.buffer_line(destination, scaled_fr_mm_s, active_extruder);
    #endif

    current_position = destination;
  }

#endif // IS_KINEMATIC

void _internal_move_to_destination(const feedRate_t &fr_mm_s/*=0.0f*/
  #if IS_KINEMATIC
    , const bool is_fast/*=false*/
  #endif
) {
  const feedRate_t old_feedrate = feedrate_mm_s;
  if (fr_mm_s) feedrate_mm_s = fr_mm_s;

  const uint16_t old_pct = feedrate_percentage;
  feedrate_percentage = 100;

  #if EXTRUDERS
     const float old_fac = planner.e_factor[active_extruder];
     planner.e_factor[active_extruder] = 1.0f;
  #endif

  #if IS_KINEMATIC
    if (is_fast)
      prepare_fast_move_to_destination();
    else
  #endif
      prepare_move_to_destination();

  feedrate_mm_s = old_feedrate;
  feedrate_percentage = old_pct;
  #if EXTRUDERS
    planner.e_factor[active_extruder] = old_fac;
  #endif
}

/**
 * Performs a blocking fast parking move to (X, Y, Z) and sets the current_position.
 * Parking (Z-Manhattan): Moves XY and Z independently. Raises Z before or lowers Z after XY motion.
 */
void do_blocking_move_to(const float rx, const float ry, const float rz, const feedRate_t &fr_mm_s/*=0.0*/) {
  if (DEBUGGING(LEVELING)) DEBUG_XYZ(">>> do_blocking_move_to", rx, ry, rz);

  const feedRate_t z_feedrate = fr_mm_s ?: homing_feedrate(Z_AXIS),
                  xy_feedrate = fr_mm_s ?: feedRate_t(XY_PROBE_FEEDRATE_MM_S);

  plan_park_move_to(rx, ry, rz, xy_feedrate, z_feedrate);
  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("<<< do_blocking_move_to");
  planner.synchronize();
}

/// Z-Manhattan fast move
void plan_park_move_to(const float rx, const float ry, const float rz, const feedRate_t &fr_xy, const feedRate_t &fr_z){
  #if ENABLED(DELTA)

    if (!position_is_reachable(rx, ry)) return;

    REMEMBER(fr, feedrate_mm_s, fr_xy);

    destination = current_position;          // sync destination at the start

    if (DEBUGGING(LEVELING)) DEBUG_POS("destination = current_position", destination);

    // when in the danger zone
    if (current_position.z > delta_clip_start_height) {
      if (rz > delta_clip_start_height) {   // staying in the danger zone
        destination.set(rx, ry, rz);        // move directly (uninterpolated)
        prepare_internal_fast_move_to_destination();          // set current_position from destination
        if (DEBUGGING(LEVELING)) DEBUG_POS("danger zone move", current_position);
        return;
      }
      destination.z = delta_clip_start_height;
      prepare_internal_fast_move_to_destination();            // set current_position from destination
      if (DEBUGGING(LEVELING)) DEBUG_POS("zone border move", current_position);
    }

    if (rz > current_position.z) {                            // raising?
      destination.z = rz;
      prepare_internal_fast_move_to_destination(fr_z);  // set current_position from destination
      if (DEBUGGING(LEVELING)) DEBUG_POS("z raise move", current_position);
    }

    destination.set(rx, ry);
    prepare_internal_move_to_destination();                   // set current_position from destination
    if (DEBUGGING(LEVELING)) DEBUG_POS("xy move", current_position);

    if (rz < current_position.z) {                            // lowering?
      destination.z = rz;
      prepare_internal_fast_move_to_destination(fr_z);  // set current_position from destination
      if (DEBUGGING(LEVELING)) DEBUG_POS("z lower move", current_position);
    }

  #elif IS_SCARA

    if (!position_is_reachable(rx, ry)) return;

    destination = current_position;

    // If Z needs to raise, do it before moving XY
    if (destination.z < rz) {
      destination.z = rz;
      prepare_internal_fast_move_to_destination(fr_z);
    }

    destination.set(rx, ry);
    prepare_internal_fast_move_to_destination(fr_xy);

    // If Z needs to lower, do it after moving XY
    if (destination.z > rz) {
      destination.z = rz;
      prepare_internal_fast_move_to_destination(fr_z);
    }

  #else

    // If Z needs to raise, do it before moving XY
    if (current_position.z < rz) {
      current_position.z = rz;
      line_to_current_position(fr_z);
    }

    current_position.set(rx, ry);
    line_to_current_position(fr_xy);

    // If Z needs to lower, do it after moving XY
    if (current_position.z > rz) {
      current_position.z = rz;
      line_to_current_position(fr_z);
    }
  #endif
}

void do_blocking_move_to(const xy_pos_t &raw, const feedRate_t &fr_mm_s/*=0.0f*/) {
  do_blocking_move_to(raw.x, raw.y, current_position.z, fr_mm_s);
}
void do_blocking_move_to(const xyz_pos_t &raw, const feedRate_t &fr_mm_s/*=0.0f*/) {
  do_blocking_move_to(raw.x, raw.y, raw.z, fr_mm_s);
}
void do_blocking_move_to(const xyze_pos_t &raw, const feedRate_t &fr_mm_s/*=0.0f*/) {
  do_blocking_move_to(raw.x, raw.y, raw.z, fr_mm_s);
}

void do_blocking_move_to_x(const float &rx, const feedRate_t &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(rx, current_position.y, current_position.z, fr_mm_s);
}
void do_blocking_move_to_y(const float &ry, const feedRate_t &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(current_position.x, ry, current_position.z, fr_mm_s);
}
void do_blocking_move_to_z(const float &rz, const feedRate_t &fr_mm_s/*=0.0*/) {
  do_blocking_move_to_xy_z(current_position, rz, fr_mm_s);
}

void do_blocking_move_to_xy(const float &rx, const float &ry, const feedRate_t &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(rx, ry, current_position.z, fr_mm_s);
}
void do_blocking_move_to_xy(const xy_pos_t &raw, const feedRate_t &fr_mm_s/*=0.0f*/) {
  do_blocking_move_to_xy(raw.x, raw.y, fr_mm_s);
}

void do_blocking_move_to_xy_z(const xy_pos_t &raw, const float &z, const feedRate_t &fr_mm_s/*=0.0f*/) {
  do_blocking_move_to(raw.x, raw.y, z, fr_mm_s);
}

#if HAS_Z_AXIS
  void do_z_clearance(const_float_t zclear, const bool lower_allowed/*=false*/) {
    float zdest = zclear;
    if (!lower_allowed) NOLESS(zdest, current_position.z);
    do_blocking_move_to_z(_MIN(zdest, Z_MAX_POS), homing_feedrate(Z_AXIS));
  }
#endif

//
// Prepare to do endstop or probe moves with custom feedrates.
//  - Save / restore current feedrate and multiplier
//
static float saved_feedrate_mm_s;
static int16_t saved_feedrate_percentage;
void remember_feedrate_and_scaling() {
  saved_feedrate_mm_s = feedrate_mm_s;
  saved_feedrate_percentage = feedrate_percentage;
}
void remember_feedrate_scaling_off() {
  remember_feedrate_and_scaling();
  feedrate_percentage = 100;
}
void restore_feedrate_and_scaling() {
  if (feedrate_percentage == 100) {
    feedrate_mm_s = saved_feedrate_mm_s;
    feedrate_percentage = saved_feedrate_percentage;
  }
}

#if HAS_SOFTWARE_ENDSTOPS

  bool soft_endstops_enabled = true;

  // Software Endstops are based on the configured limits.
  axis_limits_t soft_endstop = {
    { X_MIN_POS, Y_MIN_POS, Z_MIN_POS },
    { X_MAX_POS, Y_MAX_POS, Z_MAX_POS }
  };

  /**
   * Software endstops can be used to monitor the open end of
   * an axis that has a hardware endstop on the other end. Or
   * they can prevent axes from moving past endstops and grinding.
   *
   * To keep doing their job as the coordinate system changes,
   * the software endstop positions must be refreshed to remain
   * at the same positions relative to the machine.
   */
  void update_software_endstops(const AxisEnum axis
    #if HAS_HOTEND_OFFSET
      , const uint8_t old_tool_index/*=0*/, const uint8_t new_tool_index/*=0*/
    #endif
  ) {

    #if ENABLED(DUAL_X_CARRIAGE)

      if (axis == X_AXIS) {

        // In Dual X mode hotend_offset[X] is T1's home position
        const float dual_max_x = _MAX(hotend_offset[1].x, X2_MAX_POS);

        if (new_tool_index != 0) {
          // T1 can move from X2_MIN_POS to X2_MAX_POS or X2 home position (whichever is larger)
          soft_endstop.min.x = X2_MIN_POS;
          soft_endstop.max.x = dual_max_x;
        }
        else if (dxc_is_duplicating()) {
          // In Duplication Mode, T0 can move as far left as X1_MIN_POS
          // but not so far to the right that T1 would move past the end
          soft_endstop.min.x = X1_MIN_POS;
          soft_endstop.max.x = _MIN(X1_MAX_POS, dual_max_x - duplicate_extruder_x_offset);
        }
        else {
          // In other modes, T0 can move from X1_MIN_POS to X1_MAX_POS
          soft_endstop.min.x = X1_MIN_POS;
          soft_endstop.max.x = X1_MAX_POS;
        }

      }

    #elif ENABLED(DELTA)

      soft_endstop.min[axis] = base_min_pos(axis);
      soft_endstop.max[axis] = (axis == Z_AXIS ? delta_height
      #if HAS_BED_PROBE
        - probe_offset.z
      #endif
      : base_max_pos(axis));

      switch (axis) {
        case X_AXIS:
        case Y_AXIS:
          // Get a minimum radius for clamping
          delta_max_radius = _MIN(ABS(_MAX(soft_endstop.min.x, soft_endstop.min.y)), soft_endstop.max.x, soft_endstop.max.y);
          delta_max_radius_2 = sq(delta_max_radius);
          break;
        case Z_AXIS:
          delta_clip_start_height = soft_endstop.max[axis] - delta_safe_distance_from_top();
        default: break;
      }

    #elif HAS_HOTEND_OFFSET && DISABLED(PRUSA_TOOLCHANGER)

      // Software endstops are relative to the tool 0 workspace, so
      // the movement limits must be shifted by the tool offset to
      // retain the same physical limit when other tools are selected.
      if (old_tool_index != new_tool_index) {
        const float offs = hotend_offset[new_tool_index][axis] - hotend_offset[old_tool_index][axis];
        soft_endstop.min[axis] += offs;
        soft_endstop.max[axis] += offs;
      }
      else {
        const float offs = hotend_offset[active_extruder][axis];
        soft_endstop.min[axis] = base_min_pos(axis) + offs;
        soft_endstop.max[axis] = base_max_pos(axis) + offs;
      }

    #else

      soft_endstop.min[axis] = base_min_pos(axis);
      soft_endstop.max[axis] = base_max_pos(axis);

    #endif

  if (DEBUGGING(LEVELING))
    SERIAL_ECHOLNPAIR("Axis ", axis_codes[axis], " min:", soft_endstop.min[axis], " max:", soft_endstop.max[axis]);
}

  /**
   * Constrain the given coordinates to the software endstops.
   *
   * For DELTA/SCARA the XY constraint is based on the smallest
   * radius within the set software endstops.
   */
  void apply_motion_limits(xyz_pos_t &target) {

    if (!soft_endstops_enabled || !all_axes_homed()) return;

    #if IS_KINEMATIC

      #if HAS_HOTEND_OFFSET && ENABLED(DELTA)
        // The effector center position will be the target minus the hotend offset.
        const xy_pos_t offs = hotend_offset[active_extruder];
      #else
        // SCARA needs to consider the angle of the arm through the entire move, so for now use no tool offset.
        constexpr xy_pos_t offs{0};
      #endif

      const float dist_2 = HYPOT2(target.x - offs.x, target.y - offs.y);
      if (dist_2 > delta_max_radius_2)
        target *= delta_max_radius / SQRT(dist_2); // 200 / 300 = 0.66

    #else

      #if !HAS_SOFTWARE_ENDSTOPS || ENABLED(MIN_SOFTWARE_ENDSTOP_X)
        NOLESS(target.x, soft_endstop.min.x);
      #endif
      #if !HAS_SOFTWARE_ENDSTOPS || ENABLED(MAX_SOFTWARE_ENDSTOP_X)
        NOMORE(target.x, soft_endstop.max.x);
      #endif
      #if !HAS_SOFTWARE_ENDSTOPS || ENABLED(MIN_SOFTWARE_ENDSTOP_Y)
        NOLESS(target.y, soft_endstop.min.y);
      #endif
      #if !HAS_SOFTWARE_ENDSTOPS || ENABLED(MAX_SOFTWARE_ENDSTOP_Y)
        NOMORE(target.y, soft_endstop.max.y);
      #endif

    #endif

    #if !HAS_SOFTWARE_ENDSTOPS || ENABLED(MIN_SOFTWARE_ENDSTOP_Z)
      NOLESS(target.z, soft_endstop.min.z);
    #endif
    #if !HAS_SOFTWARE_ENDSTOPS || ENABLED(MAX_SOFTWARE_ENDSTOP_Z)
      NOMORE(target.z, soft_endstop.max.z);
    #endif
  }

#endif // HAS_SOFTWARE_ENDSTOPS

#if IS_KINEMATIC

  #if IS_SCARA
    /**
     * Before raising this value, use M665 S[seg_per_sec] to decrease
     * the number of segments-per-second. Default is 200. Some deltas
     * do better with 160 or lower. It would be good to know how many
     * segments-per-second are actually possible for SCARA on AVR.
     *
     * Longer segments result in less kinematic overhead
     * but may produce jagged lines. Try 0.5mm, 1.0mm, and 2.0mm
     * and compare the difference.
     */
    #define SCARA_MIN_SEGMENT_LENGTH 0.5f
  #endif

  /**
   * Prepare a linear move in a DELTA or SCARA setup.
   *
   * Called from prepare_move_to_destination as the
   * default Delta/SCARA segmenter.
   *
   * This calls planner.buffer_line several times, adding
   * small incremental moves for DELTA or SCARA.
   *
   * For Unified Bed Leveling (Delta or Segmented Cartesian)
   * the ubl.line_to_destination_segmented method replaces this.
   *
   * For Auto Bed Leveling (Bilinear) with SEGMENT_LEVELED_MOVES
   * this is replaced by segmented_line_to_destination below.
   */
  inline bool line_to_destination_kinematic() {

    // Get the top feedrate of the move in the XY plane
    const float scaled_fr_mm_s = MMS_SCALED(feedrate_mm_s);

    const xyze_float_t diff = destination - current_position;

    // If the move is only in Z/E don't split up the move
    if (!diff.x && !diff.y) {
      planner.buffer_line(destination, scaled_fr_mm_s, active_extruder);
      return false; // caller will update current_position
    }

    // Fail if attempting move outside printable radius
    if (!position_is_reachable(destination)) return true;

    // Get the linear distance in XYZ
    float cartesian_mm = diff.magnitude();

    // If the move is very short, check the E move distance
    if (UNEAR_ZERO(cartesian_mm)) cartesian_mm = ABS(diff.e);

    // No E move either? Game over.
    if (UNEAR_ZERO(cartesian_mm)) return true;

    // Minimum number of seconds to move the given distance
    const float seconds = cartesian_mm / scaled_fr_mm_s;

    // The number of segments-per-second times the duration
    // gives the number of segments
    uint16_t segments = delta_segments_per_second * seconds;

    // For SCARA enforce a minimum segment size
    #if IS_SCARA
      NOMORE(segments, cartesian_mm * RECIPROCAL(SCARA_MIN_SEGMENT_LENGTH));
    #endif

    // At least one segment is required
    NOLESS(segments, 1U);

    // The approximate length of each segment
    const float inv_segments = 1.0f / float(segments),
                cartesian_segment_mm = cartesian_mm * inv_segments;
    const xyze_float_t segment_distance = diff * inv_segments;

    #if ENABLED(SCARA_FEEDRATE_SCALING)
      const float inv_duration = scaled_fr_mm_s / cartesian_segment_mm;
    #endif

    /*
    SERIAL_ECHOPAIR("mm=", cartesian_mm);
    SERIAL_ECHOPAIR(" seconds=", seconds);
    SERIAL_ECHOPAIR(" segments=", segments);
    SERIAL_ECHOPAIR(" segment_mm=", cartesian_segment_mm);
    SERIAL_EOL();
    //*/

    // Get the current position as starting point
    xyze_pos_t raw = current_position;

    // Calculate and execute the segments
    while (--segments) {

      static millis_t next_idle_ms = millis() + 200UL;
      thermalManager.manage_heater();  // This returns immediately if not really needed.
      if (ELAPSED(millis(), next_idle_ms)) {
        next_idle_ms = millis() + 200UL;
        idle(false);
      }

      raw += segment_distance;

      if (!planner.buffer_line(raw, scaled_fr_mm_s, active_extruder, cartesian_segment_mm
        #if ENABLED(SCARA_FEEDRATE_SCALING)
          , inv_duration
        #endif
      ))
        break;
    }

    // Ensure last segment arrives at target location.
    planner.buffer_line(destination, scaled_fr_mm_s, active_extruder, cartesian_segment_mm
      #if ENABLED(SCARA_FEEDRATE_SCALING)
        , inv_duration
      #endif
    );

    return false; // caller will update current_position
  }

#else // !IS_KINEMATIC

  #if ENABLED(SEGMENT_LEVELED_MOVES) && DISABLED(AUTO_BED_LEVELING_UBL)

    /**
     * Prepare a segmented move on a CARTESIAN setup.
     *
     * This calls planner.buffer_line several times, adding
     * small incremental moves. This allows the planner to
     * apply more detailed bed leveling to the full move.
     */
    inline void segmented_line_to_destination(const feedRate_t &fr_mm_s, const float segment_size=LEVELED_SEGMENT_LENGTH) {

      const xyze_float_t diff = destination - current_position;

      // If the move is only in Z/E don't split up the move
      if (!diff.x && !diff.y) {
        planner.buffer_line(destination, fr_mm_s, active_extruder);
        return;
      }

      // Get the linear distance in XYZ
      // If the move is very short, check the E move distance
      // No E move either? Game over.
      float cartesian_mm = diff.magnitude();
      if (UNEAR_ZERO(cartesian_mm)) cartesian_mm = ABS(diff.e);
      if (UNEAR_ZERO(cartesian_mm)) return;

      // The length divided by the segment size
      // At least one segment is required
      uint16_t segments = cartesian_mm / segment_size;
      NOLESS(segments, 1U);

      // The approximate length of each segment
      const float inv_segments = 1.0f / float(segments),
                  cartesian_segment_mm = cartesian_mm * inv_segments;
      const xyze_float_t segment_distance = diff * inv_segments;

      #if ENABLED(SCARA_FEEDRATE_SCALING)
        const float inv_duration = scaled_fr_mm_s / cartesian_segment_mm;
      #endif

      // SERIAL_ECHOPAIR("mm=", cartesian_mm);
      // SERIAL_ECHOLNPAIR(" segments=", segments);
      // SERIAL_ECHOLNPAIR(" segment_mm=", cartesian_segment_mm);

      // Get the raw current position as starting point
      xyze_pos_t raw = current_position;

      // Calculate and execute the segments
      while (--segments) {
        static millis_t next_idle_ms = millis() + 200UL;
        thermalManager.manage_heater();  // This returns immediately if not really needed.
        if (ELAPSED(millis(), next_idle_ms)) {
          next_idle_ms = millis() + 200UL;
          idle(false);
        }
        raw += segment_distance;
        if (!planner.buffer_line(raw, fr_mm_s, active_extruder, cartesian_segment_mm
          #if ENABLED(SCARA_FEEDRATE_SCALING)
            , inv_duration
          #endif
        ))
          break;
      }

      // Since segment_distance is only approximate,
      // the final move must be to the exact destination.
      planner.buffer_line(destination, fr_mm_s, active_extruder, cartesian_segment_mm
        #if ENABLED(SCARA_FEEDRATE_SCALING)
          , inv_duration
        #endif
      );
    }

  #endif // SEGMENT_LEVELED_MOVES

  /**
   * Prepare a linear move in a Cartesian setup.
   *
   * When a mesh-based leveling system is active, moves are segmented
   * according to the configuration of the leveling system.
   *
   * Return true if 'current_position' was set to 'destination'
   */
  inline bool prepare_move_to_destination_cartesian() {
    const float scaled_fr_mm_s = MMS_SCALED(feedrate_mm_s);
    #if HAS_MESH
      if (planner.leveling_active && planner.leveling_active_at_z(destination.z)) {
        #if ENABLED(AUTO_BED_LEVELING_UBL)
          // UBL's motion routine needs to know about
          // all moves, including Z-only moves.
          #if ENABLED(SEGMENT_LEVELED_MOVES)
            return ubl.line_to_destination_segmented(scaled_fr_mm_s);
          #else
            ubl.line_to_destination_cartesian(scaled_fr_mm_s, active_extruder);
            return true;
          #endif
        #elif ENABLED(SEGMENT_LEVELED_MOVES)
          segmented_line_to_destination(scaled_fr_mm_s);
          return false; // caller will update current_position
        #else
          /**
           * For MBL and ABL-BILINEAR only segment moves when X or Y are involved.
           * Otherwise fall through to do a direct single move.
           */
          if (xy_pos_t(current_position) != xy_pos_t(destination)) {
            #if ENABLED(MESH_BED_LEVELING)
              mbl.line_to_destination(scaled_fr_mm_s);
            #elif ENABLED(AUTO_BED_LEVELING_BILINEAR)
              bilinear_line_to_destination(scaled_fr_mm_s);
            #endif
            return true;
          }
        #endif
      }
    #endif // HAS_MESH

    planner.buffer_line(destination, scaled_fr_mm_s, active_extruder);
    return false; // caller will update current_position
  }

#endif // !IS_KINEMATIC

#if HAS_DUPLICATION_MODE
  bool extruder_duplication_enabled,
       mirrored_duplication_mode;
  #if ENABLED(MULTI_NOZZLE_DUPLICATION)
    uint8_t duplication_e_mask; // = 0
  #endif
#endif

#if ENABLED(DUAL_X_CARRIAGE)

  DualXMode dual_x_carriage_mode         = DEFAULT_DUAL_X_CARRIAGE_MODE;
  float inactive_extruder_x_pos          = X2_MAX_POS,                    // used in mode 0 & 1
        duplicate_extruder_x_offset      = DEFAULT_DUPLICATION_X_OFFSET;  // used in mode 2
  xyz_pos_t raised_parked_position;                                       // used in mode 1
  bool active_extruder_parked            = false;                         // used in mode 1 & 2
  millis_t delayed_move_time             = 0;                             // used in mode 1
  int16_t duplicate_extruder_temp_offset = 0;                             // used in mode 2

  float x_home_pos(const int extruder) {
    if (extruder == 0)
      return base_home_pos(X_AXIS);
    else
      /**
       * In dual carriage mode the extruder offset provides an override of the
       * second X-carriage position when homed - otherwise X2_HOME_POS is used.
       * This allows soft recalibration of the second extruder home position
       * without firmware reflash (through the M218 command).
       */
      return hotend_offset[1].x > 0 ? hotend_offset[1].x : X2_HOME_POS;
  }

  /**
   * Prepare a linear move in a dual X axis setup
   *
   * Return true if current_position[] was set to destination[]
   */
  inline bool dual_x_carriage_unpark() {
    if (active_extruder_parked) {
      switch (dual_x_carriage_mode) {
        case DXC_FULL_CONTROL_MODE:
          break;
        case DXC_AUTO_PARK_MODE:
          if (current_position.e == destination.e) {
            // This is a travel move (with no extrusion)
            // Skip it, but keep track of the current position
            // (so it can be used as the start of the next non-travel move)
            if (delayed_move_time != 0xFFFFFFFFUL) {
              current_position = destination;
              NOLESS(raised_parked_position.z, destination.z);
              delayed_move_time = millis();
              return true;
            }
          }
          // unpark extruder: 1) raise, 2) move into starting XY position, 3) lower

            #define CUR_X    current_position.x
            #define CUR_Y    current_position.y
            #define CUR_Z    current_position.z
            #define CUR_E    current_position.e
            #define RAISED_X raised_parked_position.x
            #define RAISED_Y raised_parked_position.y
            #define RAISED_Z raised_parked_position.z

            if (  planner.buffer_line(RAISED_X, RAISED_Y, RAISED_Z, CUR_E, planner.settings.max_feedrate_mm_s[Z_AXIS], active_extruder))
              if (planner.buffer_line(   CUR_X,    CUR_Y, RAISED_Z, CUR_E, PLANNER_XY_FEEDRATE(),                      active_extruder))
                  line_to_current_position(planner.settings.max_feedrate_mm_s[Z_AXIS]);
          delayed_move_time = 0;
          active_extruder_parked = false;
          if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Clear active_extruder_parked");
          break;
        case DXC_MIRRORED_MODE:
        case DXC_DUPLICATION_MODE:
          if (active_extruder == 0) {
            xyze_pos_t new_pos = current_position;
            if (dual_x_carriage_mode == DXC_DUPLICATION_MODE)
              new_pos.x += duplicate_extruder_x_offset;
            else
              new_pos.x = inactive_extruder_x_pos;
            // move duplicate extruder into correct duplication position.
            if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("Set planner X", inactive_extruder_x_pos, " ... Line to X", new_pos.x);
            planner.set_position_mm(inactive_extruder_x_pos, current_position.y, current_position.z, current_position.e);
            if (!planner.buffer_line(new_pos, planner.settings.max_feedrate_mm_s[X_AXIS], 1)) break;
            planner.synchronize();
            sync_plan_position();
            extruder_duplication_enabled = true;
            active_extruder_parked = false;
            if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Set extruder_duplication_enabled\nClear active_extruder_parked");
          }
          else if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Active extruder not 0");
          break;
      }
    }
    stepper.set_directions();
    return false;
  }

#endif // DUAL_X_CARRIAGE

void plan_move_by(const feedRate_t fr, const float dx, const float dy, const float dz, const float de){
  /// save default value
  feedRate_t dfr = feedrate_mm_s;
  destination.x = current_position.x + dx;
  destination.y = current_position.y + dy;
  destination.z = current_position.z + dz;
  destination.e = current_position.e + de;
  feedrate_mm_s = fr;
  prepare_move_to_destination();
  /// restore default
  feedrate_mm_s = dfr;
}

/**
 * Prepare a single move and get ready for the next one
 *
 * This may result in several calls to planner.buffer_line to
 * do smaller moves for DELTA, SCARA, mesh moves, etc.
 *
 * Make sure current_position.e and destination.e are good
 * before calling or cold/lengthy extrusion may get missed.
 *
 * Before exit, current_position is set to destination.
 */
void prepare_move_to_destination() {
  apply_motion_limits(destination);

  #if EITHER(PREVENT_COLD_EXTRUSION, PREVENT_LENGTHY_EXTRUDE)

    if (!DEBUGGING(DRYRUN)) {
      if (destination.e != current_position.e) {
        #if ENABLED(PREVENT_COLD_EXTRUSION)
          if (thermalManager.tooColdToExtrude(active_extruder)) {
            current_position.e = destination.e; // Behave as if the move really took place, but ignore E part
            SERIAL_ECHO_MSG(MSG_ERR_COLD_EXTRUDE_STOP);
          }
        #endif // PREVENT_COLD_EXTRUSION
        #if ENABLED(PREVENT_LENGTHY_EXTRUDE)
          const float e_delta = ABS(destination.e - current_position.e) * planner.e_factor[active_extruder];
          if (e_delta > (EXTRUDE_MAXLENGTH)) {
            #if ENABLED(MIXING_EXTRUDER)
              bool ignore_e = false;
              float collector[MIXING_STEPPERS];
              mixer.refresh_collector(1.0, mixer.get_current_vtool(), collector);
              MIXER_STEPPER_LOOP(e)
                if (e_delta * collector[e] > (EXTRUDE_MAXLENGTH)) { ignore_e = true; break; }
            #else
              constexpr bool ignore_e = true;
            #endif
            if (ignore_e) {
              current_position.e = destination.e; // Behave as if the move really took place, but ignore E part
              SERIAL_ECHO_MSG(MSG_ERR_LONG_EXTRUDE_STOP);
            }
          }
        #endif // PREVENT_LENGTHY_EXTRUDE
      }
    }

  #endif // PREVENT_COLD_EXTRUSION || PREVENT_LENGTHY_EXTRUDE

  #if ENABLED(DUAL_X_CARRIAGE)
    if (dual_x_carriage_unpark()) return;
  #endif

  if (
    #if UBL_SEGMENTED
      #if IS_KINEMATIC // UBL using Kinematic / Cartesian cases as a workaround for now.
        ubl.line_to_destination_segmented(MMS_SCALED(feedrate_mm_s))
      #else
        prepare_move_to_destination_cartesian()
      #endif
    #elif IS_KINEMATIC
      line_to_destination_kinematic()
    #else
      prepare_move_to_destination_cartesian()
    #endif
  ) return;

  current_position = destination;
}

uint8_t axes_need_homing(uint8_t axis_bits/*=0x07*/) {
  #if ENABLED(HOME_AFTER_DEACTIVATE)
    #define HOMED_FLAGS axis_known_position
  #else
    #define HOMED_FLAGS axis_homed
  #endif
  // Clear test bits that are homed
  if (TEST(axis_bits, X_AXIS) && TEST(HOMED_FLAGS, X_AXIS)) CBI(axis_bits, X_AXIS);
  if (TEST(axis_bits, Y_AXIS) && TEST(HOMED_FLAGS, Y_AXIS)) CBI(axis_bits, Y_AXIS);
  if (TEST(axis_bits, Z_AXIS) && TEST(HOMED_FLAGS, Z_AXIS)) CBI(axis_bits, Z_AXIS);
  return axis_bits;
}

bool axis_unhomed_error(uint8_t axis_bits/*=0x07*/) {
  if ((axis_bits = axes_need_homing(axis_bits))) {
    PGM_P home_first = GET_TEXT(MSG_HOME_FIRST);
    char msg[strlen_P(home_first)+1];
    sprintf_P(msg, home_first,
      TEST(axis_bits, X_AXIS) ? "X" : "",
      TEST(axis_bits, Y_AXIS) ? "Y" : "",
      TEST(axis_bits, Z_AXIS) ? "Z" : ""
    );
    SERIAL_ECHO_START();
    SERIAL_ECHOLN(msg);
    #if HAS_DISPLAY
      ui.set_status(msg);
    #endif
    return true;
  }
  return false;
}

/**
 * Homing bump feedrate (mm/s)
 */
feedRate_t get_homing_bump_feedrate(const AxisEnum axis) {
  float hbd = homing_bump_divisor[axis];
  if (hbd < 0.5) {
    hbd = 10;
    SERIAL_ECHO_MSG("Warning: Homing Bump Divisor < 0.5");
  }
  return homing_feedrate(axis) / float(hbd);
}

  #if ENABLED(SENSORLESS_HOMING)
    /**
     * Set sensorless homing if the axis has it, accounting for Core Kinematics.
     */
    sensorless_t start_sensorless_homing_per_axis(const AxisEnum axis) {
      sensorless_t stealth_states { false };

      switch (axis) {
        default: break;
        #if X_SENSORLESS
          case X_AXIS:
            #if ENABLED(CRASH_RECOVERY)
              crash_s.start_sensorless_homing_per_axis(axis);
            #endif

            stealth_states.x = tmc_enable_stallguard(stepperX);
            TERN_(X2_SENSORLESS, stealth_states.x2 = tmc_enable_stallguard(stepperX2));
            #if ANY(CORE_IS_XY, MARKFORGED_XY, MARKFORGED_YX) && Y_SENSORLESS
              stealth_states.y = tmc_enable_stallguard(stepperY);
            #elif CORE_IS_XZ && Z_SENSORLESS
              stealth_states.z = tmc_enable_stallguard(stepperZ);
            #endif
            break;
        #endif
        #if Y_SENSORLESS
          case Y_AXIS:
            #if ENABLED(CRASH_RECOVERY)
              crash_s.start_sensorless_homing_per_axis(axis);
            #endif

            stealth_states.y = tmc_enable_stallguard(stepperY);
            TERN_(Y2_SENSORLESS, stealth_states.y2 = tmc_enable_stallguard(stepperY2));
            #if ANY(CORE_IS_XY, MARKFORGED_XY, MARKFORGED_YX) && X_SENSORLESS
              stealth_states.x = tmc_enable_stallguard(stepperX);
            #elif CORE_IS_YZ && Z_SENSORLESS
              stealth_states.z = tmc_enable_stallguard(stepperZ);
            #endif
            break;
        #endif
        #if Z_SENSORLESS
          case Z_AXIS:
            stealth_states.z = tmc_enable_stallguard(stepperZ);
            TERN_(Z2_SENSORLESS, stealth_states.z2 = tmc_enable_stallguard(stepperZ2));
            TERN_(Z3_SENSORLESS, stealth_states.z3 = tmc_enable_stallguard(stepperZ3));
            TERN_(Z4_SENSORLESS, stealth_states.z4 = tmc_enable_stallguard(stepperZ4));
            #if CORE_IS_XZ && X_SENSORLESS
              stealth_states.x = tmc_enable_stallguard(stepperX);
            #elif CORE_IS_YZ && Y_SENSORLESS
              stealth_states.y = tmc_enable_stallguard(stepperY);
            #endif
            break;
        #endif
        #if I_SENSORLESS
          case I_AXIS: stealth_states.i = tmc_enable_stallguard(stepperI); break;
        #endif
        #if J_SENSORLESS
          case J_AXIS: stealth_states.j = tmc_enable_stallguard(stepperJ); break;
        #endif
        #if K_SENSORLESS
          case K_AXIS: stealth_states.k = tmc_enable_stallguard(stepperK); break;
        #endif
        #if U_SENSORLESS
          case U_AXIS: stealth_states.u = tmc_enable_stallguard(stepperU); break;
        #endif
        #if V_SENSORLESS
          case V_AXIS: stealth_states.v = tmc_enable_stallguard(stepperV); break;
        #endif
        #if W_SENSORLESS
          case W_AXIS: stealth_states.w = tmc_enable_stallguard(stepperW); break;
        #endif
      }

      #if ENABLED(SPI_ENDSTOPS)
        switch (axis) {
          case X_AXIS: if (ENABLED(X_SPI_SENSORLESS)) endstops.tmc_spi_homing.x = true; break;
          #if HAS_Y_AXIS
            case Y_AXIS: if (ENABLED(Y_SPI_SENSORLESS)) endstops.tmc_spi_homing.y = true; break;
          #endif
          #if HAS_Z_AXIS
            case Z_AXIS: if (ENABLED(Z_SPI_SENSORLESS)) endstops.tmc_spi_homing.z = true; break;
          #endif
          #if HAS_I_AXIS
            case I_AXIS: if (ENABLED(I_SPI_SENSORLESS)) endstops.tmc_spi_homing.i = true; break;
          #endif
          #if HAS_J_AXIS
            case J_AXIS: if (ENABLED(J_SPI_SENSORLESS)) endstops.tmc_spi_homing.j = true; break;
          #endif
          #if HAS_K_AXIS
            case K_AXIS: if (ENABLED(K_SPI_SENSORLESS)) endstops.tmc_spi_homing.k = true; break;
          #endif
          #if HAS_U_AXIS
            case U_AXIS: if (ENABLED(U_SPI_SENSORLESS)) endstops.tmc_spi_homing.u = true; break;
          #endif
          #if HAS_V_AXIS
            case V_AXIS: if (ENABLED(V_SPI_SENSORLESS)) endstops.tmc_spi_homing.v = true; break;
          #endif
          #if HAS_W_AXIS
            case W_AXIS: if (ENABLED(W_SPI_SENSORLESS)) endstops.tmc_spi_homing.w = true; break;
          #endif
          default: break;
        }
      #endif

      #if ENABLED(IMPROVE_HOMING_RELIABILITY) && HOMING_SG_GUARD_DURATION > 0
        sg_guard_period = millis() + default_sg_guard_duration;
      #endif

      return stealth_states;
    }

    void end_sensorless_homing_per_axis(const AxisEnum axis, sensorless_t enable_stealth) {
      switch (axis) {
        default: break;
        #if X_SENSORLESS
          case X_AXIS:
            #if ENABLED(CRASH_RECOVERY)
              crash_s.end_sensorless_homing_per_axis(axis, enable_stealth.x);
            #else
              tmc_disable_stallguard(stepperX, enable_stealth.x);
              TERN_(X2_SENSORLESS, tmc_disable_stallguard(stepperX2, enable_stealth.x2));
              #if ANY(CORE_IS_XY, MARKFORGED_XY, MARKFORGED_YX) && Y_SENSORLESS
                tmc_disable_stallguard(stepperY, enable_stealth.y);
              #elif CORE_IS_XZ && Z_SENSORLESS
                tmc_disable_stallguard(stepperZ, enable_stealth.z);
              #endif
            #endif // ENABLED(CRASH_RECOVERY)
          break;
        #endif
        #if Y_SENSORLESS
          case Y_AXIS:
            #if ENABLED(CRASH_RECOVERY)
              crash_s.end_sensorless_homing_per_axis(axis, enable_stealth.y);
            #else
              tmc_disable_stallguard(stepperY, enable_stealth.y);
              TERN_(Y2_SENSORLESS, tmc_disable_stallguard(stepperY2, enable_stealth.y2));
              #if ANY(CORE_IS_XY, MARKFORGED_XY, MARKFORGED_YX) && X_SENSORLESS
                tmc_disable_stallguard(stepperX, enable_stealth.x);
              #elif CORE_IS_YZ && Z_SENSORLESS
                tmc_disable_stallguard(stepperZ, enable_stealth.z);
              #endif
            #endif // ENABLED(CRASH_RECOVERY)
          break;
        #endif
        #if Z_SENSORLESS
          case Z_AXIS:
            tmc_disable_stallguard(stepperZ, enable_stealth.z);
            TERN_(Z2_SENSORLESS, tmc_disable_stallguard(stepperZ2, enable_stealth.z2));
            TERN_(Z3_SENSORLESS, tmc_disable_stallguard(stepperZ3, enable_stealth.z3));
            TERN_(Z4_SENSORLESS, tmc_disable_stallguard(stepperZ4, enable_stealth.z4));
            #if CORE_IS_XZ && X_SENSORLESS
              tmc_disable_stallguard(stepperX, enable_stealth.x);
            #elif CORE_IS_YZ && Y_SENSORLESS
              tmc_disable_stallguard(stepperY, enable_stealth.y);
            #endif
            break;
        #endif
        #if I_SENSORLESS
          case I_AXIS: tmc_disable_stallguard(stepperI, enable_stealth.i); break;
        #endif
        #if J_SENSORLESS
          case J_AXIS: tmc_disable_stallguard(stepperJ, enable_stealth.j); break;
        #endif
        #if K_SENSORLESS
          case K_AXIS: tmc_disable_stallguard(stepperK, enable_stealth.k); break;
        #endif
        #if U_SENSORLESS
          case U_AXIS: tmc_disable_stallguard(stepperU, enable_stealth.u); break;
        #endif
        #if V_SENSORLESS
          case V_AXIS: tmc_disable_stallguard(stepperV, enable_stealth.v); break;
        #endif
        #if W_SENSORLESS
          case W_AXIS: tmc_disable_stallguard(stepperW, enable_stealth.w); break;
        #endif
      }

      #if ENABLED(SPI_ENDSTOPS)
        switch (axis) {
          case X_AXIS: if (ENABLED(X_SPI_SENSORLESS)) endstops.tmc_spi_homing.x = false; break;
          #if HAS_Y_AXIS
            case Y_AXIS: if (ENABLED(Y_SPI_SENSORLESS)) endstops.tmc_spi_homing.y = false; break;
          #endif
          #if HAS_Z_AXIS
            case Z_AXIS: if (ENABLED(Z_SPI_SENSORLESS)) endstops.tmc_spi_homing.z = false; break;
          #endif
          #if HAS_I_AXIS
            case I_AXIS: if (ENABLED(I_SPI_SENSORLESS)) endstops.tmc_spi_homing.i = false; break;
          #endif
          #if HAS_J_AXIS
            case J_AXIS: if (ENABLED(J_SPI_SENSORLESS)) endstops.tmc_spi_homing.j = false; break;
          #endif
          #if HAS_K_AXIS
            case K_AXIS: if (ENABLED(K_SPI_SENSORLESS)) endstops.tmc_spi_homing.k = false; break;
          #endif
          #if HAS_U_AXIS
            case U_AXIS: if (ENABLED(U_SPI_SENSORLESS)) endstops.tmc_spi_homing.u = false; break;
          #endif
          #if HAS_V_AXIS
            case V_AXIS: if (ENABLED(V_SPI_SENSORLESS)) endstops.tmc_spi_homing.v = false; break;
          #endif
          #if HAS_W_AXIS
            case W_AXIS: if (ENABLED(W_SPI_SENSORLESS)) endstops.tmc_spi_homing.w = false; break;
          #endif
          default: break;
        }
      #endif
    }

  #endif // SENSORLESS_HOMING

/**
 * Home an individual linear axis
 * @param homing_z_with_probe false to use sensorless homing instead of probe
 */
void do_homing_move(const AxisEnum axis, const float distance, const feedRate_t fr_mm_s, [[maybe_unused]] bool can_move_back_before_homing, [[maybe_unused]] bool homing_z_with_probe) {

  if (DEBUGGING(LEVELING)) {
    DEBUG_ECHOPAIR(">>> do_homing_move(", axis_codes[axis], ", ", distance, ", ");
    if (fr_mm_s)
      DEBUG_ECHO(fr_mm_s);
    else
      DEBUG_ECHOPAIR("[", homing_feedrate(axis), "]");
    DEBUG_ECHOLNPGM(")");
  }

  #if HOMING_Z_WITH_PROBE && HAS_HEATED_BED && ENABLED(WAIT_FOR_BED_HEATER)
    if (homing_z_with_probe) {
      // Wait for bed to heat back up between probing points
      if (axis == Z_AXIS && distance < 0 && thermalManager.isHeatingBed()) {
        serialprintPGM(msg_wait_for_bed_heating);
        LCD_MESSAGEPGM(MSG_BED_HEATING);
        thermalManager.wait_for_bed();
        ui.reset_status();
      }
    }
  #endif

    #if ENABLED(SENSORLESS_HOMING)
      sensorless_t stealth_states;
    #endif

  #if HOMING_Z_WITH_PROBE && QUIET_PROBING
    if (axis == Z_AXIS && homing_z_with_probe) {
      probing_pause(true);
    }
  #endif

  #if HOMING_Z_WITH_PROBE
    [[maybe_unused]] bool moving_probe_toward_bed = false;
    if (axis == Z_AXIS && homing_z_with_probe)
      moving_probe_toward_bed = (home_dir(axis) > 0) == (distance > 0);
  #endif

      // Disable stealthChop if used. Enable diag1 pin on driver.
      #if ENABLED(SENSORLESS_HOMING)
        bool enable_sensorless_homing =
        #if HOMING_Z_WITH_PROBE && !Z_SENSORLESS
          !moving_probe_toward_bed || !homing_z_with_probe
        #else
          true
        #endif
          ;
        if (enable_sensorless_homing) {
          stealth_states = start_sensorless_homing_per_axis(axis);
          #if SENSORLESS_STALLGUARD_DELAY
            safe_delay(SENSORLESS_STALLGUARD_DELAY); // Short delay needed to settle
          #endif
	}
      #endif

  const feedRate_t real_fr_mm_s = fr_mm_s ?: homing_feedrate(axis);

  #if ENABLED(MOVE_BACK_BEFORE_HOMING)
    if (can_move_back_before_homing && ((axis == X_AXIS) || (axis == Y_AXIS))) {
      abce_pos_t target = { planner.get_axis_position_mm(A_AXIS), planner.get_axis_position_mm(B_AXIS), planner.get_axis_position_mm(C_AXIS), planner.get_axis_position_mm(E_AXIS) };
      target[axis] = 0;
      planner.set_machine_position_mm(target);
      float dist = (distance > 0) ? -MOVE_BACK_BEFORE_HOMING_DISTANCE : MOVE_BACK_BEFORE_HOMING_DISTANCE;
      target[axis] = dist;
      
      #if IS_KINEMATIC && DISABLED(CLASSIC_JERK)
        const xyze_float_t delta_mm_cart{0};
      #endif
      
      // Set delta/cartesian axes directly
      planner.buffer_segment(target      
        #if IS_KINEMATIC && DISABLED(CLASSIC_JERK)
          , delta_mm_cart
        #endif
        , real_fr_mm_s, active_extruder
      );

      planner.synchronize();
  }
  #endif

  #if ENABLED(NOZZLE_LOAD_CELL) && HOMING_Z_WITH_PROBE
    if (homing_z_with_probe) {
      endstops.enable_z_probe(moving_probe_toward_bed);
    }

    // These guards cannot be hidden behind if (homing_z_with_probe) {
    auto precisionEnabler = Loadcell::HighPrecisionEnabler(loadcell, moving_probe_toward_bed && homing_z_with_probe);
    auto H = loadcell.CreateLoadAboveErrEnforcer(3000, moving_probe_toward_bed && homing_z_with_probe);
  #endif

  #if IS_SCARA
    // Tell the planner the axis is at 0
    current_position[axis] = 0;
    sync_plan_position();
    current_position[axis] = distance;
    line_to_current_position(real_fr_mm_s);
  #else
    abce_pos_t target = { planner.get_axis_position_mm(A_AXIS), planner.get_axis_position_mm(B_AXIS), planner.get_axis_position_mm(C_AXIS), planner.get_axis_position_mm(E_AXIS) };
    target[axis] = 0;
    planner.set_machine_position_mm(target);
    target[axis] = distance;

    #if IS_KINEMATIC && DISABLED(CLASSIC_JERK)
      const xyze_float_t delta_mm_cart{0};
    #endif

    // Set delta/cartesian axes directly
    planner.buffer_segment(target
      #if IS_KINEMATIC && DISABLED(CLASSIC_JERK)
        , delta_mm_cart
      #endif
      , real_fr_mm_s, active_extruder
    );

  #endif

  planner.synchronize();

  #if ENABLED(NOZZLE_LOAD_CELL) && HOMING_Z_WITH_PROBE
    if (homing_z_with_probe) {
      endstops.enable_z_probe(false);
    }
  #endif

  #if HOMING_Z_WITH_PROBE && QUIET_PROBING
    if (axis == Z_AXIS && homing_z_with_probe) {
      probing_pause(false);
    }
  #endif

  endstops.validate_homing_move();

      // Re-enable stealthChop if used. Disable diag1 pin on driver.
      #if ENABLED(SENSORLESS_HOMING)
	if (enable_sensorless_homing) {
	  end_sensorless_homing_per_axis(axis, stealth_states);
          #if SENSORLESS_STALLGUARD_DELAY
            safe_delay(SENSORLESS_STALLGUARD_DELAY); // Short delay needed to settle
          #endif
	}
      #endif

  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("<<< do_homing_move(", axis_codes[axis], ")");
}

static void do_blocking_move_axis(const AxisEnum axis, const float distance, const feedRate_t fr_mm_s) {
  if (DEBUGGING(LEVELING)) {
    DEBUG_ECHOPAIR(">>> do_blocking_move_axis(", axis_codes[axis], ", ", distance, ", ");
    if (fr_mm_s)
      DEBUG_ECHO(fr_mm_s);
    else
      DEBUG_ECHOPAIR("[", homing_feedrate(axis), "]");
    DEBUG_ECHOLNPGM(")");
  }

  const feedRate_t real_fr_mm_s = fr_mm_s ?: homing_feedrate(axis);

  #if IS_SCARA
    // Tell the planner the axis is at 0
    current_position[axis] = 0;
    sync_plan_position();
    current_position[axis] = distance;
    line_to_current_position(real_fr_mm_s);
  #else
    abce_pos_t target = { planner.get_axis_position_mm(A_AXIS), planner.get_axis_position_mm(B_AXIS), planner.get_axis_position_mm(C_AXIS), planner.get_axis_position_mm(E_AXIS) };
    target[axis] = 0;
    planner.set_machine_position_mm(target);
    target[axis] = distance;

    #if IS_KINEMATIC && DISABLED(CLASSIC_JERK)
      const xyze_float_t delta_mm_cart{0};
    #endif

    // Set delta/cartesian axes directly
    planner.buffer_segment(target
      #if IS_KINEMATIC && DISABLED(CLASSIC_JERK)
        , delta_mm_cart
      #endif
      , real_fr_mm_s, active_extruder
    );

  #endif

  planner.synchronize();

  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("<<< do_blocking_move_axis(", axis_codes[axis], ")");
}

/**
 * Set an axis' current position to its home position (after homing).
 *
 * For Core and Cartesian robots this applies one-to-one when an
 * individual axis has been homed.
 *
 * DELTA should wait until all homing is done before setting the XYZ
 * current_position to home, because homing is a single operation.
 * In the case where the axis positions are already known and previously
 * homed, DELTA could home to X or Y individually by moving either one
 * to the center. However, homing Z always homes XY and Z.
 *
 * SCARA should wait until all XY homing is done before setting the XY
 * current_position to home, because neither X nor Y is at home until
 * both are at home. Z can however be homed individually.
 *
 * Callers must sync the planner position after calling this!
 *
 * @param homing_z_with_probe false when sensorless homing was used instead of probe
 */
void set_axis_is_at_home(const AxisEnum axis, [[maybe_unused]] bool homing_z_with_probe) {
  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR(">>> set_axis_is_at_home(", axis_codes[axis], ")");

  SBI(axis_known_position, axis);
  SBI(axis_homed, axis);

  #if ENABLED(DUAL_X_CARRIAGE)
    if (axis == X_AXIS && (active_extruder == 1 || dual_x_carriage_mode == DXC_DUPLICATION_MODE)) {
      current_position.x = x_home_pos(active_extruder);
      return;
    }
  #endif

  #if ENABLED(MORGAN_SCARA)
    scara_set_axis_is_at_home(axis);
  #elif ENABLED(DELTA)
    current_position[axis] = (axis == Z_AXIS ? delta_height
    #if HAS_BED_PROBE
      - probe_offset.z
    #endif
    : base_home_pos(axis));
  #else
    #ifdef WORKSPACE_HOME 
      /*Fill workspace_homes[] with data from config*/
      xyz_pos_t workspace_homes[MAX_COORDINATE_SYSTEMS]={{{{0}}}};

      #ifdef WORKSPACE_0_X_POS
        workspace_homes[0].set(WORKSPACE_0_X_POS, WORKSPACE_0_Y_POS, WORKSPACE_0_Z_POS);
      #endif
      #ifdef WORKSPACE_1_X_POS
        workspace_homes[1].set(WORKSPACE_1_X_POS, WORKSPACE_1_Y_POS, WORKSPACE_1_Z_POS);
      #endif
      #ifdef WORKSPACE_2_X_POS
        workspace_homes[2].set(WORKSPACE_2_X_POS, WORKSPACE_2_Y_POS, WORKSPACE_2_Z_POS);
      #endif
      #ifdef WORKSPACE_3_X_POS
        workspace_homes[3].set(WORKSPACE_3_X_POS, WORKSPACE_3_Y_POS, WORKSPACE_3_Z_POS);
      #endif
      #ifdef WORKSPACE_4_X_POS
        workspace_homes[4].set(WORKSPACE_4_X_POS, WORKSPACE_4_Y_POS, WORKSPACE_4_Z_POS);
      #endif
      #ifdef WORKSPACE_5_X_POS
        workspace_homes[5].set(WORKSPACE_5_X_POS, WORKSPACE_5_Y_POS, WORKSPACE_5_Z_POS);
      #endif
      #ifdef WORKSPACE_6_X_POS
        workspace_homes[6].set(WORKSPACE_6_X_POS, WORKSPACE_6_Y_POS, WORKSPACE_6_Z_POS);
      #endif
      #ifdef WORKSPACE_7_X_POS
        workspace_homes[7].set(WORKSPACE_7_X_POS, WORKSPACE_7_Y_POS, WORKSPACE_7_Z_POS);
      #endif
      #ifdef WORKSPACE_8_X_POS
        workspace_homes[8].set(WORKSPACE_8_X_POS, WORKSPACE_8_Y_POS, WORKSPACE_8_Z_POS);
      #endif
      #ifdef WORKSPACE_9_X_POS
        workspace_homes[9].set(WORKSPACE_9_X_POS, WORKSPACE_9_Y_POS, WORKSPACE_9_Z_POS);
      #endif
      

      int8_t active_coordinate_system = GcodeSuite::get_coordinate_system();
      if (active_coordinate_system == -1){ /*If base coordinate system, proceed as usual*/
        current_position[axis] = base_home_pos(axis);
      } else { /*If in alternate system, update position shift and system offset from base system*/
        position_shift[axis] = - current_position[axis] + workspace_homes[active_coordinate_system][axis];
        GcodeSuite::set_coordinate_system_offset(0, axis, position_shift[axis]);
        update_workspace_offset(axis);        
      }
    #else
      current_position[axis] = base_home_pos(axis)
        #if ENABLED(PRECISE_HOMING)
          - calibrated_home_offset(axis)
        #endif // ENABLED(PRECISE_HOMING)
      ;
    #endif
  #endif

  /**
   * Z Probe Z Homing? Account for the probe's Z offset.
   */
  #if HAS_BED_PROBE && Z_HOME_DIR < 0
    if (axis == Z_AXIS) {
      #if HOMING_Z_WITH_PROBE
        if (homing_z_with_probe) {
          current_position.z -= probe_offset.z;

          if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("*** Z HOMED WITH PROBE (Z_MIN_PROBE_USES_Z_MIN_ENDSTOP_PIN) ***\n> probe_offset.z = ", probe_offset.z);
        } else
      #endif /*HOMING_Z_WITH_PROBE*/
      {
        if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("*** Z HOMED TO ENDSTOP ***");
      }
    }
  #endif

  #if ENABLED(I2C_POSITION_ENCODERS)
    I2CPEM.homed(axis);
  #endif

  #if ENABLED(BABYSTEP_DISPLAY_TOTAL)
    babystep.reset_total(axis);
  #endif

  if (DEBUGGING(LEVELING)) {
    #if HAS_HOME_OFFSET
      DEBUG_ECHOLNPAIR("> home_offset[", axis_codes[axis], "] = ", home_offset[axis]);
    #endif
    DEBUG_POS("", current_position);
    DEBUG_ECHOLNPAIR("<<< set_axis_is_at_home(", axis_codes[axis], ")");
  }
}

/**
 * Set an axis' to be unhomed.
 */
void set_axis_is_not_at_home(const AxisEnum axis) {
  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR(">>> set_axis_is_not_at_home(", axis_codes[axis], ")");

  CBI(axis_known_position, axis);
  CBI(axis_homed, axis);

  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("<<< set_axis_is_not_at_home(", axis_codes[axis], ")");

  #if ENABLED(I2C_POSITION_ENCODERS)
    I2CPEM.unhomed(axis);
  #endif
}

// declare function used by homeaxis
static float homeaxis_single_run(const AxisEnum axis, const int axis_home_dir, const feedRate_t fr_mm_s=0.0, bool invert_home_dir=false, bool homing_z_with_probe = true);

// those metrics are intentionally not static, as it is expected that they might be referenced
// from outside this file for early registration
metric_t metric_home_diff = METRIC("home_diff", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_ENABLE_ALL);

#if ENABLED(PRECISE_HOMING_COREXY)
  // convert raw AB steps to XY mm
  void corexy_ab_to_xy(const xy_long_t& steps, xy_pos_t& mm) {
    float x = static_cast<float>(steps.a + steps.b) / 2.f;
    float y = static_cast<float>(CORESIGN(steps.a - steps.b)) / 2.f;
    mm.x = x / planner.settings.axis_steps_per_mm[0];
    mm.y = y / planner.settings.axis_steps_per_mm[1];
  }

  // convert raw AB steps to XY mm and position
  static void corexy_ab_to_xy(const xy_long_t& steps, xy_pos_t& mm, xy_long_t& pos) {
    float x = static_cast<float>(steps.a + steps.b) / 2.f;
    float y = static_cast<float>(CORESIGN(steps.a - steps.b)) / 2.f;
    mm.x = x / planner.settings.axis_steps_per_mm[0];
    mm.y = y / planner.settings.axis_steps_per_mm[1];
    pos.x = LROUND(x);
    pos.y = LROUND(y);
  }

  // convert raw AB steps to XY mm, filling others from current state
  void corexy_ab_to_xyze(const xy_long_t& steps, xyze_pos_t& mm) {
    corexy_ab_to_xy(steps, mm);
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
      mm[i] = planner.get_axis_position_mm((AxisEnum)i);
    }
  }

  // convert raw AB steps to XY mm and position, filling others from current state
  static void corexy_ab_to_xyze(const xy_long_t& steps, xyze_pos_t& mm, xyze_long_t& pos) {
    pos = planner.get_position();
    corexy_ab_to_xy(steps, mm, pos);
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
      mm[i] = planner.get_axis_position_mm((AxisEnum)i);
    }
  }

  static void plan_raw_move(const xyze_pos_t target_mm, const xyze_long_t target_pos,
    const xyze_long_t delta_steps, const feedRate_t fr_mm_s) {
    planner._buffer_steps_raw(target_pos, target_mm, delta_steps, fr_mm_s, active_extruder);
    planner.synchronize();
  }

  static void plan_corexy_raw_move(const xy_long_t& target_steps_ab, const feedRate_t fr_mm_s) {
    // reconstruct full final position
    xyze_pos_t target_mm;
    xyze_long_t target_pos;
    corexy_ab_to_xyze(target_steps_ab, target_mm, target_pos);

    // axis shift
    xyze_long_t cur_steps = { stepper.position(A_AXIS), stepper.position(B_AXIS),
                              stepper.position(C_AXIS), stepper.position(E_AXIS) };
    xyze_long_t target_steps = { target_steps_ab.a, target_steps_ab.b, cur_steps.c, cur_steps.e };

    plan_raw_move(target_mm, target_pos, target_steps - cur_steps, fr_mm_s);
  }

  // TMC µsteps(phase) per Marlin µsteps
  static int16_t phase_per_ustep(const AxisEnum axis) {
    return 256 / stepper_axis(axis).microsteps();
  }

  // TMC full cycle µsteps per Marlin µsteps
  static int16_t phase_cycle_steps(const AxisEnum axis) {
    return 1024 / phase_per_ustep(axis);
  }

  static int16_t phase_backoff_steps(const AxisEnum axis) {
    int16_t effectorBackoutDir; // Direction in which the effector mm coordinates move away from endstop.
    int16_t stepperBackoutDir;  // Direction in which the TMC µstep count(phase) move away from endstop.
    switch (axis) {
    case X_AXIS:
      effectorBackoutDir = -X_HOME_DIR;
      stepperBackoutDir = IF_DISABLED(INVERT_X_DIR, -)effectorBackoutDir;
      break;
    case Y_AXIS:
      effectorBackoutDir = -Y_HOME_DIR;
      stepperBackoutDir = IF_DISABLED(INVERT_Y_DIR, -)effectorBackoutDir;
      break;
    default:
      bsod("invalid backoff axis");
    }

    int16_t phaseCurrent = stepper_axis(axis).MSCNT(); // The TMC µsteps(phase) count of the current position
    int16_t phaseDelta = (0 - phaseCurrent) * stepperBackoutDir;
    if (phaseDelta < 0) phaseDelta += 1024;
    return int16_t(phaseDelta / phase_per_ustep(axis)) * effectorBackoutDir;
  }

  static bool phase_aligned(AxisEnum axis) {
    int16_t phase_cur = stepper_axis(axis).MSCNT();
    int16_t ustep_max = phase_per_ustep(axis) / 2;
    return (phase_cur <= ustep_max || phase_cur >= (1024 - ustep_max));
  }

  /**
   * @brief Part of precise homing.
   * @param origin_steps
   * @param dist
   * @param m_steps
   * @param m_dist
   * @param orig_crash whether crash_s was active before temporarily disabling it
   * @return
   */
  static bool measure_b_axis_distance(xy_long_t origin_steps, int32_t dist, int32_t& m_steps, float& m_dist, const bool orig_crash) {
    // full initial position
    xyze_long_t initial_steps = { origin_steps.a, origin_steps.b, stepper.position(C_AXIS), stepper.position(E_AXIS) };
    xyze_pos_t initial_mm;
    corexy_ab_to_xyze(initial_steps, initial_mm);

    // full target position
    xyze_long_t target_steps = { initial_steps.a, initial_steps.b + dist, initial_steps.c, initial_steps.e };
    xyze_pos_t target_mm;
    xyze_long_t target_pos;
    corexy_ab_to_xy(target_steps, target_mm, target_pos);
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
      target_mm[i] = initial_mm[i];
    }
    xyze_long_t initial_pos = planner.get_position();
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
      target_pos[i] = initial_pos[i];
    }

    // move towards the endstop
    sensorless_t stealth_states = start_sensorless_homing_per_axis(B_AXIS);
    endstops.enable(true);
    plan_raw_move(target_mm, target_pos, target_steps - initial_steps, homing_feedrate(B_AXIS));
    uint8_t hit = endstops.trigger_state();
    endstops.not_homing();

    xyze_long_t hit_steps;
    xyze_pos_t hit_mm;
    if (hit)
    {
      // resync position from steppers to get hit position
      endstops.hit_on_purpose();
      planner.reset_position();
      hit_steps = { stepper.position(A_AXIS), stepper.position(B_AXIS), stepper.position(C_AXIS), stepper.position(E_AXIS) };
      corexy_ab_to_xyze(hit_steps, hit_mm);
    }
    else
    {
      hit_steps = target_steps;
      hit_mm = target_mm;
    }
    end_sensorless_homing_per_axis(B_AXIS, stealth_states);

    // move back to starting point
    plan_raw_move(initial_mm, initial_pos, initial_steps - hit_steps, homing_feedrate(B_AXIS));

    // sanity checks
    if(hit_steps.a != initial_steps.a || initial_steps.a != stepper.position(A_AXIS)) {
      ui.status_printf_P(0, "A_AXIS didn't return");
      homing_failed([]() { fatal_error(ErrCode::ERR_MECHANICAL_PRECISE_REFINEMENT_FAILED); }, orig_crash);
    }
    if(initial_steps.b != stepper.position(B_AXIS)) {
      ui.status_printf_P(0, "B_AXIS didn't return");
      homing_failed([]() { fatal_error(ErrCode::ERR_MECHANICAL_PRECISE_REFINEMENT_FAILED); }, orig_crash);
    }

    // result values
    m_steps = hit_steps.b - initial_steps.b;
    m_dist = hypotf(hit_mm[X_AXIS] - initial_mm[X_AXIS], hit_mm[Y_AXIS] - initial_mm[Y_AXIS]);
    return hit;
  }

  /**
   * @brief Return struct for measure_phase_cycles.
   */
  struct measure_phase_cycles_ret {
    bool output_valid; // c_dist_n are valid and position can be refined
    bool success; // True on success
    measure_phase_cycles_ret(bool output_valid_, bool success_) : output_valid(output_valid_), success(success_) {}
  };

  /**
   * @brief Part of precise homing.
   * @param c_dist_a
   * @param c_dist_b
   * @param orig_crash whether crash_s was active before temporarily disabling it
   * @return output validity and successfulness
   */
  static measure_phase_cycles_ret measure_phase_cycles(int32_t& c_dist_a, int32_t& c_dist_b, const bool orig_crash) {
    const int32_t measure_max_dist = (XY_HOMING_ORIGIN_OFFSET * 4) / planner.mm_per_step[B_AXIS];
    xy_long_t origin_steps = { stepper.position(A_AXIS), stepper.position(B_AXIS) };
    const int n = 2;
    xy_long_t m_steps[n] = {-1, -1};
    xy_pos_t m_dist[n] = {-1.f, -1.f};
    uint8_t retry;
    for(retry = 0; retry != XY_HOMING_ORIGIN_MAX_RETRIES; ++retry) {
      uint8_t slot = retry % n;
      uint8_t slot2 = (retry - 1) % n;

      // measure distance B+/B-
      if(!measure_b_axis_distance(origin_steps, measure_max_dist, m_steps[slot].b, m_dist[slot].b, orig_crash)
      || !measure_b_axis_distance(origin_steps, -measure_max_dist, m_steps[slot].a, m_dist[slot].a, orig_crash)) {
        if(planner.draining()) {
          return {false, true}; // Do not refine position but end succesfully
        } else {
          ui.status_printf_P(0, "Endstop not reached");
          homing_failed([]() { fatal_error(ErrCode::ERR_MECHANICAL_PRECISE_REFINEMENT_FAILED); }, orig_crash);
          return {false, false}; // Homing failed
        }
      }

      // keep signs positive
      m_steps[slot].a = -m_steps[slot].a;
      m_dist[slot].a = -m_dist[slot].a;

      if(ABS(m_dist[slot].a - m_dist[slot2].a) < XY_HOMING_ORIGIN_BUMPS_MAX_ERR
      && ABS(m_dist[slot].b - m_dist[slot2].b) < XY_HOMING_ORIGIN_BUMPS_MAX_ERR)
        break;
    }
    if(retry == XY_HOMING_ORIGIN_MAX_RETRIES) {
      ui.status_printf_P(0, "Precise refinement failed"); //User is most likely to get this version of ERR_MECHANICAL_PRECISE_REFINEMENT_FAILED
      homing_failed([]() { fatal_error(ErrCode::ERR_MECHANICAL_PRECISE_REFINEMENT_FAILED); }, orig_crash);
      return {false, false}; // Homing failed
    }

    // calculate the absolute cycle coordinates
    const int32_t c_steps_a = phase_cycle_steps(A_AXIS);
    const int32_t c_steps_b = phase_cycle_steps(B_AXIS);

    int32_t d = (m_steps[0].a + m_steps[1].a + m_steps[0].b + m_steps[1].b) / 2;
    c_dist_a = (d + c_steps_a) / (2 * c_steps_a);
    int32_t d_y = d - (m_steps[0].b + m_steps[1].b);
    int32_t d_y2 = abs(d_y) + c_steps_b;
    if (d_y < 0) d_y2 = -d_y2;
    c_dist_b = d_y2 / (2 * c_steps_b);

    if (DEBUGGING(LEVELING)) {
      // measured distance and cycle
      SERIAL_ECHOLNPAIR("home B+ steps 1: ", m_steps[0].a, " 2: ", m_steps[1].a, " cycle:", c_dist_a);
      SERIAL_ECHOLNPAIR("home B- steps 1: ", m_steps[0].b, " 2: ", m_steps[1].b, " cycle:", c_dist_b);
    }
    return {true, true}; // Success
  }

  static bool wait_for_standstill(uint8_t axis_mask, millis_t max_delay = 150) {
    millis_t timeout = millis() + max_delay;
    for(;;) {
      bool stst = true;
      LOOP_L_N(i, XYZE_N) {
        if (TEST(axis_mask, i)) {
          if (!static_cast<TMC2130Stepper &>(stepper_axis((AxisEnum)i)).stst()) {
            stst = false;
            break;
          }
        }
      }
      if (stst)
        return true;
      if (millis() > timeout || planner.draining()) {
        return false;
      }
      safe_delay(10);
    }
  }

  /**
   * @brief Precise homing on core-XY.
   * @return true on success
   */
  bool refine_corexy_origin() {
    // finish previous moves and disable main endstop/crash recovery handling
    planner.synchronize();
    endstops.not_homing();
    #if ENABLED(CRASH_RECOVERY)
      Crash_Temporary_Deactivate ctd;
      const bool orig_crash = ctd.get_orig_state();
    #else /*ENABLED(CRASH_RECOVERY)*/
      constexpr bool orig_crash = false;
    #endif /*ENABLED(CRASH_RECOVERY)*/

    // reposition parallel to the origin
    float fr_mm_s = homing_feedrate(A_AXIS);
    xyze_pos_t origin_tmp = current_position;
    origin_tmp[X_AXIS] = (base_home_pos(X_AXIS) + XY_HOMING_ORIGIN_OFFSET);
    origin_tmp[Y_AXIS] = (base_home_pos(Y_AXIS) + XY_HOMING_ORIGIN_OFFSET);
    planner.buffer_line(origin_tmp, fr_mm_s, active_extruder);
    planner.synchronize();

    // align both motors to a full phase
    wait_for_standstill(_BV(A_AXIS) | _BV(B_AXIS));
    xy_long_t origin_steps = { stepper.position(A_AXIS) + phase_backoff_steps(A_AXIS),
                               stepper.position(B_AXIS) + phase_backoff_steps(B_AXIS) };
    plan_corexy_raw_move(origin_steps, fr_mm_s);

    // sanity checks
    wait_for_standstill(_BV(A_AXIS) | _BV(B_AXIS));
    if (!phase_aligned(A_AXIS) || !phase_aligned(B_AXIS)) {
      if (planner.draining()) return true;

      SERIAL_ECHOLN("phase alignment failed");
      SERIAL_ECHOLNPAIR("phase A:", stepperX.MSCNT(), " B:", stepperY.MSCNT());
      ui.status_printf_P(0, "Phase alignment failed");
      homing_failed([]() { fatal_error(ErrCode::ERR_MECHANICAL_PRECISE_REFINEMENT_FAILED); }, orig_crash);
      return false;
    }

    // increase current of the holding motor
    int32_t orig_cur = stepperX.rms_current();
    float orig_hold = stepperX.hold_multiplier();
    stepperX.rms_current(XY_HOMING_HOLDING_CURRENT_A, 1.);

    // measure from current origin
    int32_t c_dist_a = 0, c_dist_b = 0;
    auto ret = measure_phase_cycles(c_dist_a, c_dist_b, orig_crash);
    if(ret.output_valid) {
      // convert the full cycle back to steps
      xy_long_t c_ab = { c_dist_a * phase_cycle_steps(A_AXIS), c_dist_b * phase_cycle_steps(B_AXIS) };
      xy_pos_t c_mm;
      corexy_ab_to_xy(c_ab, c_mm);
      current_position.x = c_mm[X_AXIS] + origin_tmp[X_AXIS] + XY_HOMING_ORIGIN_SHIFT_X;
      current_position.y = c_mm[Y_AXIS] + origin_tmp[Y_AXIS] + XY_HOMING_ORIGIN_SHIFT_Y;
      planner.set_machine_position_mm(current_position);
    }

    // restore current
    stepperX.rms_current(orig_cur, orig_hold);

    return ret.success;
  }
#endif // ENABLED(PRECISE_HOMING_COREXY)

// in order to enable wavetable, we need to move the stepper to its zero-position so that it is safe
// in order to reach zero-position, we must reconfigure the stepper to single-microstep stepping
// in order to configure the stepper back, we need to step back to its natural position
void do_blocking_move_enable_wavetable(const AxisEnum axis, void (*enable_wavetable)(AxisEnum), int direction, float min_mm, feedRate_t fdrt) {
    float min_mm1 = min_mm / 2, min_mm2 = min_mm / 2;

    uint16_t tmp_microsteps = planner.nsteps_per_qstep(axis) / 4;
    uint16_t orig_microsteps = stepper_microsteps(axis, tmp_microsteps);
    uint16_t ustep_ratio = tmp_microsteps / orig_microsteps; // assert tmp_microsteps % orig_microsteps == 0

    // reeconfigure stepper to stepping by single-usteps, i.e. tmp_microsteps
    float bck_steps_per_mm = planner.settings.axis_steps_per_mm[axis];
    float bck_mm_per_step = planner.mm_per_step[axis];
    planner.settings.axis_steps_per_mm[axis] *= ustep_ratio;
    planner.mm_per_step[axis] /= ustep_ratio;
    planner.position[axis] *= ustep_ratio;

    // calculate distance so that we end at stepper zero, going by at least min_mm/2
    float amount_mm = planner.mm_per_qsteps(axis, (uint32_t)(min_mm1 * planner.qsteps_per_mm(axis)) + 1);
    amount_mm += planner.distance_to_stepper_zero(axis, has_inverted_axis(axis));

    // go down the calculated distance
    current_position[axis] += ABS(amount_mm) * direction;
    line_to_current_position(fdrt); // NOTE by recofiguring planner, the speed is also proportionally slowed down. This is OK since we need to go slow in order not to loose steps.
    planner.synchronize();

    // enable wavetable for this axis
    enable_wavetable(axis);

    // calculate distance so we end up at stepper position valid for originally configured steps, going by at least min_mm/2
    amount_mm = planner.mm_per_qsteps(axis, (uint32_t)(min_mm2 * planner.qsteps_per_mm(axis)) + 1);
    uint16_t go_to_ustep = tmp_microsteps / orig_microsteps / 2;
    amount_mm += go_to_ustep * planner.mm_per_step[axis];

    // go down the calculated distance
    current_position[axis] += ABS(amount_mm) * direction;
    line_to_current_position(fdrt);
    planner.synchronize();

    // configure back steps to original state
    (void)stepper_microsteps(axis, orig_microsteps);
    planner.settings.axis_steps_per_mm[axis] = bck_steps_per_mm;
    planner.mm_per_step[axis] = bck_mm_per_step;
    planner.position[axis] /= ustep_ratio;
}

/**
 * @brief Call this when homing fails, it will try to recover.
 * After calling this, homing needs to end right away with fail return.
 * @param fallback_error called when homing cannot be recovered
 * @param crash_was_active true if crash recovery was active, this is used if crash_recovery was temporarily disabled
 */
void homing_failed(std::function<void()> fallback_error, [[maybe_unused]] bool crash_was_active) {
  #if ENABLED(CRASH_RECOVERY)
    const bool is_active = crash_s.is_active();
    if ((is_active || crash_was_active) // Allow if crash recovery was temporarily disabled
      && (crash_s.get_state() == Crash_s::PRINTING)) {
      if (!is_active && crash_was_active) {
        crash_s.activate(); // Reactivate temporarily disabled crash recovery
      }
      if (crash_s.is_toolchange_in_progress()) {
        crash_s.set_state(Crash_s::TRIGGERED_TOOLCRASH);
      } else {
        crash_s.set_state(Crash_s::TRIGGERED_HOMEFAIL);
      }
      return;
    }

    if ((crash_s.get_state() == Crash_s::TRIGGERED_ISR)       // ISR crash happened, it will replay homing
      || (crash_s.get_state() == Crash_s::TRIGGERED_HOMEFAIL) // Rehoming is already in progress
      || (crash_s.get_state() == Crash_s::TRIGGERED_TOOLCRASH)
      || (crash_s.get_state() == Crash_s::RECOVERY) // Recovery in progress, it will know that homing didn't succeed from return
      || (crash_s.get_state() == Crash_s::REPEAT_WAIT)) {
      return; // Ignore
    }
  #endif /*ENABLED(CRASH_RECOVERY)*/

  fallback_error();
}

/**
 * Home an individual "raw axis" to its endstop.
 * This applies to XYZ on Cartesian and Core robots, and
 * to the individual ABC steppers on DELTA and SCARA.
 *
 * At the end of the procedure the axis is marked as
 * homed and the current position of that axis is updated.
 * Kinematic robots should wait till all axes are homed
 * before updating the current position.
 *
 * @param axis Axist to home
 * @param fr_mm_s Homing feed rate in millimeters per second
 * @param invert_home_dir
 *  @arg @c false Default homing direction
 *  @arg @c true Home to opposite end of axis than default.
 *               Warning - axis is considered homed and in known position.
 *               @todo Current position is wrong in case of invert_home_dir true after this call.
 * @param enable_wavetable pointer to reenable wavetable during backoff move
 * @param can_calibrate allows/avoids re-calibration if homing is not successful
 * @param homing_z_with_probe default true, set to false to home without using probe (useful to calibrate Z on XL)
 * @return true on success
 */
bool homeaxis(const AxisEnum axis, const feedRate_t fr_mm_s, bool invert_home_dir, void (*enable_wavetable)(AxisEnum), [[maybe_unused]] bool can_calibrate, bool homing_z_with_probe) {

  // clear the axis state while running
  CBI(axis_known_position, axis);

  #if ENABLED(CRASH_RECOVERY)
    Crash_Temporary_Deactivate ctd;
    const bool orig_crash = ctd.get_orig_state();
  #else /*ENABLED(CRASH_RECOVERY)*/
    constexpr bool orig_crash = false;
  #endif /*ENABLED(CRASH_RECOVERY)*/

  #if IS_SCARA
    // Only Z homing (with probe) is permitted
    if (axis != Z_AXIS) { BUZZ(100, 880); return; }
  #else
    #define _CAN_HOME(A) \
      (axis == _AXIS(A) && ((A##_MIN_PIN > -1 && A##_HOME_DIR < 0) || (A##_MAX_PIN > -1 && A##_HOME_DIR > 0)))
    #if X_SPI_SENSORLESS
      #define CAN_HOME_X true
    #else
      #define CAN_HOME_X _CAN_HOME(X)
    #endif
    #if Y_SPI_SENSORLESS
      #define CAN_HOME_Y true
    #else
      #define CAN_HOME_Y _CAN_HOME(Y)
    #endif
    #if Z_SPI_SENSORLESS
      #define CAN_HOME_Z true
    #else
      #define CAN_HOME_Z _CAN_HOME(Z)
    #endif
    if (!CAN_HOME_X && !CAN_HOME_Y && !CAN_HOME_Z) return true;
  #endif

  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR(">>> homeaxis(", axis_codes[axis], ")");

  const int axis_home_dir = (
    #if ENABLED(DUAL_X_CARRIAGE)
      axis == X_AXIS ? x_home_dir(active_extruder) :
    #endif
      invert_home_dir ? (-home_dir(axis)) : home_dir(axis)
  );

  #ifdef HOMING_MAX_ATTEMPTS
  const float* min_diff = invert_home_dir ? axis_home_invert_min_diff : axis_home_min_diff;
  const float* max_diff = invert_home_dir ? axis_home_invert_max_diff : axis_home_max_diff;

    float probe_offset;
    for(size_t attempt = 0;;) {
      #if ENABLED(PRECISE_HOMING)
        if ((axis == X_AXIS || axis == Y_AXIS) && !invert_home_dir) {
          probe_offset = home_axis_precise(axis, axis_home_dir, can_calibrate);
          attempt = HOMING_MAX_ATTEMPTS; // call home_axis_precise() just once
        }
        else
      #endif // ENABLED(PRECISE_HOMING)
        {
          if (attempt > 0 && axis == Z_AXIS) {
            // Z has no move back and after the first attempt we might be left too close on the
            // build plate (for example, with a loadcell we're _on_ the plate). Move back now before
            // we attempt to probe again so that we can zero the sensor again.
            float bump = axis_home_dir * (
              #if HOMING_Z_WITH_PROBE
                (axis == Z_AXIS && homing_z_with_probe && (Z_HOME_BUMP_MM)) ? _MAX(Z_CLEARANCE_BETWEEN_PROBES, Z_HOME_BUMP_MM) :
              #endif
              home_bump_mm(axis)
            );
            current_position[axis] -= bump;
            line_to_current_position(fr_mm_s ? fr_mm_s : homing_feedrate(Z_AXIS));
            planner.synchronize();
          }

          probe_offset = homeaxis_single_run(axis, axis_home_dir, fr_mm_s, invert_home_dir, homing_z_with_probe) * static_cast<float>(axis_home_dir);
        }
      if (planner.draining()) {
        // move intentionally aborted, do not retry/kill
        return true;
      }

      // check if the offset is acceptable
      bool in_range = min_diff[axis] <= probe_offset && probe_offset <= max_diff[axis];
      metric_record_custom(&metric_home_diff, ",ax=%u,ok=%u v=%.3f,n=%u", (unsigned)axis, (unsigned)in_range, probe_offset, (unsigned)attempt);
      if (in_range) break; // OK offset in range

      // check whether we should try again
      if (++attempt >= HOMING_MAX_ATTEMPTS) {
        // not OK run out attempts
        switch (axis) {
        case X_AXIS:
          homing_failed([]() { fatal_error(ErrCode::ERR_ELECTRO_HOMING_ERROR_X); }, orig_crash);
          return false;
        case Y_AXIS:
          homing_failed([]() { fatal_error(ErrCode::ERR_ELECTRO_HOMING_ERROR_Y); }, orig_crash);
          return false;
        default:
          homing_failed([]() { fatal_error(ErrCode::ERR_ELECTRO_HOMING_ERROR_Z); }, orig_crash);
          return false;
        }
      }

      if((axis == X_AXIS || axis == Y_AXIS) && !invert_home_dir){
        //print only for normal homing, messages from precise homing are taken care inside precise homing
        ui.status_printf_P(0,"%c axis homing failed, retrying", axis_codes[axis]);
      }
    }
  #else // HOMING_MAX_ATTEMPTS 
    homeaxis_single_run(axis, axis_home_dir);
  #endif // HOMING_MAX_ATTEMPTS

  #ifdef HOMING_BACKOFF_POST_MM
    constexpr xyz_float_t endstop_backoff = HOMING_BACKOFF_POST_MM;
    const float backoff_mm = endstop_backoff[
      #if ENABLED(DELTA)
        Z_AXIS
      #else
        axis
      #endif
    ];
    if (backoff_mm) {
      if (enable_wavetable != NULL) {
        do_blocking_move_enable_wavetable(axis, enable_wavetable, -axis_home_dir, backoff_mm,
          #if HOMING_Z_WITH_PROBE
            (axis == Z_AXIS && homing_z_with_probe) ? MMM_TO_MMS(Z_PROBE_SPEED_FAST) :
          #endif
          homing_feedrate(axis)
        );
      } else {
        current_position[axis] -= ABS(backoff_mm) * axis_home_dir;
        line_to_current_position(
          #if HOMING_Z_WITH_PROBE
            (axis == Z_AXIS && homing_z_with_probe) ? MMM_TO_MMS(Z_PROBE_SPEED_FAST) :
          #endif
          homing_feedrate(axis)
        );
        planner.synchronize();
      }
      SERIAL_ECHO_START();
      SERIAL_ECHOLNPAIR_F("Backoff ipos:", stepper.position_from_startup(axis));
    }
  #endif

  return true;
}

/**
 * home axis and
 * return distance between fast and slow probe
 * @param homing_z_with_probe default true, set to false to home without using probe (useful to calibrate Z on XL)
 */
static float homeaxis_single_run(const AxisEnum axis, const int axis_home_dir, const feedRate_t fr_mm_s, bool invert_home_dir, bool homing_z_with_probe) {
  int steps;

  // Homing Z towards the bed? Deploy the Z probe or endstop.
  #if HOMING_Z_WITH_PROBE
    if (axis == Z_AXIS && homing_z_with_probe && DEPLOY_PROBE()) {
      return NAN;
    }
  #endif

  // Set flags for X, Y, Z motor locking
  #if HAS_EXTRA_ENDSTOPS
    switch (axis) {
      #if ENABLED(X_DUAL_ENDSTOPS)
        case X_AXIS:
      #endif
      #if ENABLED(Y_DUAL_ENDSTOPS)
        case Y_AXIS:
      #endif
      #if Z_MULTI_ENDSTOPS
        case Z_AXIS:
      #endif
      stepper.set_separate_multi_axis(true);
      default: break;
    }
  #endif

  // Fast move towards endstop until triggered
  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Home 1 Fast:");

  #if HOMING_Z_WITH_PROBE && ENABLED(BLTOUCH)
    if (axis == Z_AXIS && homing_z_with_probe && bltouch.deploy()) {
      return NAN; // The initial DEPLOY
    }
  #endif

  #if ENABLED(MOVE_BACK_BEFORE_HOMING)
    if ((axis == X_AXIS) || (axis == Y_AXIS)) {
      do_blocking_move_axis(axis, axis_home_dir * -MOVE_BACK_BEFORE_HOMING_DISTANCE, fr_mm_s);
    }
  #endif // ENABLED(MOVE_BACK_BEFORE_HOMING)

  do_homing_move(axis, 1.5f * max_length(
    #if ENABLED(DELTA)
      Z_AXIS
    #else
      axis
    #endif
      ) * axis_home_dir, fr_mm_s, false, homing_z_with_probe);

  steps = stepper.position_from_startup(axis);

  #if HOMING_Z_WITH_PROBE && ENABLED(BLTOUCH) && DISABLED(BLTOUCH_HS_MODE)
    if (axis == Z_AXIS && homing_z_with_probe) {
      bltouch.stow(); // Intermediate STOW (in LOW SPEED MODE)
    }
  #endif

  // When homing Z with probe respect probe clearance
  float bump = axis_home_dir * (
    #if HOMING_Z_WITH_PROBE
      (axis == Z_AXIS && homing_z_with_probe && (Z_HOME_BUMP_MM)) ? _MAX(Z_CLEARANCE_BETWEEN_PROBES, Z_HOME_BUMP_MM) :
    #endif
    home_bump_mm(axis)
  );

  // If a second homing move is configured...
  if (bump) {
    // Move away from the endstop by the axis HOME_BUMP_MM
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Move Away:");
    do_blocking_move_axis(axis, -bump, fr_mm_s);

    // Slow move towards endstop until triggered
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Home 2 Slow:");

    #if HOMING_Z_WITH_PROBE && ENABLED(BLTOUCH) && DISABLED(BLTOUCH_HS_MODE)
      if (axis == Z_AXIS && homing_z_with_probe && bltouch.deploy()) {
        return NAN; // Intermediate DEPLOY (in LOW SPEED MODE)
      }
    #endif

    MINDA_BROKEN_CABLE_DETECTION__POST_ZHOME_0();

    #if HOMING_Z_WITH_PROBE
    if (axis == Z_AXIS) {
      if (axis_home_dir < 0) {
        do_homing_move(axis, 2 * bump, fr_mm_s, false, homing_z_with_probe);
      } else {
        // moving away from the bed
        do_homing_move(axis, 2 * bump, HOMING_FEEDRATE_INVERTED_Z, false, homing_z_with_probe);
      }
    } else
    #endif //HOMING_Z_WITH_PROBE
    {
      do_homing_move(axis, 2 * bump, fr_mm_s ? fr_mm_s : get_homing_bump_feedrate(axis), false, homing_z_with_probe);
    }  
    steps -= stepper.position_from_startup(axis);

    #if HOMING_Z_WITH_PROBE && ENABLED(BLTOUCH)
      if (axis == Z_AXIS && homing_z_with_probe) {
        bltouch.stow(); // The final STOW
      }
    #endif
  }

  #if HAS_EXTRA_ENDSTOPS
    const bool pos_dir = axis_home_dir > 0;
    #if ENABLED(X_DUAL_ENDSTOPS)
      if (axis == X_AXIS) {
        const float adj = ABS(endstops.x2_endstop_adj);
        if (adj) {
          if (pos_dir ? (endstops.x2_endstop_adj > 0) : (endstops.x2_endstop_adj < 0)) stepper.set_x_lock(true); else stepper.set_x2_lock(true);
          do_homing_move(axis, pos_dir ? -adj : adj, 0, false, homing_z_with_probe);
          stepper.set_x_lock(false);
          stepper.set_x2_lock(false);
        }
      }
    #endif
    #if ENABLED(Y_DUAL_ENDSTOPS)
      if (axis == Y_AXIS) {
        const float adj = ABS(endstops.y2_endstop_adj);
        if (adj) {
          if (pos_dir ? (endstops.y2_endstop_adj > 0) : (endstops.y2_endstop_adj < 0)) stepper.set_y_lock(true); else stepper.set_y2_lock(true);
          do_homing_move(axis, pos_dir ? -adj : adj, 0, false, homing_z_with_probe);
          stepper.set_y_lock(false);
          stepper.set_y2_lock(false);
        }
      }
    #endif
    #if ENABLED(Z_DUAL_ENDSTOPS)
      if (axis == Z_AXIS) {
        const float adj = ABS(endstops.z2_endstop_adj);
        if (adj) {
          if (pos_dir ? (endstops.z2_endstop_adj > 0) : (endstops.z2_endstop_adj < 0)) stepper.set_z_lock(true); else stepper.set_z2_lock(true);
          do_homing_move(axis, pos_dir ? -adj : adj, 0, false, homing_z_with_probe);
          stepper.set_z_lock(false);
          stepper.set_z2_lock(false);
        }
      }
    #endif
    #if ENABLED(Z_TRIPLE_ENDSTOPS)
      if (axis == Z_AXIS) {
        // we push the function pointers for the stepper lock function into an array
        void (*lock[3]) (bool)= {&stepper.set_z_lock, &stepper.set_z2_lock, &stepper.set_z3_lock};
        float adj[3] = {0, endstops.z2_endstop_adj, endstops.z3_endstop_adj};

        void (*tempLock) (bool);
        float tempAdj;

        // manual bubble sort by adjust value
        if (adj[1] < adj[0]) {
          tempLock = lock[0], tempAdj = adj[0];
          lock[0] = lock[1], adj[0] = adj[1];
          lock[1] = tempLock, adj[1] = tempAdj;
        }
        if (adj[2] < adj[1]) {
          tempLock = lock[1], tempAdj = adj[1];
          lock[1] = lock[2], adj[1] = adj[2];
          lock[2] = tempLock, adj[2] = tempAdj;
        }
        if (adj[1] < adj[0]) {
          tempLock = lock[0], tempAdj = adj[0];
          lock[0] = lock[1], adj[0] = adj[1];
          lock[1] = tempLock, adj[1] = tempAdj;
        }

        if (pos_dir) {
          // normalize adj to smallest value and do the first move
          (*lock[0])(true);
          do_homing_move(axis, adj[1] - adj[0], 0, false, homing_z_with_probe);
          // lock the second stepper for the final correction
          (*lock[1])(true);
          do_homing_move(axis, adj[2] - adj[1], 0, false, homing_z_with_probe);
        }
        else {
          (*lock[2])(true);
          do_homing_move(axis, adj[1] - adj[2], 0, false, homing_z_with_probe);
          (*lock[1])(true);
          do_homing_move(axis, adj[0] - adj[1], 0, false, homing_z_with_probe);
        }

        stepper.set_z_lock(false);
        stepper.set_z2_lock(false);
        stepper.set_z3_lock(false);
      }
    #endif

    // Reset flags for X, Y, Z motor locking
    switch (axis) {
      #if ENABLED(X_DUAL_ENDSTOPS)
        case X_AXIS:
      #endif
      #if ENABLED(Y_DUAL_ENDSTOPS)
        case Y_AXIS:
      #endif
      #if Z_MULTI_ENDSTOPS
        case Z_AXIS:
      #endif
      stepper.set_separate_multi_axis(false);
      default: break;
    }
  #endif

    // Check if any of the moves were aborted and avoid setting any state
    if (planner.draining())
      return NAN;

  #if IS_SCARA

    set_axis_is_at_home(axis);
    sync_plan_position();

  #elif ENABLED(DELTA)

    // Delta has already moved all three towers up in G28
    // so here it re-homes each tower in turn.
    // Delta homing treats the axes as normal linear axes.

    // retrace by the amount specified in delta_endstop_adj + additional dist in order to have minimum steps
    if (delta_endstop_adj[axis] * Z_HOME_DIR <= 0) {
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("delta_endstop_adj:");
      do_homing_move(axis, delta_endstop_adj[axis] - (MIN_STEPS_PER_SEGMENT + 1) * planner.mm_per_step[axis] * Z_HOME_DIR, 0, false, homing_z_with_probe);
    }

  #else // CARTESIAN / CORE

    if (!invert_home_dir) {
      set_axis_is_at_home(axis, homing_z_with_probe);
    }
    sync_plan_position();

    destination[axis] = current_position[axis];

    if (DEBUGGING(LEVELING)) DEBUG_POS("> AFTER set_axis_is_at_home", current_position);

  #endif

  // Put away the Z probe
  #if HOMING_Z_WITH_PROBE
    if (axis == Z_AXIS && homing_z_with_probe && STOW_PROBE()) {
      return NAN;
    }
  #endif

  // Clear retracted status if homing the Z axis
  #if ENABLED(FWRETRACT)
    if (axis == Z_AXIS) fwretract.current_hop = 0.0;
  #endif

  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("<<< homeaxis(", axis_codes[axis], ")");

  if (bump) return static_cast<float>(steps) * planner.mm_per_step[axis];
  return 0;
} // homeaxis()

#if HAS_WORKSPACE_OFFSET
  void update_workspace_offset(const AxisEnum axis) {
    workspace_offset[axis] = home_offset[axis] + position_shift[axis];
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("Axis ", axis_codes[axis], " home_offset = ", home_offset[axis], " position_shift = ", position_shift[axis]);
  }
#endif

#if HAS_M206_COMMAND
  /**
   * Change the home offset for an axis.
   * Also refreshes the workspace offset.
   */
  void set_home_offset(const AxisEnum axis, const float v) {
    home_offset[axis] = v;
    update_workspace_offset(axis);
  }
#endif // HAS_M206_COMMAND

#if ENABLED(PRECISE_HOMING)

  /**
   * Turns automatic reports off until destructor is called.
   * Then it sets reports to previous value.
   */
  class Temporary_Report_Off{
      bool suspend_reports = false;
    public:
      Temporary_Report_Off(){
        suspend_reports = suspend_auto_report;
        suspend_auto_report = true;
      }
      ~Temporary_Report_Off(){
        suspend_auto_report = suspend_reports;
      }
  };

  /**
   *  Move back and forth to endstop
   * \returns MSCNT position after endstop has been hit
   */
  static int32_t home_and_get_mscnt(AxisEnum axis, int axis_home_dir, feedRate_t fr_mm_s, float &probe_offset) {
    probe_offset = homeaxis_single_run(axis, axis_home_dir, fr_mm_s);
    const int32_t mscnt = (axis == X_AXIS) ? stepperX.MSCNT() : stepperY.MSCNT();
    return mscnt;
  }


  /**
     * \returns calibrated value from EEPROM in microsteps
     *          always 256 microsteps per step, range 0 to 1023
     */
    static int get_calibrated_home(const AxisEnum axis, bool &calibrated) {
      uint16_t mscntRead[PersistentStorage::homeSamplesCount];
      calibrated = PersistentStorage::isCalibratedHome(mscntRead, axis);
      return home_modus(mscntRead, PersistentStorage::homeSamplesCount, 96);
    }

  /**
   * \param axis axis to be evaluated
   * \param mscnt measured motor position if known,
   *              otherwise, current motor position will be taken
   * \returns shortest distance of the motor to the calibrated position in microsteps,
   *          always 256 microsteps per step, range -512 to 512
   */
  static int calibrated_offset_mscnt(const AxisEnum axis, const int mscnt, bool &calibrated) {
    const int cal = get_calibrated_home(axis, calibrated);
    return to_calibrated(cal, mscnt);
  }

  /**
   * \returns offset [mm] to be subtracted from the current axis position to have correct position
   */
  float calibrated_home_offset(const AxisEnum axis) {
    bool calibrated;
    const int cal = get_calibrated_home(axis, calibrated);
    if (!calibrated)
      return 0;

    const constexpr float steps_per_unit[] = DEFAULT_AXIS_STEPS_PER_UNIT;
    switch (axis) {
      case X_AXIS: {
        return ((X_HOME_DIR < 0 ? X_HOME_GAP : -X_HOME_GAP)
          - ((((INVERT_X_DIR) ? -1.f : 1.f) * to_calibrated(cal, stepperX.MSCNT())) / (steps_per_unit[X_AXIS] * (256 / X_MICROSTEPS))));
      }
      case Y_AXIS: {
        return ((Y_HOME_DIR < 0 ? Y_HOME_GAP : -Y_HOME_GAP)
          - ((((INVERT_Y_DIR) ? -1.f : 1.f) * to_calibrated(cal, stepperY.MSCNT())) / (steps_per_unit[Y_AXIS] * (256 / Y_MICROSTEPS))));
      }
      default:;
    }
    return 0;
  }

  /**
   * \brief Home and get offset from calibrated point
   *
   * \returns offset from calibrated point -512 .. +512
   */
  static int32_t home_and_get_calibration_offset(AxisEnum axis, int axis_home_dir, float &probe_offset, bool store_samples)
  {
    int32_t calibration_offset = 0;
    bool calibrated = false;
    bool break_loop = false;
    do {
      const int32_t mscnt = home_and_get_mscnt(axis, axis_home_dir, homing_feedrate_mm_s[axis] / homing_bump_divisor[axis], probe_offset);

      if ((probe_offset >= axis_home_min_diff[axis])
           && (probe_offset <= axis_home_max_diff[axis])
           && store_samples) {
        PersistentStorage::pushHomeSample(mscnt, 255, axis); //todo board_temp
      }
      else {
        break_loop = true;
      }

      calibration_offset = calibrated_offset_mscnt(axis, mscnt, calibrated);
      if (!calibrated) calibration_offset = 0;

      SERIAL_ECHO_START();
      SERIAL_ECHOPAIR("Precise homing axis: ", axis);
      SERIAL_ECHOPAIR(" probe_offset: ", probe_offset);
      SERIAL_ECHOPAIR(" HB divisor: ", homing_bump_divisor[axis]);
      SERIAL_ECHOPAIR(" mscnt: ", mscnt);
      SERIAL_ECHOPAIR(" ipos: ", stepper.position_from_startup(axis));
      if (calibrated) {
        SERIAL_ECHOLNPAIR(" Home position diff: ", calibration_offset);
      } else {
        ui.status_printf_P(0,"Calibrating %c axis",axis_codes[axis]);
        SERIAL_ECHOLN(" Not yet calibrated.");
      }

    } while(store_samples && !calibrated && !break_loop);

    return calibration_offset;
  }

  static void load_divisor_from_eeprom() {
      for (int axis = 0; axis < XY; axis++) {
          const float max = pgm_read_float(&homing_bump_divisor_max[axis]);
          const float min = pgm_read_float(&homing_bump_divisor_min[axis]);
          const float hbd = homing_bump_divisor[axis];
          if (hbd >= min && hbd <= max) {
              continue;
          }

          const float loaded = eeprom_get_flt(axis ? EEVAR_HOMING_BDIVISOR_Y : EEVAR_HOMING_BDIVISOR_X);
          if (loaded >= min && loaded <= max) {
              homing_bump_divisor[axis] = loaded;
          } else {
              homing_bump_divisor[axis] = pgm_read_float(&homing_bump_divisor_dflt[axis]);
          }
      }
  }

  static void homing_failed_update_divisor(AxisEnum axis) {
      homing_bump_divisor[axis] *= HOMING_BUMP_DIVISOR_STEP;
      const float max = pgm_read_float(&homing_bump_divisor_max[axis]);
      const float min = pgm_read_float(&homing_bump_divisor_min[axis]);
      const float hbd = homing_bump_divisor[axis];
      __attribute__((unused)) const float dflt = pgm_read_float(&homing_bump_divisor_dflt[axis]);
      if (hbd > max || /* shouldnt happen, just to make sure */ hbd < min) {
          homing_bump_divisor[axis] = min;
      }
  }

  static void save_divisor_to_eeprom(int try_nr, AxisEnum axis) {
      if (try_nr > 0 && axis < XY) {
          eeprom_set_flt(axis ? EEVAR_HOMING_BDIVISOR_Y : EEVAR_HOMING_BDIVISOR_X, homing_bump_divisor[axis]);
      }
  }

  /**
   * \brief Home and decide if position of both probes is close enough to calibrated home position
   *
   * Do homing probe and decide if can be accepted. If it is not
   * good enough do the probe again up to PRECISE_HOMING_TRIES times.
   *
   * \param axis axis to be homed (cartesian printers only)
   * \param axis_home_dir direction where the home of the axis is
   * \param can_calibrate Can be calibrated home position updated?
   * \return Distance between two probes in mm.
   */
  float home_axis_precise(AxisEnum axis, int axis_home_dir, bool can_calibrate) {
    const int tries = can_calibrate ? PRECISE_HOMING_TRIES : (3 * PRECISE_HOMING_TRIES);
    const int accept_perfect_only_tries = can_calibrate ? (PRECISE_HOMING_TRIES / 3) : 0;
    constexpr int perfect_offset = 96;
    constexpr int acceptable_offset = 288;
    float probe_offset;
    bool first_acceptable = false;

    load_divisor_from_eeprom();

    for (int try_nr = 0; try_nr < tries; ++try_nr){
      const int32_t calibration_offset = home_and_get_calibration_offset(axis, axis_home_dir, probe_offset, can_calibrate);
      if (planner.draining()) {
        // homing intentionally aborted, do not retry
        break;
      }

      SERIAL_ECHO_START();
      SERIAL_ECHO("Probe classified as ");

      if ((probe_offset < axis_home_min_diff[axis])
           || (probe_offset > axis_home_max_diff[axis])) {
        SERIAL_ECHOLN("failed.");
        ui.status_printf_P(0,"%c axis homing failed, retrying",axis_codes[axis]);
        homing_failed_update_divisor(axis);
      } else if (std::abs(calibration_offset) <= perfect_offset) {
        SERIAL_ECHOLN("perfect.");
        save_divisor_to_eeprom(try_nr, axis);
        return probe_offset;
      } else if ((std::abs(calibration_offset) <= acceptable_offset)) {
        SERIAL_ECHOLN("acceptable.");
        if (try_nr >= accept_perfect_only_tries) {
            save_divisor_to_eeprom(try_nr, axis);
            return probe_offset;
        }
        if(first_acceptable){
          ui.status_printf_P(0,"Updating precise home point %c axis",axis_codes[axis]);
          homing_failed_update_divisor(axis);
        }
        first_acceptable = true;
      } else {
        SERIAL_ECHOLN("bad.");
        if(can_calibrate){
          ui.status_printf_P(0,"Updating precise home point %c axis",axis_codes[axis]);
        }else{
          ui.status_printf_P(0,"%c axis homing failed,retrying",axis_codes[axis]);
        }
      }
    }

    SERIAL_ERROR_MSG("Precise homing runs out of tries to get acceptable probe.");
    return probe_offset;
  }

#endif // ENABLED(PRECISE_HOMING)
