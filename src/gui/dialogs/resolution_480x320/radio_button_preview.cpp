/**
 * @file radio_button_preview.cpp
 */

#include "radio_button_preview.hpp"
#include "window_icon.hpp"
#include "window_text.hpp"
#include "i18n.h"
#include "png_resources.hpp"

const char *label_texts[] = {
    N_("Print"),
    N_("Back")
};

static constexpr const png::Resource *icons[] = {
    &png::print_80x80,
    &png::back_80x80,
    &png::print_80x80_focused,
    &png::back_80x80_focused
};

static constexpr const uint8_t icon_label_delim = 5;
static constexpr const uint8_t label_height = 16;
static constexpr const uint8_t button_cnt = 2;

RadioButtonPreview::RadioButtonPreview(window_t *parent, Rect16 rect)
    : AddSuperWindow<RadioButtonFsm<PhasesPrintPreview>>(parent, rect, PhasesPrintPreview::main_dialog) {
}

Rect16 RadioButtonPreview::getVerticalIconRect(uint8_t idx) const {
    Rect16 rect = GetRect();
    return Rect16(
        rect.Left(),
        rect.Top() + idx * (GuiDefaults::ButtonIconSize + GuiDefaults::ButtonIconVerticalSpacing),
        GuiDefaults::ButtonIconSize,
        GuiDefaults::ButtonIconSize);
}

Rect16 RadioButtonPreview::getVerticalLabelRect(uint8_t idx) const {
    Rect16 rect = GetRect();
    return Rect16(
        rect.Left(),
        rect.Top() + GuiDefaults::ButtonIconSize + icon_label_delim + idx * (GuiDefaults::ButtonIconSize + GuiDefaults::ButtonIconVerticalSpacing),
        GuiDefaults::ButtonIconSize,
        label_height);
}

void RadioButtonPreview::unconditionalDraw() {
    for (int i = 0; i < button_cnt; i++) {
        uint8_t res_offset = GetBtnIndex() == i ? button_cnt : 0;
        window_icon_t icon(nullptr, getVerticalIconRect(i), icons[res_offset + i]);
        window_text_t label(nullptr, getVerticalLabelRect(i), is_multiline::no, is_closed_on_click_t::no, _(label_texts[i]));

        label.font = resource_font(IDR_FNT_SMALL);
        label.SetAlignment(Align_t::Center());

        icon.Draw();
        label.Draw();
    }
}

void RadioButtonPreview::windowEvent(EventLock /*has private ctor*/, window_t *sender, GUI_event_t event, void *param) {
    switch (event) {
    case GUI_event_t::CLICK: {
        Response response = Click();
        event_conversion_union un;
        un.response = response;
        marlin_FSM_response(phase, response); // Use FSM logic from RadioButtonFsm<>
        if (GetParent()) {
            GetParent()->WindowEvent(this, GUI_event_t::CHILD_CLICK, un.pvoid);
        }
    } break;
    case GUI_event_t::TOUCH: {
        event_conversion_union un;
        un.pvoid = param;
        std::optional<size_t> new_index = std::nullopt;
        for (size_t i = 0; i < button_cnt; ++i) {
            if (getVerticalIconRect(i).Contain(un.point)) {
                new_index = i;
                break;
            }
        }

        if (new_index) {
            SetBtnIndex(*new_index);
            // Sound_Play(eSOUND_TYPE::ButtonEcho);
            // Generate click for itself
            WindowEvent(this, GUI_event_t::CLICK, param);
        }
    } break;
    default:
        SuperWindowEvent(sender, event, param);
    }
}
