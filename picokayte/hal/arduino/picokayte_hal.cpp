/*
 * picokayte_hal.cpp - the PicoKayte HAL (picokayte.h) for Arduino cores.
 *
 * Written against the plain Arduino API (Serial, pinMode, digitalWrite,
 * analogRead, analogWrite, millis, delay), so it builds for any core that
 * has a C++ Serial; ESP32 gets pin checks and a 64-bit clock from ESP-IDF.
 * picokayte copies it into the sketch next to the program.
 */
#include <Arduino.h>
#include "picokayte.h"
#include "picokayte_config.h"

#ifdef ARDUINO_ARCH_ESP32
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#endif

#ifndef PICOKAYTE_BAUD
#define PICOKAYTE_BAUD 115200
#endif

extern "C" {

/* "\n" goes out as "\r\n", for terminals (screen, minicom ...). */
void kayte_hal_write(const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') {
            Serial.write((const uint8_t *)s + start, i - start);
            Serial.write((const uint8_t *)"\r\n", 2);
            start = i + 1;
        }
    if (start < n)
        Serial.write((const uint8_t *)s + start, n - start);
}

void kayte_hal_flush(void)
{
    Serial.flush();
}

int kayte_hal_getc(void)
{
    static bool after_cr = false;
    for (;;) {
        while (Serial.available() <= 0)
            delay(1);
        int c = Serial.read();
        if (c < 0)
            continue;
        if (c == '\n' && after_cr) { /* the "\n" of "\r\n" */
            after_cr = false;
            continue;
        }
        after_cr = c == '\r';
        return c == '\r' ? '\n' : c;
    }
}

void kayte_hal_fatal(const char *msg)
{
    Serial.print("Runtime Error: ");
    Serial.print(msg);
    Serial.print("\r\n");
    Serial.flush();
    for (;;)
        delay(1000);
}

int kayte_hal_pin_mode(int pin, int mode)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!GPIO_IS_VALID_GPIO(pin) || (mode == 1 && !GPIO_IS_VALID_OUTPUT_GPIO(pin)))
        return -1;
#endif
    switch (mode) {
    case 0:
        pinMode(pin, INPUT);
        return 0;
    case 1:
        pinMode(pin, OUTPUT);
        return 0;
    case 2:
        pinMode(pin, INPUT_PULLUP);
        return 0;
    default:
        /* A macro on ESP32, an enum value in ArduinoCore-API cores; AVR has none. */
#if defined(INPUT_PULLDOWN) || defined(ARDUINO_API_VERSION)
        pinMode(pin, INPUT_PULLDOWN);
        return 0;
#else
        return -1;
#endif
    }
}

void kayte_hal_digital_write(int pin, int level)
{
    digitalWrite(pin, level ? HIGH : LOW);
}

int kayte_hal_digital_read(int pin)
{
    return digitalRead(pin) == HIGH;
}

int kayte_hal_analog_read(int pin)
{
#ifdef ARDUINO_ARCH_ESP32
    if (digitalPinToAnalogChannel(pin) < 0)
        return -1;
#endif
    return analogRead(pin);
}

int kayte_hal_analog_write(int pin, int duty)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin))
        return -1;
#endif
    analogWrite(pin, duty);
    return 0;
}

uint64_t kayte_hal_millis(void)
{
#ifdef ARDUINO_ARCH_ESP32
    return (uint64_t)esp_timer_get_time() / 1000;
#else
    /* millis() wraps after 49 days; count the wraps. */
    static uint32_t last;
    static uint64_t high;
    uint32_t now = millis();
    if (now < last)
        high += (uint64_t)1 << 32;
    last = now;
    return high + now;
#endif
}

uint64_t kayte_hal_micros(void)
{
#ifdef ARDUINO_ARCH_ESP32
    return (uint64_t)esp_timer_get_time();
#else
    /* micros() wraps after 71 minutes: counted when MICROS is read at least that often. */
    static uint32_t last;
    static uint64_t high;
    uint32_t now = micros();
    if (now < last)
        high += (uint64_t)1 << 32;
    last = now;
    return high + now;
#endif
}

void kayte_hal_delay_ms(uint32_t ms)
{
    delay(ms);
}

uint32_t kayte_hal_entropy(void)
{
#ifdef ARDUINO_ARCH_ESP32
    return esp_random();
#else
    return micros(); /* how long the board took to get here, serial port included */
#endif
}

/* setup(): open the serial port, wait a little for a USB one to be opened
 * so the first PRINTs aren't lost, then run the program to its end. */
void picokayte_run(void)
{
    Serial.begin(PICOKAYTE_BAUD);
    uint32_t start = millis();
    while (!Serial && millis() - start < 3000)
        delay(10);
    static char name[] = "kayte";
    char *argv[] = {name, NULL};
    kayte_main(1, argv);
    Serial.flush();
}

/* loop(): the program has ended; nothing left to do. */
void picokayte_idle(void)
{
    delay(1000);
}

} /* extern "C" */
