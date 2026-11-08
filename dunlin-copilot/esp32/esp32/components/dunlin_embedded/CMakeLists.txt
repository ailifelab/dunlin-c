get_filename_component(DUNLIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
idf_component_register(SRCS "${DUNLIN_ROOT}/dunlin-copilot/esp32/common/pool.c" "${DUNLIN_ROOT}/dunlin-copilot/esp32/common/probe.c" "${DUNLIN_ROOT}/dunlin-copilot/esp32/io/service.c" "${DUNLIN_ROOT}/dunlin-copilot/esp32/serial/commands.c" "${DUNLIN_ROOT}/dunlin-copilot/esp32/serial/serial.c" INCLUDE_DIRS "${DUNLIN_ROOT}/include" REQUIRES dunlin_kernel)
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=missing-prototypes -Werror=implicit-function-declaration)
