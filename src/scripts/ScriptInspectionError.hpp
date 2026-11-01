#pragma once
#include <stdexcept>
#include <string>
#include <utility>

namespace medicon::scripts {
// Host-internal diagnostics; never part of the script DLL ABI.
class ScriptInspectionError final : public std::runtime_error {
public:
    ScriptInspectionError(std::string code,std::string message,std::string hint)
        : std::runtime_error(std::move(message)),code(std::move(code)),hint(std::move(hint)) {}
    std::string code,hint;
};
}
