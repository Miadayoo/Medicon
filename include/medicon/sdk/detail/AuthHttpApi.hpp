#pragma once

#include <cstdint>
#include <string_view>

namespace medicon::sdk::detail {

struct AuthHttpApi {
    std::string_view module;
    std::uintptr_t perform{}, status{}, url{}, header{};

    explicit operator bool() const noexcept { return perform && status && url && header; }
};

// 1.26.60 imports libHttpClient.dll; earlier GDK builds used the other name.
// Look up the game's already-loaded runtime, never load a second libHC instance.
// A call handle and all four APIs must belong to the same runtime. When the
// current game's module is loaded, an incomplete API must fail closed rather
// than install hooks on an unrelated GDK runtime loaded by another component.
template<class FindModule, class FindExport>
AuthHttpApi resolve_auth_http_api(FindModule find_module, FindExport find_export) {
    struct Candidate { const wchar_t* wide; const char* name; };
    constexpr Candidate candidates[] = {
        {L"libHttpClient.dll", "libHttpClient.dll"},
        {L"libHttpClient.GDK.dll", "libHttpClient.GDK.dll"},
    };
    for (const auto& candidate : candidates) {
        const auto module = find_module(candidate.wide);
        if (!module) continue;
        AuthHttpApi api{candidate.name,
            find_export(module, "HCHttpCallPerformAsync"),
            find_export(module, "HCHttpCallResponseGetStatusCode"),
            find_export(module, "HCHttpCallGetRequestUrl"),
            find_export(module, "HCHttpCallRequestSetHeader")};
        return api;
    }
    return {};
}

} // namespace medicon::sdk::detail
