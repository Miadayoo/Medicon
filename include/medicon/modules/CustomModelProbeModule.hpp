#pragma once

#include <medicon/core/Module.hpp>

namespace medicon {
class CustomModelProbeModule final : public Module {
public:
    static constexpr const char* module_id = "custommodel";
    CustomModelProbeModule();
    Setting& show_debug;
    Setting& mode;
    Setting& model;
    Setting& animation;
    Setting& show_users;
    Setting& test_box;
    Setting& material;
    Setting& brightness;
};
}
