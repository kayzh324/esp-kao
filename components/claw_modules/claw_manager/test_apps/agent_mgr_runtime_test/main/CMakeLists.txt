idf_component_register(SRCS "test_app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES claw_manager cap_agent_mgr claw_core claw_cap esp_timer unity fatfs wear_levelling
                       WHOLE_ARCHIVE)

# The report-spill unit test calls claw_agent_mgr_report_spill() directly, so it
# needs claw_manager's private src/ (the declaration is deliberately not public).
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../src")
