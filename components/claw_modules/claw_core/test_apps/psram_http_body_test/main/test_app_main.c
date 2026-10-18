/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TEST_HTTP_PORT 18182
#define TEST_PSRAM_SIZE (128 * 1024)
#define TEST_TASK_STACK 6144
#define TEST_HTTP_TIMEOUT_MS 10000

static const char *TAG = "psram_http_test";

typedef struct {
    int sock;
} test_conn_t;

typedef struct {
    int listen_sock;
    test_conn_t conn;
    bool server_ready;
    bool got_request;
    size_t received_bytes;
    size_t expected_bytes;
    bool content_ok;
} test_server_ctx_t;

static test_server_ctx_t s_server;

static void log_mem_snapshot(const char *label)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "[%s] internal_free=%u largest_block=%u 8bit_free=%u psram_free=%u psram_largest=%u",
             label,
             (unsigned int)free_internal,
             (unsigned int)largest_internal,
             (unsigned int)free_8bit,
             (unsigned int)free_psram,
             (unsigned int)largest_psram);
}

static void fill_psram_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    /* Deterministic pseudo-random pattern for content verification */
    uint32_t state = seed ? seed : 0x12345678;

    for (size_t i = 0; i < len; i++) {
        state = state * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(state >> 24);
    }
}

static bool verify_psram_pattern(const uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t state = seed ? seed : 0x12345678;

    for (size_t i = 0; i < len; i++) {
        state = state * 1664525u + 1013904223u;
        uint8_t expected = (uint8_t)(state >> 24);
        if (buf[i] != expected) {
            ESP_LOGE(TAG, "Content mismatch at offset %u: got 0x%02x expected 0x%02x",
                     (unsigned int)i, buf[i], expected);
            return false;
        }
    }
    return true;
}

static esp_err_t read_http_request_head(int sock, char *buf, size_t buf_size)
{
    size_t used = 0;

    if (sock < 0 || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    while (used + 1 < buf_size) {
        int len = recv(sock, buf + used, buf_size - used - 1, 0);
        if (len <= 0) {
            return ESP_FAIL;
        }
        used += (size_t)len;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) {
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_SIZE;
}

static size_t parse_content_length(const char *head)
{
    const char *p = strcasestr(head, "Content-Length:");
    if (!p) {
        return 0;
    }
    p += 15;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return (size_t)strtoul(p, NULL, 10);
}

static void http_conn_task(void *arg)
{
    test_server_ctx_t *ctx = (test_server_ctx_t *)arg;
    char head_buf[2048];
    int sock = ctx ? ctx->conn.sock : -1;

    if (!ctx || sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    if (read_http_request_head(sock, head_buf, sizeof(head_buf)) == ESP_OK) {
        size_t content_len = parse_content_length(head_buf);

        ESP_LOGI(TAG, "Server: got request, Content-Length=%u", (unsigned int)content_len);

        if (content_len > 0 && content_len <= TEST_PSRAM_SIZE) {
            uint8_t *recv_buf = malloc(content_len);
            if (recv_buf) {
                size_t received = 0;

                while (received < content_len) {
                    int len = recv(sock, recv_buf + received, content_len - received, 0);
                    if (len <= 0) {
                        break;
                    }
                    received += (size_t)len;
                }
                ctx->received_bytes = received;
                ctx->expected_bytes = content_len;
                ctx->got_request = true;
                ctx->content_ok = verify_psram_pattern(recv_buf, received, 0x12345678);
                ESP_LOGI(TAG, "Server: received %u/%u bytes, content_ok=%d",
                         (unsigned int)received,
                         (unsigned int)content_len,
                         ctx->content_ok);
                free(recv_buf);
            } else {
                ESP_LOGE(TAG, "Server: OOM allocating recv buffer");
            }
        } else {
            ESP_LOGE(TAG, "Server: invalid Content-Length=%u", (unsigned int)content_len);
        }
    }

    /* Send response */
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "ok";
    (void)send(sock, resp, strlen(resp), 0);
    shutdown(sock, 0);
    close(sock);
    ctx->conn.sock = -1;
    vTaskDelete(NULL);
}

static void http_server_task(void *arg)
{
    test_server_ctx_t *ctx = (test_server_ctx_t *)arg;
    struct sockaddr_in server_addr = {0};
    int listen_sock = -1;

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    (void)setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(TEST_HTTP_PORT);
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0 ||
            listen(listen_sock, 2) != 0) {
        shutdown(listen_sock, 0);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ctx->listen_sock = listen_sock;
    ctx->server_ready = true;

    int sock = accept(listen_sock, NULL, NULL);
    if (sock >= 0) {
        ctx->conn.sock = sock;
        xTaskCreate(http_conn_task, "test_conn", TEST_TASK_STACK, ctx, 5, NULL);
    }
    vTaskDelete(NULL);
}

static esp_err_t start_test_server(void)
{
    memset(&s_server, 0, sizeof(s_server));
    s_server.conn.sock = -1;

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (xTaskCreate(http_server_task, "test_srv", TEST_TASK_STACK, &s_server, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 100 && !s_server.server_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_server.server_ready ? ESP_OK : ESP_FAIL;
}

static void stop_test_server(void)
{
    if (s_server.conn.sock >= 0) {
        shutdown(s_server.conn.sock, 0);
        close(s_server.conn.sock);
        s_server.conn.sock = -1;
    }
    if (s_server.listen_sock >= 0) {
        shutdown(s_server.listen_sock, 0);
        close(s_server.listen_sock);
        s_server.listen_sock = -1;
    }
}

static esp_err_t post_psram_body(const char *url, const uint8_t *body, size_t len)
{
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;

    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = TEST_HTTP_TIMEOUT_MS;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;

    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    /* KEY TEST: set_post_field with PSRAM pointer */
    esp_http_client_set_post_field(client, (const char *)body, (int)len);

    ESP_LOGI(TAG, "POSTing %u bytes to %s (body ptr=%p, is_psram=%d)",
             (unsigned int)len, url, body,
             (int)heap_caps_get_allocated_size(body) > 0 ? 1 : 0);

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_perform failed: %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(client);
        int len_read = esp_http_client_read_response(client, NULL, 0);
        ESP_LOGI(TAG, "HTTP status=%d, response_len=%d", status, len_read);
        if (status != 200) {
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

void app_main(void)
{
    printf("\n=== PSRAM HTTP body test ===\n");

    log_mem_snapshot("boot");

    /* Allocate PSRAM buffer */
    uint8_t *psram_buf = heap_caps_malloc(TEST_PSRAM_SIZE, MALLOC_CAP_SPIRAM);
    if (!psram_buf) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes PSRAM", (unsigned int)TEST_PSRAM_SIZE);
        printf("RESULT: FAIL (PSRAM alloc)\n");
        return;
    }
    ESP_LOGI(TAG, "Allocated %u bytes PSRAM at %p", (unsigned int)TEST_PSRAM_SIZE, psram_buf);

    fill_psram_pattern(psram_buf, TEST_PSRAM_SIZE, 0x12345678);
    log_mem_snapshot("after_psram_alloc");

    /* Start local HTTP server */
    esp_err_t err = start_test_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start test server: %s", esp_err_to_name(err));
        free(psram_buf);
        printf("RESULT: FAIL (server start)\n");
        return;
    }
    ESP_LOGI(TAG, "Test server ready on port %d", TEST_HTTP_PORT);
    log_mem_snapshot("after_server_start");

    /* POST PSRAM body to local server */
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/echo", TEST_HTTP_PORT);

    err = post_psram_body(url, psram_buf, TEST_PSRAM_SIZE);
    log_mem_snapshot("after_post");

    /* Wait for server to finish receiving */
    for (int i = 0; i < 100 && !s_server.got_request; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    stop_test_server();

    printf("\n=== RESULT ===\n");
    printf("HTTP perform: %s (%s)\n", err == ESP_OK ? "OK" : "FAIL", esp_err_to_name(err));
    printf("Server received: %u / %u bytes\n",
           (unsigned int)s_server.received_bytes,
           (unsigned int)s_server.expected_bytes);
    printf("Content verified: %s\n", s_server.content_ok ? "YES" : "NO");
    printf("PSRAM body can be sent via set_post_field: %s\n",
           (err == ESP_OK && s_server.content_ok) ? "YES" : "NO");

    log_mem_snapshot("final");

    free(psram_buf);

    if (err == ESP_OK && s_server.content_ok) {
        printf("\nRESULT: PASS\n");
    } else {
        printf("\nRESULT: FAIL\n");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
