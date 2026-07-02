# Changelog

All notable changes to `jusiai-assistant` on the `headless` branch. Format
loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Dates are China Standard Time. Formal tracking begins with this entry; older
history is in `git log`.

## [Unreleased] — 2026-07-01 → 07-02

Two days of work turning the headless RV1126B port from "boots and dials"
into "product-shippable". Highlights: runtime-reconfigurable AI agent
settings via HTTP, single-request room+agent bootstrap, SoC-derived stable
`device_id`, LAN-reachable control API out of the box, and full UI code
removal.

### Added
- **Runtime AI-agent config control API** — `GET /config` and `POST /config`
  let a phone or remote change `profile_code` / `voice` / `prompt_id`
  mid-call. The worker auto-restarts the LiveKit session so the new
  configuration takes effect immediately without a full device restart.
  Persists to `~/.config/jusiai-assistant/agent_config.json`; the runtime
  layer sits between the config file and env / CLI so operator overrides
  always win on next boot. (`6b225f1`)
- **Self-heal guardrail for a poisoned runtime config** — if `autostart`
  triggers a `start-chat-bot` request the backend rejects with HTTP 4xx
  (e.g. a `voice` / `prompt_id` was removed backend-side, or an operator
  POSTed a typo), the runtime override file is quarantined as
  `agent_config.json.rejected.<epoch>` and the app retries once with the
  config-file / env / CLI defaults. Prevents the device getting
  permanently config-locked by a single bad POST. One-shot only —
  subsequent failures surface normally. (`e1e5372`)
- **README §4.3** documenting `nohup ./run.sh > /tmp/jusiai.log 2>&1 &` +
  `disown` for keeping the assistant running after an SSH disconnect,
  plus the matching `kill -TERM $(pidof jusiai-assistant)` graceful-stop
  line. (`48d55c2`)

### Changed
- **One-shot room bootstrap** — room creation, LiveKit token minting and
  AI-agent dispatch are now a single `POST /rooms/start-chat-bot/`, not
  two sequential calls. Trims call-setup latency by ~1 s. (`1de1b44`)
- **Backend contract sync** with the Meet API refactor:
  - `POST /rooms/connect/` renamed to `POST /rooms/start-chat-bot/`;
    response body reduced to `{ id, livekit: { url, token } }`. (`2584a80`)
  - `POST /rooms/{id}/stop-ai-agent/` replaced with
    `POST /rooms/stop-chat-bot/` keyed by `device_id` (idempotent; no
    room id needed). (`2584a80`)
  - AI-setting fields flattened onto the top level of the request body
    (no more nested `config: { ... }`); `provider` renamed to
    `profile_code` to match `ai-agent-config-v2`'s `profiles[].code`.
    (`fd44b5b`)
- **`device_id` derived from the SoC eFuse serial** (`/proc/cpuinfo`
  `Serial`) as `JUSI-<64-bit hex>`. The value lives in on-die OTP and
  survives rootfs / storage / U-Boot resets — only a board-level SoC
  replacement changes it. Previously the auto-generated id lived under
  `~/.config/jusiai-assistant/device_id` and disappeared with the
  rootfs. The old random-generation path is kept as a fallback for
  x86 / desktop builds where no SoC serial exists. (`c497123`)
- **`control_bind` default flipped to `0.0.0.0`** so a phone or remote on
  the LAN can drive the device out of the box — no config edit or CLI
  flag needed. The API still has no authentication; use `control_bind =
  127.0.0.1` to restrict access to sibling modules on the same device.
  Trusted home WiFi only. (`c06e31e`)
- **Runtime config file is written atomically** — write to a sibling
  `.tmp` and `rename()` on top. Prevents a mid-write interrupt (SIGKILL,
  power loss) from leaving a half-written file that the load path would
  silently discard. (`e1e5372`)
- Sample config defaults to `headless = true` — the target form factor.
  (`ef507e4`)
- Voice / prompt option wording in the README clarified. (`5dad1aa`)

### Removed
- **All UI code** — `src/ui/` (LVGL framebuffer display, evdev touch,
  full GUI shell), `src/i18n.{cpp,h}`, `src/media/frame_buffer.h`,
  `assets/fonts/lv_font_zh_*.c`, `tools/gen_zh_font.js`, `lv_conf.h`,
  `cmake/lvgl.cmake`, plus their CMake link and source entries. State
  labels are now hard-coded Simplified Chinese constants in the
  controller — a phone / remote UI already localises for itself.
  Binary shrinks from ~1778 KB to ~1078 KB (-40%). (`b799546`)
- **`room_name`** configuration key. It was a static per-product display
  label (`"Linux AI 助手"`) that never distinguished individual devices
  — `device_id` does that — so removing it also removes the request
  field, env var, and CLI flag. The backend accepts `name` as optional
  and picks its own default. (`4f1be24`)
- **`prompt_label` / `prompt_content`** from every input and output
  surface (`AppConfig`, conf / env / CLI parsers, `POST /config` accept,
  `GET /config` response, `start-chat-bot` request body). The backend
  endpoint only accepts `prompt_id`, so keeping these was a silent
  no-op. Client bodies that still send them are now silently ignored
  when accompanying a valid field; a body containing ONLY them returns
  HTTP 400. (`e1e5372`)
- Stale UI-era references in `build.sh` (staged-LVGL discovery) and
  `run.sh` (Wayland compositor kill). Also silences a spurious CMake
  "manually-specified variable not used" warning for
  `FETCHCONTENT_SOURCE_DIR_LVGL`. (`a1d3a2a`)

### Branch layout note
- **`port/rv1126b` was renamed to `headless`** and now evolves
  independently. It is deliberately NOT merged back into `main`; `main`
  still carries the last snapshot that had the display UI, kept as a
  starting point for a future touchscreen product.
- The feature branch `feature/headless-control-api` was merged
  (`487e704`) and is gone.
