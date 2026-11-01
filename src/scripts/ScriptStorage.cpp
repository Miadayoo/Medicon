#include <medicon/scripts/ScriptStorage.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>

namespace medicon::scripts {
namespace {
using Json = nlohmann::json;
Json document(std::string_view module,const std::map<std::string,std::string>& values) {
    return {{"version",1},{"module_id",module},{"values",values}};
}
bool utf8_string(std::string_view value) {
    return value.empty() || (value.size() <= ScriptStorage::quota && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0) != 0);
}
}
ScriptStorage::ScriptStorage(fs::path root,std::function<void(std::string_view)> report) : root_(std::move(root)),report_(std::move(report)) {}
ScriptStorage::~ScriptStorage() { stop(); }
void ScriptStorage::start() { std::lock_guard lock(mutex_); if (!worker_.joinable()) { stopping_=false; worker_=std::thread([this]{worker();}); } }
bool ScriptStorage::valid_key(std::string_view key) {
    return !key.empty() && key.size() <= 128 && key != "." && key != ".." && key.find_first_of("/\\\0",0,3) == std::string_view::npos && utf8_string(key);
}
fs::path ScriptStorage::file(std::string_view module) const { return root_ / (sha256(module)+".json"); }
void ScriptStorage::preload(std::string_view module) {
    { std::lock_guard lock(mutex_); if (values_.contains(std::string(module))) return; }
    Values values;
    try {
        if (!valid_id(module)) throw std::runtime_error("Invalid storage module ID");
        fs::create_directories(root_);
        if ((GetFileAttributesW(root_.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) throw std::runtime_error("Linked storage directory is unsupported");
        const auto path = file(module);
        if (fs::exists(path)) {
            if ((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || fs::file_size(path) > quota) throw std::runtime_error("Storage file is linked or exceeds 256 KiB");
            std::ifstream stream(path,std::ios::binary); const auto json = Json::parse(stream);
            if (json.value("version",0) != 1 || json.value("module_id","") != module || !json.at("values").is_object()) throw std::runtime_error("Storage schema/owner mismatch");
            values = json.at("values").get<Values>();
            if(values.size()>1024 || document(module,values).dump().size()>quota) throw std::runtime_error("Storage key/serialized quota exceeded");
            for (const auto& [key,value] : values) if (!valid_key(key) || !utf8_string(value)) throw std::runtime_error("Invalid storage key/value");
        }
    } catch (const std::exception& e) {
        if (report_) report_(std::string("Storage preload for ")+std::string(module)+": "+
            (dynamic_cast<const Json::exception*>(&e)?"Malformed storage JSON/schema":e.what()));
        std::error_code ignored;const auto source=file(module),backup=fs::path(source.wstring()+L".corrupt");
        if(fs::is_regular_file(source,ignored)&&ScriptPaths::contains(root_,source)&&ScriptPaths::contains(root_,backup)&&
           !(GetFileAttributesW(root_.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)&&!(GetFileAttributesW(source.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)&&
           (!fs::exists(backup)||!(GetFileAttributesW(backup.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)))
            fs::copy_file(source,backup,fs::copy_options::overwrite_existing,ignored);
        values.clear();
    }
    std::lock_guard lock(mutex_); values_.try_emplace(std::string(module),std::move(values));
}
std::optional<std::string> ScriptStorage::get(std::string_view module,std::string_view key) const {
    if (!valid_key(key)) return std::nullopt;
    std::lock_guard lock(mutex_); const auto owner = values_.find(std::string(module));
    if (owner == values_.end()) return std::nullopt;
    const auto found = owner->second.find(std::string(key)); if (found == owner->second.end()) return std::nullopt; return found->second;
}
bool ScriptStorage::set(std::string_view module,std::string_view key,std::string_view value) {
    if (!valid_id(module) || !valid_key(key) || !utf8_string(value)) return false;
    std::lock_guard lock(mutex_);
    auto owner = values_.find(std::string(module)); if (owner == values_.end() || stopping_) return false;
    auto candidate = owner->second; candidate[std::string(key)] = value;
    if (candidate.size() > 1024 || document(module,candidate).dump().size() > quota) return false;
    owner->second.swap(candidate); dirty_.insert(std::string(module)); wake_.notify_one(); return true;
}
void ScriptStorage::worker() {
    for (;;) {
        std::string module; Values values;
        {
            std::unique_lock lock(mutex_); wake_.wait(lock,[&]{return stopping_ || !dirty_.empty();});
            if (dirty_.empty() && stopping_) break;
            module=*dirty_.begin(); dirty_.erase(dirty_.begin()); values=values_.at(module); writing_=true;
        }
        try {
            fs::create_directories(root_); const auto path=file(module);
            if (!ScriptPaths::contains(root_,path) || (GetFileAttributesW(root_.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) throw std::runtime_error("Storage path changed");
            for(const auto& p:{path,fs::path(path.wstring()+L".bak")}) if(fs::exists(p)&&(GetFileAttributesW(p.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Linked storage file/backup is unsupported");
            if (fs::exists(path)) fs::copy_file(path,fs::path(path.wstring()+L".bak"),fs::copy_options::overwrite_existing);
            atomic_write(path,document(module,values).dump());
        } catch (const std::exception& e) { if (report_) report_(std::string("Storage persistence for ")+module+": "+e.what()); }
        { std::lock_guard lock(mutex_); writing_=false; if (dirty_.empty()) flushed_.notify_all(); }
    }
}
void ScriptStorage::flush() { std::unique_lock lock(mutex_); flushed_.wait(lock,[&]{return dirty_.empty() && !writing_;}); }
void ScriptStorage::stop() {
    { std::lock_guard lock(mutex_); stopping_=true; wake_.notify_all(); }
    if (worker_.joinable()) worker_.join();
}
}
