#include "medicon/modules/BuiltinModules.hpp"
#include "medicon/modules/ClientModules.hpp"
#include "medicon/modules/ExampleModule.hpp"
#include "medicon/core/ModuleManager.hpp"

namespace medicon {

void register_builtin_modules(ModuleManager& modules) {
    modules.add<ExampleModule>();
    modules.add<ClickGuiModule>();
    modules.add<ArrayListModule>();
}

} // namespace medicon
