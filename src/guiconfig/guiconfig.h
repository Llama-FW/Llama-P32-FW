//guiconfig.h - guiapi configuration file
#pragma once

#include <inttypes.h>
#include <device/board.h>

///////////////////////////
// display type selector //
///////////////////////////
#if BOARD_IS_XBUDDY || BOARD_IS_XLBUDDY
    #define USE_ILI9488
    #define MENU_HAS_SCROLLBAR true
    #define MENU_HAS_BUTTONS   true
#elif BOARD_IS_BUDDY
    #define USE_ST7789
    #define MENU_HAS_SCROLLBAR false
    #define MENU_HAS_BUTTONS   false
#else
    #error "macro BOARD_TYPE is not defined"
#endif

//--------------------------------------
//GUI configuration
#define GUI_USE_RTOS
#define GUI_JOGWHEEL_SUPPORT
#define GUI_WINDOW_SUPPORT

//--------------------------------------
//FreeRTOS Signals

static const uint32_t MENU_TIMEOUT_MS = 30000;
//--------------------------------------
#ifdef USE_ST7789
    //ST7789v configuration
    #define ST7789V_USE_RTOS
    #define ST7789V_PNG_SUPPORT

//st7789v - spi DMA transmit complete (triggered from callback, gui thread is waiting for this signal)
static const uint32_t ST7789V_SIG_SPI_TX = 0x08;
#endif

#ifdef USE_ILI9488
    //ILI9488 configuration
    #define ILI9488_USE_RTOS
    #define ILI9488_PNG_SUPPORT
    #define ILI9488_SIG_SPI_TX 0x0008
    #define ILI9488_SIG_SPI_RX 0x0008

#endif
