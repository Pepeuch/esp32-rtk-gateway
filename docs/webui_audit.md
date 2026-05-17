# WebUI / API Audit

Date: 2026-05-17

## Scope

Audit target:

- `www/index.html`
- `www/dashboard.html`
- `www/config.html`
- `www/advanced.html`
- `www/log.html`
- `www/c/*.js`
- `main/web_server.c`

Goal:

- inventory the current WebUI pages, JS modules, and API calls
- compare WebUI calls with firmware handlers actually registered
- identify legacy paths, stale UI, duplicated logic, and NTRIP slot regressions

This report captures the state observed before the remediation changes in the same patch series.

## 1. Pages

| Page | Purpose in current source | Notes |
| --- | --- | --- |
| `index.html` | root redirect | immediate redirect to `/dashboard.html`; no nav |
| `dashboard.html` | runtime dashboard | uses shared nav and modular runtime/GNSS/NTRIP/LoRa renderers |
| `config.html` | mixed config + runtime + legacy controls | shared nav exists, but page still mixes new GNSS/NTRIP/LoRa modules with large legacy form blocks |
| `advanced.html` | developer/debug tools | shared nav exists; some page-local runtime rendering remains inline |
| `log.html` | live log stream | shared nav exists |

## 2. JS Modules

| File | Used by | Current role |
| --- | --- | --- |
| `www/app-lite.js` | dashboard, config, advanced, log | mini DOM/AJAX/helper layer replacing jQuery/Bootstrap behaviors |
| `www/c/nav.js` | dashboard, config, advanced, log | shared top navigation renderer |
| `www/c/runtime.js` | dashboard, config, advanced | capabilities, status, Wi-Fi status, QoS, stream stats, dashboard summaries |
| `www/c/gnss.js` | dashboard, config, advanced | GNSS summary, diagnostics, satellites, base status, profiles, raw console, commands |
| `www/c/ntrip.js` | dashboard, config | NTRIP slot editor, runtime summary, dashboard slot summary, restart, mock/fake RTCM hooks |
| `www/c/lora.js` | dashboard, config | LoRa status/config preview panel |
| `www/c/config.js` | config | legacy form load/save, Wi-Fi scan, disable-if logic, role-based visibility |

## 3. Page -> JS Mapping

| Page | JS files |
| --- | --- |
| `advanced.html` | `app-lite.js`, `c/runtime.js`, `c/gnss.js`, `c/nav.js` |
| `config.html` | `app-lite.js`, `c/config.js`, `c/gnss.js`, `c/ntrip.js`, `c/lora.js`, `c/runtime.js`, `c/nav.js` |
| `dashboard.html` | `app-lite.js`, `c/runtime.js`, `c/gnss.js`, `c/ntrip.js`, `c/lora.js`, `c/nav.js` |
| `log.html` | `app-lite.js`, `c/nav.js` |
| `index.html` | none |

## 4. WebUI API Calls

Normalized endpoint list used by the WebUI:

| WebUI caller | Endpoints |
| --- | --- |
| `www/c/runtime.js` | `/api/capabilities`, `/status` |
| `www/c/config.js` | `/config`, `/wifi/scan` |
| `www/c/gnss.js` | `/api/gnss/diagnostics`, `/api/gnss/satellites`, `/api/gnss/base/status`, `/api/gnss/receiver/raw`, `/api/gnss/profiles`, `/api/gnss/detect`, `/api/gnss/profile/apply`, `/api/gnss/command`, `/api/gnss/base/start-survey`, `/api/gnss/base/stop-survey`, `/api/gnss/base/apply-fixed`, `/api/gnss/base/clear` |
| `www/c/ntrip.js` | `/api/ntrip/runtime`, `/api/ntrip`, `/api/ntrip/restart`, `/api/dev/fake-rtcm/start`, `/api/dev/fake-rtcm/stop`, `/api/dev/ntrip/mock` |
| `www/advanced.html` inline script | `/status`, `/api/ntrip/runtime`, `/api/dev/fake-rtcm/start`, `/api/dev/fake-rtcm/stop`, `/api/dev/ntrip/selftest/start`, `/api/dev/ntrip/selftest/result` |
| `www/log.html` inline script | `/log` |

Notes:

- Some calls use absolute paths (`/api/...`, `/status`).
- Some calls use relative paths (`api/...`, `status`, `config`, `wifi/scan`, `log`).
- Those relative paths currently work only because every page is served from the root path. They are valid today but fragile.

## 5. Firmware Endpoints Implemented

Handlers registered in `main/web_server.c`:

- `/config` GET/POST
- `/status` GET
- `/api/config` GET/POST
- `/api/status` GET
- `/api/capabilities` GET
- `/api/gnss/status` GET
- `/api/gnss/satellites` GET
- `/api/gnss/diagnostics` GET
- `/api/gnss/base/status` GET
- `/api/gnss/base/start-survey` POST
- `/api/gnss/base/stop-survey` POST
- `/api/gnss/base/apply-fixed` POST
- `/api/gnss/base/clear` POST
- `/api/gnss/profiles` GET
- `/api/gnss/profile/apply` POST
- `/api/gnss/command` POST
- `/api/gnss/receiver/raw` GET
- `/api/gnss/capabilities` GET
- `/api/gnss/detect` POST
- `/api/ntrip` GET/POST
- `/api/ntrip/runtime` GET
- `/api/ntrip/restart` POST
- `/api/ntrip/enable/*` POST
- `/api/ntrip/disable/*` POST
- `/api/dev/fake-rtcm/start` POST
- `/api/dev/fake-rtcm/stop` POST
- `/api/dev/ntrip/mock` POST
- `/api/dev/ntrip/selftest/start` POST
- `/api/dev/ntrip/selftest/result` GET
- `/log` GET
- `/core_dump` GET
- `/heap_info` GET
- `/wifi/scan` GET
- `/*` GET static files

## 6. WebUI vs Firmware Comparison

### Missing endpoints

None found for the currently referenced WebUI endpoints.

### Implemented but currently unused by WebUI

- `/api/status`
- `/api/config`
- `/api/gnss/status`
- `/api/gnss/capabilities`
- `/api/ntrip/enable/*`
- `/api/ntrip/disable/*`
- `/core_dump`
- `/heap_info`

### Wrong paths

No currently broken path mismatch was found in source, but path style is inconsistent:

- absolute: `/api/...`, `/status`
- relative: `api/...`, `status`, `config`, `wifi/scan`, `log`

This is functional today but should be normalized.

## 7. NTRIP Slot Audit

### Firmware model

- `NTRIP_SLOT_COUNT` is `5`
- runtime descriptors exist for `slot0` through `slot4`
- default names are `NTRIP Server A` through `NTRIP Server E`
- `ntrip_post_handler()` requires a full array of exactly 5 slots
- `ntrip_slots_max_allowed()` returns `capabilities.max_ntrip_slots`

### Capabilities source

`main/capabilities.c` computes:

- `max_ntrip_slots = BOARD_NTRIP_MAX_SLOTS_PSRAM` when PSRAM is available
- otherwise `BOARD_NTRIP_MAX_SLOTS_NO_PSRAM`

For `board_waveshare_esp32s3_eth.h`:

- no PSRAM: `4`
- PSRAM: `5`

Implication:

- a Waveshare ESP32-S3 ETH build reports `5` only when the runtime sees PSRAM as available
- if the dashboard ever reports `4`, the current source would do that only when PSRAM is not available at runtime or the wrong profile/board is flashed

### Frontend dynamic editor

`www/c/ntrip.js` already supports dynamic slot rendering:

- labels A-E
- capability-driven slot count
- full 5-slot payload save
- fallback to 5 on ESP32-S3 / unknown capability state

### Why Config still looks like “2 servers”

There are two separate causes:

1. `config.html` still contains legacy “NTRIP server A” and “NTRIP server B” cards after the new dynamic editor.
2. `config_post_handler()` still calls `ntrip_slots_sync_from_legacy()` after every `/config` save, and that sync reads only slots `0` and `1` from legacy config keys.

That means the page still exposes two competing NTRIP architectures:

- new 5-slot API: `/api/ntrip`
- old 2-slot config form: `/config` + legacy keys

This is the main structural source of the recurring NTRIP regressions.

## 8. Findings

### Critical

1. `config.html` still ships both the new 5-slot NTRIP editor and the old A/B-only NTRIP cards.
   - Dynamic editor root: `#ntrip-slot-editor`
   - Legacy cards still present: `NTRIP server A`, `NTRIP server B`
   - User-visible result: the page still reads like a 2-slot UI even when firmware supports 5

2. `/config` save still has a legacy NTRIP side effect.
   - `config_post_handler()` unconditionally calls `ntrip_slots_sync_from_legacy()`
   - `ntrip_slots_sync_from_legacy()` only reads slots `0` and `1`
   - This keeps the old 2-slot model alive and can reintroduce drift between the general config form and `/api/ntrip`

### High

3. `config.js` role visibility is coupled to the wrong CSS class.
   - `applyRoleVisibility()` toggles `.ntrip-config-card`
   - That class is used by unrelated cards such as Wi-Fi, Admin, Socket server
   - A base/rover role switch can therefore hide unrelated config sections

4. `config.html` still includes developer/debug actions that belong on Advanced.
   - Fake RTCM controls are embedded in the config NTRIP card
   - Each slot card also exposes mock-mode controls
   - These are debug/runtime tools, not persistent config

5. GNSS profile status is sourced inconsistently.
   - The current profile label shown in the UI is refreshed mainly through `/api/gnss/receiver/raw`
   - `/api/gnss/status` and `/api/gnss/profiles` already expose profile state directly
   - If raw-console polling is skipped, delayed, or QoS-rejected, the visible profile state can look wrong or stale

### Medium

6. The LoRa panel on Config is not a real config path.
   - `www/c/lora.js` renders editable controls
   - no matching save endpoint exists
   - the panel currently behaves as a build/runtime preview, not persisted configuration

7. `advanced.html` still contains page-local runtime rendering logic instead of using only shared renderers.
   - shared nav is used
   - shared GNSS module is used
   - but runtime summary and self-test rendering remain inline in the page

8. Utility logic is duplicated between `config.js` and `runtime.js`.
   - shared helpers are redefined defensively in both files
   - not immediately broken, but it increases regression risk

9. API path style is inconsistent.
   - current root layout masks the problem
   - future path changes or sub-path hosting would break relative calls first

### Low

10. No legacy `incarvr6`, `esp32-ntrip`, `releases_api_url`, or `releases_html_url` references were found in the current repo scan.

11. All four main user pages already use the shared nav renderer.
   - `dashboard.html`
   - `config.html`
   - `advanced.html`
   - `log.html`

12. `index.html` is intentionally a redirect-only page and does not render nav.

## 9. Fields Displayed But Not Backed

Current UI fields that are not backed by a persistence/save path:

- LoRa editable controls in `www/c/lora.js`
  - role
  - region
  - chip family
  - frequency
  - TX power
  - radio profile
  - RTCM profile

Current UI fields that are only partially meaningful:

- NTRIP `Use TLS (future)`
  - stored by `/api/ntrip`
  - not presented as an active transport feature in the rest of the UI

## 10. Firmware Fields Available But Not Surfaced Well

Useful firmware fields currently underused in the UI:

- GNSS:
  - `profile`
  - `profile_pending`
  - `firmware`
  - `mode`
  - `last_command_status`
- capabilities:
  - memory totals/minimums
- Wi-Fi:
  - IPv6 addresses
- NTRIP:
  - `max_slots`
  - `requested_enabled_slots`
  - `allowed_by_platform`
- optional diagnostics endpoints:
  - `/api/gnss/capabilities`
  - `/heap_info`
  - `/core_dump`

## 11. Recommended Fix Set

Minimal targeted changes to make the WebUI match the firmware:

1. Remove legacy NTRIP cards from the visible Config UI.
2. Make `/config` -> legacy NTRIP sync conditional, or remove it from the normal save path.
3. Keep Config focused on:
   - network basic
   - GNSS basic/profile
   - dynamic NTRIP slots
   - LoRa status/config preview with honest labeling
   - save/reboot
4. Move Fake RTCM and mock-mode controls to Advanced only.
5. Source GNSS profile display from `/api/gnss/profiles` and/or `/api/gnss/status`, not only raw console polling.
6. Normalize all WebUI API calls to absolute `/...` paths.
7. Add a static checker for:
   - missing assets
   - unknown endpoints
   - missing shared nav on main pages
   - reintroduced legacy strings
