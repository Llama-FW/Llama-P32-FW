/*
 * window_dlg_popup.hpp
 *
 *  Created on: Nov 11, 2019
 *      Author: Migi
 */

#pragma once

#include "window_frame.hpp"
#include "window_text.hpp"

// Singleton dialog for messages
class window_dlg_popup_t : public window_frame_t {
    window_text_t text;
    uint32_t open_time;
    uint32_t ttl; // time to live

    window_dlg_popup_t(Rect16 rect, const string_view_utf8 &txt);
    window_dlg_popup_t(const window_dlg_popup_t &) = delete;

protected:
    virtual void windowEvent(window_t *sender, GUI_event_t event, void *param) override;

public:
    // register dialog to actual screen
    // 1 screen should provide same rectangle, or it might draw incorrectly
    static void Show(Rect16 rect, const string_view_utf8 &txt, uint32_t time = 1000);
};
