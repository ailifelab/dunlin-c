get_filename_component(DUNLIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
idf_component_register(SRCS "${DUNLIN_ROOT}/dunlin-compact/kernel/kernel.c" INCLUDE_DIRS "${DUNLIN_ROOT}/include")
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=missing-prototypes -Werror=implicit-function-declaration)
