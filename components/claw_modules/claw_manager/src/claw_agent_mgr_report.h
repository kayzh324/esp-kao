/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Write a subagent's terminal text in full under the writable DATA root
 *
 *         (added) A notification is the only channel through which a parent learns
 *         how a subagent ended, and it is bounded. When the terminal text is larger
 *         than the notification's inline budget the parent needs the rest anyway -
 *         measured on device: a subagent's OOM diagnostic was cut in the middle and
 *         lost its most actionable field. So the full text goes to
 *         "<DATA>/.reports/" and the notification carries a head plus this path,
 *         which the parent reads with read_file (same contract as .toolout).
 *
 * @param[in]   session_id     Subagent session id (names the file; its tail is kept)
 * @param[in]   request_id     Terminal request id (disambiguates repeat turns)
 * @param[in]   kind           Short tag for the file name, e.g. "report" / "error"
 * @param[in]   body           Full text to persist (NUL-terminated)
 * @param[out]  out_path       Receives the composed path
 * @param[in]   out_path_size  Size of out_path
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG on bad arguments
 *         - ESP_ERR_INVALID_STATE if the DATA root is not set
 *         - ESP_ERR_INVALID_SIZE if the path does not fit
 *         - other esp_err_t from the filesystem on write failure
 */
esp_err_t claw_agent_mgr_report_spill(const char *session_id,
                                      uint32_t request_id,
                                      const char *kind,
                                      const char *body,
                                      char *out_path,
                                      size_t out_path_size);

#ifdef __cplusplus
}
#endif
