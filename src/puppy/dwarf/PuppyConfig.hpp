#pragma once
#include <stdint.h>
#include <stddef.h>
#include "device/hal.h"

//***********************
//*** RS485 configuration
// TODO: Implement DMA reading and decrease this priority. Currently it has to be high so that
static constexpr uint32_t RS485_IRQ_PRIORITY = 0;
static constexpr size_t RS485_BUFFER_SIZE = 256; //Modbus specification needs 256 Bytes

static constexpr uint32_t RS485_BAUDRATE = 230400;
static constexpr uint32_t RS485_STOP_BITS = UART_STOPBITS_2;
static constexpr uint32_t RS485_PARITY = UART_PARITY_NONE;

static constexpr uint32_t RS485_ASSERTION_TIME = 16;
static constexpr uint32_t RS485_DEASSERTION_TIME = 16;

//Modbus specification requires at least 3.5 char delay between frames, one character uses 11 bits
static constexpr uint32_t RS485_RX_TIMEOUT_BITS = 33;

#define RS485_DE_SIGNAL_GPIO_PORT GPIOA
#define RS485_DE_SIGNAL_GPIO_PIN  GPIO_PIN_12

//***********************
//*** Modbus configuration
static constexpr int32_t MODBUS_WATCHDOG_TIMEOUT = 30000;
static constexpr auto MODBUS_FIFO_MAX_COUNT = 31; //Same value as MODBUS_FIFO_LEN defined in fifo_coder.hpp, maximal value allowed by Modbus specification is 31
