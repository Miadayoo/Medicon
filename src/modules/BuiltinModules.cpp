#include "medicon/modules/BuiltinModules.hpp"
#include "medicon/modules/ClientModules.hpp"
#include "medicon/modules/ExampleModule.hpp"
#include "medicon/modules/GameplayModules.hpp"
#include "medicon/modules/VisualModules.hpp"
#include "medicon/core/ModuleManager.hpp"

namespace medicon {

void register_builtin_modules(ModuleManager& modules) {
    modules.add<ExampleModule>();
    modules.add<ClickGuiModule>();
    modules.add<ArrayListModule>();
    modules.add<ChinaHatModule>();
    modules.add<DerpModule>();
    modules.add<AimbotModule>();
    modules.add<KillauraModule>();
    modules.add<ScaffoldModule>();
    modules.add<EspModule>();
}

} // namespace medicon
