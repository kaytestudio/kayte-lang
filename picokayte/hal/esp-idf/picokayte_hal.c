/*
 * picokayte_hal.c - the PicoKayte HAL (picokayte.h) for ESP-IDF 5.x.
 *
 * The console is ESP-IDF's stdio (UART0 or USB-Serial-JTAG, as configured),
 * pins are GPIOs, ANALOGWRITE is LEDC PWM (8 bits, 5 kHz, one channel per
 * pin) and ANALOGREAD the ADC oneshot driver (12 dB attenuation: 0 - ~3.1 V).
 * app_main() runs the program in a task of its own (PICOKAYTE_STACK bytes).
 * picokayte copies it into the project's main/ next to the program.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "picokayte.h"
#include "picokayte_config.h"

#ifndef PICOKAYTE_STACK
#define PICOKAYTE_STACK 16384
#endif

void kayte_hal_write(const char *s, size_t n)
{
    fwrite(s, 1, n, stdout);
}

void kayte_hal_flush(void)
{
    fflush(stdout);
}

/* stdin doesn't block (no UART driver is installed): poll it. */
int kayte_hal_getc(void)
{
    static int after_cr;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\n' && after_cr) { /* the "\n" of "\r\n" */
            after_cr = 0;
            continue;
        }
        after_cr = c == '\r';
        return c == '\r' ? '\n' : c;
    }
}

void kayte_hal_fatal(const char *msg)
{
    fflush(stdout);
    printf("Runtime Error: %s\n", msg);
    fflush(stdout);
    for (;;)
        vTaskDelay(portMAX_DELAY);
}

/* ---- PWM: an LEDC channel per pin, on one shared 8-bit timer ---- */

static int8_t pwm_channel[GPIO_NUM_MAX]; /* channel + 1, or 0 */
static uint32_t pwm_used;                /* channels taken, a bit each */
static int pwm_timer_ready;

static void pwm_release(int pin)
{
    if (pin < 0 || pin >= GPIO_NUM_MAX || !pwm_channel[pin])
        return;
    int ch = pwm_channel[pin] - 1;
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
    pwm_used &= ~(1u << ch);
    pwm_channel[pin] = 0;
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);
}

int kayte_hal_analog_write(int pin, int duty)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin))
        return -1;
    if (!pwm_timer_ready) {
        ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&t) != ESP_OK)
            return -1;
        pwm_timer_ready = 1;
    }
    if (!pwm_channel[pin]) {
        int ch = 0;
        while (ch < LEDC_CHANNEL_MAX && (pwm_used & (1u << ch)))
            ch++;
        if (ch == LEDC_CHANNEL_MAX)
            return -1; /* every channel is in use */
        ledc_channel_config_t c = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)ch,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        if (ledc_channel_config(&c) != ESP_OK)
            return -1;
        pwm_used |= 1u << ch;
        pwm_channel[pin] = (int8_t)(ch + 1);
    }
    ledc_channel_t ch = (ledc_channel_t)(pwm_channel[pin] - 1);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, (uint32_t)duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    return 0;
}

/* ---- GPIO ---- */

int kayte_hal_pin_mode(int pin, int mode)
{
    if (!GPIO_IS_VALID_GPIO(pin) || (mode == 1 && !GPIO_IS_VALID_OUTPUT_GPIO(pin)))
        return -1;
    pwm_release(pin);
    gpio_reset_pin((gpio_num_t)pin);
    /* An output stays readable, so DIGITALREAD gives what was written. */
    gpio_set_direction((gpio_num_t)pin, mode == 1 ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, mode == 2 ? GPIO_PULLUP_ONLY
                                        : mode == 3 ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
    return 0;
}

void kayte_hal_digital_write(int pin, int level)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin))
        return;
    pwm_release(pin); /* as in Arduino: a digital write ends PWM */
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

int kayte_hal_digital_read(int pin)
{
    if (!GPIO_IS_VALID_GPIO(pin))
        return 0;
    return gpio_get_level((gpio_num_t)pin);
}

/* ---- ADC ---- */

static adc_oneshot_unit_handle_t adc_units[2];
static uint32_t adc_ready[2]; /* channels configured, a bit each */

int kayte_hal_analog_read(int pin)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK || unit > ADC_UNIT_2)
        return -1;
    if (!adc_units[unit]) {
        adc_oneshot_unit_init_cfg_t cfg = {.unit_id = unit};
        if (adc_oneshot_new_unit(&cfg, &adc_units[unit]) != ESP_OK)
            return -1;
    }
    if (!(adc_ready[unit] & (1u << ch))) {
        adc_oneshot_chan_cfg_t cfg = {
            .atten = (adc_atten_t)3, /* ADC_ATTEN_DB_12 (DB_11 before IDF 5.2) */
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_config_channel(adc_units[unit], ch, &cfg) != ESP_OK)
            return -1;
        adc_ready[unit] |= 1u << ch;
    }
    int raw = 0;
    if (adc_oneshot_read(adc_units[unit], ch, &raw) != ESP_OK)
        return -1;
    return raw;
}

/* ---- Time ---- */

uint64_t kayte_hal_millis(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

uint64_t kayte_hal_micros(void)
{
    return (uint64_t)esp_timer_get_time();
}

void kayte_hal_delay_ms(uint32_t ms)
{
    if (ms >= portTICK_PERIOD_MS)
        vTaskDelay(ms / portTICK_PERIOD_MS);
    if (ms % portTICK_PERIOD_MS)
        esp_rom_delay_us((ms % portTICK_PERIOD_MS) * 1000);
}

uint32_t kayte_hal_entropy(void)
{
    return esp_random();
}

/* ---- Start-up ---- */

static void kayte_task(void *arg)
{
    (void)arg;
    static char name[] = "kayte";
    char *argv[] = {name, NULL};
    kayte_main(1, argv);
    fflush(stdout);
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* On the last core: on dual-core chips, away from Wi-Fi / Bluetooth. */
    xTaskCreatePinnedToCore(kayte_task, "kayte", PICOKAYTE_STACK, NULL, 1, NULL, portNUM_PROCESSORS - 1);
}
