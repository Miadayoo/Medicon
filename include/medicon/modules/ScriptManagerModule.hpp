#pragma once
#include <medicon/core/Module.hpp>

namespace medicon {
class ScriptManagerModule final : public Module {
public:
    static constexpr const char* module_id = "script_manager";
    ScriptManagerModule() : Module(module_id, "Script Manager", "Build, approve and reload C++ scripts. Expand to manage.", Category::Scripts) {
        set_show_in_arraylist(false);
    }
};
}
