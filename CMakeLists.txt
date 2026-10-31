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
set(MEDICON_UPDATE_HOST "medicon.auau1000.com" CACHE STRING "Host MediconLoader downloads Core releases from")
set(MEDICON_MSA_CLIENT_ID "" CACHE STRING "Application (client) ID of your Microsoft Entra app for AltManager sign-in; see docs/alt-manager.md")

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
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/permissive-;/utf-8;/EHsc>")
    else()
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
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

# Core auto-update rules shared by MediconLoader and its tests (docs/auto-update.md).
add_library(medicon_update STATIC src/update/Update.cpp)
medicon_target(medicon_update)
target_link_libraries(medicon_update PUBLIC nlohmann_json::nlohmann_json)

# The update orchestration (run_update), tested with fakes; the Win32 fetch,
# verify and storage live in the DLL target below.
add_library(medicon_loader_core STATIC src/loader/UpdateManager.cpp)
medicon_target(medicon_loader_core)
target_link_libraries(medicon_loader_core PUBLIC medicon_update)

add_library(medicon_mouse_capture STATIC src/platform/MouseCaptureController.cpp)
medicon_target(medicon_mouse_capture)

add_library(medicon_music_core STATIC src/music/AudioSpectrum.cpp)
medicon_target(medicon_music_core)

# Social protocol, settings and render-thread state: no WinHTTP, no ImGui.
add_library(medicon_social_core STATIC
    src/alt/Auth.cpp
    src/alt/Session.cpp
    src/social/Config.cpp
    src/social/Protocol.cpp
    src/social/Store.cpp)
medicon_target(medicon_social_core)
target_compile_definitions(medicon_social_core PUBLIC MEDICON_SOCIAL_SERVER_URL="${MEDICON_SOCIAL_SERVER_URL}"
    MEDICON_MSA_CLIENT_ID="${MEDICON_MSA_CLIENT_ID}")
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
    add_executable(medicon_update_tests tests/update_tests.cpp)
    medicon_target(medicon_update_tests)
    target_link_libraries(medicon_update_tests PRIVATE medicon_update)
    add_test(NAME update COMMAND medicon_update_tests)
    add_executable(medicon_loader_tests tests/loader_tests.cpp)
    medicon_target(medicon_loader_tests)
    target_link_libraries(medicon_loader_tests PRIVATE medicon_loader_core)
    add_test(NAME loader COMMAND medicon_loader_tests)
    add_executable(medicon_social_tests tests/social_tests.cpp)
    medicon_target(medicon_social_tests)
    target_link_libraries(medicon_social_tests PRIVATE medicon_social_core)
    add_test(NAME social COMMAND medicon_social_tests)
    add_executable(medicon_alt_tests tests/alt_tests.cpp)
    medicon_target(medicon_alt_tests)
    target_link_libraries(medicon_alt_tests PRIVATE medicon_social_core)
    add_test(NAME alt COMMAND medicon_alt_tests)
endif()

if(MEDICON_BUILD_WINDOWS)
    enable_language(RC)
    # Older build trees can reload a bare "windres" from CMakeRCCompiler.cmake
    # even after -DCMAKE_RC_COMPILER supplies an absolute path. Resolve the
    # loaded value beside the C++ compiler before generating the RC commands.
    if(MINGW AND NOT IS_ABSOLUTE "${CMAKE_RC_COMPILER}")
        get_filename_component(_medicon_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(_medicon_resource_compiler NAMES "${CMAKE_RC_COMPILER}"
            HINTS "${_medicon_toolchain_bin}" NO_CACHE REQUIRED)
        set(CMAKE_RC_COMPILER "${_medicon_resource_compiler}")
    endif()
    include(cmake/EmbedModel.cmake)
    # ufbx v0.20.1, commit 13b5df8e89eff0ce898f7c37fa0716e3bc186573.
    # Vendored single C source; see third_party/ufbx/LICENSE.
    add_library(medicon_ufbx STATIC third_party/ufbx/ufbx.c)
    target_include_directories(medicon_ufbx PUBLIC third_party/ufbx)
    set_target_properties(medicon_ufbx PROPERTIES C_STANDARD 99)
    add_library(medicon_model_asset STATIC src/custom_model/ModelAsset.cpp src/custom_model/Animation.cpp src/custom_model/RigImporter.cpp src/custom_model/FirstPerson.cpp src/custom_model/BundledModel.cpp)
    medicon_target(medicon_model_asset)
    target_include_directories(medicon_model_asset PRIVATE "${MEDICON_EMBEDDED_MODEL_DIR}")
    target_link_libraries(medicon_model_asset PUBLIC medicon_ufbx nlohmann_json::nlohmann_json bcrypt)
    add_executable(MediconModelInspect tools/custom-model-inspect.cpp)
    medicon_target(MediconModelInspect)
    target_link_libraries(MediconModelInspect PRIVATE medicon_model_asset)
    include(cmake/EmbedFonts.cmake)
    include(cmake/CppWinRT.cmake)
    add_library(medicon_music STATIC
        src/music/AlbumArtProvider.cpp
        src/music/AudioVisualizer.cpp
        src/music/MusicSessionManager.cpp)
    medicon_target(medicon_music)
    target_include_directories(medicon_music SYSTEM PRIVATE "${MEDICON_CPPWINRT_INCLUDE_DIR}")
    target_link_libraries(medicon_music PUBLIC medicon_music_core windowsapp ole32 uuid)
    if(MINGW)
        # Microsoft's C++/WinRT projection uses __declspec and MS extensions.
        target_compile_options(medicon_music PRIVATE -fms-extensions)
    endif()
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
        src/gui/AltScreen.cpp
        src/gui/DisconnectScreen.cpp
        src/gui/ArrayList.cpp
        src/gui/ArrayListAnimator.cpp
        src/gui/BlockCount.cpp
        src/gui/ClickGui.cpp
        src/gui/Draw.cpp
        src/gui/EdgeLit.cpp
        src/gui/Fonts.cpp
        src/gui/Glass.cpp
        src/gui/GameplayVisuals.cpp
        src/gui/Ime.cpp
        src/gui/InjectSplash.cpp
        src/gui/KeyNames.cpp
        src/gui/LiquidGlass.cpp
        src/gui/Log.cpp
        src/gui/MainMenu.cpp
        src/gui/MusicWidget.cpp
        src/gui/NativeFileDialog.cpp
        src/gui/Overlay.cpp
        src/gui/Shadow.cpp
        src/gui/SocialBackground.cpp
        src/gui/SocialChat.cpp
        src/gui/SocialFonts.cpp
        src/gui/SocialScreen.cpp
        src/gui/SocialUi.cpp
        src/gui/SocialWidgets.cpp
        src/gui/Textures.cpp
        src/gui/Toasts.cpp
        src/gui/TopNav.cpp
        src/gui/Watermark.cpp)
    medicon_target(medicon_gui)
    target_include_directories(medicon_gui PRIVATE "${MEDICON_EMBEDDED_FONTS_DIR}")
    # The Watermark shows it; the DLL and the Preview define the same value.
    target_compile_definitions(medicon_gui PRIVATE MEDICON_VERSION="${PROJECT_VERSION}")
    target_link_libraries(medicon_gui PUBLIC medicon_core medicon_social_core medicon_music medicon_imgui d3d11 d3dcompiler d2d1 windowscodecs ole32 imm32 comdlg32)

    # Social networking (spec 4.2): WinHTTP REST/WebSocket, DPAPI license, notification sound.
    add_library(medicon_social_net STATIC
        src/alt/AltService.cpp
        src/social/Files.cpp
        src/social/LicenseStore.cpp
        src/social/Service.cpp
        src/social/SocialApi.cpp
        src/social/SocialNet.cpp
        src/social/Sound.cpp
        src/social/WinHttp.cpp)
    medicon_target(medicon_social_net)
    target_link_libraries(medicon_social_net PUBLIC medicon_social_core winhttp crypt32 winmm shell32 ole32)

    add_library(medicon_bedrock STATIC src/sdk/BedrockReconnect.cpp src/sdk/BedrockAuth.cpp src/sdk/BedrockChat.cpp src/sdk/BedrockInput.cpp src/sdk/BedrockMenu.cpp src/sdk/VanillaNavigation.cpp src/sdk/Gameplay.cpp src/sdk/CustomModelProbe.cpp src/sdk/CustomModelBox.cpp src/sdk/CustomModelStatic.cpp src/sdk/CustomModelAnimation.cpp src/sdk/CustomModelView.cpp)
    medicon_target(medicon_bedrock)
    target_link_libraries(medicon_bedrock PUBLIC medicon_memory medicon_model_asset version)

    add_library(medicon_runtime STATIC src/client/Client.cpp src/hooks/HookManager.cpp)
    medicon_target(medicon_runtime)
    target_link_libraries(medicon_runtime PUBLIC medicon_core medicon_memory medicon_gui medicon_bedrock medicon_mouse_capture minhook shell32 ole32)

    add_library(Medicon SHARED
        "${MEDICON_EMBEDDED_MODEL_RC}"
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

    # MediconLoader.dll: the small DLL the user installs once. It downloads,
    # verifies and loads MediconCore.dll (docs/auto-update.md). Built static so
    # it needs no extra runtime DLLs alongside it.
    add_library(MediconLoader SHARED
        src/loader/LoaderMain.cpp
        src/loader/CoreLauncher.cpp
        src/update/Download.cpp
        src/update/Verify.cpp
        src/update/Storage.cpp)
    medicon_target(MediconLoader)
    target_link_libraries(MediconLoader PRIVATE medicon_loader_core winhttp bcrypt shell32 ole32 version user32)
    target_compile_definitions(MediconLoader PRIVATE MEDICON_UPDATE_HOST="${MEDICON_UPDATE_HOST}")
    set_target_properties(MediconLoader PROPERTIES PREFIX "" OUTPUT_NAME "MediconLoader")

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
        add_executable(medicon_alt_clipboard_tests tests/alt_clipboard_tests.cpp)
        medicon_target(medicon_alt_clipboard_tests)
        target_link_libraries(medicon_alt_clipboard_tests PRIVATE user32)
        add_test(NAME alt_clipboard COMMAND medicon_alt_clipboard_tests)
        add_executable(medicon_custom_model_animation_tests tests/custom_model_animation_tests.cpp)
        medicon_target(medicon_custom_model_animation_tests)
        target_link_libraries(medicon_custom_model_animation_tests PRIVATE medicon_bedrock)
        add_test(NAME custom_model_animation COMMAND medicon_custom_model_animation_tests)
        add_executable(medicon_custom_model_asset_tests tests/custom_model_asset_tests.cpp "${MEDICON_EMBEDDED_MODEL_RC}")
        medicon_target(medicon_custom_model_asset_tests)
        target_link_libraries(medicon_custom_model_asset_tests PRIVATE medicon_model_asset)
        add_test(NAME custom_model_asset COMMAND medicon_custom_model_asset_tests)
        add_executable(medicon_custom_model_probe_tests tests/custom_model_probe_tests.cpp)
        medicon_target(medicon_custom_model_probe_tests)
        target_link_libraries(medicon_custom_model_probe_tests PRIVATE medicon_bedrock)
        add_test(NAME custom_model_probe COMMAND medicon_custom_model_probe_tests)
        add_executable(medicon_custom_model_box_tests tests/custom_model_box_tests.cpp)
        medicon_target(medicon_custom_model_box_tests)
        target_link_libraries(medicon_custom_model_box_tests PRIVATE medicon_bedrock)
        add_test(NAME custom_model_box COMMAND medicon_custom_model_box_tests)
        add_executable(medicon_bedrock_chat_tests tests/bedrock_chat_tests.cpp)
        medicon_target(medicon_bedrock_chat_tests)
        target_link_libraries(medicon_bedrock_chat_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_chat COMMAND medicon_bedrock_chat_tests)
        add_executable(medicon_bedrock_auth_tests tests/bedrock_auth_tests.cpp)
        medicon_target(medicon_bedrock_auth_tests)
        target_link_libraries(medicon_bedrock_auth_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_auth COMMAND medicon_bedrock_auth_tests)
        add_executable(medicon_bedrock_input_tests tests/bedrock_input_tests.cpp)
        medicon_target(medicon_bedrock_input_tests)
        target_link_libraries(medicon_bedrock_input_tests PRIVATE medicon_runtime)
        add_test(NAME bedrock_input COMMAND medicon_bedrock_input_tests)
        add_executable(medicon_bedrock_gameplay_tests tests/bedrock_gameplay_tests.cpp)
        medicon_target(medicon_bedrock_gameplay_tests)
        target_link_libraries(medicon_bedrock_gameplay_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_gameplay COMMAND medicon_bedrock_gameplay_tests)
        add_executable(medicon_bedrock_menu_tests tests/bedrock_menu_tests.cpp)
        medicon_target(medicon_bedrock_menu_tests)
        target_link_libraries(medicon_bedrock_menu_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_menu COMMAND medicon_bedrock_menu_tests)
        add_executable(medicon_bedrock_reconnect_tests tests/bedrock_reconnect_tests.cpp)
        medicon_target(medicon_bedrock_reconnect_tests)
        target_link_libraries(medicon_bedrock_reconnect_tests PRIVATE medicon_bedrock)
        add_test(NAME bedrock_reconnect COMMAND medicon_bedrock_reconnect_tests)
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
        add_executable(medicon_liquid_glass_tests tests/liquid_glass_tests.cpp)
        medicon_target(medicon_liquid_glass_tests)
        target_link_libraries(medicon_liquid_glass_tests PRIVATE medicon_gui d3d12 dxgi)
        add_test(NAME liquid_glass COMMAND medicon_liquid_glass_tests)
        add_executable(medicon_dll_tests tests/dll_tests.cpp)
        medicon_target(medicon_dll_tests)
        target_link_libraries(medicon_dll_tests PRIVATE medicon_imgui)
        add_dependencies(medicon_dll_tests Medicon)
        add_test(NAME dll_lifecycle COMMAND medicon_dll_tests $<TARGET_FILE:Medicon>)
        add_executable(medicon_overlay_host_tests tests/overlay_host_tests.cpp)
        medicon_target(medicon_overlay_host_tests)
        target_link_libraries(medicon_overlay_host_tests PRIVATE d3d11 d3d12 dxgi user32)
        # Exercise the exact overlay/hooks with a deterministic Social transport.
        # An empty live account opens the locked sign-in gate before these
        # tests can reach ClickGUI; tests must not require real credentials.
        get_target_property(_medicon_overlay_sources Medicon SOURCES)
        add_library(MediconOverlayTest SHARED ${_medicon_overlay_sources} tests/overlay_social_stub.cpp)
        medicon_target(MediconOverlayTest)
        target_compile_definitions(MediconOverlayTest PRIVATE MEDICON_EXPORTS MEDICON_VERSION="${PROJECT_VERSION}")
        target_link_libraries(MediconOverlayTest PRIVATE medicon_runtime medicon_social_net d3d11 d3d12 dxgi d3dcompiler dwmapi imm32)
        set_target_properties(MediconOverlayTest PROPERTIES PREFIX "")
        add_dependencies(medicon_overlay_host_tests Medicon MediconOverlayTest)
        add_test(NAME overlay_d3d11 COMMAND medicon_overlay_host_tests $<TARGET_FILE:MediconOverlayTest>)
        add_test(NAME overlay_d3d11_late COMMAND medicon_overlay_host_tests $<TARGET_FILE:MediconOverlayTest> --late-load)
        add_test(NAME overlay_d3d12 COMMAND medicon_overlay_host_tests $<TARGET_FILE:MediconOverlayTest> --d3d12)
        add_test(NAME overlay_d3d12_late COMMAND medicon_overlay_host_tests $<TARGET_FILE:MediconOverlayTest> --d3d12 --late-load)
        add_test(NAME overlay_present1_threaded COMMAND medicon_overlay_host_tests $<TARGET_FILE:MediconOverlayTest> --d3d12 --late-load --present1 --threaded-input)
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
        add_executable(medicon_music_tests tests/music_tests.cpp)
        medicon_target(medicon_music_tests)
        target_link_libraries(medicon_music_tests PRIVATE medicon_gui)
        add_test(NAME music COMMAND medicon_music_tests)
        add_executable(medicon_music_native_tests tests/music_native_tests.cpp)
        medicon_target(medicon_music_native_tests)
        target_include_directories(medicon_music_native_tests SYSTEM PRIVATE "${MEDICON_CPPWINRT_INCLUDE_DIR}")
        target_link_libraries(medicon_music_native_tests PRIVATE medicon_music)
        if(MINGW)
            target_compile_options(medicon_music_native_tests PRIVATE -fms-extensions)
        endif()
        add_test(NAME music_native_art COMMAND medicon_music_native_tests)
        add_test(NAME preview_block_count COMMAND MediconPreview --scenario block_count --size 1920x1080 --fixed-dt 0.01)
        add_test(NAME preview_music COMMAND MediconPreview --scenario music --size 1920x1080 --fixed-dt 0.016666667)
        add_test(NAME preview_music_small COMMAND MediconPreview --scenario music --size 640x480 --dpi 144 --fixed-dt 0.016666667)
        set_tests_properties(preview_music preview_music_small PROPERTIES TIMEOUT 30)
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
        add_test(NAME preview_social_admin_focus COMMAND MediconPreview --scenario social_admin_focus --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_sign_in_gate COMMAND MediconPreview --scenario social_sign_in_gate --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_update_notice COMMAND MediconPreview --scenario social_update_notice --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_admin_dll COMMAND MediconPreview --scenario social_admin_dll --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_social_changelog_marks COMMAND MediconPreview --scenario social_changelog_marks --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_alt_manager COMMAND MediconPreview --scenario alt_manager --size 1280x720 --fixed-dt 0.016666667)
        add_test(NAME preview_disconnect_recovery COMMAND MediconPreview --scenario disconnect_recovery --size 1280x720 --fixed-dt 0.016666667)
        set_tests_properties(preview_disconnect_recovery PROPERTIES TIMEOUT 40)
        set_tests_properties(preview_social_global preview_social_dm preview_social_friends preview_social_friend_remove
                             preview_social_admin preview_social_admin_focus preview_social_admin_dll
                             preview_social_sign_in_gate preview_social_update_notice preview_social_changelog_marks
                             preview_alt_manager PROPERTIES TIMEOUT 30)
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

