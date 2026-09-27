set(CALIBRATION_DIR "${PROJECT_DIR}/calibration")
set(GENERATOR_SCRIPT "${PROJECT_DIR}/tools/generate_linear_actuator_calibration.py")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_HEADER "${GENERATED_DIR}/linear_actuator_calibration_data.h")
set(GENERATED_SOURCE "${GENERATED_DIR}/linear_actuator_calibration_data.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB CALIBRATION_CSVS CONFIGURE_DEPENDS
        "${CALIBRATION_DIR}/servo_*.csv")
    file(MAKE_DIRECTORY "${GENERATED_DIR}")

    execute_process(
        COMMAND "${PYTHON}" "${GENERATOR_SCRIPT}"
                --calibration-dir "${CALIBRATION_DIR}"
                --output-header "${GENERATED_HEADER}"
                --output-source "${GENERATED_SOURCE}"
        RESULT_VARIABLE GENERATOR_RESULT
        OUTPUT_VARIABLE GENERATOR_OUTPUT
        ERROR_VARIABLE GENERATOR_ERROR
    )
    if(NOT GENERATOR_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Linear actuator calibration generation failed:\n"
            "${GENERATOR_OUTPUT}${GENERATOR_ERROR}")
    endif()
    string(STRIP "${GENERATOR_OUTPUT}" GENERATOR_OUTPUT)
    message(STATUS "${GENERATOR_OUTPUT}")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${GENERATOR_SCRIPT}")
    foreach(CALIBRATION_CSV IN LISTS CALIBRATION_CSVS)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CALIBRATION_CSV}")
    endforeach()
endif()

idf_component_register(
    SRCS "linear_actuator.c" "${GENERATED_SOURCE}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${GENERATED_DIR}"
    REQUIRES servo_control
)
