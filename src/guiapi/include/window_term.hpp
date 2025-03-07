// window_term.hpp
#pragma once

#include "window.hpp"
#include "term.h"

// each character is folowed by its flags
// at the end of array is stored bit array of change bits
template <size_t COL, size_t ROW>
struct term_buff_t {
    static constexpr size_t size() {
        return ((ROW * COL * 2) + (ROW * COL + 7) / 8);
    }
    static constexpr size_t Columns() { return COL; }
    static constexpr size_t Rows() { return ROW; }

    uint8_t buff[size()];
};

// todo, why is this using 2nd class term_t?
// to be sizeable?
class window_term_t : public window_t {
public:
    Color color_text;
    term_t term;

    // font must be known in ctor, used to calculate rectangle, do not change it later
    template <size_t COL, size_t ROW>
    window_term_t(window_t *parent, point_i16_t pt, term_buff_t<COL, ROW> *term_buff)
        : window_term_t(parent, pt, term_buff->buff, COL, ROW) {}

    int Printf(const char *fmt, ...);
    void WriteChar(uint8_t ch);

protected:
    virtual void unconditionalDraw() override;

    // protected ctors - unsafe. To be used by public template ctors
    window_term_t(window_t *parent, point_i16_t pt, uint8_t *buff, size_t cols, size_t rows);
};
