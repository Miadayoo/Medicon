#include <medicon/sdk/CustomModelStatic.hpp>
#include <medicon/sdk/CustomModelP2Patterns.hpp>
#include <medicon/sdk/CustomModelRender.hpp>
#include <medicon/memory/signature_registry.hpp>
#include <Windows.h>
#include <cstring>
namespace medicon::sdk::custom_model {
std::array<std::byte,56> raw_texture_location(std::string_view path) noexcept {
    std::array<std::byte,56> out{};
    const auto put=[&](std::size_t offset,const auto& value) { std::memcpy(out.data()+offset,&value,sizeof(value)); };
    put(0,std::uint32_t{2});
    const auto size=static_cast<std::uint64_t>(path.size());
    if (size<=15) { std::memcpy(out.data()+8,path.data(),path.size());put(0x20,std::uint64_t{15}); }
    else { put(8,path.data());put(0x20,size); }
    put(0x18,size);
    constexpr std::uint64_t basis=0xCBF29CE484222325ULL,prime=0x100000001B3ULL;
    auto hash=basis;for (unsigned char c:path) hash=(hash^c)*prime;
    put(0x28,hash);put(0x30,((basis^2)*prime)^hash);return out;
}
StaticTargets locate_static_targets(std::string& diagnostic) {
    const auto module=memory::current_process_module(&diagnostic);
    if (!module) return {};
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};SIZE_T count{};
    if (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(module->image.address),&dos,sizeof dos,&count)||
        dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||!module->image.contains(module->image.address+dos.e_lfanew,sizeof nt)||
        !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(module->image.address+dos.e_lfanew),&nt,sizeof nt,&count)||
        nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64) {
        diagnostic="P2 executable profile differs from IDA evidence.";return {};
    }
    if (!bedrock_1_26_52_3::known_pe_identity(nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage)) {
        diagnostic="P2 executable profile differs from IDA evidence ("+
            bedrock_1_26_52_3::pe_identity_text(nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage)+").";return {};
    }
    using D=memory::SignatureDefinition;using R=memory::SignatureResolution;
    constexpr auto version=bedrock_1_26_52_3::file_version;
    constexpr std::array definitions{
        D{"texture_get",p2_native::texture_get,R::direct,0,0,version,false},
        D{"end",p2_native::tess_end,R::direct,0,0,version,false},
        D{"draw",p2_native::mesh_draw,R::direct,0,0,version,false},
        D{"destroy",p2_native::mesh_destroy,R::direct,0,0,version,false},
        D{"clear",p2_native::builder_clear,R::direct,0,0,version,false},
        D{"view",p2_native::view_id,R::direct,0,0,version,false}};
    memory::SignatureRegistry registry;registry.scan(definitions,module->executable_regions,module->image);
    std::array<std::uintptr_t,6> a{};
    for (unsigned i=0;i<a.size();++i) {
        const auto* r=registry.find(definitions[i].name);
        if (!r||!r->address) { diagnostic="P2 "+std::string(definitions[i].name)+": "+std::string(r?memory::to_string(r->status):"missing");return {}; }
        a[i]=*r->address;
    }
    diagnostic="P2 texture and cached mesh entries uniquely resolved.";
    return {reinterpret_cast<TextureGetFn>(a[0]),reinterpret_cast<TessEndFn>(a[1]),reinterpret_cast<BuiltMeshDrawFn>(a[2]),
        reinterpret_cast<MeshDestroyFn>(a[3]),reinterpret_cast<BuilderClearFn>(a[4]),reinterpret_cast<ViewIdFn>(a[5])};
}
}
