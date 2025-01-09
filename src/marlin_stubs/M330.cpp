#include <gcode/gcode.h>

#include "M330.h"
#include <metric.h>
#include <metric_handlers.h>
#include <stdint.h>
#include <config_store/store_instance.hpp>
#include <logging/log_dest_syslog.hpp>
#include <client_fsm_types.h>
#include <client_response.hpp>
#include <marlin_server.hpp>

/** \addtogroup G-Codes
 * @{
 */

/**
 * M331: Enable metric
 *
 * ## Parameters
 *
 * - <metric> - Enable `metric` for the currently selected `handler`
 */

void PrusaGcodeSuite::M331() {
    if (!config_store().enable_metrics.get()) {
        SERIAL_ERROR_MSG("Warning: Metrics are not enabled");
    }

    for (auto metric = metric_get_iterator_begin(), e = metric_get_iterator_end(); metric != e; metric++) {
        if (strcmp(metric->name, parser.string_arg) == 0) {
            metric_enable_for_handler(metric, &metric_handler_syslog);
            SERIAL_ECHO_START();
            SERIAL_ECHOLNPAIR_F("Metric enabled: ", parser.string_arg);
            return;
        }
    }

    SERIAL_ERROR_START();
    SERIAL_ECHOLNPAIR("Metric not found: ", parser.string_arg);
}

/**
 * M332: Disable metric
 *
 * ## Parameters
 *
 * - <metric> - Disable `metric` for the currently selected `handler`
 */

void PrusaGcodeSuite::M332() {
    if (!config_store().enable_metrics.get()) {
        SERIAL_ERROR_MSG("Warning: Metrics are not enabled");
    }

    for (auto metric = metric_get_iterator_begin(), e = metric_get_iterator_end(); metric != e; metric++) {
        if (strcmp(metric->name, parser.string_arg) == 0) {
            metric_disable_for_handler(metric, &metric_handler_syslog);
            SERIAL_ECHO_START();
            SERIAL_ECHOLNPAIR_F("Metric disabled: ", parser.string_arg);
            return;
        }
    }

    SERIAL_ERROR_START();
    SERIAL_ECHOLNPAIR("Metric not found: ", parser.string_arg);
}

/**
 * M333: List all metrics and whether they are enabled for the currently selected `handler`.
 */

void PrusaGcodeSuite::M333() {
    for (auto metric = metric_get_iterator_begin(), e = metric_get_iterator_end(); metric != e; metric++) {
        SERIAL_ECHO_START();
        SERIAL_ECHOPGM(metric->name);
        SERIAL_ECHOPGM(is_metric_enabled_for_handler(metric, &metric_handler_syslog) ? " 1" : " 0");
        SERIAL_EOL();
    }
}

/**
 * M334: Metrics & syslog configuration
 *
 * Format: M334 (host) <metrics_port> <syslog_port>
 * Ports are optional
 * Also enables metrics.
 *
 * Empty M334 without anything disables metrics (but does not clear configuration)
 */

void PrusaGcodeSuite::M334() {
    if (!config_store().enable_metrics.get()) {
        SERIAL_ERROR_MSG("Warning: Metrics are not enabled");
    }

    auto host = config_store().metrics_host.get();
    int metrics_port = config_store().metrics_port.get();
    int syslog_port = config_store().syslog_port.get();

    // If metrics_host_size changes, we gotta change the scan format
    static_assert(config_store_ns::metrics_host_size == 20);
    const auto scanned_fields = sscanf(parser.string_arg, "%20s %i %i", host.data(), &metrics_port, &syslog_port);
    const bool enable_metrics = (scanned_fields > 0);

    // Check if the gcode is trying to change the metrics configuration
    {
        bool changes_metrics_config = false;
        changes_metrics_config |= (config_store().enable_metrics.get() != enable_metrics);
        changes_metrics_config |= (strcmp(host.data(), config_store().metrics_host.get().data()) != 0);
        changes_metrics_config |= (metrics_port != config_store().metrics_port.get());
        changes_metrics_config |= (syslog_port != config_store().syslog_port.get());

        // Nothing changed -> nothing needs to be done
        if (!changes_metrics_config) {
            return;
        }
    }

    // Prompt the user if he wants to allow the metrics change
    if (!metrics_config_change_prompt()) {
        return;
    }

    // Store the new settings in the config store
    {
        auto &store = config_store();
        auto transaction = store.get_backend().transaction_guard();
        store.enable_metrics.set(enable_metrics);
        store.metrics_host.set(host);
        store.metrics_port.set(metrics_port);
        store.syslog_port.set(syslog_port);
    }

    // Let the new settings take effect
    metrics_reconfigure();
    logging::syslog_reconfigure();
}

bool PrusaGcodeSuite::metrics_config_change_prompt() {
    marlin_server::set_warning(WarningType::MetricsConfigChangePrompt, PhasesWarning::MetricsConfigChangePrompt);

    Response r;
    while ((r = marlin_server::get_response_from_phase(PhasesWarning::MetricsConfigChangePrompt)) == Response::_none) {
        idle(true);
    }

    marlin_server::fsm_destroy(ClientFSM::Warning);
    return r == Response::Yes;
}

/** @}*/
