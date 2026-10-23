cmake_minimum_required(VERSION 3.25)

project(MyProcessMonitor VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MPM_BUILD_TESTS "Build MyProcessMonitor unit tests" ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
if(MPM_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
    add_link_options(/DYNAMICBASE /NXCOMPAT)
endif()

add_library(mpm_core STATIC
    src/core/cpu_math.cpp
    src/core/detector.cpp
    src/core/types.cpp
)
target_include_directories(mpm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(mpm_win32 STATIC
    src/win32/idle_provider.cpp
    src/win32/process_catalog.cpp
    src/win32/system_sampler.cpp
)
target_link_libraries(mpm_win32
    PUBLIC mpm_core
    PRIVATE ntdll psapi wintrust crypt32 wtsapi32 shell32 user32 advapi32 ole32
)

add_library(mpm_infra STATIC
    src/infra/config_loader.cpp
    src/infra/event_json.cpp
    src/infra/logging.cpp
    src/infra/paths.cpp
    src/infra/sqlite_event_store.cpp
    src/infra/text.cpp
)
target_link_libraries(mpm_infra
    PUBLIC
        mpm_core
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    PRIVATE
        unofficial::sqlite3::sqlite3
        shell32
        ole32
)

add_library(mpm_app STATIC
    src/app/monitor_session.cpp
)
target_link_libraries(mpm_app PUBLIC mpm_core mpm_win32 mpm_infra)

add_executable(MyProcessMonitor WIN32
    src/app/tray_main.cpp
    resources/app.rc
)
target_link_libraries(MyProcessMonitor PRIVATE mpm_app comctl32)
if(MSVC)
    # The manifest is embedded as resource 1 by app.rc. Disable the linker's
    # generated manifest so the two do not collide.
    target_link_options(MyProcessMonitor PRIVATE /MANIFEST:NO)
endif()

add_executable(mpm-cli
    src/cli/main.cpp
)
target_link_libraries(mpm-cli PRIVATE mpm_app)

if(MPM_BUILD_TESTS)
    enable_testing()
    add_executable(mpm_tests
        tests/test_main.cpp
        tests/test_detector.cpp
        tests/test_config.cpp
        tests/test_store.cpp
    )
    target_link_libraries(mpm_tests
        PRIVATE
            mpm_core
            mpm_win32
            mpm_infra
            GTest::gtest_main
    )
    add_test(NAME mpm_tests COMMAND mpm_tests)
endif()

install(TARGETS MyProcessMonitor mpm-cli RUNTIME DESTINATION .)
install(FILES resources/config.example.json README.md DESTINATION .)
install(FILES docs/ARCHITECTURE.md DESTINATION docs)