#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace medicon {
class ModuleManager;
namespace gui {
class Overlay;
}
} // namespace medicon

namespace medicon::preview {

// What scenarios and input tests can do with the running Preview.
class ScenarioHost {
public:
    virtual ~ScenarioHost() = default;
    virtual ModuleManager& modules() = 0;
    virtual gui::Overlay& overlay() = 0;
    virtual void frame() = 0;                                   // render one frame
    virtual void recreate_graphics() = 0;                      // simulate loss after resources exist
    virtual void capture(const std::string& suffix) = 0;        // save the last frame
    virtual LRESULT send(UINT message, WPARAM wparam, LPARAM lparam) = 0; // to the window procedure
    virtual float dt() const = 0;
    virtual float width() const = 0;
    virtual float height() const = 0;
    virtual void log(const std::string& line) = 0;
    // Runs at the start of every frame until replaced (timed scenarios that keep input going).
    virtual void set_frame_hook(std::function<void()> hook) = 0;
    virtual void set_ui_hook(std::function<void()> hook) = 0;
};

std::vector<std::string> scenario_names();
void run_script_scenario(ScenarioHost& host, bool capture);
// Sets up the named scenario and drives frames/captures. Returns false if unknown.
bool run_scenario(const std::string& name, ScenarioHost& host, bool capture);
// Scripted Win32-message input checks (spec 9.4). Returns the number of failures.
int run_input_tests(ScenarioHost& host);

} // namespace medicon::preview
