#include <medicon/hooks/HookManager.hpp>
#include <MinHook.h>
#include <algorithm>
#include <mutex>
#include <utility>

namespace medicon {
namespace {
std::mutex library_mutex;
std::size_t library_users{};
bool library_owned{};

bool check(MH_STATUS status, std::string_view operation, std::string& error) {
    if (status == MH_OK) return true;
    error = std::string(operation) + ": " + MH_StatusToString(status);
    return false;
}
}

HookManager::~HookManager() {
    std::string ignored;
    shutdown(ignored);
}

bool HookManager::initialize(std::string& error) {
    if (initialized_) return true;
    std::lock_guard lock(library_mutex);
    if (library_users == 0) {
        const auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
            return check(status, "MH_Initialize", error);
        library_owned = status == MH_OK;
    }
    ++library_users;
    initialized_ = true;
    return true;
}

bool HookManager::create(std::string name, void* target, void* detour, void** original, std::string& error) {
    error.clear();
    if (name.empty() || !target || !detour || !original) {
        error = "A hook requires a name, resolved target, detour and trampoline output.";
        return false;
    }
    if (std::ranges::any_of(hooks_, [&](const Hook& hook) { return hook.name == name || hook.target == target; })) {
        error = "Duplicate hook name or target: " + name;
        return false;
    }
    if (!initialize(error)) return false;
    // Allocate bookkeeping before creating an external resource.
    hooks_.push_back({std::move(name), target, false});
    if (!check(MH_CreateHook(target, detour, original), "MH_CreateHook", error)) {
        hooks_.pop_back();
        return false;
    }
    return true;
}

bool HookManager::enable_all(std::string& error) {
    error.clear();
    std::vector<Hook*> activated;
    activated.reserve(hooks_.size());
    for (auto& hook : hooks_) {
        if (hook.enabled) continue;
        if (!check(MH_EnableHook(hook.target), hook.name, error)) {
            // Preserve hooks that were already enabled before this attempt.
            for (auto it = activated.rbegin(); it != activated.rend(); ++it) {
                const auto status = MH_DisableHook((*it)->target);
                if (status == MH_OK) (*it)->enabled = false;
                else error += std::string("; rollback: ") + MH_StatusToString(status);
            }
            return false;
        }
        hook.enabled = true;
        activated.push_back(&hook);
    }
    return true;
}

bool HookManager::disable_all(std::string& error) {
    error.clear();
    bool ok = true;
    for (auto it = hooks_.rbegin(); it != hooks_.rend(); ++it) {
        if (!it->enabled) continue;
        std::string detail;
        if (!check(MH_DisableHook(it->target), it->name, detail)) {
            if (!error.empty()) error += "; ";
            error += detail;
            ok = false;
        } else it->enabled = false;
    }
    return ok;
}

bool HookManager::clear(std::string& error) {
    if (!disable_all(error)) return false;
    for (auto it = hooks_.begin(); it != hooks_.end();) {
        if (!check(MH_RemoveHook(it->target), it->name, error)) return false;
        it = hooks_.erase(it);
    }
    return true;
}

bool HookManager::shutdown(std::string& error) {
    if (!initialized_) return true;
    if (!clear(error)) return false;
    std::lock_guard lock(library_mutex);
    if (!initialized_) return true;
    initialized_ = false;
    if (--library_users == 0 && library_owned) {
        const auto status = MH_Uninitialize();
        if (status != MH_OK) {
            ++library_users;
            initialized_ = true;
            return check(status, "MH_Uninitialize", error);
        }
        library_owned = false;
    }
    return true;
}

} // namespace medicon
