#include "puppies/puppy_task.hpp"

#include "task.h"

#include <iterator>
#include "cmsis_os.h"
#include "log.h"
#include "main.h"
#include "puppies/modular_bed.hpp"
#include "puppies/Dwarf.hpp"
#include "puppies/PuppyBootstrap.hpp"
#include "tasks.h"
#include "timing.h"
#include "Marlin/src/module/stepper.h"
#include "Marlin/src/module/prusa/toolchanger.h"
#include <esp_flash.hpp>
#include <tasks.h>
#include <option/has_embedded_esp32.h>
#include "bsod_gui.hpp"

LOG_COMPONENT_DEF(Puppies, LOG_SEVERITY_DEBUG);

namespace buddy::puppies {

osThreadId puppy_task_handle;
osMutexDef(bootstrap_progress_lock);
osMutexId bootstrap_progress_lock_id;
std::optional<PuppyBootstrap::Progress> bootstrap_progress;

static PuppyBootstrap::BootstrapResult bootstrap_puppies(PuppyBootstrap::BootstrapResult minimal_config) {
    // boostrap first
    log_info(Puppies, "Starting bootstrap");
    PuppyBootstrap puppy_bootstrap([](PuppyBootstrap::Progress progress) {
        log_info(Puppies, "Bootstrap stage: %s", progress.description());
        osMutexWait(bootstrap_progress_lock_id, osWaitForever);
        bootstrap_progress = progress;
        osMutexRelease(bootstrap_progress_lock_id);
    });
    return puppy_bootstrap.run(minimal_config);
}

static void verify_puppies_running() {
    // wait for all the puppies to be reacheable
    log_info(Puppies, "Waiting for puppies to boot");

    auto err_supressor = PuppyModbus::ErrorLogSupressor();

    constexpr uint32_t WAIT_TIME = 5000;
    auto reacheability_wait_start = ticks_ms();
    do {
        bool modular_bed_ok = !modular_bed.is_enabled() || (modular_bed.ping() == ModularBed::CommunicationStatus::OK);

        uint8_t num_dwarfs_ok = 0, num_dwarfs_dead = 0;
        for (Dwarf &dwarf : dwarfs) {
            if (dwarf.is_enabled()) {
                if (dwarf.ping() == Dwarf::CommunicationStatus::OK) {
                    ++num_dwarfs_ok;
                } else {
                    ++num_dwarfs_dead;
                }
            }
        }

        if (num_dwarfs_dead == 0 && modular_bed_ok) {
            log_info(Puppies, "All puppies are reacheable. Continuing");
            return;
        } else if (ticks_diff(reacheability_wait_start + WAIT_TIME, ticks_ms()) > 0) {
            log_info(Puppies, "Puppies not ready (dwarfs_num: %d/%d, bed: %i), waiting another 200 ms",
                num_dwarfs_ok, num_dwarfs_ok + num_dwarfs_dead, modular_bed_ok);
            osDelay(200);
            continue;
        } else {
            fatal_error(ErrCode::ERR_SYSTEM_PUPPY_RUN_TIMEOUT);
        }
    } while (true);
}

std::optional<PuppyBootstrap::Progress> get_bootstrap_progress() {
    if (!bootstrap_progress_lock_id) {
        return std::nullopt;
    }

    osMutexWait(bootstrap_progress_lock_id, osWaitForever);
    auto progress = bootstrap_progress;
    osMutexRelease(bootstrap_progress_lock_id);
    return progress;
}

static void puppy_task_loop() {
    // periodically update puppies until there is a failure
    while (true) {
        if (!prusa_toolchanger.update())
            return;

        Dwarf &selected = prusa_toolchanger.getActiveToolOrFirst();

        for (Dwarf &dwarf : dwarfs) {
            if (!dwarf.is_enabled())
                continue; // skip if this dwarf is not enabled

            // do fast refresh of selected dwarf
            if (selected.fast_refresh() != Dwarf::CommunicationStatus::OK)
                return;

            // if we are servicing other then selected
            if (&selected != &dwarf) {
                // fast refresh of non-selected dwarf
                if (dwarf.fast_refresh() != Dwarf::CommunicationStatus::OK)
                    return;

                // do fast refresh of selected dwarf
                if (selected.fast_refresh() != Dwarf::CommunicationStatus::OK)
                    return;
            }

            // then do slow refresh of one dwarf
            if (dwarf.refresh() != Dwarf::CommunicationStatus::OK)
                return;
        }

        if (selected.fast_refresh() != Dwarf::CommunicationStatus::OK)
            return;

        if (modular_bed.refresh() != ModularBed::CommunicationStatus::OK)
            return;

        osDelay(1);
    }
}

static bool puppy_initial_scan() {
    // init each puppy
    for (Dwarf &dwarf : dwarfs) {
        if (dwarf.is_enabled())
            if (dwarf.initial_scan() != Dwarf::CommunicationStatus::OK)
                return false;
    }

    if (modular_bed.initial_scan() != ModularBed::CommunicationStatus::OK) {
        return false;
    }
    return true;
}

static void puppy_task_body(void const *argument) {
    wait_for_dependecies(PUPPY_TASK_START_DEPS);

#if BOARD_VER_EQUAL_TO(0, 5, 0)
    // This is temporary, remove once everyone has compatible hardware.
    // Requires new sandwich rev. 06 or rev. 05 with R83 removed.

    #if HAS_EMBEDDED_ESP32()
    // Flash ESP
    ESPFlash esp_flash;
    auto esp_result = esp_flash.flash();
    if (esp_result != ESPFlash::State::Done) {
        log_error(Puppies, "ESP flash failed with %d", esp_result);
        ESPFlash::fatal_err(esp_result);
    }
    provide_dependecy(ComponentDependencies::ESP_FLASHED);
    #endif
#endif

    bool toolchanger_first_run = true;

    // by default, we want one modular bed and one dwarf
    PuppyBootstrap::BootstrapResult minimal_puppy_config = PuppyBootstrap::MINIMAL_PUPPY_CONFIG;

    do {
        // reset and flash the puppies
        auto bootstrap_result = bootstrap_puppies(minimal_puppy_config);
        // once some puppies are detected, consider this minimal puppy config (do no allow disconnection of puppy while running)
        minimal_puppy_config = bootstrap_result;

        // set what puppies are connected
        modular_bed.set_enabled(bootstrap_result.is_kennel_occupied(Kennel::MODULAR_BED));
        for (int i = 0; i < DWARF_MAX_COUNT; i++) {
            dwarfs[i].set_enabled(bootstrap_result.is_kennel_occupied(Kennel::DWARF_1 + i));
        }

        // wait for puppies to boot up, ensure they are runining
        verify_puppies_running();

        do {
            // do intial scan of puppies to init them
            if (!puppy_initial_scan()) {
                break;
            }

            // select active tool (previously active tool, or first one when starting)
            if (!prusa_toolchanger.init(toolchanger_first_run)) {
                log_error(Puppies, "Unable to select tool, retring");
                break;
            }
            toolchanger_first_run = false;

            provide_dependecy(ComponentDependencies::PUPPIES_READY_IDX);
            log_info(Puppies, "Puppies are ready");

            wait_for_dependecies(PUPPY_TASK_RUN_DEPS);

            // write current Marlin's state of the E TMC
            stepperE0.push();

            // now run puppy main loop
            puppy_task_loop();
        } while (false);

        log_error(Puppies, "Communication error, going to recovery puppies");
        osDelay(1300); // Needs to be here to give puppies time to finish dumping
    } while (true);
}

void start_puppy_task() {
    bootstrap_progress_lock_id = osMutexCreate(osMutex(bootstrap_progress_lock));

    osThreadDef(puppies, puppy_task_body, osPriorityRealtime, 0, 128 * 6);
    puppy_task_handle = osThreadCreate(osThread(puppies), NULL);
}

}
