set(scd41_srcs
    "src/lua_module_scd41_sensor.c"
)

set(scd41_requires
    cap_lua
)

set(scd41_priv_requires
    espressif__esp_board_manager
    esp_driver_i2c
    claw_hw_registry
)

idf_component_register(
    SRCS
        ${scd41_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        ${scd41_requires}
    PRIV_REQUIRES
        ${scd41_priv_requires}
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
