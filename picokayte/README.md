# PicoKayte: Kayte for ESP32 and Arduino

PicoKayte runs Kayte programs on microcontrollers. You write the program in Kayte (`.kayte` or `.kjs`), and `picokayte` turns it into an **Arduino sketch** or an **ESP-IDF project**. You then build and flash that with the board's usual tools.

```bash
picokayte/picokayte arduino picokayte/examples/blink.kayte          # → blink/blink.ino
arduino-cli compile --fqbn esp32:esp32:esp32 --upload --port /dev/cu.usbserial-0001 blink

picokayte/picokayte esp-idf picokayte/examples/blink.kayte          # → blink-idf/
cd blink-idf && idf.py set-target esp32 && idf.py -p /dev/cu.usbserial-0001 flash monitor
```

```kayte
CONST LED = 2

PinMode LED, OUTPUT
FOR i = 1 TO 5
    DigitalWrite LED, HIGH
    Delay 250
    DigitalWrite LED, LOW
    Delay 250
NEXT
PRINT "Done after " & Millis() & " ms"
```

## How it works

Nothing is interpreted on the board. `kayte --native` compiles the program to C. That C is built by the board's toolchain together with the Kayte native runtime (`source/native/kayte_native_rt.c`), which is the same runtime used for desktop `--native` executables, in its `KAYTE_EMBEDDED` mode. A small HAL connects the runtime to the board:

| | Arduino (`hal/arduino/picokayte_hal.cpp`) | ESP-IDF (`hal/esp-idf/picokayte_hal.c`) |
|---|---|---|
| `PRINT` / `INPUT` | `Serial`, 115200 baud (`--baud`) | the ESP-IDF console (UART0 or USB-Serial-JTAG) |
| Pins | `pinMode`, `digitalWrite`, `digitalRead` | GPIO driver |
| `ANALOGWRITE` | `analogWrite` | LEDC PWM: 8 bits, 5 kHz, a channel per pin |
| `ANALOGREAD` | `analogRead` | ADC oneshot driver, 12 dB attenuation (0 – ~3.1 V) |
| Runs in | `setup()`; `loop()` idles afterwards | a FreeRTOS task of its own (`--stack`, 16 KB) |

So the whole language works on the board: classes, arrays, functions and recursion, `TRY` / `CATCH`, strings and doubles. A program runs from start to `END`, like on the desktop. For a program that should run forever, use a loop (`DO … LOOP`).

## Generated files

`picokayte arduino PROGRAM [-o DIR]` writes a sketch directory, named after the program unless you pass `-o`:

```
blink/
  blink.ino             setup() runs the program, loop() idles
  kayte_program.c       the program, from kayte --native (KAYTE_EMBEDDED)
  kayte_rt.h            the runtime, #included by kayte_program.c
  picokayte.h           the HAL interface
  picokayte_hal.cpp     the Arduino HAL
  picokayte_config.h    --baud / --stack
```

`picokayte esp-idf PROGRAM [-o DIR]` writes an ESP-IDF project (`NAME-idf/`) with the same files in `main/`. It also writes a `CMakeLists.txt` for the project and one for `main/`, plus `sdkconfig.defaults`, which stops the task watchdog from flagging a busy loop.

Don't edit the generated files. Change the Kayte program and run `picokayte` again.

Options:

- `--fqbn FQBN [--port PORT]` (arduino): also compile with `arduino-cli`, and upload when you give a port.
- `--target CHIP [--port PORT]` (esp-idf): also build with `idf.py` for `esp32`, `esp32s3`, `esp32c3` …, and flash when you give a port.
- `--baud N`: the serial speed (Arduino; ESP-IDF uses its console setting).
- `--stack BYTES`: the stack for the program's task (ESP32).
- `KAYTE` and `KAYTE_NATIVE_RT`: where the compiler and the runtime are. By default `picokayte` uses this repository's `bin/kayte` (build it first, see the main README) and `source/native/`.

## Microcontroller built-ins

These work in both syntaxes. In `.kayte` files they're case-insensitive and can be written as statements (`DigitalWrite 2, HIGH`). In `.kjs` files they use Arduino's names (`digitalWrite(2, HIGH)`).

| Built-in | Does |
|---|---|
| `PINMODE pin, mode` | `INPUT`, `OUTPUT`, `INPUT_PULLUP`, `INPUT_PULLDOWN` (also as strings, `"output"`, or 0 – 3). An error if the board can't use the pin that way. |
| `DIGITALWRITE pin, level` | `HIGH` / `LOW`, or any value: true is HIGH. |
| `DIGITALREAD(pin)` | 1 or 0 |
| `ANALOGWRITE pin, duty` | PWM, duty 0 – 255 |
| `ANALOGREAD(pin)` | The ADC's raw reading: 0 – 4095 on ESP32, 0 – 1023 on AVR / most Arduinos. An error on a pin with no ADC (ESP32). |
| `DELAY ms` | Waits. Other tasks (Wi-Fi, USB) keep running. |
| `MILLIS()`, `MICROS()` | Time since the board started (on a desktop: since the program started) |

`HIGH`, `LOW`, `OUTPUT`, `INPUT_PULLUP` and `INPUT_PULLDOWN` are constants. A variable with the same name hides them. `INPUT` is a Kayte keyword, so in `.kayte` it can be used only as `PINMODE`'s mode.

Pins are numbered 0 – 255 by their GPIO / Arduino number. Names such as `A0` and `LED_BUILTIN` differ from board to board and aren't known to Kayte, so write the number (see the board's pinout).

### Trying programs on the desktop

The VM (`kayte --run`) and desktop `--native` / `--llvm` builds simulate the pins:

- a pin keeps what was written to it;
- an input reads 0, or 1 with a pull-up;
- `ANALOGREAD` gives 0.

Set `KAYTE_GPIO_TRACE=1` to log every pin change to stderr:

```bash
bin/kayte --native picokayte/examples/blink.kayte -o blink && KAYTE_GPIO_TRACE=1 ./blink
# [gpio] pinMode 2 OUTPUT
# Blinking pin 2
# [gpio] pin 2 = 1
# ...
```

## Examples

| | |
|---|---|
| [`blink.kayte`](examples/blink.kayte) | Blinks the on-board LED |
| [`button.kayte`](examples/button.kayte) | An LED follows a button with a pull-up, and presses are counted (ESP32: the BOOT button) |
| [`fade.kjs`](examples/fade.kjs) | PWM fading, in the JavaScript-like syntax |
| [`thermostat.kayte`](examples/thermostat.kayte) | Analog input, a class that smooths the readings, and a relay with hysteresis |
| [`guess.kayte`](examples/guess.kayte) | A number game with `INPUT` over the serial port, and `TRY` / `CATCH` |

## Boards

- **ESP32** (ESP32, -S2, -S3, -C3, -C6 …) is the main target. It works through the Arduino core (`esp32:esp32`, version 3.x; the examples build for ESP32, ESP32-S3 and ESP32-C3 with 3.3.12) or ESP-IDF 5.x.
- **Other 32-bit Arduino boards** (RP2040 / RP2350, SAMD, Uno R4, STM32, nRF52, ESP8266): the Arduino HAL uses only the standard Arduino API. Two caveats:
  - On ESP32 a program adds 15 – 35 KB of flash to an empty sketch (the C library is already there). Other cores also link in `printf` and the math library, and the runtime needs a few KB of RAM.
  - Cores that link newlib-nano without 64-bit `printf` support can print 64-bit whole numbers wrongly.
- **8-bit AVR** (Uno R3, Nano, Mega) isn't supported. The runtime uses 64-bit numbers and `malloc` throughout, so it doesn't fit in 2 – 8 KB of RAM.

## Limits on a microcontroller

- No `PROCESS`, `QT` / `QML` or forms: they're runtime errors, as on WebAssembly. File statements fail unless the board has a file system mounted (ESP-IDF: SPIFFS / LittleFS / FAT through VFS).
- Calls nest at most 256 deep instead of 10,000. To change the limit, define `K_MAX_CALL_DEPTH` at the top of `kayte_program.c`.
- An uncaught runtime error prints `Runtime Error: …` on the serial port, and the program stops there. Reset the board to start again.
- `TIMER`, `DATE$` and `TIME$` use the board's clock, which starts at 0 unless something sets it (e.g. SNTP).
