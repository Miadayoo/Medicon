#include <medicon/scripts/ScriptServices.hpp>
#include "ScriptInspectionError.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <cctype>
#include <stdexcept>
#include <sstream>

namespace medicon::scripts {
namespace {
[[noreturn]] void missing_registration(std::string_view detail) {
    throw ScriptInspectionError("SCRIPT_REGISTRATION","Script registration is missing ("+std::string(detail)+"). Exactly one MEDICON_MODULE(...) is required.",
        "Add MEDICON_MODULE(YourModule, \"unique_id\", \"Display Name\", medicon::Category::Utility); after your Module class in one .cpp file. See SDK/examples/HelloWorld.cpp or Create Script > HelloWorld. If already present, check that the header is the bundled SDK/include/medicon.hpp; restart Medicon to restore the SDK.");
}
struct Pe {
    std::vector<char> bytes;
    IMAGE_NT_HEADERS64 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
    template<class T> T read(size_t offset) const {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) throw std::runtime_error("Truncated PE image");
        T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
    }
    size_t rva(uint64_t address,size_t length,std::string_view field) const {
        for (const auto& section : sections) {
            if(address>UINT32_MAX) break;
            if (address < section.VirtualAddress) continue;
            const auto delta = static_cast<uint64_t>(address) - section.VirtualAddress;
            if (delta > section.SizeOfRawData || length > section.SizeOfRawData - delta) continue;
            const auto offset = static_cast<uint64_t>(section.PointerToRawData) + delta;
            if (offset > bytes.size() || length > bytes.size() - offset) break;
            return static_cast<size_t>(offset);
        }
        std::ostringstream message;
        message<<"Invalid script DLL: "<<field<<" RVA 0x"<<std::hex<<address<<std::dec<<" ("<<length<<" bytes) is outside file-backed sections";
        throw ScriptInspectionError("SCRIPT_PE_RVA",message.str(),
            "Rebuild with the installed SDK and a Windows x64 toolchain. Share Build log and the generated module.dll if this also fails for SDK/examples/HelloWorld.cpp.");
    }
    std::string string(uint32_t address,std::string_view field) const {
        std::string result;
        for (uint32_t i = 0; i < 256; ++i) {
            const char c = read<char>(rva(static_cast<uint64_t>(address)+i,1,field));
            if (!c) return result;
            result += c;
        }
        throw std::runtime_error("Unterminated PE string");
    }
    bool executable(uint32_t address) const {
        return std::ranges::any_of(sections, [&](const auto& s) { return address >= s.VirtualAddress &&
            static_cast<uint64_t>(address) - s.VirtualAddress < s.SizeOfRawData && (s.Characteristics & IMAGE_SCN_MEM_EXECUTE); });
    }
};
}

MediconScriptManifest inspect_plugin(const fs::path& path) {
    if (fs::file_size(path) > 128 * 1024 * 1024) throw std::runtime_error("DLL exceeds 128 MiB");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open script DLL");
    Pe pe; pe.bytes.assign(std::istreambuf_iterator<char>(file), {});
    const auto dos = pe.read<IMAGE_DOS_HEADER>(0);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) throw std::runtime_error("Invalid DOS header");
    pe.nt = pe.read<IMAGE_NT_HEADERS64>(static_cast<size_t>(dos.e_lfanew));
    if (pe.nt.Signature != IMAGE_NT_SIGNATURE || pe.nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(pe.nt.FileHeader.Characteristics & IMAGE_FILE_DLL) || pe.nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe.nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) || pe.nt.FileHeader.NumberOfSections > 96 ||
        pe.nt.OptionalHeader.NumberOfRvaAndSizes < IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        throw std::runtime_error("Scripts require a Windows x64 PE DLL");
    for (size_t i = 0; i < pe.nt.FileHeader.NumberOfSections; ++i)
        pe.sections.push_back(pe.read<IMAGE_SECTION_HEADER>(static_cast<size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) + i * sizeof(IMAGE_SECTION_HEADER)));
    const auto dir = pe.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    // A successfully linked DLL can have no exports (e.g. the registration
    // macro was omitted). RVA 0 is absence, not a section offset to parse.
    if(!dir.VirtualAddress) missing_registration("DLL has no exports");
    if(dir.Size<sizeof(IMAGE_EXPORT_DIRECTORY)) throw ScriptInspectionError("SCRIPT_PE_EXPORTS","Invalid script DLL: truncated export directory","Rebuild the script with the installed SDK and inspect Build log.");
    const auto exports = pe.read<IMAGE_EXPORT_DIRECTORY>(pe.rva(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY),"export directory"));
    if (exports.NumberOfNames > 1024 || exports.NumberOfFunctions > 65536) throw std::runtime_error("Excessive PE exports");
    if(!exports.NumberOfNames||!exports.NumberOfFunctions) missing_registration("DLL has no named exports");
    uint32_t manifest_rva = 0, entry_rva = 0;
    for (uint32_t i = 0; i < exports.NumberOfNames; ++i) {
        const auto name = pe.string(pe.read<uint32_t>(pe.rva(static_cast<uint64_t>(exports.AddressOfNames)+i*4,4,"export name table")),"export name");
        if (name != "medicon_script_manifest" && name != "medicon_script_entry") continue;
        const auto ordinal = pe.read<uint16_t>(pe.rva(static_cast<uint64_t>(exports.AddressOfNameOrdinals)+i*2,2,"export ordinal table"));
        if (ordinal >= exports.NumberOfFunctions) throw std::runtime_error("Invalid PE export ordinal");
        const auto address = pe.read<uint32_t>(pe.rva(static_cast<uint64_t>(exports.AddressOfFunctions)+ordinal*4,4,"export function table"));
        if (address >= dir.VirtualAddress && static_cast<uint64_t>(address) < static_cast<uint64_t>(dir.VirtualAddress) + dir.Size)
            throw std::runtime_error("Forwarded script exports are forbidden");
        if (name == "medicon_script_manifest") manifest_rva = address; else entry_rva = address;
    }
    if(!manifest_rva||!entry_rva) missing_registration("medicon_script_manifest or medicon_script_entry is not exported");
    if(!pe.executable(entry_rva)) throw ScriptInspectionError("SCRIPT_PE_ENTRY","Invalid script DLL: medicon_script_entry is not in executable file-backed code","Rebuild with MEDICON_MODULE and the installed SDK.");
    auto m = pe.read<MediconScriptManifest>(pe.rva(manifest_rva, sizeof(MediconScriptManifest),"medicon_script_manifest"));
    if (m.magic != MEDICON_SCRIPT_MAGIC || m.size != sizeof(m) || m.abi_version != MEDICON_SCRIPT_ABI_VERSION ||
        (m.api_version != MEDICON_SCRIPT_API_VERSION && m.api_version != MEDICON_SCRIPT_LEGACY_API_VERSION)) throw std::runtime_error("Incompatible script ABI/API; rebuild with the installed SDK");
    if (!std::memchr(m.id, 0, sizeof(m.id)) || !std::memchr(m.name, 0, sizeof(m.name)) || !valid_id(m.id) || !m.name[0] || m.category > 4 || m.reserved)
        throw std::runtime_error("Invalid script module metadata");
    // Static CRT is required: dependent DLL lookup may not execute unapproved code.
    const std::set<std::string> system_imports{"kernel32.dll", "ntdll.dll", "user32.dll", "advapi32.dll", "shell32.dll", "ole32.dll",
        "oleaut32.dll", "ws2_32.dll", "bcrypt.dll", "crypt32.dll", "gdi32.dll", "comdlg32.dll", "combase.dll", "ucrtbase.dll", "msvcrt.dll"};
    const auto imports = pe.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (pe.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size) throw std::runtime_error("Delay-loaded dependencies are not supported");
    for (uint64_t offset = 0; imports.VirtualAddress && offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const auto d = pe.read<IMAGE_IMPORT_DESCRIPTOR>(pe.rva(static_cast<uint64_t>(imports.VirtualAddress)+offset,sizeof(IMAGE_IMPORT_DESCRIPTOR),"import directory"));
        if (!d.Name) break;
        auto name = pe.string(d.Name,"import DLL name"); std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!system_imports.contains(name) && !(name.starts_with("api-ms-win-") && name.ends_with(".dll")))
            throw std::runtime_error("Non-system DLL dependency is unsupported: " + name + "; use the static CRT");
    }
    return m;
}
} // namespace medicon::scripts
