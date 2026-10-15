/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

static esp_err_t status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    esp_err_t err = ctx->services.get_wifi_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Wi-Fi status");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    http_server_json_add_string(root, "ip", status.ip);
    http_server_json_add_string(root, "storage_base_path", ctx->storage_base_path);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    http_server_json_add_string(root, "ap_ssid", status.ap_ssid);
    http_server_json_add_string(root, "ap_ip", status.ap_ip);
    http_server_json_add_string(root, "wifi_mode", status.wifi_mode);
    return http_server_send_json_response(req, root);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.restart_device ? ctx->services.restart_device() : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restart device");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "device restart scheduled");
    return http_server_send_json_response(req, root);
}

/* agent-monitor (added): subagent status for the web UI's status lights. */
#define HTTP_SERVER_AGENTS_JSON_MAX 2048

static esp_err_t agents_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    char chat_id[64] = {0};
    char *buf = NULL;
    const char *query;
    esp_err_t err;

    if (!ctx->services.get_agents) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Agents endpoint not supported");
    }

    /* Optional ?chat_id=<id> narrows the list to one chat. */
    query = strchr(req->uri, '?');
    if (query) {
        const char *p = strstr(query, "chat_id=");

        if (p) {
            size_t n;

            p += strlen("chat_id=");
            n = strcspn(p, "&");
            if (n >= sizeof(chat_id)) {
                n = sizeof(chat_id) - 1;
            }
            memcpy(chat_id, p, n);
            chat_id[n] = '\0';
        }
    }

    buf = malloc(HTTP_SERVER_AGENTS_JSON_MAX);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    err = ctx->services.get_agents(chat_id, buf, HTTP_SERVER_AGENTS_JSON_MAX);
    if (err != ESP_OK) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read agents");
    }
    err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        err = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    free(buf);
    return err;
}

esp_err_t http_server_register_status_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/agents", .method = HTTP_GET, .handler = agents_handler },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
