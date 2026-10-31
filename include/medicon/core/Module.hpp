#pragma once

#include "medicon/core/Setting.hpp"

#include <deque>
#include <string>
#include <string_view>

namespace medicon {

class ModuleManager;

class Module {
public:
    Module(std::string id, std::string name, std::string description, Category category,
           int key = 0);
    virtual ~Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    const std::string& id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const std::string& description() const noexcept { return description_; }
    Category category() const noexcept { return category_; }
    int keybind() const noexcept { return keybind_; }
    void set_keybind(int key);

    bool enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept;
    void toggle() noexcept { set_enabled(!enabled_); }

    std::deque<Setting>& settings() noexcept { return settings_; }
    const std::deque<Setting>& settings() const noexcept { return settings_; }
    [[nodiscard]] virtual bool setting_visible(const Setting& setting) const noexcept { return setting.visible; }

    // HUD metadata. Client infrastructure modules hide themselves from the ArrayList.
    bool show_in_arraylist() const noexcept { return show_in_arraylist_; }
    // Short state shown after the name in the ArrayList. Defaults to the value of a
    // visible choice setting named "Mode"; override for module-specific text.
    // View into a stored choice, or empty. Valid while this module's settings live.
    virtual std::string_view suffix() const;

protected:
    // deque keeps these references stable as additional settings are registered.
    Setting& add_setting(Setting setting);
    void set_show_in_arraylist(bool show) noexcept { show_in_arraylist_ = show; }
    virtual void on_enable() noexcept {}
    virtual void on_disable() noexcept {}
    virtual void on_tick(float delta_seconds) noexcept { (void)delta_seconds; }
    virtual void on_render() noexcept {}

private:
    friend class ModuleManager;
    std::string id_;
    std::string name_;
    std::string description_;
    Category category_;
    int keybind_ = 0;
    bool enabled_ = false;
    bool show_in_arraylist_ = true;
    std::deque<Setting> settings_;
};

} // namespace medicon
