/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * claw_tier (added): single source of truth for every performance-dependent
 * budget in the agent stack, selected at build time by CONFIG_CLAW_TIER_*.
 *
 * Why: the budgets below used to be hard-coded constants scattered across
 * claw_core / claw_memory / claw_manager / cap_files. They were all tuned
 * against ESP32-S3 N16R8, which is the *floor* this project must not crash on -
 * not the ceiling it should be limited to. On this class of device every round
 * re-sends the whole context, so a budget that is too small is not "cheap": it
 * evicts content the model then has to re-read, turning RAM pressure into extra
 * LLM rounds. A device with more RAM should therefore get *larger* one-round
 * budgets, not the same ones.
 *
 * Tiers:
 *   ESP32-S3   - the measured baseline. Values are the shipped constants and
 *                must stay bit-identical unless re-measured on device.
 *   ESP32-P4   - ~4-8x the per-round budgets (2x RISC-V @400MHz, 768KB HP SRAM,
 *                large PSRAM).
 *   ESP32-S31  - placeholder equal to P4; calibrate after on-device
 *                measurement (the target only exists from IDF 6.x, so the
 *                Kconfig default cannot bind to it yet).
 *
 * P4/S31 values are estimates from chip resources, not measurements. Re-tune
 * them from device data; keep the three blocks in the same order so the diff
 * stays reviewable.
 */

#if defined(CONFIG_CLAW_TIER_ESP32P4)

/* per-request budgets ---------------------------------------------------- */
#define CLAW_TIER_RUNTIME_BUDGET_BYTES           (640 * 1024)
/* Dynamic window ceiling: the real per-request context cap (the window is derived
 * from free PSRAM, then clamped to this). On a P4 with tens of MB of PSRAM the
 * binding constraint is cost and model context, not memory - calibration knob #1. */
#define CLAW_TIER_CTX_W_CEILING                  (640 * 1024)
#define CLAW_TIER_RUNTIME_KEEP_ROUNDS            16
#define CLAW_TIER_RUNTIME_ARG_MAX_BYTES          600
#define CLAW_TIER_TOOL_RESULT_MAX_BYTES          (48 * 1024)
#define CLAW_TIER_TOOL_RESULT_HEAD_BYTES         (24 * 1024)
#define CLAW_TIER_CAP_FILES_MAX_FILE_SIZE        (128 * 1024)
#define CLAW_TIER_CAP_FILES_EDIT_MAX_FILE_SIZE   (128 * 1024)
#define CLAW_TIER_CAP_FILES_GREP_MAX_FILE_SIZE   (256 * 1024)
/* Size guidance (added): not a limit - a hint the write tools report back. A file
 * that is a large fraction of the window is expensive on EVERY read, and re-reads
 * dominate long tasks, so the useful unit is "how much of one round's context does
 * this file cost". Expressed as a fraction of the window so it widens on its own
 * with a bigger chip. Source is the work product and gets the larger share.
 * Measured example that set these: a 19 KB skill file = 40% of the S3 window, and
 * a 12 KB main.lua re-read 23 times in one task. */
#define CLAW_TIER_FILE_GUIDE_CODE_BYTES          (CLAW_TIER_RUNTIME_BUDGET_BYTES / 3)
#define CLAW_TIER_FILE_GUIDE_TEXT_BYTES          (CLAW_TIER_RUNTIME_BUDGET_BYTES / 8)
#define CLAW_TIER_CAP_CORE_OUTPUT_SIZE           (96 * 1024)

/* Serialize guard preamble. Larger than S3's because the per-tier tool surface and
 * system prompt are assumed to grow with it (see the S3 block for the rule). */
#define CLAW_TIER_SERIALIZE_PREAMBLE_BYTES       (64 * 1024)

/* session projection / summary ------------------------------------------ */
#define CLAW_TIER_PROJECT_TRIGGER_CHARS          (320 * 1024)
#define CLAW_TIER_PROJECT_KEEP_CHARS             (192 * 1024)
#define CLAW_TIER_SUMMARY_MAX_CHARS              (16 * 1024)
/* Was missing from this tier (found when the P4 target was first compiled): the
 * summarizer prompt sizes its output with this, so the tier did not build at all.
 * Kept at the S3 ratio (~0.3 of MAX): a summary is replayed every round. */
#define CLAW_TIER_SUMMARY_TARGET_CHARS           5000
#define CLAW_TIER_COMPACT_MIN_NEW_RECORDS        10
#define CLAW_TIER_COMPACT_TOOL_TURNS             2

/* pinned core ------------------------------------------------------------ */
#define CLAW_TIER_PINNED_MAX_BYTES               (8 * 1024)

/* subagent admission ----------------------------------------------------- */
#define CLAW_TIER_SPAWN_PROMPT_MAX_BYTES         6144
#define CLAW_TIER_SPAWN_MIN_INTERNAL_FREE_BYTES  (96 * 1024)
#define CLAW_TIER_SPAWN_MIN_PSRAM_BLOCK_BYTES    (1024 * 1024)

/* task shape ------------------------------------------------------------- */
#define CLAW_TIER_CORE_TOOL_ITERATIONS           120
#define CLAW_TIER_MEMORY_TOOL_ITERATIONS         40
#define CLAW_TIER_APP_MAX_TOOL_ITERATIONS        240

/* stacks / queues -------------------------------------------------------- */
#define CLAW_TIER_CORE_STACK_SIZE                (24 * 1024)
#define CLAW_TIER_CORE_REQUEST_Q                 8
#define CLAW_TIER_CORE_RESPONSE_Q                8
#define CLAW_TIER_APP_CORE_STACK_SIZE            (32 * 1024)
#define CLAW_TIER_APP_CORE_QUEUE_LEN             8
#define CLAW_TIER_MEMORY_EXTRACT_STACK_SIZE      (12 * 1024)
#define CLAW_TIER_MEMORY_EXTRACT_QUEUE_LEN       8
#define CLAW_TIER_REAPER_STACK_SIZE              8192

#elif defined(CONFIG_CLAW_TIER_ESP32S31)

/* per-request budgets ---------------------------------------------------- */
/* Placeholder: copied from the P4 tier. Re-measure on real hardware. */
#define CLAW_TIER_RUNTIME_BUDGET_BYTES           (256 * 1024)
#define CLAW_TIER_CTX_W_CEILING                  (256 * 1024)
#define CLAW_TIER_RUNTIME_KEEP_ROUNDS            16
#define CLAW_TIER_RUNTIME_ARG_MAX_BYTES          600
#define CLAW_TIER_TOOL_RESULT_MAX_BYTES          (48 * 1024)
#define CLAW_TIER_TOOL_RESULT_HEAD_BYTES         (24 * 1024)
#define CLAW_TIER_CAP_FILES_MAX_FILE_SIZE        (128 * 1024)
#define CLAW_TIER_CAP_FILES_EDIT_MAX_FILE_SIZE   (128 * 1024)
#define CLAW_TIER_CAP_FILES_GREP_MAX_FILE_SIZE   (256 * 1024)
/* Size guidance (added): not a limit - a hint the write tools report back. A file
 * that is a large fraction of the window is expensive on EVERY read, and re-reads
 * dominate long tasks, so the useful unit is "how much of one round's context does
 * this file cost". Expressed as a fraction of the window so it widens on its own
 * with a bigger chip. Source is the work product and gets the larger share.
 * Measured example that set these: a 19 KB skill file = 40% of the S3 window, and
 * a 12 KB main.lua re-read 23 times in one task. */
#define CLAW_TIER_FILE_GUIDE_CODE_BYTES          (CLAW_TIER_RUNTIME_BUDGET_BYTES / 3)
#define CLAW_TIER_FILE_GUIDE_TEXT_BYTES          (CLAW_TIER_RUNTIME_BUDGET_BYTES / 8)
#define CLAW_TIER_CAP_CORE_OUTPUT_SIZE           (96 * 1024)

/* Serialize guard preamble. Larger than S3's because the per-tier tool surface and
 * system prompt are assumed to grow with it (see the S3 block for the rule). */
#define CLAW_TIER_SERIALIZE_PREAMBLE_BYTES       (64 * 1024)

/* session projection / summary ------------------------------------------ */
#define CLAW_TIER_PROJECT_TRIGGER_CHARS          (160 * 1024)
#define CLAW_TIER_PROJECT_KEEP_CHARS             (96 * 1024)
#define CLAW_TIER_SUMMARY_MAX_CHARS              (16 * 1024)
/* Was missing from this tier (found when the P4 target was first compiled): the
 * summarizer prompt sizes its output with this, so the tier did not build at all.
 * Kept at the S3 ratio (~0.3 of MAX): a summary is replayed every round. */
#define CLAW_TIER_SUMMARY_TARGET_CHARS           5000
#define CLAW_TIER_COMPACT_MIN_NEW_RECORDS        10
#define CLAW_TIER_COMPACT_TOOL_TURNS             2

/* pinned core ------------------------------------------------------------ */
#define CLAW_TIER_PINNED_MAX_BYTES               (8 * 1024)

/* subagent admission ----------------------------------------------------- */
#define CLAW_TIER_SPAWN_PROMPT_MAX_BYTES         6144
#define CLAW_TIER_SPAWN_MIN_INTERNAL_FREE_BYTES  (256 * 1024)
#define CLAW_TIER_SPAWN_MIN_PSRAM_BLOCK_BYTES    (1024 * 1024)

/* task shape ------------------------------------------------------------- */
#define CLAW_TIER_CORE_TOOL_ITERATIONS           120
#define CLAW_TIER_MEMORY_TOOL_ITERATIONS         40
#define CLAW_TIER_APP_MAX_TOOL_ITERATIONS        240

/* stacks / queues -------------------------------------------------------- */
#define CLAW_TIER_CORE_STACK_SIZE                (24 * 1024)
#define CLAW_TIER_CORE_REQUEST_Q                 8
#define CLAW_TIER_CORE_RESPONSE_Q                8
#define CLAW_TIER_APP_CORE_STACK_SIZE            (32 * 1024)
#define CLAW_TIER_APP_CORE_QUEUE_LEN             8
#define CLAW_TIER_MEMORY_EXTRACT_STACK_SIZE      (12 * 1024)
#define CLAW_TIER_MEMORY_EXTRACT_QUEUE_LEN       8
#define CLAW_TIER_REAPER_STACK_SIZE              8192

#else /* CONFIG_CLAW_TIER_ESP32S3, or no sdkconfig in scope: the S3 floor */

/* per-request budgets ---------------------------------------------------- */
#define CLAW_TIER_RUNTIME_BUDGET_BYTES           (48 * 1024)
/* Same value the window module used as a flat constant before this moved here. */
#define CLAW_TIER_CTX_W_CEILING                  (256 * 1024)
#define CLAW_TIER_RUNTIME_KEEP_ROUNDS            4
#define CLAW_TIER_RUNTIME_ARG_MAX_BYTES          200
#define CLAW_TIER_TOOL_RESULT_MAX_BYTES          (8 * 1024)
#define CLAW_TIER_TOOL_RESULT_HEAD_BYTES         (4 * 1024)
#define CLAW_TIER_CAP_FILES_MAX_FILE_SIZE        (32 * 1024)
#define CLAW_TIER_CAP_FILES_EDIT_MAX_FILE_SIZE   (32 * 1024)
#define CLAW_TIER_CAP_FILES_GREP_MAX_FILE_SIZE   (64 * 1024)
/* Size guidance (added): not a limit - a hint the write tools report back. A file
 * that is a large fraction of the window is expensive on EVERY read, and re-reads
 * dominate long tasks, so the useful unit is "how much of one round's context does
 * this file cost". Expressed as a fraction of the window so it widens on its own
 * with a bigger chip. Source is the work product and gets the larger share.
 * Measured example that set these: a 19 KB skill file = 40% of the S3 window, and
 * a 12 KB main.lua re-read 23 times in one task. */
#define CLAW_TIER_FILE_GUIDE_CODE_BYTES          (CLAW_TIER_RUNTIME_BUDGET_BYTES / 3)
#define CLAW_TIER_FILE_GUIDE_TEXT_BYTES          (CLAW_TIER_RUNTIME_BUDGET_BYTES / 8)
#define CLAW_TIER_CAP_CORE_OUTPUT_SIZE           (32 * 1024)

/* Serialize guard (added). The guard's budget is the sum of the parts it bounds -
 * replayed history (projection trigger) + in-turn runtime (W) + this fixed
 * preamble (system prompt and tool schemas; measured ~30 KB on S3) - so it can
 * never become the binding constraint and fire before the per-part policies do.
 * The old fixed 96 KB total was below that sum: every ordinary multi-round turn
 * tripped it and its one-pass eviction blanked the whole turn's evidence. */
#define CLAW_TIER_SERIALIZE_PREAMBLE_BYTES       (32 * 1024)

/* session projection / summary ------------------------------------------ */
#define CLAW_TIER_PROJECT_TRIGGER_CHARS          (48 * 1024)
#define CLAW_TIER_PROJECT_KEEP_CHARS             (24 * 1024)
#define CLAW_TIER_SUMMARY_MAX_CHARS              (8 * 1024)
#define CLAW_TIER_SUMMARY_TARGET_CHARS           2500
#define CLAW_TIER_COMPACT_MIN_NEW_RECORDS        6
#define CLAW_TIER_COMPACT_TOOL_TURNS             1

/* pinned core ------------------------------------------------------------ */
#define CLAW_TIER_PINNED_MAX_BYTES               (3 * 1024)

/* subagent admission ----------------------------------------------------- */
#define CLAW_TIER_SPAWN_PROMPT_MAX_BYTES         2000
#define CLAW_TIER_SPAWN_MIN_INTERNAL_FREE_BYTES  (64 * 1024)
#define CLAW_TIER_SPAWN_MIN_PSRAM_BLOCK_BYTES    (128 * 1024)

/* task shape ------------------------------------------------------------- */
#define CLAW_TIER_CORE_TOOL_ITERATIONS           40
#define CLAW_TIER_MEMORY_TOOL_ITERATIONS         10
#define CLAW_TIER_APP_MAX_TOOL_ITERATIONS        100

/* stacks / queues -------------------------------------------------------- */
#define CLAW_TIER_CORE_STACK_SIZE                (8 * 1024)
#define CLAW_TIER_CORE_REQUEST_Q                 4
#define CLAW_TIER_CORE_RESPONSE_Q                4
#define CLAW_TIER_APP_CORE_STACK_SIZE            (16 * 1024)
#define CLAW_TIER_APP_CORE_QUEUE_LEN             4
#define CLAW_TIER_MEMORY_EXTRACT_STACK_SIZE      (6 * 1024)
#define CLAW_TIER_MEMORY_EXTRACT_QUEUE_LEN       4
#define CLAW_TIER_REAPER_STACK_SIZE              4096

#endif
