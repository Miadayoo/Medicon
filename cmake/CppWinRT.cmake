# Prefer an installed Windows SDK projection. LLVM-MinGW installations usually
# do not ship it: generate the official projection using a pinned Microsoft
# tool and the host Windows metadata. No new runtime DLL or UI dependency.
if(MINGW AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "C++/WinRT requires MSVC or LLVM-MinGW. Use tools/build.ps1 -Toolchain LLVM (or MSVC).")
endif()
set(MEDICON_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Directory containing winrt/Windows.Media.Control.h")
if(NOT MEDICON_CPPWINRT_INCLUDE_DIR)
    file(GLOB _medicon_sdk_cppwinrt "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
    find_path(_medicon_cppwinrt winrt/Windows.Media.Control.h HINTS ${_medicon_sdk_cppwinrt})
    if(_medicon_cppwinrt)
        set(MEDICON_CPPWINRT_INCLUDE_DIR "${_medicon_cppwinrt}")
    else()
        FetchContent_Declare(medicon_cppwinrt_tool
            URL https://api.nuget.org/v3-flatcontainer/microsoft.windows.cppwinrt/2.0.240405.15/microsoft.windows.cppwinrt.2.0.240405.15.nupkg
            URL_HASH SHA256=e889007b5d9235931e7340ddf737d2c346eebdd23c619f1f4f2426a2aae47180
            DOWNLOAD_NAME cppwinrt.zip
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
        FetchContent_MakeAvailable(medicon_cppwinrt_tool)
        set(MEDICON_CPPWINRT_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/cppwinrt")
        if(NOT EXISTS "${MEDICON_CPPWINRT_INCLUDE_DIR}/winrt/Windows.Media.Control.h")
            set(MEDICON_WINRT_METADATA "$ENV{SystemRoot}/System32/WinMetadata" CACHE PATH "Windows runtime metadata directory")
            if(NOT EXISTS "${MEDICON_WINRT_METADATA}/Windows.Media.winmd")
                message(FATAL_ERROR "Install C++/WinRT in the Windows SDK, or set MEDICON_WINRT_METADATA / MEDICON_CPPWINRT_INCLUDE_DIR.")
            endif()
            execute_process(COMMAND "${medicon_cppwinrt_tool_SOURCE_DIR}/bin/cppwinrt.exe"
                -input "${MEDICON_WINRT_METADATA}" -output "${MEDICON_CPPWINRT_INCLUDE_DIR}"
                COMMAND_ERROR_IS_FATAL ANY)
        endif()
    endif()
endif()
configure_file("${PROJECT_SOURCE_DIR}/third_party/cppwinrt/LICENSE"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses/CppWinRT-LICENSE.txt" COPYONLY)
