#include <medicon/scripts/ScriptServices.hpp>
#include <nlohmann/json.hpp>
#include "ScriptSdkEmbedded.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace medicon::scripts {
namespace {
using Json = nlohmann::json;
constexpr uintmax_t max_input = 8 * 1024 * 1024;

std::string read_file(const fs::path& file, uintmax_t limit = max_input) {
    if (fs::file_size(file) > limit) throw std::runtime_error("File exceeds limit: " + utf8(file));
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read: " + utf8(file));
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
bool reparse(const fs::path& path) {
    const auto a = GetFileAttributesW(path.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
Json read_json(const fs::path& file) {
    if (!fs::exists(file)) return Json::object();
    return Json::parse(read_file(file));
}
void collect_inputs(const ScriptPaths& paths, const fs::path& directory, std::vector<fs::path>& inputs,
                    std::vector<fs::path>* sources) {
    for (const auto& item : fs::recursive_directory_iterator(directory)) {
        if (reparse(item.path()) || !ScriptPaths::contains(paths.root, item.path()))
            throw std::runtime_error("Linked/reparse paths are not supported: " + utf8(item.path()));
        if (!item.is_regular_file()) continue;
        if (fs::file_size(item.path()) > max_input) throw std::runtime_error("Source file exceeds 8 MiB: " + utf8(item.path()));
        inputs.push_back(item.path());
        if (sources && lower(utf8(item.path().extension())) == ".cpp") sources->push_back(item.path());
        if (inputs.size() > 2048) throw std::runtime_error("Module exceeds 2048 input files");
    }
}
void attributes(ScriptSource& source) {
    const auto file = source.directory / "medicon.toml";
    if (!fs::exists(file)) return;
    std::istringstream stream(read_file(file));
    std::string line;
    int number = 0;
    const std::regex assignment(R"(^\s*(id|description|required_features)\s*=\s*(.*?)\s*$)");
    while (std::getline(stream, line)) {
        ++number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        if (line.substr(first) == "[module]" || line.substr(first) == "[module]\r") continue;
        std::smatch match;
        if (!std::regex_match(line, match, assignment))
            throw std::runtime_error(utf8(file) + ":" + std::to_string(number) + ": unsupported medicon.toml attribute");
        const auto key = match[1].str();
        const auto value = Json::parse(match[2].str()); // Supported TOML strings/arrays use JSON-compatible escaping.
        if (key == "id") { source.declared_id = value.get<std::string>(); if (!valid_id(source.declared_id)) throw std::runtime_error("Invalid manifest ID"); }
        else if (key == "description") source.description = value.get<std::string>();
        else for (const auto& feature : value) {
            const auto name = feature.get<std::string>();
            if (name == "chat") source.required_features |= MEDICON_FEATURE_CHAT;
            else if (name == "render") source.required_features |= MEDICON_FEATURE_RENDER;
            else if (name == "local_player") source.required_features |= MEDICON_FEATURE_PLAYER;
            else if (name == "glass") source.required_features |= MEDICON_FEATURE_GLASS;
            else if (name == "refraction") source.required_features |= MEDICON_FEATURE_REFRACTION;
            else if (name == "ui") source.required_features |= MEDICON_FEATURE_UI;
            else if (name == "animation") source.required_features |= MEDICON_FEATURE_ANIMATION;
            else if (name == "notifications") source.required_features |= MEDICON_FEATURE_NOTIFICATIONS;
            else if (name == "storage") source.required_features |= MEDICON_FEATURE_STORAGE;
            else if (name == "timers") source.required_features |= MEDICON_FEATURE_TIMERS;
            else if (name == "theme") source.required_features |= MEDICON_FEATURE_THEME;
            else if (name == "input") source.required_features |= MEDICON_FEATURE_INPUT;
            else if (name == "sound") source.required_features |= MEDICON_FEATURE_SOUND;
            else throw std::runtime_error("Unknown required feature: " + name);
        }
    }
}
} // namespace

std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path path_from_utf8(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
bool valid_id(std::string_view s) {
    return !s.empty() && s.size() < 64 && std::ranges::all_of(s, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

ScriptPaths::ScriptPaths(fs::path base) : root(fs::absolute(std::move(base)).lexically_normal()),
    sources(root / "Scripts"), sdk(root / "SDK"), cache(root / "Cache" / "build"), logs(root / "Logs") {}

bool ScriptPaths::contains(const fs::path& base, const fs::path& child) {
    const auto a = fs::weakly_canonical(base), b = fs::weakly_canonical(child);
    auto i = a.begin(), j = b.begin();
    for (; i != a.end(); ++i, ++j) if (j == b.end() || _wcsicmp(i->c_str(), j->c_str()) != 0) return false;
    return true;
}
void ScriptPaths::initialize() const {
    const auto validate = [&](const fs::path& file) {
        if (!contains(root, file)) throw std::runtime_error("Script installation path escapes root: " + utf8(file));
        for (auto current = file; current != root && !current.empty() && current != current.parent_path(); current = current.parent_path())
            if (fs::exists(current) && reparse(current)) throw std::runtime_error("Linked SDK/cache path is not supported: " + utf8(current));
    };
    for (const auto& directory : {root, sources, sdk, sdk / "include" / "medicon", cache, logs}) {
        validate(directory);
        if (fs::exists(directory) && reparse(directory)) throw std::runtime_error("Refusing linked script directory: " + utf8(directory));
        fs::create_directories(directory);
        if (!contains(root, directory)) throw std::runtime_error("Script path escapes root");
    }
    const auto install = [&](const fs::path& file, std::string_view text) {
        validate(file);
        fs::create_directories(file.parent_path());
        if (!fs::exists(file) || read_file(file) != text) atomic_write(file, text);
    };
    for(const auto& header:embedded::headers) install(sdk/"include"/path_from_utf8(header.first),header.second);
    install(sdk / "README.md", std::string("# SDK 0.4\n\n[Getting started](docs/01-getting-started.md) | [API](docs/03-api-reference.md) | [Glass](docs/11-liquid-glass.md) | [UI / Motion](docs/13-animation-and-ui.md) | [Support / QA](docs/16-acceptance-tests-v02.md)\n\n")+std::string(embedded::guide));
    for(const auto& doc:embedded::docs) install(sdk/"docs"/path_from_utf8(doc.first),doc.second);
    for (const auto& example : embedded::examples) {
        const auto file = sdk / "examples" / path_from_utf8(example.first);
        install(file, example.second);
    }
    install(sources / ".clangd", "CompileFlags:\n  CompilationDatabase: ../SDK\n");
}

std::string sha256(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA256 provider unavailable");
    struct Close { BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE* h; ~Close() { if (*h) BCryptDestroyHash(*h); BCryptCloseAlgorithmProvider(a, 0); } } close{algorithm, &hash};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("Cannot create SHA256");
    if (bytes.size() > UINT32_MAX || BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0)
        throw std::runtime_error("Cannot hash input");
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("Cannot finish SHA256");
    std::ostringstream result; result << std::hex << std::setfill('0');
    for (auto byte : digest) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}
std::string sha256_file(const fs::path& path) { return sha256(read_file(path, 128 * 1024 * 1024)); }

void atomic_write(const fs::path& path, std::string_view content) {
    static std::atomic_uint64_t sequence{};
    auto tmp = path; tmp += ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(sequence++);
    struct Remove { fs::path p; ~Remove() { std::error_code e; fs::remove(p, e); } } remove{tmp};
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size())); file.close();
    if (!file || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("Cannot replace: " + utf8(path));
}

std::vector<ScriptSource> ScriptDiscovery::scan(const ScriptPaths& paths, std::vector<ScriptDiagnostic>& diagnostics) {
    std::vector<ScriptSource> result;
    std::vector<fs::path> root_headers;
    for (const auto& item : fs::directory_iterator(paths.sources)) {
        if (item.is_regular_file()) {
            const auto ext = lower(utf8(item.path().extension()));
            if (ext == ".hpp" || ext == ".h" || ext == ".inl") root_headers.push_back(item.path());
        }
    }
    for (const auto& item : fs::directory_iterator(paths.sources)) {
        try {
            if (reparse(item.path()) || !ScriptPaths::contains(paths.sources, item.path())) throw std::runtime_error("Linked sources are not supported");
            ScriptSource s; s.key = utf8(item.path().filename()); s.entry = item.path();
            if (item.is_directory()) {
                s.directory = item.path(); collect_inputs(paths, s.directory, s.inputs, &s.sources);
                if (s.sources.empty()) continue;
                s.entry = s.sources.front(); attributes(s);
            } else if (item.is_regular_file() && lower(utf8(item.path().extension())) == ".cpp") {
                s.directory = paths.sources; s.sources.push_back(item.path()); s.inputs = root_headers; s.inputs.push_back(item.path());
            } else continue;
            if (s.inputs.size() > 2048) throw std::runtime_error("Module exceeds 2048 input files");
            for (const auto& input : s.inputs) if (reparse(input) || fs::file_size(input) > max_input)
                throw std::runtime_error("Source input is linked or exceeds 8 MiB: " + utf8(input));
            std::ranges::sort(s.sources); std::ranges::sort(s.inputs);
            result.push_back(std::move(s));
        } catch (const std::exception& e) {
            diagnostics.push_back({item.path(), 0, 0, "error", "DISCOVERY", e.what(), "Use regular files inside Scripts; review medicon.toml."});
        }
    }
    std::ranges::sort(result, {}, &ScriptSource::key);
    return result;
}
std::string ScriptDiscovery::fingerprint(const ScriptSource& source, const ScriptPaths& paths, std::string_view toolchain) {
    std::string bytes = "medicon-build-v1\n" + std::string(toolchain) + "\n";
    auto inputs = source.inputs;
    std::vector<fs::path> sdk_inputs; collect_inputs(paths, paths.sdk / "include", sdk_inputs, nullptr);
    inputs.insert(inputs.end(), sdk_inputs.begin(), sdk_inputs.end());
    std::ranges::sort(inputs); inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
    for (const auto& input : inputs) { bytes += utf8(input) + "\n" + sha256_file(input) + "\n"; }
    return sha256(bytes);
}

std::vector<ScriptDiagnostic> parse_diagnostics(std::string_view output) {
    std::vector<ScriptDiagnostic> result;
    const std::regex msvc(R"(^(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning|note)\s*([A-Za-z]+\d+)?\s*:\s*(.*)$)");
    const std::regex clang(R"(^(.+?):(\d+):(\d+):\s*(fatal error|error|warning|note):\s*(.*)$)");
    std::istringstream stream{std::string(output)}; std::string line;
    while (std::getline(stream, line)) {
        std::smatch match; ScriptDiagnostic d;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_match(line, match, msvc)) {
            d.file = path_from_utf8(match[1].str()); d.line = std::stoi(match[2]); d.column = match[3].matched ? std::stoi(match[3]) : 0;
            d.severity = match[4]; d.code = match[5]; d.message = match[6];
        } else if (std::regex_match(line, match, clang)) {
            d.file = path_from_utf8(match[1].str()); d.line = std::stoi(match[2]); d.column = std::stoi(match[3]);
            d.severity = match[4]; d.code = "CLANG"; d.message = match[5];
        } else continue;
        d.hint = "Open the source at this line; check SDK/README.md and the medicon.hpp declaration.";
        result.push_back(std::move(d));
        if (result.size() >= 256) break;
    }
    return result;
}

ScriptTrustManager::ScriptTrustManager(fs::path path) : path_(std::move(path)) {}
bool ScriptTrustManager::approved(const BuildResult& b) const {
    try {
        const auto j = read_json(path_); const auto found = j.find(b.source.key);
        return found != j.end() && found->value("source", "") == b.source.fingerprint && found->value("binary", "") == b.binary_hash;
    } catch (...) { return false; } // Corrupt approval state always fails closed.
}
void ScriptTrustManager::approve(const BuildResult& b) {
    auto j = read_json(path_);
    j[b.source.key] = {{"source", b.source.fingerprint}, {"binary", b.binary_hash}};
    atomic_write(path_, j.dump(2));
}
void ScriptTrustManager::revoke(std::string_view key) {
    auto j = read_json(path_); j.erase(std::string(key)); atomic_write(path_, j.dump(2));
}
void append_audit(const ScriptPaths& paths, std::string_view event, std::string_view detail) {
    const auto file = paths.logs / "script-manager.log";
    if (fs::exists(file) && fs::file_size(file) > 4 * 1024 * 1024) {
        std::error_code ignored; fs::remove(paths.logs / "script-manager.log.1", ignored); fs::rename(file, paths.logs / "script-manager.log.1");
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::ofstream stream(file, std::ios::binary | std::ios::app);
    stream << Json{{"time_unix_ms", now}, {"event", event}, {"detail", detail}}.dump() << '\n';
}
} // namespace medicon::scripts
