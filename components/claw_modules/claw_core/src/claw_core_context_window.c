/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "claw_core_window";

/* Dynamic window derivation policy.
 *
 * W is computed from available PSRAM at session start (and re-computed on
 * projection events, with hysteresis to avoid thrashing). The formula is:
 *
 *   B = largest free block in PSRAM (or internal if no PSRAM)
 *   R = reserve for other PSRAM users (LVGL, image, audio, ...)
 *   W_max = (B - R) / CTX_PSRAM_FACTOR
 *   W = clamp(W_max * k_policy, W_floor, W_ceiling)
 *
 * CTX_PSRAM_FACTOR = 2.2 (empirical: messages + serialized body peak ≈ 2x body + JSON scaffolding)
 * k_policy = 0.75 (balanced) — can be tuned per device class
 * W_floor = 48 KB (current fixed budget, availability floor)
 * W_ceiling = model context limit (not hardware bound)
 */

#ifndef CLAW_CTX_PSRAM_FACTOR
#define CLAW_CTX_PSRAM_FACTOR 2.2
#endif

#ifndef CLAW_CTX_RESERVE_RATIO
#define CLAW_CTX_RESERVE_RATIO 0.25
#endif

#ifndef CLAW_CTX_RESERVE_ABS
#define CLAW_CTX_RESERVE_ABS (256 * 1024)
#endif

#ifndef CLAW_CTX_K_POLICY
#define CLAW_CTX_K_POLICY 0.75
#endif

#ifndef CLAW_CTX_W_FLOOR
#define CLAW_CTX_W_FLOOR (48 * 1024)
#endif

/* Memory-safety ceiling for W. Independent of how much PSRAM the chip reports:
 * the serialized request body must still fit in PSRAM with margin while the
 * display/TLS/WiFi are also using it. Raise only after on-device measurement. */
#ifndef CLAW_CTX_W_CEILING
#define CLAW_CTX_W_CEILING (256 * 1024)
#endif

static size_t s_window_size = CLAW_CTX_W_FLOOR;
static size_t s_last_psram_largest = 0;

size_t claw_core_window_get(void)
{
    return s_window_size;
}

/* Serialization feasibility (added, C3). The byte budgets say how much context we
 * allow; they say nothing about whether the heaps can hand the JSON writer ONE
 * buffer of that size. On device a ~200 KB body passed a 336 KB budget and then
 * died inside cJSON because the largest contiguous block was 63 KB. These two
 * functions are what the guard has to compare, and they are also what makes the
 * decision observable from a log or from get_system_info. */

size_t claw_core_serialize_block_have(void)
{
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    /* One allocation succeeds when the LARGER of the two can serve it: malloc
     * prefers PSRAM but falls back to internal RAM, so the best case is the max.
     * Reporting only the preferred heap would refuse requests that would work. */
    return (psram_largest > internal_largest) ? psram_largest : internal_largest;
}

size_t claw_core_serialize_block_need(size_t body_bytes)
{
    /* The writer must obtain ONE contiguous buffer for the printed body, while the
     * cJSON tree still holds the same strings - so the peak is ~2x the body, not 1x.
     *
     * Measured on device (2026-10-19, session firmware1019-1): a ~238 KB body failed
     * with a 467 KB largest PSRAM block, i.e. the real requirement was between 1.96x
     * and 2x. The 1.5x gate this function used at first let exactly that body
     * through, so the crash it was meant to prevent still happened. 2.2x adds
     * fragmentation margin and matches the factor W itself is derived with. */
    if (body_bytes > (SIZE_MAX / 3)) {
        return SIZE_MAX;
    }
    return body_bytes + (body_bytes * 6) / 5;  /* 2.2x */
}

void claw_core_window_update(void)
{
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t B;
    size_t reserve;
    size_t w_max;
    size_t w_new;
    bool has_psram = psram_largest > 0;

    /* Use PSRAM if available, otherwise fall back to internal RAM */
    if (has_psram) {
        B = psram_largest;
    } else {
        B = internal_largest;
    }

    /* Reserve for other PSRAM users: max(absolute, ratio) */
    reserve = (size_t)(B * CLAW_CTX_RESERVE_RATIO);
    if (reserve < CLAW_CTX_RESERVE_ABS) {
        reserve = CLAW_CTX_RESERVE_ABS;
    }

    /* W_max = (B - reserve) / factor */
    if (B <= reserve) {
        w_max = CLAW_CTX_W_FLOOR;
    } else {
        w_max = (size_t)((double)(B - reserve) / CLAW_CTX_PSRAM_FACTOR);
    }

    /* Apply policy factor */
    w_new = (size_t)(w_max * CLAW_CTX_K_POLICY);

    /* Clamp to floor */
    if (w_new < CLAW_CTX_W_FLOOR) {
        w_new = CLAW_CTX_W_FLOOR;
    }
    /* A0 safety cap: never let W (and therefore the request body) exceed a
     * fixed memory-safety bound, even on a chip reporting far more PSRAM. */
    if (w_new > CLAW_CTX_W_CEILING) {
        w_new = CLAW_CTX_W_CEILING;
    }

    /* Hysteresis: only change if delta > 10% to avoid thrashing */
    if (s_last_psram_largest > 0) {
        size_t delta = (psram_largest > s_last_psram_largest)
                           ? (psram_largest - s_last_psram_largest)
                           : (s_last_psram_largest - psram_largest);
        size_t threshold = s_last_psram_largest / 10;
        if (delta <= threshold && w_new == s_window_size) {
            return;
        }
    }

    s_window_size = w_new;
    s_last_psram_largest = psram_largest;

    ESP_LOGI(TAG,
             "window updated: W=%u bytes (B=%u reserve=%u w_max=%u policy=%0.2f psram_largest=%u)",
             (unsigned)s_window_size,
             (unsigned)B,
             (unsigned)reserve,
             (unsigned)w_max,
             CLAW_CTX_K_POLICY,
             (unsigned)psram_largest);
}

void claw_core_window_init(void)
{
    s_window_size = CLAW_CTX_W_FLOOR;
    s_last_psram_largest = 0;
    claw_core_window_update();
}
