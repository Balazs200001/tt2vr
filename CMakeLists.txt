cmake_minimum_required(VERSION 3.28)
project(tt2vr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the mod does not depend on the user's installed VC runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(spdlog CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

# Warnings as errors for our targets only (CLAUDE.md: /W4 /WX).
function(tt2vr_warnings target)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endfunction()

# ---- tt2vr.dll: the mod itself, loaded from <game>\tt2vr\ by the proxy ----
add_library(tt2vr SHARED
    src/core/log.cpp
    src/core/paths.cpp
    src/d3d/device_hooks.cpp
    src/main.cpp
)
target_include_directories(tt2vr PRIVATE src)
target_link_libraries(tt2vr PRIVATE spdlog::spdlog minhook::minhook)
tt2vr_warnings(tt2vr)

# ---- dinput8.dll: tiny forwarding proxy dropped into the game root ----
add_library(proxy SHARED
    src/proxy/dinput8_proxy.cpp
    src/proxy/dinput8.def
)
set_target_properties(proxy PROPERTIES OUTPUT_NAME dinput8)
tt2vr_warnings(proxy)

# ---- deploy: copy into TT2_GAME_DIR (never hard-code the game path) ----
if(DEFINED ENV{TT2_GAME_DIR})
    cmake_path(SET TT2_GAME_DIR NORMALIZE "$ENV{TT2_GAME_DIR}")
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TT2_GAME_DIR}/tt2vr/logs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:proxy>" "${TT2_GAME_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:tt2vr>" "${TT2_GAME_DIR}/tt2vr/"
        DEPENDS proxy tt2vr
        COMMENT "Deploying to ${TT2_GAME_DIR} (fails while the game is running: DLL locked)"
        VERBATIM)
else()
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E echo "TT2_GAME_DIR is not set" COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
endif()
