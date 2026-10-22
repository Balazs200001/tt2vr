cmake_minimum_required(VERSION 3.28)
project(tt2vr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the mod does not depend on the user's installed VC runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(spdlog CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

# Warnings as errors for our targets only (CLAUDE.md: /W4 /WX).
function(tt2vr_warnings target)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endfunction()

# ---- tt2vr_core: code that does not need the game (also linked by the unit tests) ----
file(GLOB TT2VR_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(tt2vr_core STATIC ${TT2VR_CORE_SOURCES})
target_include_directories(tt2vr_core PUBLIC src)
target_link_libraries(tt2vr_core PUBLIC spdlog::spdlog minhook::minhook tomlplusplus::tomlplusplus bcrypt)
tt2vr_warnings(tt2vr_core)

# ---- tt2vr_math: engine-convention matrix code (projection, XR -> engine); pure, unit-tested ----
file(GLOB TT2VR_MATH_SOURCES CONFIGURE_DEPENDS src/stereo/math/*.cpp)
add_library(tt2vr_math STATIC ${TT2VR_MATH_SOURCES})
target_include_directories(tt2vr_math PUBLIC src)
tt2vr_warnings(tt2vr_math)

# ---- tt2vr.dll: the mod itself, loaded from <game>\tt2vr\ by the proxy ----
file(GLOB TT2VR_SOURCES CONFIGURE_DEPENDS
    src/main.cpp src/d3d/*.cpp src/game/*.cpp src/stereo/*.cpp src/ui/*.cpp src/xr/*.cpp)
add_library(tt2vr SHARED ${TT2VR_SOURCES})
# FSR1 shader headers as byte arrays for the runtime shader compiler (PLAN 9e; approved 2026-10-22).
include(cmake/embed_fsr1.cmake)
tt2vr_embed_fsr1("${CMAKE_BINARY_DIR}/generated")
target_include_directories(tt2vr PRIVATE src third_party "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(tt2vr PRIVATE tt2vr_core tt2vr_math imgui::imgui Microsoft::DirectXTK xxHash::xxhash
    OpenXR::openxr_loader d3d11 dxgi dxguid d3dcompiler windowscodecs ole32)
tt2vr_warnings(tt2vr)

# ---- dinput8.dll: tiny forwarding proxy dropped into the game root ----
add_library(proxy SHARED
    src/proxy/dinput8_proxy.cpp
    src/proxy/dinput8.def
)
set_target_properties(proxy PROPERTIES OUTPUT_NAME dinput8)
tt2vr_warnings(proxy)

# ---- unit tests (no game needed) ----
enable_testing()
file(GLOB TT2VR_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(tt2vr_tests ${TT2VR_TEST_SOURCES})
target_link_libraries(tt2vr_tests PRIVATE tt2vr_core tt2vr_math doctest::doctest)
tt2vr_warnings(tt2vr_tests)
add_test(NAME tt2vr_tests COMMAND tt2vr_tests)

# ---- deploy: copy into TT2_GAME_DIR (never hard-code the game path) ----
if(DEFINED ENV{TT2_GAME_DIR})
    cmake_path(SET TT2_GAME_DIR NORMALIZE "$ENV{TT2_GAME_DIR}")
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TT2_GAME_DIR}/tt2vr/logs" "${TT2_GAME_DIR}/tt2vr/captures"
            "${TT2_GAME_DIR}/tt2vr/commands"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:proxy>" "${TT2_GAME_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:tt2vr>" "${TT2_GAME_DIR}/tt2vr/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/config/tt2vr.toml" "${TT2_GAME_DIR}/tt2vr/"
        DEPENDS proxy tt2vr
        COMMENT "Deploying to ${TT2_GAME_DIR} (fails while the game is running: DLL locked)"
        VERBATIM)
else()
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E echo "TT2_GAME_DIR is not set" COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
endif()
