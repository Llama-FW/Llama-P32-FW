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

#include "../gcode.h"
#include "../../module/tool_change.h"

#if ENABLED(DEBUG_LEVELING_FEATURE) || EXTRUDERS > 1
  #include "../../module/motion.h"
#endif

#if ENABLED(PRUSA_MMU2)
  #include "../../feature/prusa/MMU2/mmu2_mk4.h"
#endif

#if ENABLED(PRUSA_TOOLCHANGER)
  #include "../../module/prusa/toolchanger.h"
#endif

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../../core/debug_out.h"

/**
 * T0-T<n>: Switch tool, usually switching extruders
 *
 *   F[units/min] Set the movement feedrate
 *   S1           Don't move the tool in XY after change
 *
 * For PRUSA_MMU2:
 *   T[n] Gcode to extrude at least 38.10 mm at feedrate 19.02 mm/s must follow immediately to load to extruder wheels.
 *   T?   Gcode to extrude shouldn't have to follow. Load to extruder wheels is done automatically.
 *   Tx   Same as T?, but nozzle doesn't have to be preheated. Tc requires a preheated nozzle to finish filament load.
 *   Tc   Load to nozzle after filament was prepared by Tc and nozzle is already heated.
 */
void GcodeSuite::T(const uint8_t tool_index) {

  if (DEBUGGING(LEVELING)) {
    DEBUG_ECHOLNPAIR(">>> T(", tool_index, ")");
    DEBUG_POS("BEFORE", current_position);
  }

  #if ENABLED(PRUSA_MMU2)
    if (parser.string_arg) {
      // @@TODO MMU2::mmu2.tool_change(parser.string_arg);   // Special commands T?/Tx/Tc
      return;
    }
  #endif

  #if EXTRUDERS < 2

    tool_change(tool_index);

  #else

    int move_type = !parser.seen('S') ? 0 : parser.intval('S', 1);
    #if DISABLED(PRUSA_TOOLCHANGER)
      constexpr bool no_tool = false;
    #else
      bool no_tool = (tool_index == PrusaToolChanger::MARLIN_NO_TOOL_PICKED
        || active_extruder == PrusaToolChanger::MARLIN_NO_TOOL_PICKED);
    #endif
    tool_return_t return_type =
      ( (tool_index == active_extruder) || (move_type == 1) ?
          tool_return_t::no_move // same tool or no move requested: don't move
        : no_tool || (move_type == 2) || (!all_axes_known()) ?
          tool_return_t::no_return // no tool, unknown position or no return requested: just lift/retract
        : tool_return_t::to_destination
      );

    get_destination_from_command();
    tool_change(tool_index, return_type);

  #endif

  if (DEBUGGING(LEVELING)) {
    DEBUG_POS("AFTER", current_position);
    DEBUG_ECHOLNPGM("<<< T()");
  }
}
