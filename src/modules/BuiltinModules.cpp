#include "medicon/modules/BuiltinModules.hpp"
#include "medicon/modules/AntiBotModule.hpp"
#include "medicon/modules/ArrayListModule.hpp"
#include "medicon/modules/ClickGuiModule.hpp"
#include "medicon/modules/WatermarkModule.hpp"
#include "medicon/modules/ExampleModule.hpp"
#include "medicon/modules/ChinaHatModule.hpp"
#include "medicon/modules/DerpModule.hpp"
#include "medicon/modules/DisablerModule.hpp"
#include "medicon/modules/AimbotModule.hpp"
#include "medicon/modules/KillauraModule.hpp"
#include "medicon/modules/ScaffoldModule.hpp"
#include "medicon/modules/RegenModule.hpp"
#include "medicon/modules/ChestStealerModule.hpp"
#include "medicon/modules/InvManagerModule.hpp"
#include "medicon/modules/EspModule.hpp"
#include "medicon/modules/RotationsModule.hpp"
#include "medicon/modules/SprintModule.hpp"
#include "medicon/modules/VelocityModule.hpp"
#include "medicon/modules/CustomModelProbeModule.hpp"
#include "medicon/modules/ScriptManagerModule.hpp"
#include "medicon/core/ModuleManager.hpp"

namespace medicon {

void register_builtin_modules(ModuleManager& modules) {
    modules.add<ExampleModule>();
    modules.add<ClickGuiModule>();
    modules.add<ArrayListModule>();
    modules.add<WatermarkModule>();
    modules.add<ChinaHatModule>();
    modules.add<DerpModule>();
    modules.add<AntiBotModule>();
    modules.add<AimbotModule>();
    modules.add<KillauraModule>();
    modules.add<ScaffoldModule>();
    modules.add<RegenModule>();
    modules.add<ChestStealerModule>();
    modules.add<InvManagerModule>();
    modules.add<EspModule>();
    modules.add<RotationsModule>();
    modules.add<SprintModule>();
    modules.add<VelocityModule>();
    modules.add<DisablerModule>();
    modules.add<CustomModelProbeModule>();
    modules.add<ScriptManagerModule>();
}

} // namespace medicon
