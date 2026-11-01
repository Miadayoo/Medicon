#include <medicon/gui/ScriptScreen.hpp>
#include <medicon/scripts/ScriptManager.hpp>
#include <medicon/gui/ClickGui.hpp>
#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/ScriptGlassBridge.hpp>
#include <misc/cpp/imgui_stdlib.h>
#include <medicon/modules/ScriptManagerModule.hpp>
#include <imgui.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>

namespace medicon::gui {
void attach_script_gui(scripts::ScriptManager& manager, ClickGui& gui, FontManager* fonts,Overlay* overlay) {
    manager.set_before_remove([&gui](const Module& module) { gui.forget_module(module); });
    gui.set_module_settings_renderer(ScriptManagerModule::module_id, [&manager](Module&) { render_script_screen(manager); });
    auto& host = manager.host();
    host.features |= MEDICON_FEATURE_RENDER;
    host.render_size = [] { const auto s = ImGui::GetIO().DisplaySize; return MediconPoint{s.x, s.y}; };
    host.circle = [](MediconPoint p, float radius, MediconColor c) {
        ImGui::GetBackgroundDrawList()->AddCircleFilled({p.x,p.y}, radius, ImGui::ColorConvertFloat4ToU32({c.r,c.g,c.b,c.a}));
    };
    host.text = [fonts](MediconPoint p, std::string_view text, MediconColor c, float size) {
        if (fonts) fonts->add_text(text);
        auto* font = fonts ? fonts->face(FontRole::Label).font : ImGui::GetFont();
        ImGui::GetBackgroundDrawList()->AddText(font, size, {p.x,p.y}, ImGui::ColorConvertFloat4ToU32({c.r,c.g,c.b,c.a}), text.data(), text.data() + text.size());
    };
    attach_script_glass_bridge(host,fonts,overlay);
}
namespace {
void open(const std::filesystem::path& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL); }
void render_build_progress(const scripts::ScriptView& script) {
    const auto& progress = script.progress;
    const bool failed = progress.stage == scripts::BuildStage::failed;
    if (failed) ImGui::PushStyleColor(ImGuiCol_Text, {1.F, .4F, .35F, 1.F});
    ImGui::TextWrapped("%s", script.status.c_str());
    if (!progress.message.empty() && progress.message != script.status) ImGui::TextWrapped("%s", progress.message.c_str());
    if (failed) ImGui::PopStyleColor();
    if (progress.total_steps) {
        const auto fraction = std::clamp(static_cast<float>(progress.completed_steps) / static_cast<float>(progress.total_steps), 0.F, 1.F);
        const auto label = std::to_string(progress.completed_steps) + " / " + std::to_string(progress.total_steps) + " build steps";
        if (failed) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, {1.F, .4F, .35F, 1.F});
        ImGui::ProgressBar(fraction, {-1.F, 0.F}, label.c_str());
        if (failed) ImGui::PopStyleColor();
    } else if (progress.running()) {
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), {-1.F, 0.F}, "Waiting...");
    }
}
}
void render_script_screen(scripts::ScriptManager& manager) {
    const auto size = ImGui::GetIO().DisplaySize;
    if (!ImGui::BeginChild("script-manager-content", {std::clamp(size.x - 100.F, 300.F, 780.F), std::clamp(size.y - 150.F, 200.F, 560.F)})) {
        ImGui::EndChild(); return;
    }
    const auto status = manager.snapshot();
    const auto running = std::ranges::count_if(status.scripts, [](const auto& script) { return script.progress.running(); });
    const auto succeeded = std::ranges::count_if(status.scripts, [](const auto& script) { return script.progress.stage == scripts::BuildStage::complete; });
    const auto failed = std::ranges::count_if(status.scripts, [](const auto& script) { return script.progress.stage == scripts::BuildStage::failed; });
    ImGui::TextUnformatted("C++ Scripts");
    ImGui::Text("API %u | ABI %u", MEDICON_SCRIPT_API_VERSION, MEDICON_SCRIPT_ABI_VERSION);
    ImGui::Text("MCBE: %s", status.mcbe_version.c_str());
    ImGui::Text("Features: chat=%s render=%s local_player=%s", (status.features & MEDICON_FEATURE_CHAT) ? "yes" : "no",
        (status.features & MEDICON_FEATURE_RENDER) ? "yes" : "no", (status.features & MEDICON_FEATURE_PLAYER) ? "yes" : "no");
    if (status.detecting) ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), {-1.F, 0.F}, "Looking for a C++ compiler...");
    if (!status.error.empty()) ImGui::TextWrapped("%s", status.error.c_str());
    if (running || succeeded || failed) ImGui::Text("In progress: %d | Succeeded: %d | Failed / blocked: %d", static_cast<int>(running), static_cast<int>(succeeded), static_cast<int>(failed));
    if (!status.build_status.empty() && !running) ImGui::TextWrapped("%s", status.build_status.c_str());
    ImGui::BeginDisabled(status.detecting || running > 0);
    if (ImGui::Button("Build / Reload all")) manager.request_build();
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Scripts folder")) open(manager.paths().sources);
    ImGui::SameLine(); if (ImGui::Button("Docs")) ShellExecuteW(nullptr, L"open", L"https://medicon.auau1000.com/docs", nullptr, nullptr, SW_SHOWNORMAL);
    if(ImGui::TreeNode("Create Script")) {
        static std::string name="MyScript"; static int selected=0;
        const char* templates[]{"HelloWorld","Settings","GlassHUD","GlassQuickPanel","AnimatedToast","Events","MultiFile"};
        ImGui::InputText("Name",&name); ImGui::Combo("Template",&selected,templates,7);
        if(ImGui::Button("Create from template")) manager.request_create(name,templates[selected]);
        ImGui::TextWrapped("Created in Scripts on the worker. Review, build and approve before enabling."); ImGui::TreePop();
    }
    if(ImGui::TreeNode("Capabilities / Permissions")) {
        struct Feature { const char* name; uint64_t bit; };
        const Feature features[]{{"Native Glass",MEDICON_FEATURE_GLASS},{"Refraction",MEDICON_FEATURE_REFRACTION},
            {"UI",MEDICON_FEATURE_UI},{"Animation",MEDICON_FEATURE_ANIMATION},{"Timers",MEDICON_FEATURE_TIMERS},
            {"Storage",MEDICON_FEATURE_STORAGE},{"Notifications",MEDICON_FEATURE_NOTIFICATIONS},{"Theme",MEDICON_FEATURE_THEME},
            {"Input",MEDICON_FEATURE_INPUT},{"Sound (Success chime)",MEDICON_FEATURE_SOUND}};
        for(const auto& feature:features) ImGui::Text("%s: %s",feature.name,(status.features&feature.bit)?"available":"unavailable");
        ImGui::TextWrapped("Native DLLs run outside a sandbox. Approval applies to the exact source and binary. Capabilities describe services, not security permissions.");
        ImGui::TextWrapped("Native glass supports regular / radius / refraction / opacity. Other presets, independent blur, tint, saturation, highlight, border, shadow and quality tiers use a diagnosed fallback.");
        ImGui::TreePop();
    }
    if (!status.detecting && status.toolchain.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Toolchain")) {
        if (status.toolchain.empty()) ImGui::TextWrapped("A toolchain compiles C++ scripts into DLLs. Install Visual Studio C++ Build Tools with MSVC x64 and Windows SDK, then retry Build / Reload.");
        else {
            ImGui::TextWrapped("%s", status.toolchain.c_str()); ImGui::TextWrapped("%s", status.version.c_str());
            ImGui::TextWrapped("%s", status.license.c_str());
        }
        if (ImGui::Button("Toolchain setup")) ShellExecuteW(nullptr, L"open", scripts::path_from_utf8(status.setup_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::TreePop();
    }
    if (status.scripts.empty()) ImGui::TextUnformatted("Save a .cpp in Scripts to begin. Samples: SDK/examples.");
    for (const auto& script : status.scripts) {
        ImGui::PushID(script.key.c_str()); ImGui::Separator();
        ImGui::Text("%s [%s]", script.name.empty() ? script.key.c_str() : script.name.c_str(), script.id.c_str());
        render_build_progress(script);
        if(script.active) ImGui::Text("Generation %llu | callbacks %llu | max %.1f us | panes %llu | rejected %llu",
            static_cast<unsigned long long>(script.telemetry.generation),static_cast<unsigned long long>(script.telemetry.callbacks),script.telemetry.max_us,
            static_cast<unsigned long long>(script.telemetry.glass_panels),static_cast<unsigned long long>(script.telemetry.rejected));
        if (!script.source.empty()) {
            ImGui::TextWrapped("%s", scripts::utf8(script.source).c_str());
            ImGui::BeginDisabled(status.detecting || script.progress.running());
            if (ImGui::Button("Build / Reload")) manager.request_build(script.key);
            ImGui::SameLine(); if (ImGui::Button("Rebuild")) manager.request_build(script.key, true);
            ImGui::EndDisabled();
            ImGui::SameLine(); if (ImGui::Button("Editor")) open(script.source);
        }
        if (!script.log.empty()) { ImGui::SameLine(); if (ImGui::Button("Build log")) open(script.log); }
        if (script.approval_required) {
            ImGui::TextWrapped("Native C++ runs with the game's privileges. Review the source before approving.");
            ImGui::TextWrapped("Source SHA256: %s", script.fingerprint.c_str());
            ImGui::TextWrapped("DLL SHA256: %s", script.binary_hash.c_str());
            ImGui::BeginDisabled(script.progress.running());
            if (ImGui::Button("Approve & Load")) manager.request_approval(script.key, script.binary_hash);
            ImGui::EndDisabled();
        }
        if (script.active) { if (ImGui::Button("Revoke approval & unload")) manager.request_revoke(script.key); }
        if (auto* module = manager.module(script.key); module && ImGui::TreeNode("Settings")) {
            std::vector<Setting::Value> before;
            for (const auto& setting : module->settings()) before.push_back(setting.value);
            ClickGui::render_settings(*module);
            bool changed = false;
            for (size_t i = 0; i < before.size(); ++i) changed = changed || before[i] != module->settings()[i].value;
            manager.settings_edited(changed, ImGui::IsAnyItemActive());
            ImGui::TreePop();
        }
        for (size_t i = 0; i < script.diagnostics.size(); ++i) {
            const auto& d = script.diagnostics[i]; ImGui::PushID(static_cast<int>(i));
            ImGui::TextWrapped("%s:%d:%d: %s %s: %s", scripts::utf8(d.file).c_str(), d.line, d.column, d.severity.c_str(), d.code.c_str(), d.message.c_str());
            ImGui::TextWrapped("Hint: %s", d.hint.c_str());
            if (!d.file.empty() && ImGui::SmallButton("Open source")) open(d.file);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    manager.settings_edited(false, ImGui::IsAnyItemActive());
    ImGui::EndChild();
}
} // namespace medicon::gui
