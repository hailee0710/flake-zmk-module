# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

West-manifest / build-config repo for **Anywhy Flake** (branded "Asterisk Flat"), a 57-key split keyboard with integrated PMW3610 trackball and a dongle that drives a Pacman-status-screen on ST7789P3 display. Board is `nice_nano_v2` (nRF52840) throughout.

It is NOT a from-scratch driver module — it supplies **shields** (hardware definitions) and the **keymap/config** that combine with upstream ZMK and two external modules pulled via `config/west.yml`.

**Current branch `asterisk-flat`** pins ZMK to `v0.3` while debugging split-BLE security failures. ZMK Studio is enabled (`CONFIG_ZMK_STUDIO=y`) with locking disabled.

## Build system

CI builds via standard `zmkfirmware/zmk/.github/workflows/build-user-config.yml@v0.3`, driven by `build.yaml`:

| Artifact | Board | Shield(s) | Notes |
|---|---|---|---|
| `flake_dongle` | nice_nano_v2 | anywhy_flake_dongle pacman_adapter | studio-rpc-usb-uart snippet |
| `flake_dongle_logging` | nice_nano_v2 | anywhy_flake_dongle pacman_adapter | + zmk-usb-logging snippet |
| `flake_left` | nice_nano_v2 | anywhy_flake_left | |
| `flake_right` | nice_nano_v2 | anywhy_flake_right | |
| `settings_reset` | nice_nano_v2 | settings_reset | |

Local west build (from a workspace that imports this repo as `config/`):
```
west build -b nice_nano_v2 -- -DSHIELD="anywhy_flake_left"
west build -b nice_nano_v2 -- -DSHIELD="anywhy_flake_right"
west build -b nice_nano_v2 -- -DSHIELD="anywhy_flake_dongle pacman_adapter"
```

## External dependencies (west.yml)

| Project | Remote | Revision | Purpose |
|---|---|---|---|
| `zmk` | zmkfirmware | v0.3 | Core firmware |
| `zmk-pmw3610-driver` | badjeff | main | PMW3610 trackball sensor driver |
| `zmk-pacman-module` | hailee0710 | main | Pacman status-screen game for dongle display |

`zephyr/module.yml` sets `board_root: .` so this repo is a valid west module.

## Shield architecture: split keyboard with dongle

Three shields under `boards/shields/anywhy_flake/`, sharing `anywhy_flake.dtsi` and `Kconfig.shield`/`Kconfig.defconfig`:

### Shared DTSI (`anywhy_flake.dtsi`)
Defines the 57-key `large_layout` physical layout, `large_transform` (12 columns x 5 rows matrix transform), and `position_map`. All three shields include this file. A transform change here affects left/right/dongle builds together.

### `anywhy_flake_left` (peripheral)
- Real `zmk,kscan-gpio-matrix` on left-half GPIO pins (6 cols, 5 rows)
- Standard key scanning, no trackball, no display
- Config: debounce 5ms, idle timeout 15s

### `anywhy_flake_right` (peripheral)
- Real `zmk,kscan-gpio-matrix` on right-half GPIO pins (6 cols, 5 rows)
- Includes `anywhy_flake_3610.dtsi`: PMW3610 trackball on SPI0 (SPIM_SCK=P1.0, MOSI/MISO=P0.24, CS=P0.22, IRQ=P0.11)
- `col-offset = <6>` on the transform (right half uses columns 6-11)
- Trackball split input bridge enabled (`&trackball_split`)
- Config: debounce 5ms, idle timeout 15s, `CONFIG_PMW3610_ALT=y`, `CONFIG_ZMK_POINTING=y`, `CONFIG_PMW3610_ALT_SWAP_XY=y`

### `anywhy_flake_dongle` (central)
- No physical keys (`mock_kscan` stub)
- Hosts `&trackball_listener` — receives trackball events from right peripheral via split input, processes with `zip_xy_scaler` (1:4), `zip_xy_transform` (X+Y invert). Layer 2 remaps XY to scroll via `zip_xy_to_scroll_mapper` + `zip_scroll_scaler` (1:16).
- Hosts ST7789P3 display (320x172) — actual display node is in `pacman_adapter` shield, dongle only sets `zephyr,display = &st7789p3`
- `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, 3 BT profiles, battery level fetching/proxy
- Config: debug logging enabled, idle timeout 30min, deep sleep

### Split input bridge (`anywhy_flake_split_input.dtsi`)
Connects right-peripheral trackball to dongle listener:
- `trackball_split`: `zmk,input-split` node on the peripheral side
- `trackball_listener`: `zmk,input-listener` on central, with per-layer input processors (base: pointer XY, layer 2: scroll)

## Stage model: Kconfig shield gating

`Kconfig.shield` creates `SHIELD_ANYWHY_FLAKE_DONGLE`/`LEFT`/`RIGHT` bools via `shields_list_contains`. `Kconfig.defconfig` gates on these:
- Any shield enables `CONFIG_ZMK_SPLIT=y`
- Dongle sets `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, `CONFIG_SPI=y` (when display), dedicated display work queue
- Dongle or left or right → split enabled

## Config/keymap files

- `config/anywhy_flake.keymap` — 3 layers: base, fn, scroll. Custom behaviors: `lt` (layer-tap), `mkt` (layer-mouse-key-tap hold-preferred), `mk` macro. Mouse-key combos for left/right click, middle click, MB4/MB5, left shift. `mt` flavor is `tap-preferred`.
- `config/anywhy_flake.conf` — BLE TX power +8, experimental features ON, soft-off OFF, deep sleep 30min, Studio ON (locking OFF), battery report 60s
- `config/anywhy_flake.json` — ZMK Studio metadata, layout geometry for keymap editor

## Gatekeeper file

`keymap.svg` at repo root is a visual reference for the physical layout. Regenerate when key positions change (check for `keymap-drawer` config).

## Delegation

For ZMK-specific work (Devicetree overlays, DTSI, Kconfig, keymap, behaviors, display widgets, west manifest, build failures), delegate to the `zmk-dev` agent. It has detailed documentation at `.claude/agents/zmk-dev.md` covering ZMK APIs, module architecture, debugging Devicetree issues, and build troubleshooting patterns.

For trivial mechanical edits to the keymap or .conf files that don't require ZMK expertise, edit directly.
