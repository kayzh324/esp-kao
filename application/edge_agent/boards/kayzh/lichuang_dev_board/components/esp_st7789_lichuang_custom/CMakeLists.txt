idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
    REQUIRES driver
             esp_driver_i2c
             esp_lcd
    PRIV_REQUIRES esp_io_expander
    )
