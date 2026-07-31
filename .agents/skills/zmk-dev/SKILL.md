---
name: zmk-dev
description: Specialized in ZMK Firmware development — custom modules, behaviors, displays, shields, Devicetree, Kconfig, and Zephyr RTOS integration. Use when writing or modifying ZMK keyboard firmware code, creating custom behaviors or status screens, or working with ZMK's build system.
---

# ZMK Firmware Development Skill

You are a specialist in ZMK Firmware — an open-source keyboard firmware built on Zephyr RTOS. This skill gives you access to Context7-powered documentation lookups and deep knowledge of ZMK's architecture, module system, Devicetree conventions, and common patterns.

## Quick Reference: Context7 Library IDs

When you need to look up ZMK or Zephyr APIs, use these Context7 library IDs with the `query-docs` tool:

| Library | Context7 ID | Use for |
|---------|-------------|---------|
| ZMK Firmware | `/zmkfirmware/zmk` | Behaviors, keymaps, shields, displays, ZMK APIs, build system |
| Zephyr RTOS | `/zephyrproject-rtos/zephyr` | Devicetree, Kconfig, drivers, sensors, threading, logging, LVGL |
| ZMK Helpers | `/urob/zmk-helpers` | Macros, key-labels, unicode support, helper utilities |
| ZMK Adaptive Key | `/urob/zmk-adaptive-key` | Adaptive key behavior with trigger-based bindings |
| ZMK Leader Key | `/urob/zmk-leader-key` | Leader key sequences for custom keybindings |

Always prefer `/zmkfirmware/zmk` for ZMK-specific questions. Use `/zephyrproject-rtos/zephyr` for low-level Zephyr APIs that ZMK builds on. Use `resolve-library-id` tool to discover additional libraries if needed.

## When to Query Context7 Docs

**Always** query before writing ZMK-specific code you're unsure about. In particular:
- ZMK behavior APIs (`&kp`, `&mo`, `&lt`, `&mt`, `&sk`, custom behaviors)
- Devicetree binding syntax for shields, displays, sensors
- ZMK status screen / widget API
- Kconfig conventions for ZMK modules
- Zephyr driver APIs (GPIO, SPI, I2C, PWM, display)
- ZMK event system and inter-component communication

When querying, be specific. For example:
- "How to create a custom ZMK behavior with sensor rotation"
- "ZMK display status screen widget API"
- "ZMK Devicetree binding for ILI9341 or ST7789 display over SPI"

## ZMK Module Architecture

A ZMK module follows this directory structure:

```
module-name/
├── CMakeLists.txt          # Module entry point (zephyr_include_directories, add_subdirectory)
├── Kconfig                 # Module config options (menuconfig, depends on, select)
├── include/                # Public headers for other modules to consume
│   └── zmk_<feature>/
├── src/                    # Implementation files
│   ├── behaviors/          # Custom ZMK behaviors (key press handlers)
│   ├── events/             # Custom ZMK events (inter-module communication)
│   └── <other>/            # Application logic
├── dts/
│   └── bindings/           # Devicetree binding YAML files (e.g., zmk,behavior-foo.yaml)
├── drivers/                # Zephyr driver implementations (sensor, display, etc.)
├── boards/
│   └── shields/            # Shield definitions with overlays, conf, and board-specific files
│       └── <shield>/
│           ├── Kconfig.shield
│           ├── Kconfig.defconfig
│           ├── <shield>.overlay
│           ├── <shield>.conf
│           └── boards/     # Per-board overlay files
├── config/                 # Additional Kconfig fragments
└── zephyr/
    └── module.yml          # Zephyr module manifest (for west)
```

### Shield Board Pattern

Shields define optional hardware add-ons. Key files:
- **`<shield>.overlay`**: Devicetree overlay adding nodes for the shield hardware (display, sensors, GPIO)
- **`<shield>.conf`**: Kconfig fragment enabling required subsystems (e.g., `CONFIG_DISPLAY=y`, `CONFIG_SPI=y`)
- **`Kconfig.shield`**: Shield-specific Kconfig options
- **`Kconfig.defconfig`**: Default Kconfig values applied when shield is selected
- **`boards/<board>.overlay`**: Board-specific pin muxing for the shield

### Behaviors

Custom ZMK behaviors extend keyboard functionality beyond standard keycodes. A behavior:
1. Has a Devicetree binding in `dts/bindings/zmk,behavior-<name>.yaml`
2. Implements the behavior driver in `src/behaviors/`
3. Uses `ZMK_BEHAVIOR_DEFINE` or `ZMK_SUBSCRIPTION_DEFINE` macros
4. Is referenced in keymaps as `&<name>` once compatible string is added

### Events

ZMK events allow modules to communicate without direct coupling:
- Define events in `include/zmk_dongle_events/` (or `include/zmk/events/`)
- Use `ZMK_EVENT_DEFINE` macro
- Publishers call `ZMK_EVENT_RAISE` / `ZMK_EVENT_SUBMIT`
- Subscribers use `ZMK_SUBSCRIPTION_DEFINE` with callback

## Coding Conventions

### Devicetree Bindings

Bindings are YAML files with Zephyr's binding syntax:
```yaml
description: Short description
compatible: "zmk,behavior-foo"
include: base.yaml       # or zmk,behavior-sensor-rotate, etc.
properties:
  prop-name:
    type: int
    default: 0
    description: What this property does
```

### Kconfig

- Use `if MODULE_NAME` / `endif` for conditional configs
- Prefer `select` for required dependencies, `depends on` for optional
- Module-level configs should use `menuconfig` for the top-level toggle
- Follow ZMK naming: `CONFIG_ZMK_<SUBSYSTEM>_<FEATURE>`

### CMakeLists.txt

```cmake
if(CONFIG_ZMK_MY_MODULE)
  zephyr_include_directories(include)
  add_subdirectory_ifdef(CONFIG_ZMK_MY_SUBFEATURE src/subdir)
endif()
```

### C Code

- Use Zephyr logging: `LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);`
- Use `sys_heap` or `k_malloc`/`k_free` for dynamic allocation
- Use Zephyr work queues (`k_work`, `k_work_delayable`) for deferred or periodic work
- Thread safety: Use `k_mutex`, `k_sem`, `k_msgq` for synchronization
- Display code uses Zephyr's `display` API (not direct SPI writes)

## Project Context: Pacman Dongle Module

This project (`zmk-lcd-dongle`) is a ZMK module that adds a Pacman-themed status screen and game to a dongle with an ST7789P3 172x320 display. Key details:

- **Display**: ST7789P3 172x320 via SPI
- **Board**: nice!nano v2 (nRF52840)
- **Module name**: `pacman-module` (Kconfig: `CONFIG_ZMK_DONGLE_PACMAN`)
- **Behaviors**: Custom `dongle_action` behavior for navigation/game controls
- **Events**: Custom events in `include/zmk_dongle_events/`
- **Widgets**: battery_status, layer_status, output_status, action_button, logo, splash, pacman, theme, configuration — located in `boards/shields/pacman_adapter/widgets/`
- **Helpers**: display, fonts, list, buzzer, pwm, settings — located in `boards/shields/pacman_adapter/widgets/helpers/`
- **Shield**: `pacman_adapter` shield with board-specific overlays for nice_nano_v2

When working on this project, reference existing patterns before introducing new ones. The `custom_status_screen.c` is the main integration point that ties widgets together.

## Workflow for Common Tasks

### Adding a new ZMK behavior

1. Query Context7: `query-docs("/zmkfirmware/zmk", "create custom behavior driver implementation")`
2. Create Devicetree binding in `dts/bindings/zmk,behavior-<name>.yaml`
3. Implement behavior driver in `src/behaviors/`
4. Register with `ZMK_BEHAVIOR_DEFINE` macro
5. Add Kconfig option if behavior should be conditionally compiled
6. Add CMakeLists.txt entry in `src/behaviors/`

### Adding a new display widget

1. Query Context7: `query-docs("/zmkfirmware/zmk", "status screen widget implementation display API")`
2. Create widget `.c`/`.h` files following the existing widget pattern
3. Implement `init`, `update`, and render functions
4. Register the widget in `custom_status_screen.c` or equivalent

### Debugging Devicetree issues

1. Query Context7: `query-docs("/zephyrproject-rtos/zephyr", "Devicetree troubleshooting overlay not applied")`
2. Check build output for `devicetree_unfixed.h` or `devicetree_generated.h`
3. Verify compatible strings match between binding YAML and overlay
4. Check Kconfig enables the driver (`CONFIG_DISPLAY=y`, `CONFIG_SPI=y`, etc.)
5. Use `LOG_DBG` with devicetree macros like `DT_NODE_HAS_STATUS`

### Troubleshooting build failures

1. Query Context7: `query-docs("/zmkfirmware/zmk", "build troubleshooting west build errors")`
2. Check `build/zephyr/.config` for Kconfig resolution
3. Verify west workspace is correct (`west update`, module.yml)
4. Check for missing `select` or `depends on` in Kconfig
5. Review CMakeLists.txt path correctness

## Key ZMK APIs

### Behavior System
- `ZMK_BEHAVIOR_DEFINE(name, driver_data, config_data, ...)` — register a behavior
- `ZMK_SUBSCRIPTION_DEFINE(name, callback)` — subscribe to events
- `ZMK_BEHAVIOR_HANDLER_DEFINE(name, handler)` — behavior key press handler
- `zmk_behavior_binding` — represents a keymap binding in C

### Event System
- `ZMK_EVENT_DEFINE(name, ...)` — define a new event type
- `ZMK_EVENT_RAISE(event)` — fire an event (async)
- `ZMK_EVENT_SUBMIT(event)` — fire an event (immediate)
- `ZMK_EVENT_IMPLICIT_DECLARE(event)` — declare event externally

### Display / Status Screen
- `zmk_widget_*` — widget management for status screens
- Zephyr `display` API: `display_write()`, `display_blanking_off()`, `display_set_pixel_format()`
- LVGL integration when `CONFIG_LVGL=y`

### Utility
- `zmk_keymap_layers` — iterate over layer names
- `zmk_endpoints_*` — USB/BLE endpoint management
- `zmk_split_*` — split keyboard peripheral/central communication
- Zephyr: `k_work_submit`, `k_timer_start`, `k_sleep`, `printk`, `LOG_DBG`

## Reminders

- **Always check Context7 before guessing ZMK/Zephyr APIs.** The documentation is comprehensive and up-to-date.
- When in doubt about a pattern, look at how existing ZMK modules do it (both in this project and in upstream ZMK).
- Devicetree errors often manifest as silent failures at runtime — always verify nodes are enabled in the final build.
- ZMK build system uses `west` — ensure module.yml and CMakeLists.txt are correct for module discovery.
- Kconfig changes require a clean build (or at least re-running CMake) to take effect.
