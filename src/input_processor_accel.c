#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <stdlib.h>

/* Minimal types from drivers/input_processor.h (not available to external modules) */
#define ZMK_INPUT_PROC_CONTINUE 0
#define ZMK_INPUT_PROC_STOP 1

struct zmk_input_processor_state {
    uint8_t input_device_index;
    int16_t *remainder;
};

struct zmk_input_processor_driver_api {
    int (*handle_event)(const struct device *dev, struct input_event *event,
                        uint32_t param1, uint32_t param2,
                        struct zmk_input_processor_state *state);
};

#define DT_DRV_COMPAT zmk_input_processor_acceleration

#ifndef CONFIG_ZMK_INPUT_PROCESSOR_INIT_PRIORITY
#define CONFIG_ZMK_INPUT_PROCESSOR_INIT_PRIORITY 50
#endif

#define ACCEL_MAX_CODES 4
#define SCALE 1000

#define CURVE_POLYNOMIAL 0
#define CURVE_SIGMOID    1
#define CURVE_SCROLL     2

struct accel_config {
    uint8_t  input_type;
    const uint16_t *codes;
    uint32_t codes_count;
    bool     track_remainders;

    /* ── Legacy polynomial / sigmoid-v1 params ── */
    uint16_t min_factor;
    uint16_t max_factor;
    uint32_t speed_threshold;
    uint32_t speed_max;
    uint8_t  acceleration_exponent;
    uint8_t  curve_type;
    uint32_t sigmoid_midpoint;
    uint32_t sigmoid_steepness;

    /* ── Sigmoid / scroll params ── */
    uint16_t factor_base;       /* acceleration-factor-base: base multiplier at low speed */
    uint16_t factor_max;        /* acceleration-factor-max:  ceiling multiplier at high speed */
    uint32_t factor_rate;       /* acceleration-factor-rate: steepness (higher = gentler S) */
    uint32_t start_offset;      /* acceleration-start-offset: dead-zone before curve engages */
    uint32_t max_speed;         /* max-speed: speed at which factor_max is reached (or CPS cap) */
    bool     interval_speed;    /* enable-interval-based-speed: use fixed-rate CPS instead of dt */
    uint32_t sending_rate;      /* input-default-sending-rate: PS/2 sample rate in Hz */
    uint16_t divisor;           /* divisor: output divider (scroll) */
};

struct accel_data {
    int64_t last_time_ms[ACCEL_MAX_CODES];
    int32_t last_phys[ACCEL_MAX_CODES];
    int16_t remainders[ACCEL_MAX_CODES];
};

/* ── helpers ───────────────────────────────────────────────────────── */

static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline bool code_to_index(const struct accel_config *cfg, uint16_t code, uint32_t *out_idx) {
    for (uint32_t i = 0; i < cfg->codes_count; ++i) {
        if (cfg->codes[i] == code) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static uint32_t pow_scaled(uint32_t t, uint8_t exp) {
    t = clamp_u32(t, 0, SCALE);
    if (exp <= 1) return t;
    uint64_t acc = t;
    for (uint8_t i = 1; i < exp; ++i) {
        acc = (acc * t) / SCALE;
    }
    if (acc > UINT32_MAX) acc = UINT32_MAX;
    return (uint32_t)acc;
}

/*
 * Logistic sigmoid approximation (maps (-inf, +inf) → [0, SCALE]):
 *
 *   sigmoid(t) ≈ SCALE/2 + (SCALE * t) / (2 * (SCALE + |t|))
 *
 * t is already scaled to SCALE-space by the caller.
 */
static int64_t logistic_scaled(int64_t t) {
    /* Clamp t to avoid denominator overflow */
    int64_t clamp = (int64_t)SCALE * 16;
    if (t > clamp) t = clamp;
    if (t < -clamp) t = -clamp;

    int64_t abs_t = (t < 0) ? -t : t;
    int64_t num   = SCALE * t;
    int64_t den   = 2 * (SCALE + abs_t);
    return SCALE / 2 + num / den;
}

/* ── sigmoid (base / max / rate / offset / max-speed) ─────────────── */

/*
 * Uses the user-facing parameter model:
 *
 *                factor_max  ┤          .-'''''''''''''''''''''''''''''''''''''- 
 *                            ┤        .'
 *                            ┤      .'
 *                            ┤    .'
 *              factor_base  ┤...'
 *                            ┤
 *                            └─────┬────────────┬─────────────────────────→ speed
 *                             start_offset   max_speed
 *
 *   if speed <= start_offset  →  factor = factor_base
 *   if speed >= max_speed     →  factor = factor_max
 *   otherwise, the S-curve is shaped by factor_rate (steepness).
 */
static uint32_t compute_sigmoid_factor(const struct accel_config *cfg, uint32_t cps) {
    const uint32_t f_min  = clamp_u32(cfg->factor_base, 100, 20000);
    const uint32_t f_max  = clamp_u32(cfg->factor_max, f_min, 20000);
    const uint32_t offset = cfg->start_offset;
    const uint32_t ceil   = (cfg->max_speed > offset) ? cfg->max_speed : (offset + 1);
    const uint32_t rate   = cfg->factor_rate ? cfg->factor_rate : 1;

    /* Dead zone – return base factor */
    if (cps <= offset) {
        return f_min;
    }

    /* Cap at ceiling – return max factor directly */
    if (cps >= ceil) {
        return f_max;
    }

    /* x = (adj relative to midpoint) scaled to SCALE, then through
     * logistic.  By centering the logistic on adj_max/2, adj=0
     * maps to the left (flat) tail and adj=adj_max maps to the
     * right (flat) tail of the S, producing a smooth curve with
     * no discontinuity at the dead-zone boundary. */
    uint32_t adj      = cps - offset;
    uint32_t adj_max  = ceil - offset;
    int64_t  midpoint = (int64_t)adj_max * SCALE / 2;
    int64_t  t        = (((int64_t)adj * SCALE) - midpoint) / (int64_t)rate;
    int64_t  sig      = logistic_scaled(t);

    /* Map sigmoid [0, SCALE] → factor [f_min, f_max] */
    int64_t span   = (int64_t)f_max - (int64_t)f_min;
    int64_t factor = (int64_t)f_min + (span * sig) / SCALE;

    return clamp_u32((uint32_t)factor, f_min, f_max);
}

/* ── scroll curve (speed-clamping (linear ramp)) ────────────────────────── */

/*
 * Scroll curve: maps input speed through a sigmoid to produce a
 * desired output speed in [1, max_speed], then returns
 * factor = desired_speed * SCALE / input_speed.
 *
 * This CLAMPS fast movements instead of boosting them -- ideal for
 * scroll wheels where large deltas should be tamed, not amplified.
 *
 *   desired
 *   max_speed ┤                          .-''''''''''-
 *             ┤                        .'
 *             ┤                      .'
 *             ┤                    .'
 *           1 ┤....................'
 *             └──────┬──────────────────────┬──────→ input speed
 *              start_offset             max_speed
 *
 * Then: factor = desired / input  (so factor <= 1.0 in most cases)
 * The divisor (applied later) further reduces the result.
 */
static uint32_t compute_scroll_factor(const struct accel_config *cfg, uint32_t cps) {
    if (cps == 0) {
        return SCALE;
    }

    const uint32_t offset = cfg->start_offset;
    const uint32_t ceil   = (cfg->max_speed > offset) ? cfg->max_speed : (offset + 1);

    /* Below offset: hyperbolic falloff (desired = 1) */
    if (cps <= offset) {
        uint32_t factor = SCALE / cps;
        return factor > 0 ? factor : 1;
    }

    /* At/above ceiling: output capped at max_speed */
    if (cps >= ceil) {
        uint32_t factor = (uint32_t)((uint64_t)ceil * SCALE / cps);
        return factor > 0 ? factor : 1;
    }

    /* Linear ramp of desired output from 1 (at offset) to ceil (at max_speed).
     * Simpler and more predictable than sigmoid for scroll clamping. */
    uint32_t range    = ceil - offset;
    uint32_t progress = cps - offset;
    uint32_t desired  = 1 + (uint32_t)((uint64_t)(ceil - 1) * progress / range);

    /* factor = desired / cps, scaled by SCALE */
    uint32_t factor = (uint32_t)((uint64_t)desired * SCALE / cps);
    return factor > 0 ? factor : 1;
}

/* ── legacy polynomial (piecewise with exponent) ──────────────────── */

static uint32_t compute_polynomial_factor(const struct accel_config *cfg, uint32_t cps) {
    const uint32_t f_min = clamp_u32(cfg->min_factor, 100, 20000);
    const uint32_t f_max = clamp_u32(cfg->max_factor, f_min, 20000);
    const uint32_t v1 = cfg->speed_threshold;
    const uint32_t v2 = (cfg->speed_max > v1) ? cfg->speed_max : (v1 + 1);
    const uint8_t  e  = cfg->acceleration_exponent ? cfg->acceleration_exponent : 1;

    const uint32_t base = (f_min > 1000) ? f_min : 1000;

    if (cps <= v1) {
        uint32_t t = (v1 == 0) ? SCALE : (uint32_t)((uint64_t)cps * SCALE / v1);
        uint32_t shaped = pow_scaled(t, e);
        int32_t span = (int32_t)base - (int32_t)f_min;
        int32_t f = (int32_t)f_min + (int32_t)((int64_t)span * shaped / SCALE);
        return clamp_u32((uint32_t)f, f_min, base);
    } else if (cps >= v2) {
        return f_max;
    } else {
        uint32_t t = (uint32_t)((uint64_t)(cps - v1) * SCALE / (v2 - v1));
        uint32_t shaped = pow_scaled(t, e);
        int32_t span = (int32_t)f_max - (int32_t)base;
        int32_t f = (int32_t)base + (int32_t)((int64_t)span * shaped / SCALE);
        return clamp_u32((uint32_t)f, base, f_max);
    }
}

/* ── dispatcher ───────────────────────────────────────────────────── */

static uint32_t compute_factor_scaled(const struct accel_config *cfg, uint32_t cps) {
    switch (cfg->curve_type) {
    case CURVE_SIGMOID:   /* 1 — sigmoid (base/max/rate/offset/max-speed) */
        return compute_sigmoid_factor(cfg, cps);
    case CURVE_SCROLL:    /* 2 — scroll (sigmoid + divisor) */
        return compute_scroll_factor(cfg, cps);
    default:              /* 0 — polynomial (piecewise + exponent) */
        return compute_polynomial_factor(cfg, cps);
    }
}

/* ── event handler ────────────────────────────────────────────────── */

static int accel_handle_event(const struct device *dev, struct input_event *event,
                              uint32_t param1, uint32_t param2,
                              struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    const struct accel_config *cfg = dev->config;
    struct accel_data *data = dev->data;

    if (event->type != cfg->input_type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint32_t idx = 0;
    if (!code_to_index(cfg, event->code, &idx)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const int32_t raw = event->value;
    const int64_t now = k_uptime_get();

    if (raw == 0) {
        data->last_time_ms[idx] = now;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* ── speed (CPS) computation ── */
    uint32_t cps;

    if (cfg->interval_speed) {
        /*
         * Interval-based speed: use |raw| directly as the speed
         * value so that start-offset, max-speed, and factor-rate
         * are all in the same raw-count unit space.  The
         * sending-rate is informational only in this mode.
         *
         * A trackpoint at 100 Hz reporting raw=5 means the finger
         * moved 5 counts in one sample period; the curve reacts to
         * that delta magnitude directly.
         */
        cps = (uint32_t)abs(raw);
    } else {
        uint32_t dt_ms = 1;
        if (data->last_time_ms[idx] > 0 && now > data->last_time_ms[idx]) {
            int64_t diff = now - data->last_time_ms[idx];
            if (diff > 100) diff = 100;
            dt_ms = (uint32_t)diff;
        }
        cps = (uint32_t)(((uint64_t)abs(raw) * 1000ULL) / dt_ms);
    }

    /* ── factor ── */
    uint32_t factor = compute_factor_scaled(cfg, cps);

    /* Direction reversal damping: if the user suddenly reverses direction
     * while at high speed, clamp factor to 1.0x to avoid jerk. */
    if ((int64_t)data->last_phys[idx] * (int64_t)raw < 0 && factor > 1000) {
        factor = 1000;
    }

    /* ── apply factor and optional divisor ── */
    if (cfg->track_remainders) {
        int64_t total = (int64_t)raw * (int64_t)factor + (int64_t)data->remainders[idx];
        int32_t out = (int32_t)(total / SCALE);
        int32_t rem = (int32_t)(total - (int64_t)out * SCALE);

        /* Scroll curve: apply integer divisor after acceleration */
        if (cfg->curve_type == CURVE_SCROLL && cfg->divisor > 1) {
            out /= cfg->divisor;
            rem  = 0; /* discard fractional on division */
        }

        if (out > 32767) out = 32767;
        if (out < -32768) out = -32768;
        event->value = out;
        data->remainders[idx] = (int16_t)rem;
    } else {
        int32_t out = (int32_t)(((int64_t)raw * (int64_t)factor) / SCALE);

        if (cfg->curve_type == CURVE_SCROLL && cfg->divisor > 1) {
            out /= cfg->divisor;
        }

        event->value = out;
    }

    data->last_phys[idx] = raw;
    data->last_time_ms[idx] = now;
    return ZMK_INPUT_PROC_CONTINUE;
}

/* ── init ─────────────────────────────────────────────────────────── */

static int accel_init(const struct device *dev) {
    return 0;
}

/* ── DT instantiation ─────────────────────────────────────────────── */

#define ACCEL_INST_INIT(inst)                                                      \
    static const uint16_t accel_codes_##inst[] = { INPUT_REL_X, INPUT_REL_Y };    \
    static const struct accel_config accel_config_##inst = {                      \
        .input_type    = DT_INST_PROP_OR(inst, input_type, INPUT_EV_REL),         \
        .codes         = accel_codes_##inst,                                     \
        .codes_count   = 2,                                                      \
        .track_remainders   = DT_INST_NODE_HAS_PROP(inst, track_remainders),     \
        /* legacy poly / sigmoid-v1 */                                            \
        .min_factor    = DT_INST_PROP_OR(inst, min_factor, 1000),                 \
        .max_factor    = DT_INST_PROP_OR(inst, max_factor, 3500),                 \
        .speed_threshold = DT_INST_PROP_OR(inst, speed_threshold, 1000),          \
        .speed_max     = DT_INST_PROP_OR(inst, speed_max, 6000),                  \
        .acceleration_exponent = DT_INST_PROP_OR(inst, acceleration_exponent, 1),\
        .curve_type    = DT_INST_PROP_OR(inst, curve_type, 0),                    \
        .sigmoid_midpoint  = DT_INST_PROP_OR(inst, sigmoid_midpoint, 2000),       \
        .sigmoid_steepness = DT_INST_PROP_OR(inst, sigmoid_steepness, 2000),      \
        /* sigmoid / scroll */                                             \
        .factor_base   = DT_INST_PROP_OR(inst, acceleration_factor_base, 1000),   \
        .factor_max    = DT_INST_PROP_OR(inst, acceleration_factor_max, 1500),    \
        .factor_rate   = DT_INST_PROP_OR(inst, acceleration_factor_rate, 50),     \
        .start_offset  = DT_INST_PROP_OR(inst, acceleration_start_offset, 2),     \
        .max_speed     = DT_INST_PROP_OR(inst, max_speed, 70),                    \
        .interval_speed = DT_INST_NODE_HAS_PROP(inst, enable_interval_based_speed),\
        .sending_rate  = DT_INST_PROP_OR(inst, input_default_sending_rate, 100),  \
        .divisor       = DT_INST_PROP_OR(inst, divisor, 1),                       \
    };                                                                            \
    static struct accel_data accel_data_##inst = {0};                             \
    static const struct zmk_input_processor_driver_api accel_api_##inst = {       \
        .handle_event = accel_handle_event,                                       \
    };                                                                            \
    DEVICE_DT_INST_DEFINE(inst,                                                   \
                          accel_init,                                             \
                          NULL,                                                   \
                          &accel_data_##inst,                                     \
                          &accel_config_##inst,                                   \
                          POST_KERNEL,                                            \
                          CONFIG_ZMK_INPUT_PROCESSOR_INIT_PRIORITY,               \
                          &accel_api_##inst);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST_INIT)
