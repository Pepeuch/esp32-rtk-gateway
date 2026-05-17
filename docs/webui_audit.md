# WebUI / API Audit

Date: 2026-05-17

## Scope

Audit target:

- `www/index.html`
- `www/dashboard.html`
- `www/config.html`
- `www/advanced.html`
- `www/log.html`
- `www/app-lite.css`
- `www/app-lite.js`
- `www/c/*.js`
- `main/web_server.c`

Goal:

- inventory the current WebUI surface
- compare UI calls with firmware handlers actually registered
- identify dead or misleading controls
- identify remaining legacy assumptions
- define the exact rebuild plan before refactoring

This report captures the state of the repository before the full stabilization pass described in the user request.

## Pages

| Page | Current role | Notes |
| --- | --- | --- |
| `www/index.html` | root redirect | redirects immediately to `/dashboard.html` |
| `www/dashboard.html` | runtime dashboard | already split from config, but still depends on older shared modules and compatibility glue |
| `www/config.html` | persistent config + some runtime panels | NTRIP legacy cards are removed from active HTML, but the page still carries mixed responsibilities and old module assumptions |
| `www/advanced.html` | debug/developer tools | still contains page-local JS instead of a dedicated module |
| `www/log.html` | live logs | still contains page-local polling logic instead of a dedicated module |

## JS Modules

| File | Current use | Notes |
| --- | --- | --- |
| `www/app-lite.js` | mini DOM/AJAX/helper layer | custom helper, not third-party, but pages are still written in a jQuery-like style instead of plain module APIs |
| `www/c/config.js` | config form load/save and Wi-Fi scan | still a large mixed-responsibility file |
| `www/c/gnss.js` | GNSS summary, diagnostics, profiles, base actions, raw console | useful backend coverage, but mixed dashboard/config/advanced rendering paths |
| `www/c/lora.js` | LoRa panel rendering | currently renders pseudo-config style controls even though there is no save endpoint |
| `www/c/nav.js` | shared nav renderer | already shared, but naming/API need normalization |
| `www/c/ntrip.js` | dynamic NTRIP slots and runtime rendering | mostly aligned with firmware, but still mixes config/runtime/debug concerns |
| `www/c/runtime.js` | capabilities/status/system rendering | mixes platform, dashboard, config, and helper logic |

## CSS Files

| File | Current use | Notes |
| --- | --- | --- |
| `www/app-lite.css` | global styling | custom lightweight CSS, not Bootstrap itself; contains many Bootstrap-like utility names and is safe to keep as the single shared stylesheet |

## Current Page -> Asset Mapping

| Page | Assets currently loaded |
| --- | --- |
| `index.html` | inline redirect only |
| `dashboard.html` | `app-lite.js`, `app-lite.css`, `c/runtime.js`, `c/gnss.js`, `c/ntrip.js`, `c/lora.js`, `c/nav.js` |
| `config.html` | `app-lite.js`, `app-lite.css`, `c/config.js`, `c/gnss.js`, `c/ntrip.js`, `c/lora.js`, `c/runtime.js`, `c/nav.js` |
| `advanced.html` | `app-lite.js`, `app-lite.css`, `c/runtime.js`, `c/gnss.js`, `c/nav.js` |
| `log.html` | `app-lite.js`, `app-lite.css`, `c/nav.js` |

## API Endpoints Used By Current WebUI

Current UI calls found in `www/`:

- `/config`
- `/status`
- `/api/capabilities`
- `/api/gnss/status`
- `/api/gnss/satellites`
- `/api/gnss/diagnostics`
- `/api/gnss/base/status`
- `/api/gnss/base/start-survey`
- `/api/gnss/base/stop-survey`
- `/api/gnss/base/apply-fixed`
- `/api/gnss/base/clear`
- `/api/gnss/profiles`
- `/api/gnss/profile/apply`
- `/api/gnss/command`
- `/api/gnss/receiver/raw`
- `/api/gnss/detect`
- `/api/ntrip`
- `/api/ntrip/runtime`
- `/api/ntrip/restart`
- `/api/dev/fake-rtcm/start`
- `/api/dev/fake-rtcm/stop`
- `/api/dev/ntrip/mock`
- `/api/dev/ntrip/selftest/start`
- `/api/dev/ntrip/selftest/result`
- `/wifi/scan`
- `/log`

Current path problems:

- API path style is inconsistent across modules.
- Some code still uses page-local fetch wrappers instead of one shared API module.
- Inline page scripts still duplicate fetch logic.

## API Endpoints Implemented In `main/web_server.c`

Registered handlers found in firmware:

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

## Firmware Payloads Relevant To The Rebuild

### `/api/capabilities`

Backed by `capabilities_json_fill()` and currently exposes:

- `chip_family`
- `network_profile`
- `is_esp32`
- `is_esp32s3`
- `psram_available`
- `ethernet_supported`
- `ethernet_active`
- `wifi_only`
- `advanced_diagnostics`
- `safe_mode`
- `has_lora_radio`
- `lora_tx_enabled`
- `max_ntrip_slots`
- `configured_ntrip_slots`
- `device_role`
- `lora.*`
- `memory.*`

### `/status`

Backed by `status_get_handler()` and currently exposes:

- `uptime`
- `heap.*`
- `psram.*`
- `streams.*`
- `sockets[]`
- `wifi.ap.*`
- `wifi.sta.*`
- `active_socket_count`
- `max_socket_count`
- `qos.*`
- `capabilities.*`
- `ntrip.*`
- `gnss.*`
- `buffers.*`

### `/api/gnss/profiles`

Backed by `gnss_profiles_json_fill()` and currently exposes:

- current receiver profile state
- receiver baud
- NMEA rate
- RTCM output
- RTK timeout
- DGPS timeout
- constellation mask
- AGNSS enable
- signal mask
- supported profile list
- base config defaults

### `/api/gnss/base/status`

Backed by `gnss_base_status_json_fill()` and currently exposes:

- configured mode
- active profile
- receiver mode
- fixed/survey status
- coordinates
- survey targets/progress
- RTCM output
- `last_action_status`
- `disabled_reason`

### `/api/ntrip`

Backed by `ntrip_slots_json_fill()` and currently exposes:

- `max_slots`
- `configured_slots`
- `requested_enabled_slots`
- `slots[]` with:
  - index, id, role, name
  - implemented, enabled, running, allowed_by_platform
  - host, port, mountpoint, username, masked password
  - has_password, ntrip_version, use_tls
  - status, bytes, packets, reconnects, uptime
  - ringbuffer metrics
  - last HTTP code, stale, mock mode, last error, disabled reason

Important:

- `POST /api/ntrip` expects a full array of exactly `NTRIP_SLOT_COUNT` slots.
- Platform exposure is governed by `max_ntrip_slots`.

### `/api/ntrip/runtime`

Backed by `ntrip_runtime_info_json_fill()` and currently exposes:

- fake RTCM state
- safe mode
- active slot count
- heap/PSRAM snapshots
- ringbuffer totals
- dropped packets
- socket counts
- Ethernet/Wi-Fi readiness
- QoS state/reason

## Fields Displayed But Not Properly Backed Today

These are the main UI correctness issues still present before the rebuild:

1. LoRa pseudo-config controls
   - The current LoRa UI presents form-like fields.
   - There is no firmware save endpoint for LoRa settings.
   - This should be rendered read-only unless a real POST path exists.

2. LoRa runtime availability
   - Firmware exposes LoRa build capability through `/api/capabilities`.
   - Firmware does not currently expose a dedicated `lora_ready` or `init_failed` WebUI field.
   - The UI can reliably show “supported” and “TX enabled/disabled”, but not the real post-init radio state.

3. Page-local debug actions
   - Advanced page inline JS directly wires fake RTCM and self-test.
   - This works, but it bypasses a shared module contract and makes regressions easier.

4. Log page inline polling logic
   - Functional, but not modularized.

## Firmware Fields Available But Underused Or Not Displayed Cleanly

1. `/api/status` already includes:
   - `streams`
   - `buffers`
   - `sockets`
   - `wifi.ap`
   - `wifi.sta`
   - `qos`
   These should drive the dashboard and remove duplicated summary logic.

2. `/api/gnss/status` already includes:
   - `firmware`
   - `mode`
   - `parser_errors`
   - `command_queue_depth`
   - `command_busy`
   - `last_command_status`
   - `raw_buffer_*`
   These should be surfaced more consistently.

3. `/api/gnss/diagnostics` already includes:
   - high precision position
   - horizontal/vertical accuracy
   - relative position / heading
   - constellation diagnostic summaries

4. `/api/ntrip` and `/api/ntrip/runtime` already include:
   - `allowed_by_platform`
   - `requested_enabled_slots`
   - `active_slot_count`
   - ringbuffer totals
   - stale / reconnect / HTTP code / disabled reason

5. `/core_dump` and `/heap_info` exist and can be linked from Advanced.

## Legacy / Regression Risks Still Relevant

1. Current architecture is still patch-shaped.
   - Page-local scripts remain in `advanced.html` and `log.html`.
   - Shared logic is split across large files with overlapping responsibilities.

2. There is still too much state/render duplication.
   - runtime + GNSS + NTRIP each partly render dashboard/config/advanced concerns.

3. The old `/config` universe still exists in firmware.
   - Legacy NTRIP A/B keys remain in backend config storage and interfaces.
   - The guard in `config_post_handler()` now avoids syncing legacy NTRIP unless legacy keys are posted, which is good, but the UI should stop depending on that whole model.

4. `app-lite.js` is not a blocker, but the current WebUI still behaves like a mini jQuery app instead of a clean module-based embedded UI.

## Exact Planned Changes

### File structure

Refactor the WebUI to the requested target structure:

- `www/index.html`
- `www/dashboard.html`
- `www/config.html`
- `www/advanced.html`
- `www/log.html`
- `www/app-lite.css`
- `www/app-lite.js`
- `www/c/api.js`
- `www/c/nav.js`
- `www/c/runtime.js`
- `www/c/dashboard.js`
- `www/c/config.js`
- `www/c/gnss.js`
- `www/c/ntrip.js`
- `www/c/lora.js`
- `www/c/log.js`
- `www/c/advanced.js`

### Architecture

1. Create one shared namespace:
   - `window.WebUI`

2. Create one shared nav API only:
   - `window.WebUI.nav.render("dashboard" | "config" | "advanced" | "logs")`

3. Create one shared API layer only:
   - `WebUI.api.get(path)`
   - `WebUI.api.post(path, data)`
   - `WebUI.api.tryGet(path, fallback)`

4. Remove page-local fetch wrappers and move all polling/action logic into:
   - `dashboard.js`
   - `config.js`
   - `advanced.js`
   - `log.js`

### Page rebuild

1. `dashboard.html`
   - runtime only
   - no config forms
   - no debug actions

2. `config.html`
   - persistent config only
   - network config
   - GNSS profile/apply
   - base config/actions
   - dynamic NTRIP slots only
   - LoRa read-only unless a save endpoint exists

3. `advanced.html`
   - raw console
   - fake RTCM
   - NTRIP self-test
   - NTRIP mock tools
   - deep diagnostics
   - core dump / heap info links if available

4. `log.html`
   - shared nav
   - live logs
   - reconnect/refresh action if useful

### Hard deletions / normalizations

1. Remove all active legacy ConfigPage assumptions from page bootstrap and module bootstrap.

2. Remove all relative API calls.

3. Remove any UI control that does not map to:
   - `/config`
   - `/api/gnss/*`
   - `/api/ntrip*`
   - `/api/dev/*`
   - `/wifi/scan`
   - `/log`
   - `/core_dump`
   - `/heap_info`

4. Keep legacy backend config keys untouched unless needed, but isolate them from the active UI.

5. Keep `/config` NTRIP isolation intact:
   - general config save must not resync NTRIP unless legacy keys are submitted

### Static validation

Extend `scripts/check_webui_assets.sh` so it fails on:

- missing assets
- missing shared nav on dashboard/config/advanced/log
- `ConfigPage`
- `autoTab`
- `incarvr6`
- `esp32-ntrip`
- `legacy-ntrip-card`
- `legacy-config-card`
- literal `NTRIP server A`
- literal `NTRIP server B`
- relative API calls
- calls to endpoints not registered in `main/web_server.c`

### Small backend fixes if required

If needed during the rebuild:

- add a small status field for true LoRa runtime readiness if a safe existing signal is available
- otherwise keep LoRa strictly capability/read-only in UI text and do not pretend that runtime availability is known

## Acceptance Focus For This Rebuild

The rebuild should leave the repo with:

- one shared nav path
- one shared API path
- no page-local nav logic
- no `ConfigPage`
- no `autoTab`
- no active legacy A/B NTRIP model in HTML
- no dead LoRa save controls
- no missing assets
- no UI call to a non-existent endpoint
- a config page that renders the dynamic NTRIP slot count from firmware capabilities
