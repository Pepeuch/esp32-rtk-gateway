# ESP32 RTK Gateway

ESP32 RTK Gateway is a modular embedded RTK/NTRIP platform for ESP32-class hardware. It combines a supervised multi-slot NTRIP runtime, GNSS receiver abstraction, live diagnostics, hardware-aware capabilities, and a lightweight built-in web UI designed for real devices with tight memory and socket budgets.

Originally based on the ESP32 NTRIP DUO project by Nebojša Cvetković and later updated for ESP-IDF v5.x by dr. Kónya Sándor. This codebase keeps that lineage while evolving into a broader RTK/GNSS gateway platform.

## What It Is

The current firmware is focused on four things:

- reliable RTCM transport over Wi-Fi and Ethernet
- GNSS receiver visibility and diagnostics without requiring a host PC
- portable hardware support across multiple ESP32 families and Ethernet backends
- safe runtime behavior under memory, socket, and reconnect pressure

## Current Features

### Runtime Platform

- board and target abstraction for multiple ESP32 families
- capability-driven runtime limits for NTRIP slots and diagnostics features
- Ethernet and Wi-Fi support with runtime status reporting
- PSRAM-aware memory policy and buffer diagnostics
- QoS state management for graceful degradation under load

### NTRIP Runtime

- persistent 5-slot NTRIP configuration model
- supervised multi-slot runtime with per-slot state machine
- exponential reconnect handling and runtime statistics
- shared RTCM fanout path with bounded per-slot buffers
- runtime self-test and fake RTCM tools for validation without field hardware
- buffer high-water marks and dropped packet counters

### GNSS Runtime

- generic receiver abstraction independent from NTRIP transport
- passive UART observation with safe boot when no receiver is attached
- Unicore-oriented profile manager and async command queue
- GNSS diagnostics API with fix, RTK, RTCM, AGC, antenna, jamming, and parser status
- aggregated satellite table with CN0 and constellation summaries
- preliminary u-blox UBX support for detection and telemetry

### Web Interface

- lightweight dashboard as default entry page
- separate config, advanced, log, and setup pages to reduce socket pressure
- vanilla JavaScript UI with no heavy framework dependency
- live GNSS, NTRIP, memory, and QoS visibility from the device itself

## Architecture Overview

The firmware is structured around a few independent layers:

1. Hardware layer
   - compile-time board and target selection
   - board capabilities and Ethernet backend selection

2. Network layer
   - Wi-Fi and Ethernet orchestration
   - runtime network state and capability reporting

3. NTRIP layer
   - persistent slot configuration
   - supervised runtime tasks and RTCM fanout
   - runtime QoS and self-test support

4. GNSS layer
   - receiver abstraction
   - receiver diagnostics parsing
   - receiver command/profile management

5. Web/API layer
   - lightweight embedded pages
   - JSON APIs for status, diagnostics, configuration, and test tools

This separation is meant to keep transport, GNSS logic, UI, and hardware support evolvable without forcing large rewrites.

## Supported Architectures

Current support path:

- ESP32
- ESP32-S3
- ESP32-C3
- ESP32-C6

Default target:

- Waveshare ESP32-S3 ETH

## Ethernet Backends

Supported Ethernet backend path:

- W5500 over SPI
- LAN8720 over RMII

The current hardware abstraction uses compile-time board and target selection under `main/config/`.

Important files:

- `main/config/board_config.h`
- `main/config/board_caps.h`
- `main/config/boards/*.h`
- `main/config/targets/*.h`

Backend selection is driven by board configuration rather than runtime probing.

## GNSS Support

Current receiver support level:

- generic NMEA diagnostics
- Unicore N4 ASCII diagnostics and profile workflow
- u-blox UBX detection and telemetry parsing

Current integrated profiles:

- `none`
- `diagnostics_only`
- `rover_basic`
- `rover_rtk`
- `base_fixed`
- `base_survey`

The receiver layer is intentionally conservative: detection and diagnostics come first, while active receiver configuration is added gradually and defensively.

## Stability and Resource Management

The firmware includes dedicated mechanisms to stay usable on constrained hardware:

- bounded ringbuffers and bounded config models
- PSRAM-aware allocation policy with internal RAM fallback
- chunked JSON for larger web responses
- active socket visibility via status APIs
- QoS states: `normal`, `degraded`, `critical`
- optional features reduced or disabled automatically under critical load

This is especially important for keeping RTCM transport stable while the embedded UI is open.

## Web API Highlights

Core endpoints:

- `/api/status`
- `/api/capabilities`

NTRIP endpoints:

- `/api/ntrip`
- `/api/ntrip/runtime`
- `/api/ntrip/restart`

GNSS endpoints:

- `/api/gnss/status`
- `/api/gnss/satellites`
- `/api/gnss/diagnostics`
- `/api/gnss/profiles`
- `/api/gnss/profile/apply`
- `/api/gnss/command`
- `/api/gnss/receiver/raw`
- `/api/gnss/base/status`

Development and validation endpoints are also present for fake RTCM, runtime mocks, and self-tests.

## Web UI Pages

- `/` or `/dashboard.html`
  - lightweight live status view
- `/config.html`
  - network, NTRIP, GNSS, and base configuration
- `/advanced.html`
  - raw console, self-test, fake RTCM, deep diagnostics
- `/log.html`
  - device log view
- `/setup.html`
  - hardware setup wizard and capability guidance

## Default UART Pinout

Default UART configuration:

- TX: GPIO1
- RX: GPIO3

This matches the historical WT32-ETH01 default.

## Roadmap

Near-term direction:

- stronger field validation of Unicore profile workflows
- deeper u-blox support without sacrificing safety
- more complete base station workflow coverage
- continued QoS and memory hardening before broader beta use
- broader board coverage on top of the current abstraction layer

Longer-term exploration:

- additional radio and transport options
- extended telemetry and logging backends
- more workflow-oriented field tools for rover and base deployments

## Legacy Notes

This project has evolved through several phases:

- initial ESP32 NTRIP gateway firmware with Wi-Fi and dual-server behavior
- ESP-IDF v5 migration and WT32-ETH01 maintenance
- universal board and Ethernet architecture work
- modular NTRIP runtime and GNSS abstraction redesign
- lightweight web UI and QoS/memory hardening

Older release notes are preserved below for historical context, but the sections above describe the current platform more accurately.

## Release History

### v0.5.0-dev
- modular NTRIP runtime and slot supervision
- GNSS abstraction and diagnostics APIs
- receiver profile manager and raw console
- fake RTCM and embedded self-test framework
- lightweight multi-page web UI
- QoS and memory/runtime visibility

### v0.4.1a
- universal board and target architecture groundwork
- Ethernet and Wi-Fi refactor direction
- broader ESP32 family support path

### v0.4
- ESP-IDF 5.5.3 migration
- Waveshare ESP32-S3 ETH support

### v0.3
- ESP-IDF 5.2.3 migration
- WT32-ETH01 support
- NTRIP caster path restored

### v0.2
- socket client and server support
- improved stability over the earliest branch

### v0.1
- Wi-Fi station and hotspot support
- embedded web interface
- UART configuration
- two NTRIP servers

## Credits

- Original ESP32 NTRIP DUO work by Nebojša Cvetković
- ESP-IDF v5 modernization by dr. Kónya Sándor
- current RTK Gateway direction focused on modular runtime, diagnostics, portability, and embedded observability

## Project Status

The project is still evolving quickly. APIs, configuration layouts, and receiver workflows may continue to change between development versions while the platform stabilizes.

Testing, validation feedback, and hardware reports are very welcome.
