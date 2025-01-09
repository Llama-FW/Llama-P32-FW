#include <gcode/gcode.h>

#include "M340.h"
#include "M330.h"
#include <logging/log_dest_syslog.hpp>
#include <stdint.h>
#include <config_store/store_instance.hpp>
#include <metric_handlers.h>

/** \addtogroup G-Codes
 * @{
 */

/**
 * M340: Syslog host and port configuration
 *
 * ## Parameters
 *
 * - <ip_address> - Configures the syslog handler to send all the enabled metrics to the given IP address.
 * - <port> - Configures the syslog handler to send all the enabled metrics to the given port.
 */

void PrusaGcodeSuite::M340() {
    // Syslog has to be allowed in settings
    if (config_store().enable_metrics.get()) {
        SERIAL_ERROR_MSG("Syslog is not allowed!");
        return;
    }

    decltype(config_store().metrics_host)::value_type host;
    int port;

    // If metrics_host_size changes, we gotta change the scan format
    static_assert(config_store_ns::metrics_host_size == 20);
    const auto read = sscanf(parser.string_arg, "%20s %i", host.data(), &port);

    if (read != 2) {
        SERIAL_ECHO_START();
        SERIAL_ECHOLN("does not match '<address> <port>' pattern");
        return;
    }

    // Nothing changed -> don't do anything
    if (strcmp(host.data(), config_store().metrics_host.get().data()) == 0 && port == config_store().syslog_port.get()) {
        return;
    }

    // Prompt the user if he wants to allow the metrics change
    if (!metrics_config_change_prompt()) {
        return;
    }

    config_store().metrics_host.set(host);
    config_store().syslog_port.set(port);
    metrics_reconfigure();
    logging::syslog_reconfigure();

    SERIAL_ECHO_START();
    SERIAL_ECHOLN("Syslog configured successfully");
}

/** @}*/
