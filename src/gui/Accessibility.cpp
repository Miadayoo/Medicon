#include <medicon/gui/Accessibility.hpp>
#include <windows.h>
#include <chrono>

namespace medicon::gui {
namespace {
thread_local std::optional<AccessibilityPreferences> override_preferences;
thread_local std::chrono::steady_clock::time_point last_query{};
}
void override_accessibility_preferences(std::optional<AccessibilityPreferences> preferences) {
    override_preferences=preferences;
    last_query={};
    refresh_accessibility_preferences();
}
void refresh_accessibility_preferences() {
    if(override_preferences) { accessibility_preferences=*override_preferences; return; }
    const auto now=std::chrono::steady_clock::now();
    if(now-last_query<std::chrono::seconds(1)) return;
    last_query=now;
    BOOL animate=TRUE;
    if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0)) accessibility_preferences.reduced_motion=!animate;
    HIGHCONTRASTW contrast{}; contrast.cbSize=sizeof(contrast);
    if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)) accessibility_preferences.high_contrast=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
    DWORD transparency=1,size=sizeof(transparency);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"EnableTransparency",RRF_RT_REG_DWORD,nullptr,&transparency,&size)==ERROR_SUCCESS)
        accessibility_preferences.reduced_transparency=transparency==0;
}
}
