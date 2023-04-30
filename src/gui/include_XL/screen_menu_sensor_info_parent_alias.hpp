/**
 * @file screen_menu_sensor_info_parent_alias.hpp
 * @brief alias for XL printer info menu parent
 */
#pragma once

#include "screen_menu.hpp"
#include "MItem_tools.hpp"
#include "Configuration_adv.h"

using ScreenMenuSensorInfo__ = ScreenMenu<GuiDefaults::MenuFooter, MI_RETURN
#if (TEMP_SENSOR_HEATBREAK > 0)
    ,
    MI_INFO_HEATBREAK_N_TEMP<0>,
    MI_INFO_HEATBREAK_N_TEMP<1>,
    MI_INFO_HEATBREAK_N_TEMP<2>,
    MI_INFO_HEATBREAK_N_TEMP<3>,
    MI_INFO_HEATBREAK_N_TEMP<4>
#endif

#if HAS_TEMP_BOARD
    ,
    MI_INFO_BOARD_TEMP
#endif

    ,
    MI_INFO_BED_TEMP,
    MI_INFO_NOZZLE_N_TEMP<0>,
    MI_INFO_NOZZLE_N_TEMP<1>,
    MI_INFO_NOZZLE_N_TEMP<2>,
    MI_INFO_NOZZLE_N_TEMP<3>,
    MI_INFO_NOZZLE_N_TEMP<4>,
    MI_INFO_DWARF_BOARD_TEMPERATURE,
    MI_INFO_MODULAR_BED_BOARD_TEMPERATURE,
    MI_INFO_LOADCELL,
    MI_INFO_PRINTER_FILL_SENSOR,
    MI_INFO_SIDE_FILL_SENSOR,
    MI_INFO_PRINT_FAN,
    MI_INFO_HBR_FAN,
    MI_INFO_INPUT_VOLTAGE,
    MI_INFO_5V_VOLTAGE,
    MI_INFO_SANDWICH_5V_CURRENT,
    MI_INFO_BUDDY_5V_CURRENT>;
