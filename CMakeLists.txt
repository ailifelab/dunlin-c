cmake_minimum_required(VERSION 3.20)
project(dunlin_c_kernel VERSION 0.2.5 LANGUAGES C)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Optional static PNG resources and SQLite associations. No agent/UI/host.
option(DUNLIN_BUILD_IMAGE_RESOURCES "Build independent C PNG resource store" OFF)
if(DUNLIN_BUILD_IMAGE_RESOURCES)
    find_package(ZLIB REQUIRED)
    find_package(SQLite3 REQUIRED)
    add_library(dunlin_image_resources STATIC modules/image_resource/png.c
        modules/image_resource/store.c modules/content/hash.c)
    add_library(DunlinC::ImageResources ALIAS dunlin_image_resources)
    target_include_directories(dunlin_image_resources PUBLIC include)
    target_compile_features(dunlin_image_resources PUBLIC c_std_11)
    target_link_libraries(dunlin_image_resources PUBLIC SQLite::SQLite3 ZLIB::ZLIB)
    if(BUILD_TESTING)
        enable_testing()
        add_executable(dunlin_image_resources_test tests/image_resource.c)
        target_link_libraries(dunlin_image_resources_test PRIVATE DunlinC::ImageResources)
        add_test(NAME dunlin.image-resources.standalone COMMAND dunlin_image_resources_test)
    endif()
endif()

# Optional exact PNG/text resources. Independent of kernel, UI and host runtime.
option(DUNLIN_BUILD_ATTACHMENTS "Build independent C attachment store" OFF)
if(DUNLIN_BUILD_ATTACHMENTS)
    find_package(ZLIB REQUIRED)
    find_package(SQLite3 3.34 REQUIRED)
    add_library(dunlin_attachments STATIC modules/attachment_store/store.c
        modules/image_resource/png.c modules/content/hash.c)
    add_library(DunlinC::Attachments ALIAS dunlin_attachments)
    target_include_directories(dunlin_attachments PUBLIC include)
    target_compile_features(dunlin_attachments PUBLIC c_std_11)
    target_link_libraries(dunlin_attachments PUBLIC SQLite::SQLite3 ZLIB::ZLIB)
    if(BUILD_TESTING)
        enable_testing()
        add_executable(dunlin_attachment_store_test tests/attachment_store.c)
        target_link_libraries(dunlin_attachment_store_test PRIVATE DunlinC::Attachments)
        add_test(NAME dunlin.attachments.standalone COMMAND dunlin_attachment_store_test)
    endif()
endif()

# Optional public text projection. No runtime, database, UI or host dependency.
option(DUNLIN_BUILD_MESSAGE_PROJECTION "Build public message text projection" OFF)
if(DUNLIN_BUILD_MESSAGE_PROJECTION)
    find_package(json-c CONFIG REQUIRED)
    add_library(dunlin_message_projection STATIC modules/message_projection/projection.c)
    add_library(DunlinC::MessageProjection ALIAS dunlin_message_projection)
    target_include_directories(dunlin_message_projection PUBLIC include)
    target_compile_features(dunlin_message_projection PUBLIC c_std_11)
    target_link_libraries(dunlin_message_projection PUBLIC json-c::json-c)
    if(BUILD_TESTING)
        enable_testing()
        add_executable(dunlin_message_projection_test tests/message_projection.c)
        target_link_libraries(dunlin_message_projection_test PRIVATE DunlinC::MessageProjection)
        add_test(NAME dunlin.message-projection COMMAND dunlin_message_projection_test)
    endif()
endif()

# Optional data-only YAML/JSON parser. No kernel, agent, UI, Node or filesystem.
option(DUNLIN_BUILD_STATIC_CONFIG "Build bounded static configuration parser" OFF)
if(DUNLIN_BUILD_STATIC_CONFIG)
    find_package(json-c CONFIG REQUIRED)
    add_library(dunlin_static_config STATIC modules/config_yaml/static_yaml.c
        vendor/libyaml/src/api.c vendor/libyaml/src/reader.c
        vendor/libyaml/src/scanner.c vendor/libyaml/src/parser.c)
    add_library(DunlinC::StaticConfig ALIAS dunlin_static_config)
    target_include_directories(dunlin_static_config PUBLIC include PRIVATE vendor/libyaml/include)
    target_compile_features(dunlin_static_config PUBLIC c_std_11)
    target_compile_definitions(dunlin_static_config PRIVATE YAML_DECLARE_STATIC=1
        YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=6
        YAML_VERSION_STRING="0.2.6-rc.1")
    target_link_libraries(dunlin_static_config PUBLIC json-c::json-c)
    if(BUILD_TESTING)
        enable_testing()
        add_executable(dunlin_static_config_test tests/static_config_standalone.c)
        target_link_libraries(dunlin_static_config_test PRIVATE DunlinC::StaticConfig)
        add_test(NAME dunlin.static-config.standalone COMMAND dunlin_static_config_test)
    endif()
endif()

# Optional local observed execution ledger. No agent/UI/DSH dependency.
option(DUNLIN_BUILD_TRAJECTORY "Build local C execution trajectory (POSIX host)" OFF)
if(DUNLIN_BUILD_TRAJECTORY)
    if(WIN32)
        message(FATAL_ERROR "Trajectory file ownership/locking requires a Windows adapter")
    endif()
    find_package(SQLite3 REQUIRED)
    find_package(json-c CONFIG REQUIRED)
    add_library(dunlin_trajectory STATIC modules/trajectory/trajectory.c)
    add_library(DunlinC::Trajectory ALIAS dunlin_trajectory)
    target_include_directories(dunlin_trajectory PUBLIC include)
    target_compile_features(dunlin_trajectory PUBLIC c_std_11)
    target_link_libraries(dunlin_trajectory PUBLIC SQLite::SQLite3 json-c::json-c)
    if(BUILD_TESTING)
        enable_testing()
        add_executable(dunlin_trajectory_test tests/trajectory_standalone.c)
        target_link_libraries(dunlin_trajectory_test PRIVATE DunlinC::Trajectory)
        add_test(NAME dunlin.trajectory.standalone COMMAND dunlin_trajectory_test)
    endif()
endif()

# The desktop runtime still has POSIX platform dependencies.  Keep this entry
# point limited to the portable C11 kernel until Windows platform services have
# explicit implementations and tests.
add_library(dunlin_kernel STATIC dunlin-compact/kernel/kernel.c)
add_library(DunlinC::Kernel ALIAS dunlin_kernel)
target_include_directories(dunlin_kernel PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(dunlin_kernel PUBLIC c_std_11)
set_target_properties(dunlin_kernel PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

if(MSVC)
    target_compile_options(dunlin_kernel PRIVATE /W4)
else()
    target_compile_options(dunlin_kernel PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    find_package(json-c CONFIG REQUIRED)
    find_package(CURL REQUIRED)
    add_library(dunlin_protocol STATIC
        dunlin-compact/runtime/json.c
        dunlin-compact/runtime/profile.c
        dunlin-compact/plugins/text.c
        dunlin-compact/plugins/png.c
        dunlin-compact/plugins/model.c
        dunlin-compact/plugins/protocol.c
        dunlin-compact/plugins/protocol_responses.c
        dunlin-compact/plugins/protocol_anthropic.c
        dunlin-compact/plugins/protocol_ollama.c
        dunlin-compact/plugins/protocol_gemini.c
        dunlin-compact/platform/http.c
        dunlin-compact/platform/http2.c
        dunlin-compact/platform/windows/session.c)
    add_library(DunlinC::Protocol ALIAS dunlin_protocol)
    target_include_directories(dunlin_protocol PRIVATE include dunlin-compact/plugins dunlin-compact/platform)
    target_link_libraries(dunlin_protocol PUBLIC DunlinC::Platform json-c::json-c CURL::libcurl ws2_32)
    target_compile_features(dunlin_protocol PUBLIC c_std_11)
    target_compile_options(dunlin_protocol PRIVATE /W4)

    add_library(dunlin_agent STATIC dunlin-compact/plugins/loop.c dunlin-compact/runtime/config.c dunlin-compact/platform/http_get.c dunlin-compact/platform/windows/tools.c)
    add_library(DunlinC::Agent ALIAS dunlin_agent)
    target_include_directories(dunlin_agent PRIVATE include dunlin-compact/plugins dunlin-compact/runtime)
    target_link_libraries(dunlin_agent PUBLIC DunlinC::Protocol)
    target_compile_features(dunlin_agent PUBLIC c_std_11)
    target_compile_options(dunlin_agent PRIVATE /W4)

    add_library(dunlin_runtime STATIC
        dunlin-compact/runtime/runtime.c dunlin-compact/runtime/intent.c dunlin-compact/runtime/profile_hash.c dunlin-compact/runtime/credential.c)
    add_library(DunlinC::Runtime ALIAS dunlin_runtime)
    target_include_directories(dunlin_runtime PRIVATE include dunlin-compact/plugins dunlin-compact/runtime dunlin-compact/platform)
    target_link_libraries(dunlin_runtime PUBLIC DunlinC::Kernel DunlinC::Agent)
    target_compile_features(dunlin_runtime PUBLIC c_std_11)
    target_compile_options(dunlin_runtime PRIVATE /W4)

    add_executable(dunlin dunlin-command/src/windows_main.c dunlin-compact/platform/windows/console.c)
    target_include_directories(dunlin PRIVATE include dunlin-compact/platform/windows)
    target_link_libraries(dunlin PRIVATE DunlinC::Runtime)
    target_compile_options(dunlin PRIVATE /W4)
endif()

add_library(dunlin_platform STATIC
    dunlin-compact/platform/clock.c
    dunlin-compact/platform/random.c)
add_library(DunlinC::Platform ALIAS dunlin_platform)
target_include_directories(dunlin_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dunlin-compact/platform)
target_compile_features(dunlin_platform PUBLIC c_std_11)
set_target_properties(dunlin_platform PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(WIN32)
    target_link_libraries(dunlin_platform PUBLIC bcrypt)
    target_sources(dunlin_platform PRIVATE dunlin-compact/platform/windows/store.c dunlin-compact/platform/windows/process.c)
    target_link_libraries(dunlin_platform PUBLIC advapi32)
endif()
if(MSVC)
    target_compile_options(dunlin_platform PRIVATE /W4)
else()
    target_compile_options(dunlin_platform PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(dunlin_kernel_test tests/kernel.c)
    target_link_libraries(dunlin_kernel_test PRIVATE DunlinC::Kernel)
    target_compile_features(dunlin_kernel_test PRIVATE c_std_11)
    set_target_properties(dunlin_kernel_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(dunlin_kernel_test PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(dunlin_kernel_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endif()
    add_test(NAME dunlin.kernel.unit COMMAND dunlin_kernel_test)

    add_executable(dunlin_platform_clock_test tests/platform_clock.c)
    target_include_directories(dunlin_platform_clock_test PRIVATE include dunlin-compact/platform)
    target_link_libraries(dunlin_platform_clock_test PRIVATE DunlinC::Platform)
    target_compile_features(dunlin_platform_clock_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(dunlin_platform_clock_test PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(dunlin_platform_clock_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endif()
    add_test(NAME dunlin.platform.clock COMMAND dunlin_platform_clock_test)

    add_executable(dunlin_platform_random_test tests/platform_random.c)
    target_include_directories(dunlin_platform_random_test PRIVATE dunlin-compact/platform)
    target_link_libraries(dunlin_platform_random_test PRIVATE DunlinC::Platform)
    target_compile_features(dunlin_platform_random_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(dunlin_platform_random_test PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(dunlin_platform_random_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endif()
    add_test(NAME dunlin.platform.random COMMAND dunlin_platform_random_test)

    if(WIN32)
        add_executable(dunlin_windows_store_test tests/windows_store.c)
        target_include_directories(dunlin_windows_store_test PRIVATE dunlin-compact/platform/windows)
        target_link_libraries(dunlin_windows_store_test PRIVATE DunlinC::Platform)
        target_compile_features(dunlin_windows_store_test PRIVATE c_std_11)
        if(MSVC)
            target_compile_options(dunlin_windows_store_test PRIVATE /W4 /UNDEBUG)
        else()
            target_compile_options(dunlin_windows_store_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        endif()
        add_test(NAME dunlin.windows.store COMMAND dunlin_windows_store_test)

        add_executable(dunlin_windows_process_test tests/windows_process.c)
        target_include_directories(dunlin_windows_process_test PRIVATE dunlin-compact/platform/windows)
        target_link_libraries(dunlin_windows_process_test PRIVATE DunlinC::Platform)
        target_compile_features(dunlin_windows_process_test PRIVATE c_std_11)
        target_compile_options(dunlin_windows_process_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.process COMMAND dunlin_windows_process_test)

        add_executable(dunlin_model_test tests/model_unit.c)
        target_include_directories(dunlin_model_test PRIVATE include)
        target_link_libraries(dunlin_model_test PRIVATE DunlinC::Protocol)
        target_compile_options(dunlin_model_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.model.unit COMMAND dunlin_model_test)

        add_executable(dunlin_protocol_test tests/protocol_unit.c tests/protocol_responses_unit.c)
        target_include_directories(dunlin_protocol_test PRIVATE include)
        target_link_libraries(dunlin_protocol_test PRIVATE DunlinC::Protocol)
        target_compile_options(dunlin_protocol_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.protocol.unit COMMAND dunlin_protocol_test)

        add_executable(dunlin_windows_session_test tests/windows_session.c)
        target_include_directories(dunlin_windows_session_test PRIVATE include dunlin-compact/platform/windows)
        target_link_libraries(dunlin_windows_session_test PRIVATE DunlinC::Protocol)
        target_compile_options(dunlin_windows_session_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.session COMMAND dunlin_windows_session_test)

        add_executable(dunlin_windows_agent_test tests/windows_agent.c)
        target_include_directories(dunlin_windows_agent_test PRIVATE include dunlin-compact/platform)
        target_link_libraries(dunlin_windows_agent_test PRIVATE DunlinC::Agent)
        target_compile_options(dunlin_windows_agent_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.agent COMMAND dunlin_windows_agent_test)

        add_executable(dunlin_windows_profiles_test tests/windows_profiles.c)
        target_include_directories(dunlin_windows_profiles_test PRIVATE include dunlin-compact/runtime)
        target_link_libraries(dunlin_windows_profiles_test PRIVATE DunlinC::Protocol)
        target_compile_options(dunlin_windows_profiles_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.profiles COMMAND dunlin_windows_profiles_test)

        add_executable(dunlin_windows_config_test tests/windows_config.c)
        target_include_directories(dunlin_windows_config_test PRIVATE include dunlin-compact/runtime)
        target_link_libraries(dunlin_windows_config_test PRIVATE DunlinC::Agent)
        target_compile_options(dunlin_windows_config_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.config COMMAND dunlin_windows_config_test)

        add_executable(dunlin_windows_tools_test tests/windows_tools.c)
        target_include_directories(dunlin_windows_tools_test PRIVATE include dunlin-compact/platform dunlin-compact/platform/windows)
        target_link_libraries(dunlin_windows_tools_test PRIVATE DunlinC::Agent)
        target_compile_options(dunlin_windows_tools_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.tools COMMAND dunlin_windows_tools_test)

        add_executable(dunlin_windows_preset_test tests/windows_preset.c)
        target_include_directories(dunlin_windows_preset_test PRIVATE include)
        target_link_libraries(dunlin_windows_preset_test PRIVATE DunlinC::Runtime)
        target_compile_options(dunlin_windows_preset_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.preset COMMAND dunlin_windows_preset_test)

        add_executable(dunlin_windows_console_test tests/windows_console.c dunlin-compact/platform/windows/console.c)
        target_include_directories(dunlin_windows_console_test PRIVATE dunlin-compact/platform/windows)
        target_compile_options(dunlin_windows_console_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.console COMMAND dunlin_windows_console_test)

        add_executable(dunlin_windows_cli_test tests/windows_cli.c)
        target_include_directories(dunlin_windows_cli_test PRIVATE include dunlin-compact/platform/windows)
        target_link_libraries(dunlin_windows_cli_test PRIVATE DunlinC::Agent)
        target_compile_options(dunlin_windows_cli_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.windows.cli COMMAND dunlin_windows_cli_test $<TARGET_FILE:dunlin>)

        add_executable(dunlin_permission_modes_test tests/permission_modes.c)
        target_include_directories(dunlin_permission_modes_test PRIVATE include)
        target_link_libraries(dunlin_permission_modes_test PRIVATE DunlinC::Runtime)
        target_compile_options(dunlin_permission_modes_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.permission.modes COMMAND dunlin_permission_modes_test)

        add_executable(dunlin_credential_broker_test tests/credential_broker.c)
        target_include_directories(dunlin_credential_broker_test PRIVATE include)
        target_link_libraries(dunlin_credential_broker_test PRIVATE DunlinC::Runtime)
        target_compile_options(dunlin_credential_broker_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.credential.broker COMMAND dunlin_credential_broker_test)

        add_executable(dunlin_http_get_test tests/http_get.c)
        target_include_directories(dunlin_http_get_test PRIVATE include dunlin-compact/platform)
        target_link_libraries(dunlin_http_get_test PRIVATE DunlinC::Runtime)
        target_compile_options(dunlin_http_get_test PRIVATE /W4 /UNDEBUG)
        add_test(NAME dunlin.http_get.policy COMMAND dunlin_http_get_test)
    endif()
endif()

# Optional, independently embeddable local lexical index. No agent/UI dependency.
option(DUNLIN_BUILD_HISTORY_INDEX "Build local C history index" OFF)
if(DUNLIN_BUILD_HISTORY_INDEX)
    find_package(SQLite3 REQUIRED)
    add_library(dunlin_history_index STATIC modules/history_index/index.c)
    add_library(DunlinC::HistoryIndex ALIAS dunlin_history_index)
    target_include_directories(dunlin_history_index PUBLIC include)
    target_compile_features(dunlin_history_index PUBLIC c_std_11)
    target_link_libraries(dunlin_history_index PUBLIC SQLite::SQLite3)
endif()
