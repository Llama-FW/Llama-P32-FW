#include "i18n.h"
#include <utility_extensions.hpp>
#include <printer_selftest.hpp>

namespace SelftestSnake {
enum class Tool {
    Tool1 = 0,
    Tool2 = 1,
    Tool3 = 2,
    Tool4 = 3,
    Tool5 = 4,
    _count,
    _all_tools = _count,
    _last = _count - 1,
    _first = Tool1,
};

// Order matters, snake and will be run in the same order, as well as menu items (with indices) will be
enum class Action {
    Fans,
    XYCheck,
    ZAlign, // also known as z_calib
    Loadcell,
    ZCheck,
    Heaters,
    FilamentSensorCalibration,
    _count,
    _last = _count - 1,
    _first = Fans,
};

template <Action action>
concept SubmenuActionC = false;

constexpr bool has_submenu(Action action) {
    switch (action) {
    default:
        return false;
    }
}

constexpr bool is_multitool_only_action(Action action) {
    return false;
}

constexpr bool requires_toolchanger(Action action) {
    return false;
}

constexpr bool is_singletool_only_action(Action action) {
    return false;
}

consteval auto get_submenu_label(Tool tool, Action action) -> const char * {
    struct ToolText {
        Tool tool;
        Action action;
        const char *label;
    };
    const ToolText tooltexts[] {
        { Tool::Tool1, Action::Loadcell, N_("Tool 1 Loadcell Test") },
        { Tool::Tool2, Action::Loadcell, N_("Tool 2 Loadcell Test") },
        { Tool::Tool3, Action::Loadcell, N_("Tool 3 Loadcell Test") },
        { Tool::Tool4, Action::Loadcell, N_("Tool 4 Loadcell Test") },
        { Tool::Tool5, Action::Loadcell, N_("Tool 5 Loadcell Test") },
        { Tool::Tool1, Action::FilamentSensorCalibration, N_("Tool 1 Filament Sensor Calibration") },
        { Tool::Tool2, Action::FilamentSensorCalibration, N_("Tool 2 Filament Sensor Calibration") },
        { Tool::Tool3, Action::FilamentSensorCalibration, N_("Tool 3 Filament Sensor Calibration") },
        { Tool::Tool4, Action::FilamentSensorCalibration, N_("Tool 4 Filament Sensor Calibration") },
        { Tool::Tool5, Action::FilamentSensorCalibration, N_("Tool 5 Filament Sensor Calibration") },
    };

    if (auto it = std::ranges::find_if(tooltexts, [&](const auto &elem) {
            return elem.tool == tool && elem.action == action;
        });
        it != std::end(tooltexts)) {
        return it->label;
    } else {
        consteval_assert_false("Unable to find a label for this combination");
        return "";
    }
}

struct MenuItemText {
    Action action;
    const char *label;
};

// could have been done with an array of texts directly, but there would be an order dependancy
inline constexpr MenuItemText blank_item_texts[] {
    { Action::Fans, N_("%d Fan Test") },
    { Action::ZAlign, N_("%d Z Alignment Calibration") },
    { Action::XYCheck, N_("%d XY Axis Test") },
    { Action::Loadcell, N_("%d Loadcell Test") },
    { Action::ZCheck, N_("%d Z Axis Test") },
    { Action::Heaters, N_("%d Heater Test") },
    { Action::FilamentSensorCalibration, N_("%d Filament Sensor Calibration") },
};

TestResult get_test_result(Action action, Tool tool);
uint8_t get_tool_mask(Tool tool);
uint64_t get_test_mask(Action action);
Tool get_last_enabled_tool();
}
