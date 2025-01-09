#include "screen_menu_network_settings.hpp"
#include "wui_api.h"
#include "RAII.hpp"
#include "ScreenHandler.hpp"
#include "netdev.h"
#include <http_lifetime.h>
#include <espif.h>
#include "marlin_client.hpp"

// ------------------- ETHERNET -----------------------
ScreenMenuEthernetSettings::ScreenMenuEthernetSettings()
    : ScreenMenuEthernetSettings_(_(label)) {
}

void ScreenMenuEthernetSettings::windowEvent(window_t *sender, GUI_event_t event, void *param) {
    if (event == GUI_event_t::LOOP) {
        update_all_updatable_items();
    }

    ScreenMenu::windowEvent(sender, event, param);
}

// ------------------------ WIFI -----------------------------------
ScreenMenuWifiSettings::ScreenMenuWifiSettings()
    : ScreenMenuWifiSettings_(_(label)) {
}

void ScreenMenuWifiSettings::windowEvent(window_t *sender, GUI_event_t event, void *param) {
    if (event == GUI_event_t::LOOP) {
        update_all_updatable_items();
    }

    ScreenMenu::windowEvent(sender, event, param);
}
