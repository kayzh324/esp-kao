idf_component_register(SRCS "test_app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_http_client esp_event esp_netif lwip
                       WHOLE_ARCHIVE)
