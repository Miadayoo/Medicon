# The supplied static DynaPuff face is checked in and embedded in the DLL/Preview.
# This needs only CMake; neither font loading nor a build downloads any files.
set(MEDICON_EMBEDDED_FONTS_DIR "${PROJECT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MEDICON_EMBEDDED_FONTS_DIR}")
set(_medicon_font_header "#pragma once\nnamespace medicon::gui::embedded_fonts {\n")

function(medicon_embed_font symbol filename expected_sha256)
    set(font_path "${PROJECT_SOURCE_DIR}/assets/fonts/${filename}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${font_path}")
    file(SHA256 "${font_path}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR "Bundled ${filename} differs from its pinned official font source")
    endif()
    file(READ "${font_path}" font_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," font_bytes "${font_hex}")
    string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" font_bytes "${font_bytes}")
    set(_medicon_font_header "${_medicon_font_header}inline constexpr unsigned char ${symbol}[] = {\n${font_bytes}\n};\n" PARENT_SCOPE)
endfunction()

medicon_embed_font(dynapuff_medium dynapuff.medium.ttf 3fd5ca3792117b8b38051498b96038497dac8fd74937d7785059f94a58882d1f)
string(APPEND _medicon_font_header "} // namespace medicon::gui::embedded_fonts\n")
file(CONFIGURE OUTPUT "${MEDICON_EMBEDDED_FONTS_DIR}/medicon_embedded_fonts.hpp"
    CONTENT "${_medicon_font_header}" @ONLY)
unset(_medicon_font_header)

# Keep the larger user-provided assets private to Fonts.cpp instead of making
# every consumer of the small Latin font header parse several megabytes of data.
set(_medicon_splash_header "#pragma once\nnamespace medicon::gui::embedded_splash {\n")
function(medicon_embed_splash symbol relative_path expected_sha256)
    set(asset_path "${PROJECT_SOURCE_DIR}/assets/${relative_path}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asset_path}")
    file(SHA256 "${asset_path}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR "Bundled ${relative_path} differs from the supplied splash asset")
    endif()
    file(READ "${asset_path}" asset_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," asset_bytes "${asset_hex}")
    string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" asset_bytes "${asset_bytes}")
    set(_medicon_splash_header "${_medicon_splash_header}inline constexpr unsigned char ${symbol}[] = {\n${asset_bytes}\n};\n" PARENT_SCOPE)
endfunction()
medicon_embed_splash(kawai fonts/kawai.otf 32146e718f54705267003b11df2013553c7e17d224a0fd49cca50353e81ccd87)
medicon_embed_splash(mez mez.png f364e70b55c6aebcf8234a0ef446d068bfcd73996aef58919416f818ce3bfaad)
string(APPEND _medicon_splash_header "} // namespace medicon::gui::embedded_splash\n")
file(CONFIGURE OUTPUT "${MEDICON_EMBEDDED_FONTS_DIR}/medicon_embedded_splash.hpp"
    CONTENT "${_medicon_splash_header}" @ONLY)
unset(_medicon_splash_header)

# The Social screen background (spec 4.6), private to SocialBackground.cpp.
set(_medicon_splash_header "#pragma once\nnamespace medicon::gui::embedded_social {\n")
medicon_embed_splash(back back.png 221f7da039f9e71e9c0550d58eed6933a0df6103f82ca80c5340e6e7ae3e0e9d)
string(APPEND _medicon_splash_header "} // namespace medicon::gui::embedded_social\n")
file(CONFIGURE OUTPUT "${MEDICON_EMBEDDED_FONTS_DIR}/medicon_embedded_social.hpp"
    CONTENT "${_medicon_splash_header}" @ONLY)
unset(_medicon_splash_header)

# Keep the OFL notice next to redistributable binaries as well as the sources.
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses")
configure_file("${PROJECT_SOURCE_DIR}/assets/fonts/OFL.txt"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses/Geist-OFL.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/assets/fonts/DynaPuff-OFL.txt"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses/DynaPuff-OFL.txt" COPYONLY)
