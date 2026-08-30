# ESP32 toolchain review

Checked on 2026-08-30. Sources are first-party PlatformIO, Espressif, and Arduino docs and repos.

## Short answer

PlatformIO is still a good tool, but it is no longer the clear default for all ESP32 work.

For this repo, keep PlatformIO for now. The project is a small Arduino 2.x app for the original ESP32 DevKit, and its build is already described in one `platformio.ini` file. A tool change would add work without fixing a current problem.

The catch is Arduino support. PlatformIO's current ESP32 platform, 7.0.1, still ships Arduino-ESP32 2.0.17 on ESP-IDF 4.4.7. Espressif's current stable Arduino core is 3.3.11 on ESP-IDF 5.5.5. PlatformIO is a sound choice when Arduino 2.x is enough. Arduino CLI or Arduino IDE 2 is now the safer first-party choice when a project needs current Arduino-ESP32.

For a new project that needs direct access to current Espressif features, new chips, `menuconfig`, tracing, or deep debug tools, use ESP-IDF with `idf.py` and Espressif's VS Code extension.

## What is still good about PlatformIO

- It is maintained. PlatformIO Core 6.1.19 came out on 2026-02-04. ESP32 platform releases 6.13.0, 7.0.0, and 7.0.1 followed on 2026-02-26, 2026-04-30, and 2026-05-12. [PlatformIO Core releases](https://github.com/platformio/platformio-core/releases), [ESP32 platform releases](https://github.com/platformio/platform-espressif32/releases)
- Its ESP-IDF path stays close to Espressif. ESP32 platform 7.0.1 ships ESP-IDF 6.0.1. Espressif released 6.1 on 2026-08-27, so PlatformIO trails the newest release but has not stalled. [PlatformIO 7.0.1](https://github.com/platformio/platform-espressif32/releases/tag/v7.0.1), [ESP-IDF releases](https://github.com/espressif/esp-idf/releases)
- `platformio.ini` gives one place for boards, frameworks, build flags, upload settings, and libraries. PlatformIO can resolve Registry, Git, archive, and local dependencies, isolates them by project environment, and installs them during build, test, or debug. [PlatformIO dependency management](https://docs.platformio.org/en/latest/librarymanager/dependencies.html)
- JTAG setup is still a strong point. PlatformIO has one config model for ESP-Prog, CMSIS-DAP, J-Link, ESP USB Bridge, and other probes, with VS Code and CLI support. [PlatformIO debugging](https://docs.platformio.org/en/latest/plus/debugging.html), [ESP-Prog setup](https://docs.platformio.org/en/latest/plus/debug-tools/esp-prog.html)
- CI is simple. The official guide uses normal `pio run` builds in GitHub Actions and supports package caching. [PlatformIO GitHub Actions guide](https://docs.platformio.org/en/latest/integration/ci/github-actions.html)

## The Arduino gap

PlatformIO's official ESP32 package has not moved past Arduino-ESP32 2.0.17. Its open Arduino 3.x support issue records the cause. An Espressif maintainer said Espressif would not renew its PlatformIO service deal and that official PlatformIO support for Arduino 3.x was unlikely. Both projects would keep the 2.x path working, while most new Arduino work moved to 3.x. [PlatformIO issue 1225](https://github.com/platformio/platform-espressif32/issues/1225)

That gap now spans a major ESP-IDF base change. Espressif's Arduino 2.x to 3.0 guide lists breaks in ADC, BLE, I2S, LEDC, timers, UART, Wi-Fi, and other APIs. [Espressif migration guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html)

This repo does not use the main APIs called out in that guide. It uses Arduino basics, SPI, SD, a hardware UART, and TinyGPSPlus. A move to Arduino 3.x looks modest, but only a build, flash, GPS fix, and SD write test can prove it.

## Current choices

| Choice | Framework updates | Dependencies | Debug | CI | Cost for this repo |
| --- | --- | --- | --- | --- | --- |
| PlatformIO | Current ESP-IDF support, but official Arduino stays at 2.0.17 | Strong `lib_deps` flow, project-local installs, version ranges or pins | Easy probe config and VS Code support | Simple `pio run` jobs | Lowest |
| ESP-IDF CLI and VS Code | First-party release path. ESP-IDF 6.1 is current | Component Registry, Git and local sources through `idf_component.yml`; exact versions in `dependencies.lock` | First-party OpenOCD and GDB, peripheral view, core dumps, tracing | First-party `idf-ci`, GitHub Actions, GitLab, build matrices, and tests | High for a direct rewrite; medium with Arduino as a component |
| Arduino IDE 2 and Arduino CLI | First-party path to Arduino-ESP32 3.3.11 | Boards and Library Manager; CLI build profiles can pin the board core and libraries in `sketch.yaml` | CLI has a debug command, but ESP32 debug work is less joined-up than PlatformIO or ESP-IDF | CLI works in CI, but the repo must define its setup and build commands | Medium |

ESP-IDF's Component Manager resolves dependencies and writes their exact versions to `dependencies.lock`. Its VS Code extension covers build, flash, monitor, `menuconfig`, unit tests, OpenOCD/GDB debug, core dumps, tracing, and component installs. [Component Manager lock file](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/dependencies_lock.html), [ESP-IDF VS Code extension](https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/), [ESP-IDF debug guide](https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/debugproject.html), [idf-ci](https://docs.espressif.com/projects/idf-ci/en/latest/)

Arduino IDE 2.3.10 and Arduino CLI 1.5.1 both shipped in June 2026. Arduino IDE uses the CLI for builds and uploads. Arduino CLI profiles can pin the core, board, and libraries. [Arduino IDE 2.3.10](https://github.com/arduino/arduino-ide/releases/tag/2.3.10), [Arduino CLI 1.5.1](https://github.com/arduino/arduino-cli/releases/tag/v1.5.1), [Arduino CLI build profiles](https://docs.arduino.cc/arduino-cli/sketch-project-file)

Espressif also supports Arduino 3.3.11 as an ESP-IDF component. This keeps Arduino APIs while adding the ESP-IDF build, config, and debug tools. Espressif calls it an advanced-user path. [Arduino as an ESP-IDF component](https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html)

## Recommendation for this repo

1. Keep PlatformIO unless there is a concrete need for Arduino 3.x or a newer ESP32 chip.
2. Pin `platform = espressif32 @ 7.0.1`. The current unpinned `platform = espressif32` can change the compiler and framework after a clean install. PlatformIO itself recommends a platform pin. [PlatformIO ESP32 repo](https://github.com/platformio/platform-espressif32)
3. For a fixed maintenance build, consider changing TinyGPSPlus from `^1.0.3` to an exact tested version. PlatformIO does not update dependencies on its own, but a new install can select a newer version allowed by the range. [PlatformIO dependency rules](https://docs.platformio.org/en/latest/librarymanager/dependencies.html)
4. Add one CI build after the toolchain pin. That will catch setup drift even when no board is attached.
5. If current Arduino support becomes important, test an Arduino CLI branch before replacing PlatformIO. Arduino sketches need a primary `.ino` file whose name matches the sketch folder. The present `src/main.cpp` can stay as C++ source, but the repo needs that entry file plus a pinned `sketch.yaml` profile. [Arduino sketch layout](https://docs.arduino.cc/arduino-cli/sketch-specification)

The practical answer is: PlatformIO remains the best fit for maintaining this project today. I would not choose its official Arduino path for a new ESP32 Arduino project that needs current core releases.
