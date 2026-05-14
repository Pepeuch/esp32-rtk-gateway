# ESP32 NTRIP DUO / RTK Gateway

ESP32-based multi-network RTK/NTRIP gateway firmware supporting Ethernet, Wi-Fi, GNSS diagnostics, receiver management and future multi-radio extensions.

Originally based on the ESP32 NTRIP DUO project by Nebojša Cvetković and later updated for ESP-IDF v5.x by dr. Kónya Sándor.

---

# Features

## Network Runtime
- Modular network stack
- Ethernet / Wi-Fi separation
- Dynamic network state manager
- Captive portal support
- Improved reconnect handling
- Runtime diagnostics
- Improved logging/debugging
- Better runtime stability

## NTRIP Runtime
- Modular NTRIP runtime
- Multi-slot runtime supervision
- Slot state machine
- Runtime statistics API
- Fake RTCM generator
- Runtime self-test system
- Improved reconnect stability
- Reduced blocking socket operations
- Dynamic slot monitoring
- Better watchdog behavior
- Runtime diagnostics endpoints

## GNSS Runtime
- Generic receiver abstraction layer
- Passive UART observation
- Receiver auto-detection
- GNSS profile manager
- Satellite aggregation table
- RTCM activity monitoring
- RTK diagnostics
- CN0 statistics
- Antenna diagnostics
- Jamming diagnostics
- AGC monitoring
- Raw receiver console
- Asynchronous command queue
- Safe boot behavior without GNSS connected

## Supported GNSS Receivers
### Current
- Unicore N4 (preliminary support)
- Generic NMEA receivers
- Initial u-blox detection support

### Planned
- Full u-blox UBX support
- F9P/F9R advanced support
- High precision UBX diagnostics

---

# Supported Architectures

- ESP32
- ESP32-S3
- ESP32-C3
- ESP32-C6

---

# Supported Ethernet Controllers

- W5500 (SPI)
- LAN8720 (RMII)

Board-specific configuration is handled through:

```c
config/board_*.h

Ethernet type is selected using:

```c
BOARD_ETHERNET_TYPE
```

---

# Web API

## General
- `/api/status`
- `/api/capabilities`

## NTRIP
- `/api/ntrip`
- `/api/ntrip/runtime`
- `/api/dev/ntrip/selftest/start`
- `/api/dev/ntrip/selftest/result`
- `/api/dev/fake-rtcm/start`
- `/api/dev/fake-rtcm/stop`

## GNSS
- `/api/gnss/status`
- `/api/gnss/satellites`
- `/api/gnss/diagnostics`
- `/api/gnss/profiles`
- `/api/gnss/profile/apply`
- `/api/gnss/command`
- `/api/gnss/receiver/raw`
- `/api/gnss/detect`

---

# GNSS Profiles

Integrated receiver profiles:

- `none`
- `diagnostics_only`
- `rover_basic`
- `rover_rtk`
- `base_fixed`
- `base_survey`

---

# Web Interface

## Current Features
- GNSS status card
- Satellite live table
- RTK quality indicators
- Receiver profile selection
- Receiver command interface
- Raw GNSS console
- Runtime diagnostics
- NTRIP monitoring
- Better no-GNSS handling

---

# Current Status

The firmware is actively under development.

Current focus:
- GNSS diagnostics
- Receiver management
- NTRIP runtime stability
- Unicore support
- Future u-blox support
- Bluetooth mobile integration
- Survey-in / base station workflows

---

# Planned Features

## PR7 - Advanced GNSS Diagnostics
- Real-time satellite diagnostics
- CN0 graphs
- Constellation statistics
- RTCM quality analysis
- GNSS timeline graphs
- Advanced receiver monitoring

## PR8 - Survey-In / Base Mode
- Survey-in management
- Fixed base workflows
- RTCM base configuration
- Persistent base coordinates
- Base station profiles

## PR9 - u-blox Support
- UBX parser
- NAV-PVT
- NAV-SAT
- NAV-HPPOSLLH
- NAV-RELPOSNED
- MON-VER
- F9P/F9R support
- High precision configuration

## PR10 - Hardware Setup Wizard
- Visual hardware selection
- GNSS configuration
- UART mapping
- Rover/base presets
- NTRIP presets
- Ethernet/Wi-Fi presets
- Automatic configuration generation

## PR11 - Web UI split and lightweight dashboard
/dashboard
- état GNSS
- fix / RTK
- satellites
- CN0
- NTRIP runtime
- résumé réseau

/config
- Wi-Fi / Ethernet
- GNSS profile
- NTRIP slots
- base / rover mode

/advanced
- logs
- raw GNSS console
- self-tests
- fake RTCM
- commandes manuelles
- diagnostics profonds
---

# Future Features

- Bluetooth LE support (Android/iOS)
- Rover field measurement mode
- LoRa support
- MQTT monitoring
- PSRAM optimized buffers
- Multi-radio support
- Enhanced mobile support

---

# Default UART Pinout

Default UART configuration:

- TX: GPIO1
- RX: GPIO3

(Default for WT32-ETH01)

---

# Release History

## UPDATE 2026-05-14
# Release v0.5.0-dev

Major architecture overhaul and GNSS runtime redesign.

### Added
- Modular NTRIP runtime
- GNSS abstraction layer
- GNSS diagnostics
- Satellite aggregation
- Runtime APIs
- Receiver profiles
- Fake RTCM generator
- Self-test framework
- Raw GNSS console
- Asynchronous GNSS command queue
- Better watchdog handling
- Improved reconnect logic
- Improved diagnostics UI

---

## UPDATE 2026-03-16
# Release v0.4.1a

### Added
- Modular network stack
- Ethernet / Wi-Fi separation
- network_state manager
- Improved logging/debugging
- General runtime improvements

### Supported Architectures
- ESP32 / ETH
- ESP32-S3 / ETH
- ESP32-C3
- ESP32-C6

### Supported Ethernet Controllers
- W5500 (SPI)
- LAN8720 (RMII)

---

## UPDATE 2026-03-15
# Release v0.4

### Added
- ESP-IDF updated from v5.2.3 to v5.5.3
- Waveshare ESP32-S3 ETH support

---

## UPDATE 2025-02-15
# Release v0.3

### Added
- ESP-IDF v5.2.3 migration
- WT32-ETH01 support
- NTRIP caster functionality restored

---

## UPDATE 2025-02-14
# Release v0.2

### Added
- Socket client/server
- Improved stability
- Button component removal

---

## UPDATE 2025-02-13
# Release v0.1

### Added
- WiFi station
- WiFi hotspot
- Web interface
- UART configuration
- Two NTRIP servers

---

# Notes

This project is still evolving rapidly and APIs/configuration formats may change between development releases.

Testing and feedback are welcome.


# ESP32 NTRIP Duo WT32-ETH01 

This version had been updated from ESP-IDF 4.1 to v5.2.3 and adapted to the WT32-ETH01 ESP32 Module.


UPDATE 2025-02-15:
[Release v0.3](https://github.com/sandorkonya/esp32-ntrip-DUO/releases/tag/v0.3) has the:

- NTRIP Caster functionality back (not fully tested though, can be buggy)


UPDATE 2025-02-14:
[Release v0.2](https://github.com/sandorkonya/esp32-ntrip-DUO/releases/tag/v0.2) has the:

 - socket client & server
 - increased stability by removing the button component

[Release v0.1](https://github.com/sandorkonya/esp32-ntrip-DUO/releases/tag/v0.1) has the features:

- WiFi Station
- WiFi Hotspot
- Web Interface
- UART configuration
- Two NTRIP Servers

## Pinout
By default it is set for UART0 TX gpio1, RX gpio3 (also default for the WT32-ETH01 module)
