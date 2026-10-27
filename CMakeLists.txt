cmake_minimum_required(VERSION 3.25)
project(Medicon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
include(FetchContent)
option(MEDICON_BUILD_WINDOWS "Build the Windows x64 DLL and GUI preview" ${WIN32})
set(MEDICON_SOCIAL_SERVER_URL "https://medicon.auau1000.com" CACHE STRING "Social server URL embedded in the client")

# Immutable revisions; downloaded only during configure, never at client startup.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
    URL https://codeload.github.com/nlohmann/json/zip/9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(json)

function(medicon_target target)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    endif()
endfunction()

add_library(medicon_memory STATIC
    src/memory/pattern.cpp
    src/memory/signature_catalog.cpp
    src/memory/signature_registry.cpp
    src/memory/module_view.cpp)
medicon_target(medicon_memory)

# Module .cpp additions are picked up automatically; register the type once in
# BuiltinModules.cpp so registration order and construction stay predictable.
file(GLOB_RECURSE MEDICON_MODULE_SOURCES CONFIGURE_DEPENDS "src/modules/*.cpp")
add_library(medicon_core STATIC
    src/commands/CommandManager.cpp
    src/core/Module.cpp
    src/core/ModuleManager.cpp
    src/core/ConfigStore.cpp
    src/core/Gameplay.cpp
    ${MEDICON_MODULE_SOURCES})
medicon_target(medicon_core)
target_link_libraries(medicon_core PUBLIC nlohmann_json::nlohmann_json)

add_library(medicon_mouse_capture STATIC src/platform/MouseCaptureController.cpp)
medicon_target(medicon_mouse_capture)

# Social protocol, settings and render-thread state: no WinHTTP, no ImGui.
add_library(medicon_social_core STATIC
    src/social/Config.cpp
    src/social/Protocol.cpp
    src/social/Store.cpp)
medicon_target(medicon_social_core)
target_compile_definitions(medicon_social_core PUBLIC MEDICON_SOCIAL_SERVER_URL="${MEDICON_SOCIAL_SERVER_URL}")
target_link_libraries(medicon_social_core PUBLIC nlohmann_json::nlohmann_json)

if(BUILD_TESTING)
    add_executable(medicon_command_tests tests/command_tests.cpp)
    medicon_target(medicon_command_tests)
    target_link_libraries(medicon_command_tests PRIVATE medicon_core)
    add_test(NAME commands COMMAND medicon_command_tests)
    add_executable(medicon_core_tests tests/core_tests.cpp)
    medicon_target(medicon_core_tests)
    target_link_libraries(medicon_core_tests PRIVATE medicon_core)
    add_test(NAME core COMMAND medicon_core_tests)
    add_executable(medicon_gameplay_module_tests tests/gameplay_module_tests.cpp)
    medicon_target(medicon_gameplay_module_tests)
    target_link_libraries(medicon_gameplay_module_tests PRIVATE medicon_core)
    add_test(NAME gameplay_modules COMMAND medicon_gameplay_module_tests)
    add_executable(medicon_signature_tests tests/signature_tests.cpp)
    medicon_target(medicon_signature_tests)
    target_link_libraries(medicon_signature_tests PRIVATE medicon_memory)
    add_test(NAME signatures COMMAND medicon_signature_tests)
    add_executable(medicon_mouse_capture_tests tests/mouse_capture_tests.cpp)
    medicon_target(medicon_mouse_capture_tests)
    target_link_libraries(medicon_mouse_capture_tests PRIVATE medicon_mouse_capture)
    add_test(NAME mouse_capture COMMAND medicon_mouse_capture_tests)
    add_executable(medicon_social_tests tests/social_tests.cpp)
    medicon_target(medicon_social_tests)
    target_link_libraries(medicon_social_tests PRIVATE medicon_social_core)
    add_test(NAME social COMMAND medicon_social_tests)
endif()

if(MEDICON_BUILD_WINDOWS)
    include(cmake/EmbedFonts.cmake)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Medicon DLL and Preview require Windows x64.")
    endif()
    FetchContent_Declare(imgui
        URL https://codeload.github.com/ocornut/imgui/zip/f5befd2d29e66809cd1110a152e375a7f1981f06
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_Declare(minhook
        URL https://codeload.github.com/TsudaKageyu/minhook/zip/c3fcafdc10146beb5919319d0683e44e3c30d537
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(imgui minhook)
    add_library(medicon_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        src/gui/ImGuiContext.cpp)
    target_include_directories(medicon_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp
        "${PROJECT_SOURCE_DIR}/include")
    # PUBLIC: the backends compiled into the DLL and Preview must agree on GImGui.
    target_compile_definitions(medicon_imgui PUBLIC IMGUI_USER_CONFIG="medicon/gui/ImGuiConfig.hpp")

    # Shared by the DLL and the Preview: no host-specific code or #ifdefs here.
    add_library(medicon_gui STATIC
        src/gui/ArrayList.cpp
        src/gui/ArrayListAnimator.cpp
        src/gui/ClickGui.cpp
        src/gui/Draw.cpp
        src/gui/EdgeLit.cpp
        src/gui/Fonts.cpp
        src/gui/Glass.cpp
        src/gui/GameplayVisuals.cpp
        src/gui/Ime.cpp
        src/gui/InjectSplash.cpp
        src/gui/KeyNames.cpp
        src/gui/Log.cpp
        src/gui/MainMenu.cpp
        src/gui/Overlay.cpp
        src/gui/Shadow.cpp
        src/gui/SocialBackground.cpp
        src/gui/SocialChat.cpp
        src/gui/SocialFonts.cpp
        src/gui/SocialScreen.cpp
        src/gui/SocialUi.cpp
        src/gui/SocialWidgets.cpp
        src/gui/Textures.cpp
        src/gui/Toasts.cpp)
    medicon_target(medicon_gui)
    target_include_directories(medicon_gui PRIVATE "${MEDICON_EMBEDDED_FONTS_DIR}")
    target_link_libraries(medicon_gui PUBLIC medicon_core medicon_social_core medicon_imgui d3d11 d3dcompiler d2d1 windowscodecs ole32 imm32)

    # Social networking (spec 4.2): WinHTTP REST/WebSocket, DPAPI license, notification sound.
    add_library(medicon_social_net STATIC
        src/social/Files.cpp
        src/social/LicenseStore.cpp
        src/social/Service.cpp
        src/social/SocialApi.cpp
        src/social/SocialNet.cpp
        src/social/Sound.cpp
        src/social/WinHttp.cpp)
    medicon_target(medicon_social_net)
    target_link_libraries(medicon_social_net PUBLIC medicon_social_core winhttp crypt32 winmm shell32 ole32)

    add_library(medicon_bedrock STATIC src/sdk/BedrockChat.cpp src/sdk/BedrockMenu.cpp src/sdk/VanillaNavigation.cpp src/sdk/Gameplay.cpp)
    medicon_target(medicon_bedrock)
    target_link_libraries(medicon_bedrock PUBLIC medicon_memory version)

    add_library(medicon_runtime STATIC src/client/Client.cpp src/hooks/HookManager.cpp)
    medicon_target(medicon_runtime)
    target_link_libraries(medicon_runtime PUBLIC medicon_core medicon_memory medicon_gui medicon_bedrock medicon_mouse_capture minhook shell32 ole32)

    add_library(Medicon SHARED
        src/platform/DllMain.cpp
        src/platform/ClientApi.cpp
        src/platform/AutoOverlay.cpp
        src/platform/OverlayD3D11.cpp
        src/platform/OverlayD3D12.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    medicon_target(Medicon)
    target_compile_definitions(Medicon PRIVATE MEDICON_EXPORTS MEDICON_VERSION="${PROJECT_VERSION}")
    target_link_libraries(Medicon PRIVATE medicon_runtime medicon_social_net d3d11 d3d12 dxgi d3dcompiler dwmapi imm32)
    set_target_properties(Medicon PROPERTIES PREFIX "" OUTPUT_NAME "Medicon")

    add_executable(MediconPreview WIN32
        src/preview/Main.cpp
        src/preview/DemoModules.cpp
        src/preview/ImageIO.cpp
        src/preview/Scenarios.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    medicon_target(MediconPreview)
    target_compile_definitions(MediconPreview PRIVATE MEDICON_VERSION="${PROJECT_VERSION}")
    target_link_libraries(MediconPreview PRIVATE medicon_gui medicon_social_net d3d11 dxgi d3dcompiler dwmapi windowscodecs ole32 shell32)
    if(MINGW)
        target_link_options(MediconPreview PRIVATE -municode)
    endif()

    # Shadow reference images and screenshot comparison (spec 5.5).
    add_executable(medicon_shadow_reference tools/shadow_reference.cpp src/preview/ImageIO.cpp)
    medicon_target(medicon_shadow_reference)
    target_include_directories(medicon_shadow_reference PRIVATE src/preview)
    target_link_libraries(medicon_shadow_reference PRIVATE windowscodecs ole32)

    if(BUILD_TESTING)
        add_executable(medicon_bedrock_chat_tests tests/bedrock_chat_tests.cpp)
        medicon_target(medicon_bedrock_chat_tests)
        target_link_libraries(medicon_bedrock_chat_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_chat COMMAND medicon_bedrock_chat_tests)
        add_executable(medicon_bedrock_gameplay_tests tests/bedrock_gameplay_tests.cpp)
        medicon_target(medicon_bedrock_gameplay_tests)
        target_link_libraries(medicon_bedrock_gameplay_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_gameplay COMMAND medicon_bedrock_gameplay_tests)
        add_executable(medicon_bedrock_menu_tests tests/bedrock_menu_tests.cpp)
        medicon_target(medicon_bedrock_menu_tests)
        target_link_libraries(medicon_bedrock_menu_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_menu COMMAND medicon_bedrock_menu_tests)
        add_executable(medicon_vanilla_navigation_tests tests/vanilla_navigation_tests.cpp)
        medicon_target(medicon_vanilla_navigation_tests)
        target_link_libraries(medicon_vanilla_navigation_tests PRIVATE medicon_bedrock)
        add_test(NAME vanilla_navigation COMMAND medicon_vanilla_navigation_tests)
        add_test(NAME preview_smoke COMMAND MediconPreview --smoke-test)
        set_tests_properties(preview_smoke PROPERTIES TIMEOUT 30)
        add_executable(medicon_runtime_tests tests/runtime_tests.cpp)
        medicon_target(medicon_runtime_tests)
        target_link_libraries(medicon_runtime_tests PRIVATE medicon_runtime)
        add_test(NAME runtime COMMAND medicon_runtime_tests)
        add_executable(medicon_gameplay_visual_tests tests/gameplay_visual_tests.cpp)
        medicon_target(medicon_gameplay_visual_tests)
        target_link_libraries(medicon_gameplay_visual_tests PRIVATE medicon_gui)
        add_test(NAME gameplay_visuals COMMAND medicon_gameplay_visual_tests)
        add_executable(medicon_dll_tests tests/dll_tests.cpp)
        medicon_target(medicon_dll_tests)
        target_link_libraries(medicon_dll_tests PRIVATE medicon_imgui)
        add_dependencies(medicon_dll_tests Medicon)
        add_test(NAME dll_lifecycle COMMAND medicon_dll_tests $<TARGET_FILE:Medicon>)
        add_executable(medicon_overlay_host_tests tests/overlay_host_tests.cpp)
        medicon_target(medicon_overlay_host_tests)
        target_link_libraries(medicon_overlay_host_tests PRIVATE d3d11 d3d12 dxgi user32)
        add_dependencies(medicon_overlay_host_tests Medicon)
        add_test(NAME overlay_d3d11 COMMAND medicon_overlay_host_tests $<TARGET_FILE:Medicon>)
        add_test(NAME overlay_d3d11_late COMMAND medicon_overlay_host_tests $<TARGET_FILE:Medicon> --late-load)
        add_test(NAME overlay_d3d12 COMMAND medicon_overlay_host_tests $<TARGET_FILE:Medicon> --d3d12)
        add_test(NAME overlay_d3d12_late COMMAND medicon_overlay_host_tests $<TARGET_FILE:Medicon> --d3d12 --late-load)
        add_test(NAME overlay_present1_threaded COMMAND medicon_overlay_host_tests $<TARGET_FILE:Medicon> --d3d12 --late-load --present1 --threaded-input)
        set_tests_properties(overlay_d3d11 overlay_d3d11_late overlay_d3d12 overlay_d3d12_late overlay_present1_threaded
            PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
        add_executable(medicon_gui_tests tests/gui_tests.cpp)
        medicon_target(medicon_gui_tests)
        target_link_libraries(medicon_gui_tests PRIVATE medicon_gui)
        target_compile_definitions(medicon_gui_tests PRIVATE
            MEDICON_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
        add_test(NAME gui COMMAND medicon_gui_tests)
        add_executable(medicon_font_tests tests/font_tests.cpp)
        medicon_target(medicon_font_tests)
        target_link_libraries(medicon_font_tests PRIVATE medicon_gui)
        add_test(NAME fonts COMMAND medicon_font_tests)
        add_executable(medicon_inject_splash_tests tests/inject_splash_tests.cpp)
        medicon_target(medicon_inject_splash_tests)
        target_link_libraries(medicon_inject_splash_tests PRIVATE medicon_gui)
        add_test(NAME inject_splash COMMAND medicon_inject_splash_tests)
        add_executable(medicon_main_menu_tests tests/main_menu_tests.cpp)
        medicon_target(medicon_main_menu_tests)
        target_link_libraries(medicon_main_menu_tests PRIVATE medicon_gui)
        add_test(NAME main_menu COMMAND medicon_main_menu_tests)
        add_executable(medicon_social_ui_tests tests/social_ui_tests.cpp)
        medicon_target(medicon_social_ui_tests)
        target_link_libraries(medicon_social_ui_tests PRIVATE medicon_gui medicon_social_net)
        add_test(NAME social_ui COMMAND medicon_social_ui_tests)
        add_test(NAME preview_toasts COMMAND MediconPreview --scenario toasts --size 1920x1080 --fixed-dt 0.016666667)
        set_tests_properties(preview_toasts PROPERTIES TIMEOUT 30)
        add_test(NAME preview_social_home COMMAND MediconPreview --scenario social_home --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_device_recreate COMMAND MediconPreview --scenario social_device_recreate --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_changelog COMMAND MediconPreview --scenario social_changelog --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_main_menu_click COMMAND MediconPreview --scenario main_menu_social_click --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_toast_click COMMAND MediconPreview --scenario toasts_click --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_typing COMMAND MediconPreview --scenario social_typing --size 1280x720 --fixed-dt 0.01)
        add_test(NAME preview_social_global COMMAND MediconPreview --scenario social_global --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_dm COMMAND MediconPreview --scenario social_dm --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_friends COMMAND MediconPreview --scenario social_friends --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_friend_remove COMMAND MediconPreview --scenario social_friend_remove --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_admin COMMAND MediconPreview --scenario social_admin --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_changelog_marks COMMAND MediconPreview --scenario social_changelog_marks --size 1280x720 --fixed-dt 0.016666667)
        set_tests_properties(preview_social_global preview_social_dm preview_social_friends preview_social_friend_remove
                             preview_social_admin preview_social_changelog_marks PROPERTIES TIMEOUT 30)
        set_tests_properties(preview_social_home preview_social_device_recreate preview_social_changelog
                             preview_social_main_menu_click preview_social_toast_click preview_social_typing PROPERTIES TIMEOUT 30)
        add_test(NAME preview_main_menu_device_recreate COMMAND MediconPreview --main-menu-time 1.5 --size 1280x720 --simulate-device-lost 0)
        set_tests_properties(preview_main_menu_device_recreate PROPERTIES TIMEOUT 30)
        add_test(NAME preview_hud_scale_cycle COMMAND MediconPreview --scenario edgelit_scale_cycle --size 1920x1080 --fixed-dt 0.016666667)
        set_tests_properties(preview_hud_scale_cycle PROPERTIES TIMEOUT 30)
        add_test(NAME preview_input COMMAND MediconPreview --input-test)
        set_tests_properties(preview_input PROPERTIES TIMEOUT 60)
    endif()
endif()

