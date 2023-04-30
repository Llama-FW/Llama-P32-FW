#include <inttypes.h>
#include "app_metrics.h"
#include "metric.h"
#include "log.h"
#include "version.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "malloc.h"
#include "heap.h"
#if HAS_ADVANCED_POWER
    #include "advanced_power.hpp"
#endif // HAS_ADVANCED_POWER
#include "media.h"
#include "timing.h"
#include "config_buddy_2209_02.h"
#include "otp.h"
#include <array>
#include "filament.hpp"
#include "marlin_vars.hpp"
#include "config_features.h"

#include "../Marlin/src/module/temperature.h"
#include "../Marlin/src/module/planner.h"
#include "../Marlin/src/module/stepper.h"
#include "../Marlin/src/feature/power.h"

#if BOARD_IS_XLBUDDY
    #include <puppies/Dwarf.hpp>
    #include <Marlin/src/module/prusa/toolchanger.h>
#endif

#if ENABLED(PRUSA_TOOLCHANGER)
    #define FOREACH_EXTRUDER         for (int e = 0; e < EXTRUDERS - 1; e++)
    #define active_extruder_or_first active_extruder
#else
    #define FOREACH_EXTRUDER         for (int e = 0; e < 1; e++)
    #define active_extruder_or_first 0
#endif

LOG_COMPONENT_REF(Metrics);

/// This metric is defined in Marlin/src/module/probe.cpp, thus no interface
#if HAS_BED_PROBE
extern metric_t metric_probe_z;
extern metric_t metric_probe_z_diff;
extern metric_t metric_home_diff;
#endif

struct RunApproxEvery {
    uint32_t interval_ms;
    uint32_t last_run_timestamp;

    constexpr RunApproxEvery(uint32_t interval_ms)
        : interval_ms(interval_ms)
        , last_run_timestamp(0) {
    }

    bool operator()() {
        if (ticks_diff(ticks_ms(), last_run_timestamp) > static_cast<int32_t>(interval_ms)) {
            last_run_timestamp = ticks_ms();
            return true;
        } else {
            return false;
        }
    }
};

void Buddy::Metrics::RecordRuntimeStats() {
    static metric_t fw_version = METRIC("fw_version", METRIC_VALUE_STRING, 10 * 1000, METRIC_HANDLER_ENABLE_ALL);
    metric_record_string(&fw_version, "%s", project_version_full);

    board_revision_t board_revision;
    if (otp_get_board_revision(&board_revision) == false) {
        board_revision.br = 0;
    }
    static metric_t buddy_revision = METRIC("buddy_revision", METRIC_VALUE_STRING, 10 * 1002, METRIC_HANDLER_ENABLE_ALL);
    metric_record_string(&buddy_revision, "%u.%u", board_revision.bytes[0], board_revision.bytes[1]);

    static metric_t current_filamnet = METRIC("filament", METRIC_VALUE_STRING, 10 * 1007, METRIC_HANDLER_ENABLE_ALL);
    auto current_filament = filament::get_type_in_extruder(marlin_vars()->active_extruder);
    metric_record_string(&current_filamnet, "%s", filament::get_description(current_filament).name);

    static metric_t stack = METRIC("stack", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_ENABLE_ALL);
    static auto should_record_stack = RunApproxEvery(3000);
    if (should_record_stack()) {
        static TaskStatus_t task_statuses[15];
        int count = uxTaskGetSystemState(task_statuses, sizeof(task_statuses) / sizeof(task_statuses[1]), NULL);
        if (count == 0) {
            log_error(Metrics, "Failed to record stack metrics. The task_statuses array might be too small.");
        } else {
            for (int idx = 0; idx < count; idx++) {
                const char *stack_base = (char *)task_statuses[idx].pxStackBase;
                size_t s = 0;
                /* We can only report free stack space for heap-allocated stack frames. */
                if (mem_is_heap_allocated(stack_base))
                    s = malloc_usable_size((void *)stack_base);
                const char *task_name = task_statuses[idx].pcTaskName;
                if (strcmp(task_name, "Tmr Svc") == 0)
                    task_name = "TmrSvc";
                metric_record_custom(&stack, ",n=%.7s t=%i,m=%hu", task_name, s, task_statuses[idx].usStackHighWaterMark);
            }
        }
    }

    static metric_t heap = METRIC("heap", METRIC_VALUE_CUSTOM, 503, METRIC_HANDLER_ENABLE_ALL);
    metric_record_custom(&heap, " free=%ii,total=%ii", xPortGetFreeHeapSize(), heap_total_size);
}

void Buddy::Metrics::RecordMarlinVariables() {
#if HAS_BED_PROBE
    metric_register(&metric_probe_z);
    metric_register(&metric_probe_z_diff);
    metric_register(&metric_home_diff);
#endif
    static metric_t is_printing = METRIC("is_printing", METRIC_VALUE_INTEGER, 5000, METRIC_HANDLER_ENABLE_ALL);
    metric_record_integer(&is_printing, printingIsActive() ? 1 : 0);

#if ENABLED(PRUSA_TOOLCHANGER)
    static metric_t active_extruder_metric = METRIC("active_extruder", METRIC_VALUE_INTEGER, 1000, METRIC_HANDLER_ENABLE_ALL);
    metric_record_integer(&active_extruder_metric, active_extruder);
#endif

#if HAS_TEMP_HEATBREAK
    static metric_t heatbreak = METRIC("temp_hbr", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_DISABLE_ALL); // float value, tag "n": extruder index, tag "a": is active extruder
    static auto heatbreak_should_record = RunApproxEvery(1000);
    if (heatbreak_should_record()) {
        FOREACH_EXTRUDER {
            metric_record_custom(&heatbreak, ",n=%i,a=%i value=%.2f", e, e == active_extruder_or_first, static_cast<double>(thermalManager.degHeatbreak(e)));
        }
    }
#endif

#if HAS_TEMP_BOARD
    static metric_t board
        = METRIC("temp_brd", METRIC_VALUE_FLOAT, 1000 - 9, METRIC_HANDLER_DISABLE_ALL);
    metric_record_float(&board, thermalManager.degBoard());
#endif
    static metric_t bed = METRIC("temp_bed", METRIC_VALUE_FLOAT, 2000 + 23, METRIC_HANDLER_DISABLE_ALL);
    metric_record_float(&bed, thermalManager.degBed());

    static metric_t target_bed = METRIC("ttemp_bed", METRIC_VALUE_INTEGER, 1000, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&target_bed, thermalManager.degTargetBed());

    static metric_t nozzle = METRIC("temp_noz", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_DISABLE_ALL);
    static auto nozzle_should_record = RunApproxEvery(1000 - 10);
    if (nozzle_should_record()) {
        FOREACH_EXTRUDER {
            metric_record_custom(&nozzle, ",n=%i,a=%i value=%.2f", e, e == active_extruder, static_cast<double>(thermalManager.degHotend(e)));
        }
    }

    static metric_t target_nozzle = METRIC("ttemp_noz", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_DISABLE_ALL);
    static auto target_nozzle_should_record = RunApproxEvery(1000 + 9);
    if (target_nozzle_should_record()) {
        FOREACH_EXTRUDER {
            metric_record_custom(&target_nozzle, ",n=%i,a=%i value=%ii", e, e == active_extruder, thermalManager.degTargetHotend(e));
        }
    }

#if FAN_COUNT >= 1
    static metric_t fan_speed = METRIC("fan_speed", METRIC_VALUE_INTEGER, 501, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&fan_speed, thermalManager.fan_speed[0]);
#endif

#if FAN_COUNT >= 2
    static metric_t heatbreak_fan_speed = METRIC("fan_hbr_speed", METRIC_VALUE_INTEGER, 502, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&heatbreak_fan_speed, thermalManager.fan_speed[1]);
#endif

    static metric_t ipos_x = METRIC("ipos_x", METRIC_VALUE_INTEGER, 10, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&ipos_x, stepper.position_from_startup(AxisEnum::X_AXIS));
    static metric_t ipos_y = METRIC("ipos_y", METRIC_VALUE_INTEGER, 10, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&ipos_y, stepper.position_from_startup(AxisEnum::Y_AXIS));
    static metric_t ipos_z = METRIC("ipos_z", METRIC_VALUE_INTEGER, 10, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&ipos_z, stepper.position_from_startup(AxisEnum::Z_AXIS));

    static metric_t pos_x = METRIC("pos_x", METRIC_VALUE_FLOAT, 11, METRIC_HANDLER_DISABLE_ALL);
    metric_record_float(&pos_x, planner.get_axis_position_mm(AxisEnum::X_AXIS));
    static metric_t pos_y = METRIC("pos_y", METRIC_VALUE_FLOAT, 11, METRIC_HANDLER_DISABLE_ALL);
    metric_record_float(&pos_y, planner.get_axis_position_mm(AxisEnum::Y_AXIS));
    static metric_t pos_z = METRIC("pos_z", METRIC_VALUE_FLOAT, 11, METRIC_HANDLER_DISABLE_ALL);
    metric_record_float(&pos_z, planner.get_axis_position_mm(AxisEnum::Z_AXIS));

#if HAS_BED_PROBE
    static metric_t adj_z = METRIC("adj_z", METRIC_VALUE_FLOAT, 1500, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&adj_z, probe_offset.z);
#endif

#if ENABLED(AUTO_POWER_CONTROL)
    static metric_t heater_enabled = METRIC("heater_enabled", METRIC_VALUE_INTEGER, 1500, METRIC_HANDLER_ENABLE_ALL);
    metric_record_integer(&heater_enabled, powerManager.is_power_needed());
#endif
}

#ifdef HAS_ADVANCED_POWER
    #if BOARD_IS_XBUDDY
void Buddy::Metrics::RecordPowerStats() {
    static metric_t metric_bed_v_raw = METRIC("volt_bed_raw", METRIC_VALUE_INTEGER, 1000, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&metric_bed_v_raw, advancedpower.GetBedVoltageRaw());
    static metric_t metric_bed_v = METRIC("volt_bed", METRIC_VALUE_FLOAT, 1001, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_bed_v, advancedpower.GetBedVoltage());
    static metric_t metric_nozzle_v_raw = METRIC("volt_nozz_raw", METRIC_VALUE_INTEGER, 1002, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&metric_nozzle_v_raw, advancedpower.GetHeaterVoltageRaw());
    static metric_t metric_nozzle_v = METRIC("volt_nozz", METRIC_VALUE_FLOAT, 1003, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_nozzle_v, advancedpower.GetHeaterVoltage());
    static metric_t metric_nozzle_i_raw = METRIC("curr_nozz_raw", METRIC_VALUE_INTEGER, 1004, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&metric_nozzle_i_raw, advancedpower.GetHeaterCurrentRaw());
    static metric_t metric_nozzle_i = METRIC("curr_nozz", METRIC_VALUE_FLOAT, 1005, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_nozzle_i, advancedpower.GetHeaterCurrent());
    static metric_t metric_input_i_raw = METRIC("curr_inp_raw", METRIC_VALUE_INTEGER, 1006, METRIC_HANDLER_DISABLE_ALL);
    metric_record_integer(&metric_input_i_raw, advancedpower.GetInputCurrentRaw());
    static metric_t metric_input_i = METRIC("curr_inp", METRIC_VALUE_FLOAT, 1007, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_input_i, advancedpower.GetInputCurrent());
        #if HAS_MMU2
    static metric_t metric_mmu_i = METRIC("cur_mmu_imp", METRIC_VALUE_FLOAT, 1008, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_mmu_i, advancedpower.GetMMUInputCurrent());
        #endif
    static metric_t metric_oc_nozzle_fault = METRIC("oc_nozz", METRIC_VALUE_INTEGER, 1010, METRIC_HANDLER_ENABLE_ALL);
    metric_record_integer(&metric_oc_nozzle_fault, advancedpower.HeaterOvercurentFaultDetected());
    static metric_t metric_oc_input_fault = METRIC("oc_inp", METRIC_VALUE_INTEGER, 1011, METRIC_HANDLER_ENABLE_ALL);
    metric_record_integer(&metric_oc_input_fault, advancedpower.OvercurrentFaultDetected());
}
    #elif BOARD_IS_XLBUDDY
void Buddy::Metrics::RecordPowerStats() {
    static metric_t metric_splitter_5V_current = METRIC("splitter_5V_current", METRIC_VALUE_FLOAT, 1000, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_splitter_5V_current, advancedpower.GetDwarfSplitter5VCurrent());

    static metric_t metric_24VVoltage = METRIC("24VVoltage", METRIC_VALUE_FLOAT, 1001, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_24VVoltage, advancedpower.Get24VVoltage());

    static metric_t metric_5VVoltage = METRIC("5VVoltage", METRIC_VALUE_FLOAT, 1002, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_5VVoltage, advancedpower.Get5VVoltage());

    static metric_t metric_Sandwitch5VCurrent = METRIC("Sandwitch5VCurrent", METRIC_VALUE_FLOAT, 1003, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_Sandwitch5VCurrent, advancedpower.GetDwarfSandwitch5VCurrent());

    static metric_t metric_xlbuddy5VCurrent = METRIC("xlbuddy5VCurrent", METRIC_VALUE_FLOAT, 1004, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_xlbuddy5VCurrent, advancedpower.GetXLBuddy5VCurrent());
}
    #else
        #error "This board doesn't support ADVANCED_POWER"
    #endif

#endif // HAS_ADVANCED_POWER

void Buddy::Metrics::RecordPrintFilename() {
    static metric_t file_name = METRIC("print_filename", METRIC_VALUE_STRING, 5000, METRIC_HANDLER_ENABLE_ALL);
    if (media_print_get_state() != media_print_state_t::media_print_state_NONE) {
        // The docstring for media_print_filename() advises against using this function; however, there is currently no replacement for it.
        metric_record_string(&file_name, "%s", marlin_vars()->media_LFN.get_ptr());
    } else {
        metric_record_string(&file_name, "");
    }
}

#if BOARD_IS_XLBUDDY
void Buddy::Metrics::record_dwarf_mcu_temperature() {
    buddy::puppies::Dwarf &dwarf = prusa_toolchanger.getActiveToolOrFirst();
    static metric_t metric_dwarfMCUTemperature = METRIC("dwarf_mcu_temp", METRIC_VALUE_FLOAT, 1001, METRIC_HANDLER_ENABLE_ALL);
    metric_record_float(&metric_dwarfMCUTemperature, dwarf.get_mcu_temperature());
}
#endif
