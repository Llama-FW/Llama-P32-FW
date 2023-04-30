#include "module_marlin.hpp"

#include <device/peripherals.h>
#include <device/hal.h>
#include "Marlin.h"
#include "modbus/ModbusTask.hpp"
#include "wiring_analog.h"
#include "wiring_digital.h"
#include "Marlin/src/module/temperature.h"
#include "log.h"
#include "gpio.h"
#include "timing.h"
#include "modbus/ModbusInit.hpp" //for modbus::modbus_control
#include "loadcell.hpp"
#include "led.h"
#include "adc.hpp"
#include "safe_state.h"
#include "Cheese.hpp"
#include "hwio.h"
#include "trigger_crash_dump.h"
#include "fanctl.hpp"
#include "task_startup.h"
#include <hal/HAL_MultiWatchdog.hpp>

LOG_COMPONENT_REF(Marlin);

static bool marlin_kill = false;

void stop_marlin();

/**
 * @brief Marlin's watchdog init.
 * Empty as IWDG is initialized here.
 */
void watchdog_init() {
}

static hal::MultiWatchdog marlin_timer_wdg; // Add one instance of watchdog

/**
 * @brief Watchdog refresh called from Marlin from timer interrupt.
 */
void HAL_watchdog_refresh() {
    marlin_timer_wdg.kick(false);
}

void dwarf::modules::marlin::start() {
    hal::MultiWatchdog wdg; //Add one instance of watchdog
    setup();

    modbus::ModbusTask::EnableModbus();

    while (!marlin_kill) {
        if (dwarf::ModbusControl::isDwarfSelected()) {
            led::blinking(0x0f, 0x0f, 0x0f, 500, 500);
        } else {
            led::blinking(0, 0x0f, 0, 1000, 75);
        }

        idle(false);

        dwarf::loadcell::loadcell_loop();

        Cheese::update();

        if (!dwarf::ModbusControl::isDwarfSelected()) {

            if (buddy::hw::button1.read() == buddy::hw::Pin::State::low || buddy::hw::button2.read() == buddy::hw::Pin::State::low) {
                enable_e_steppers();
                if (buddy::hw::button1.read() == buddy::hw::Pin::State::low)
                    buddy::hw::e0Dir.set();
                else
                    buddy::hw::e0Dir.reset();

                buddy::hw::e0Step.set();
                osDelay(1);
                buddy::hw::e0Step.reset();
                disable_e_steppers();
            }
        }

        // process any modbus request that are pending
        dwarf::ModbusControl::ProcessModbusMessages();
        dwarf::ModbusControl::UpdateRegisters();

        // Reload this instance of watchdog
        wdg.kick();

        // Needs to be delay and not osThreadYield(),
        // with yield it would not let idle task to refresh another watchdog instance
        osDelay(1);
    }

    // marlin killed, handle it
    while (1) {
        led::blinking(0xFF, 0x00, 0x00, 500, 500);
        stop_marlin();
        osDelay(100);

        wdg.kick(); // Reload this instance of watchdog
    }
}

void stop_marlin() {
    // stop marlin loop
    marlin_kill = true;

    DISABLE_STEPPER_DRIVER_INTERRUPT();
    DISABLE_TEMPERATURE_INTERRUPT();
    dwarf_init_done = false;
    hwio_safe_state();
}

void analogWrite(uint32_t ulPin, uint32_t ulValue) {
    switch (ulPin) {
    case MARLIN_PIN(FAN): // print fan
        fanCtlPrint[0].setPWM(ulValue);
        return;
    case MARLIN_PIN(FAN1): // heatbreak
        fanCtlHeatBreak[0].setPWM(ulValue);
        return;
    default:
        bsod("Write undefined pin");
    }
}

uint32_t analogRead(uint32_t marlin_pin) {
    switch (marlin_pin) {
    case MARLIN_PIN(TEMP_HEATBREAK): {
        return AdcGet::heatbreakTemp();
    }
    case MARLIN_PIN(TEMP_0): {
        return AdcGet::nozzle();
    }
    case MARLIN_PIN(TEMP_BOARD): {
        return AdcGet::boardTemp();
    }
    default:
        bsod("Read undefined pin");
    }
}

void pinMode(uint32_t marlin_pin, uint32_t value) {
    // not used
}

void digitalWrite(uint32_t marlin_pin, uint32_t value) {
    gpio_set(marlin_pin, value ? 1 : 0);
}

int digitalRead(uint32_t marlinPin) {
    return gpio_get(marlinPin);
}

#if DISABLED(OVERRIDE_KILL_METHOD)
    #error Dwarf needs OVERRIDE_KILL_METHOD kill method enabled
#endif

void kill(PGM_P const lcd_error /*=nullptr*/, PGM_P const lcd_component /*=nullptr*/, const bool steppers_off /*=false*/) {
    log_error(Marlin, "Printer killed: %s: %s", lcd_component, lcd_error);
    dwarf::ModbusControl::TriggerMarlinKillFault(dwarf_shared::errors::FaultStatusMask::MARLIN_KILLED, lcd_component, lcd_error);
    stop_marlin();
}

#include "SPI.h"

#define TMC_WRITE 0x80;

#define writeSCK_L  HAL_GPIO_WritePin(STEPPER_SCK_GPIO_Port, STEPPER_SCK_Pin, GPIO_PIN_RESET)
#define writeSCK_H  HAL_GPIO_WritePin(STEPPER_SCK_GPIO_Port, STEPPER_SCK_Pin, GPIO_PIN_SET)
#define writeMOSI_L HAL_GPIO_WritePin(STEPPER_MOSI_GPIO_Port, STEPPER_MOSI_Pin, GPIO_PIN_RESET)
#define writeMOSI_H HAL_GPIO_WritePin(STEPPER_MOSI_GPIO_Port, STEPPER_MOSI_Pin, GPIO_PIN_SET)
#define readMISO    HAL_GPIO_ReadPin(STEPPER_MISO_GPIO_Port, STEPPER_MISO_Pin)

SPISettings::SPISettings() {}
SPISettings::SPISettings(uint32_t clock, BitOrder bitOrder, uint8_t dataMode) {}
SPIClass::SPIClass() {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    // MISO
    GPIO_InitTypeDef GPIO_InitStruct = { 0 };
    GPIO_InitStruct.Pin = STEPPER_MISO_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(STEPPER_MISO_GPIO_Port, &GPIO_InitStruct);

    // MOSI
    GPIO_InitStruct.Pin = STEPPER_MOSI_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STEPPER_MOSI_GPIO_Port, &GPIO_InitStruct);

    //SCK CSN
    GPIO_InitStruct.Pin = STEPPER_SCK_Pin | STEPPER_CSN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}
void SPIClass::begin(uint8_t _pin) {}
void SPIClass::end(void) {}
void SPIClass::beginTransaction(SPISettings settings) {}
void SPIClass::endTransaction(void) {}

byte SPIClass::transfer(uint8_t _data, SPITransferMode _mode) {
    uint8_t value = 0;
    writeSCK_L;

    for (uint8_t i = 7; i >= 1; i--) {
        // Write bit
        !!(_data & (1 << i)) ? writeMOSI_H : writeMOSI_L;
        // Start clock pulse
        writeSCK_H;
        // Read bit
        value |= (readMISO ? 1 : 0) << i;
        // Stop clock pulse
        writeSCK_L;
    }

    !!(_data & (1 << 0)) ? writeMOSI_H : writeMOSI_L;
    writeSCK_H;
    value |= (readMISO ? 1 : 0) << 0;

    return value;
}

uint16_t SPIClass::transfer16(uint16_t _data, SPITransferMode _mode) {
    uint16_t returnVal = 0x0000;
    returnVal |= transfer((_data >> 8) & 0xFF) << 8;
    returnVal |= transfer(_data & 0xFF) & 0xFF;
    return returnVal;
}

SPIClass SPI;

#include "HardwareSerial.h"

HardwareSerial::HardwareSerial(void *) {
}

size_t HardwareSerial::write(uint8_t ch) {
    static uint8_t line_buffer_len = 0;
    static char line_buffer[128] = { 0 };

    if (line_buffer_len < sizeof(line_buffer)) {
        line_buffer[line_buffer_len++] = ch;
    }

    if (line_buffer[line_buffer_len - 1] == '\n' || line_buffer[line_buffer_len - 1] == '\r' || line_buffer_len == sizeof(line_buffer)) {
        line_buffer[line_buffer_len - 1] = 0;
        log_debug(Marlin, "%s", line_buffer);
        line_buffer_len = 0;
    }
    return 1;
}

size_t HardwareSerial::write(unsigned char const *buffer, unsigned int length) {
    for (unsigned int i = 0; i < length; i++) {
        write(buffer[i]);
    }
    return length;
}

void HardwareSerial::begin(unsigned long baud) {
}

int HardwareSerial::read() {
    return 0;
}

int HardwareSerial::available() {
    return 0;
}

void HardwareSerial::flush() {
}

void HardwareSerial::close() {
}

int HardwareSerial::peek() {
    return 0;
}

HardwareSerial::operator bool() {
    return false;
}

HardwareSerial SerialUART3(nullptr);
HardwareSerial Serial3(nullptr);

#include "Marlin/src/feature/safety_timer.h"

void safety_timer_set_interval(millis_t ms) {
}

bool safety_timer_is_expired() {
    return false;
}

void safety_timer_reset() {
}

SafetyTimer &SafetyTimer::Instance() {
    static SafetyTimer ret;
    return ret;
}

SafetyTimer::SafetyTimer()
    : pBoundPause(nullptr)
    , interval(default_interval)
    , last_reset(0)
    , knob_moves(0)
    , knob_clicks(0) {
}

SafetyTimer::expired_t SafetyTimer::Loop() {
    return SafetyTimer::expired_t::no;
}

#include "metric.h"

void metric_record_custom_at_time(metric_t *metric, uint32_t timestamp, const char *fmt, ...) {
}

void metric_record_integer_at_time(metric_t *metric, uint32_t timestamp, int) {
}

// Marlin's HAL

unsigned HAL_RCC_CSR = 0;

#include "Marlin/src/gcode/gcode.h"

millis_t GcodeSuite::previous_move_ms = 0;

#include "Marlin/src/gcode/queue.h"

void GCodeQueue::get_available_commands() {
}

uint8_t GCodeQueue::length = 0;

#include "Marlin/src/lcd/extensible_ui/ui_api.h"

void ExtUI::onStartup() {}
void ExtUI::onIdle() {}
void ExtUI::onMediaInserted() {}
void ExtUI::onMediaError() {}
void ExtUI::onMediaRemoved() {}
void ExtUI::onPlayTone(const uint16_t frequency, const uint16_t duration) {}
void ExtUI::onPrinterKilled(PGM_P const error, PGM_P const component) {}
void ExtUI::onPrintTimerStarted() {}
void ExtUI::onPrintTimerPaused() {}
void ExtUI::onPrintTimerStopped() {}
void ExtUI::onFilamentRunout(const extruder_t extruder) {}
void ExtUI::onUserConfirmRequired(const char *const msg) {}
void ExtUI::onStatusChanged(const char *const msg) {}
void ExtUI::onFactoryReset() {}
void ExtUI::onStoreSettings(char *) {}
void ExtUI::onLoadSettings(const char *) {}
void ExtUI::onConfigurationStoreWritten(bool success) {}
void ExtUI::onConfigurationStoreRead(bool success) {}
void ExtUI::onMeshUpdate(unsigned char, unsigned char, float) {}

#include "Marlin/src/feature/pause.h"

uint8_t did_pause_print;

// configuration_store

fil_change_settings_t fc_settings[EXTRUDERS];
