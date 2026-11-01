#pragma once
#include <medicon/scripts/ScriptServices.hpp>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace medicon::scripts {
// Host-owned module-private cache. Preload runs on the build worker; atomic
// persistence runs on its own I/O worker, never in a script/render callback.
class ScriptStorage {
public:
    static constexpr size_t quota = 256 * 1024;
    ScriptStorage(fs::path root, std::function<void(std::string_view)> report);
    ~ScriptStorage();
    void start();
    void preload(std::string_view module);
    std::optional<std::string> get(std::string_view module, std::string_view key) const;
    bool set(std::string_view module, std::string_view key, std::string_view value);
    void flush(); // Tests/shutdown only; never called by the public SDK.
    void stop();
    static bool valid_key(std::string_view key);
private:
    using Values = std::map<std::string,std::string>;
    fs::path file(std::string_view module) const;
    void worker();
    fs::path root_;
    std::function<void(std::string_view)> report_;
    mutable std::mutex mutex_;
    std::condition_variable wake_,flushed_;
    std::map<std::string,Values> values_;
    std::set<std::string> dirty_;
    bool stopping_ = false, writing_ = false;
    std::thread worker_;
};
}
