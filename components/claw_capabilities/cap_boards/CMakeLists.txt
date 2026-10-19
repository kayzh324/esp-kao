idf_component_register()

set(CAP_BOARDS_SKILLS_DIR "${COMPONENT_DIR}/skills")
set(CAP_BOARDS_GENERATOR "${COMPONENT_DIR}/tools/generate_board_skill.py")
set(CAP_BOARDS_METADATA_YAML "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/gen_board_metadata.yaml")
set(CAP_BOARDS_OUTPUT_MD "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info/SKILL.md")

# The authored board files carry facts bmgr's metadata export drops (device chip
# models and their bus configuration, e.g. the I2C address), which used to have to be
# repeated by hand in the board description. Resolve the board directory the same way
# the application does - from what the board-manager generator recorded - so this
# component stays self-contained; without it the skill degrades to inventory-only.
set(CAP_BOARDS_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")
set(CAP_BOARDS_BOARD_ARGS "")
set(CAP_BOARDS_BOARD_DEPS "")
if(EXISTS "${CAP_BOARDS_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${CAP_BOARDS_BOARD_GENERATED_CMAKELISTS}" CAP_BOARDS_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )
    if(CAP_BOARDS_BOARD_PATH_LINE)
        list(GET CAP_BOARDS_BOARD_PATH_LINE 0 CAP_BOARDS_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            CAP_BOARDS_BOARD_DIR
            "${CAP_BOARDS_BOARD_PATH_LINE}"
        )
        foreach(_yaml board_devices.yaml board_info.yaml)
            if(EXISTS "${CAP_BOARDS_BOARD_DIR}/${_yaml}")
                set(CAP_BOARDS_BOARD_DEPS "${CAP_BOARDS_BOARD_DEPS}" "${CAP_BOARDS_BOARD_DIR}/${_yaml}")
            endif()
        endforeach()
        if(EXISTS "${CAP_BOARDS_BOARD_DIR}/board_devices.yaml")
            set(CAP_BOARDS_BOARD_ARGS "${CAP_BOARDS_BOARD_ARGS}"
                --board-devices-yaml "${CAP_BOARDS_BOARD_DIR}/board_devices.yaml")
        endif()
        if(EXISTS "${CAP_BOARDS_BOARD_DIR}/board_info.yaml")
            set(CAP_BOARDS_BOARD_ARGS "${CAP_BOARDS_BOARD_ARGS}"
                --board-info-yaml "${CAP_BOARDS_BOARD_DIR}/board_info.yaml")
        endif()
    endif()
endif()

idf_build_get_property(python PYTHON)

add_custom_command(
    OUTPUT "${CAP_BOARDS_OUTPUT_MD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info"
    COMMAND ${python} "${CAP_BOARDS_GENERATOR}"
            --metadata-yaml "${CAP_BOARDS_METADATA_YAML}"
            --output-md "${CAP_BOARDS_OUTPUT_MD}"
            ${CAP_BOARDS_BOARD_ARGS}
    DEPENDS
        "${CAP_BOARDS_GENERATOR}"
        "${CAP_BOARDS_METADATA_YAML}"
        ${CAP_BOARDS_BOARD_DEPS}
    COMMENT "CLAW Generating cap_boards skill from gen_board_metadata.yaml + board definition"
    VERBATIM
)

add_custom_target(cap_boards_generate_skill DEPENDS "${CAP_BOARDS_OUTPUT_MD}")
add_dependencies(${COMPONENT_LIB} cap_boards_generate_skill)
skill_builder_add_target(cap_boards_generate_skill)
