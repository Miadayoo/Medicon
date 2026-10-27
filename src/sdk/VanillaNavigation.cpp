#include <medicon/sdk/VanillaNavigation.hpp>
#include <Windows.h>
#include <winver.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace medicon::sdk {
namespace {
// Exact 1.26.52.3 observations from the supplied IDA evidence and matching PE.
// These checks are not scanner patterns; no signatures or offsets are guessed.
constexpr std::uintptr_t update_rva = 0x4CF40B0, caller_rva = 0x483A4CA, return_rva = 0x483A4DB;
constexpr std::uintptr_t controller_table = 0xE900070, scene_table = 0xE8E9080;
constexpr std::uintptr_t game_table = 0xE76E2D0, app_table = 0xE76EBF0, exit_rva = 0x2240F40;
constexpr std::uint64_t expiry_ms = 1500, debounce_ms = 350;
struct Handler { NavigationAction action; std::uint32_t key; std::uintptr_t table, invoke; const char* name; };
constexpr std::array handlers{
    Handler{NavigationAction::Play,3372022197u,0xE9024C0,0x4D4A730,"Play"},
    Handler{NavigationAction::Settings,4202751130u,0xE902718,0x4D50E70,"Settings"},
    Handler{NavigationAction::Quit,116523271u,0xE805F70,0x22BAAC0,"Quit"}
};
bool copy(std::uintptr_t at, void* output, std::size_t size) noexcept {
    if (!at || !output || !size || size - 1 > std::numeric_limits<std::uintptr_t>::max() - at) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), output, size, &copied) && copied == size;
}
template<class T> bool read(std::uintptr_t at, std::size_t offset, T& output) noexcept {
    return at && offset <= std::numeric_limits<std::uintptr_t>::max() - at && copy(at + offset, &output, sizeof(output));
}
bool bytes(std::uintptr_t at, std::initializer_list<std::uint8_t> expected) noexcept {
    std::array<std::uint8_t,96> actual{};
    return expected.size() <= actual.size() && copy(at, actual.data(), expected.size()) &&
           std::equal(expected.begin(), expected.end(), actual.begin());
}
bool equals(std::uintptr_t at, std::size_t offset, std::uintptr_t expected) noexcept {
    std::uintptr_t actual = 0;
    return expected && read(at,offset,actual) && actual == expected;
}
bool owner_alive(std::uintptr_t control) noexcept {
    std::int32_t count = 0;
    return read(control,8,count) && count > 0 && count < 0x40000000;
}
bool valid_flag(std::uintptr_t control) noexcept {
    std::uint8_t flag = 0;
    return read(control,0,flag) && flag == 1;
}
bool exact_host(std::string& diagnostic) {
    std::array<wchar_t,32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) { diagnostic = "Cannot inspect navigation executable."; return false; }
    const std::wstring_view full(path.data(),length);
    const auto separator = full.find_last_of(L"\\/");
    const std::wstring name(full.substr(separator == std::wstring_view::npos ? 0 : separator + 1));
    if (_wcsicmp(name.c_str(),L"Minecraft.Windows.exe") != 0) {
        diagnostic = "Native menu navigation is disabled outside Minecraft.Windows.exe 1.26.52.3."; return false;
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.data(),&ignored);
    if (!size || size > 16*1024*1024) { diagnostic = "Cannot inspect navigation file version."; return false; }
    std::vector<std::byte> data(size);
    VS_FIXEDFILEINFO* version = nullptr;
    UINT count = 0;
    if (!GetFileVersionInfoW(path.data(),0,size,data.data()) ||
        !VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&version),&count) ||
        !version || count < sizeof(*version) || version->dwSignature != 0xFEEF04BD ||
        version->dwFileVersionMS != MAKELONG(26,1) || version->dwFileVersionLS != MAKELONG(3,52)) {
        diagnostic = "Native menu navigation requires the inspected 1.26.52.3 build; disabled."; return false;
    }
    return true;
}
// Two packagings of the same 1.26.52.3 link (identical TimeDateStamp). The
// inspected LeviLauncher copy carries a launcher-added .levildr section; the
// official install is one page smaller and has its own checksum (observed in a
// user's inject.log). profile() still verifies every instruction and table this
// feature reads or calls at the shared RVAs, so a shifted layout fails closed.
struct Packaging { std::uint32_t image_size, checksum; };
constexpr std::array packagings{
    Packaging{0x12C02000,0x128DA293}, // LeviLauncher versions/1.26.52.03, inspected
    Packaging{0x12C01000,0x128CD801}  // official Minecraft install, 8 sections
};
bool known_packaging(std::uint32_t image_size, std::uint32_t checksum) noexcept {
    return std::ranges::any_of(packagings,[=](const Packaging& p){return p.image_size==image_size && p.checksum==checksum;});
}
bool loaded_image(memory::AddressRange& image, std::vector<memory::AddressRange>& executable, std::string& diagnostic) {
    // Inspect only headers/section metadata, avoiding a second full .text copy.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!copy(base,&dos,sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew != 0x78 ||
        !copy(base + static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp != 0x6AB54E37 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        // The inspected launcher/runtime rewrites ImageBase to the ASLR load
        // address. Accept that observed loaded form as well as the disk value;
        // timestamp, checksum, size and all loaded code/table checks still apply.
        (nt.OptionalHeader.ImageBase != 0x140000000ULL && nt.OptionalHeader.ImageBase != base) ||
        !known_packaging(nt.OptionalHeader.SizeOfImage,nt.OptionalHeader.CheckSum) ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) {
        // Log the observed header so a report from another PC names the
        // differing field.
        char text[352]{};
        std::snprintf(text,sizeof text,"Native menu PE header mismatch (e_lfanew=0x%lX TimeDateStamp=0x%08lX "
            "ImageBase=0x%llX SizeOfImage=0x%lX CheckSum=0x%08lX sections=%u; known 0x78/0x6AB54E37 with "
            "0x12C02000/0x128DA293 or 0x12C01000/0x128CD801); navigation disabled.",static_cast<unsigned long>(dos.e_lfanew),
            static_cast<unsigned long>(nt.FileHeader.TimeDateStamp),static_cast<unsigned long long>(nt.OptionalHeader.ImageBase),
            static_cast<unsigned long>(nt.OptionalHeader.SizeOfImage),static_cast<unsigned long>(nt.OptionalHeader.CheckSum),
            static_cast<unsigned>(nt.FileHeader.NumberOfSections));
        diagnostic=text;
        return false;
    }
    image = {base,nt.OptionalHeader.SizeOfImage};
    const auto sections = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64,OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    executable.clear();
    if (!image.contains(sections,nt.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER))) return false;
    for (unsigned i=0; i<nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!copy(sections + i*sizeof(section),&section,sizeof(section))) return false;
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto length = static_cast<std::size_t>(section.Misc.VirtualSize);
        if (!length || !image.contains(base + section.VirtualAddress,length)) return false;
        executable.push_back({base + section.VirtualAddress,length});
    }
    return !executable.empty();
}

bool live_context(std::uintptr_t base,const MenuSessionIdentity& s,std::uintptr_t controller) noexcept {
    if (!controller || s.controller != controller || !s.definition ||
        !equals(s.client,0x1A8,s.game) || !equals(s.client,0x208,s.factory) ||
        !equals(s.factory,8,s.factory_control) || !valid_flag(s.factory_control) || !equals(s.factory,0x18,s.client) ||
        !equals(s.game,0,base + game_table) || !equals(s.game,0x1D70,s.stack) ||
        !equals(s.stack,8,s.stack_control) || !valid_flag(s.stack_control) ||
        !equals(s.scene,0,base + scene_table) || !equals(s.scene,0x40,s.view) ||
        !equals(s.view,0x40,controller) || !equals(s.view,0x50,s.definition_owner) ||
        !equals(s.definition_owner,8,s.definition) || !equals(controller,0,base + controller_table)) return false;
    std::uintptr_t begin=0,end=0,control=0,model=0,model_control=0,app_flag=0;
    std::uint8_t blocked=1,closing=1,quit=1;
    std::uint32_t mode=0;
    if (!read(s.stack,0x18,begin) || !read(s.stack,0x20,end) || end <= begin ||
        end-begin > 32*256 || (end-begin)%32 || !equals(end-24,0,s.scene) ||
        !read(s.view,0x48,control) || !owner_alive(control) ||
        !read(s.view,0x460,blocked) || blocked || !read(s.view,0x465,closing) || closing ||
        !read(controller,0xD48,model) || !model || !read(controller,0xD50,model_control) || !owner_alive(model_control) ||
        !equals(controller,0xBF0,model) || !equals(controller,0xBF8,model_control) ||
        !read(controller,0xC00,mode) || mode != 1 || !equals(s.game,0x78,base + app_table) ||
        !read(s.game,0x80,app_flag) || !valid_flag(app_flag) || !read(s.game,0xB1,quit) || quit) return false;
    return equals(s.stack,0x18,begin) && equals(s.stack,0x20,end) && equals(end-24,0,s.scene) &&
           equals(s.view,0x48,control) && equals(controller,0xD48,model) && equals(controller,0xD50,model_control) &&
           equals(s.view,0x40,controller) && valid_flag(s.stack_control) && valid_flag(s.factory_control);
}
struct Callable { std::uintptr_t node=0,begin=0,end=0,capacity=0,sentinel=0,count=0; };
bool find_callable(std::uintptr_t base,std::uintptr_t controller,const Handler& handler,Callable& found) noexcept {
    // Inspect the existing game's list with bounded bidirectional linkage.
    // No foreign unordered_map/vector declaration, mutation, copy or destruction.
    std::uintptr_t sentinel=0,count=0,node=0,previous=0;
    if (!read(controller,0x828,sentinel) || !read(controller,0x830,count) ||
        !sentinel || !count || count>1024 || !read(sentinel,0,node)) return false;
    previous=sentinel;
    for (std::uintptr_t i=0;i<count;++i) {
        std::uintptr_t next=0,back=0;
        std::uint32_t key=0;
        if (!node || node==sentinel || !read(node,0,next) || !read(node,8,back) ||
            back!=previous || !read(node,0x10,key)) return false;
        if (key==handler.key) {
            if (found.node || !read(node,0x18,found.begin) || !read(node,0x20,found.end) || !read(node,0x28,found.capacity) ||
                !found.begin || found.end<found.begin || found.end-found.begin!=64 || found.capacity<found.end ||
                found.capacity-found.begin>64*256 || (found.capacity-found.begin)%64 ||
                !equals(found.begin,0,base+handler.table) || !equals(found.begin,8,controller)) return false;
            found.node=node;
        }
        previous=node;
        node=next;
    }
    if (node!=sentinel || !equals(sentinel,8,previous) || !found.node) return false;
    found.sentinel=sentinel;
    found.count=count;
    return true;
}
bool unchanged(std::uintptr_t base,std::uintptr_t controller,const Handler& h,const Callable& f) noexcept {
    return equals(controller,0x828,f.sentinel) && equals(controller,0x830,f.count) &&
           equals(f.node,0x18,f.begin) && equals(f.node,0x20,f.end) && equals(f.node,0x28,f.capacity) &&
           equals(f.begin,0,base+h.table) && equals(f.begin,8,controller) && equals(base+h.table,0x10,base+h.invoke);
}
} // namespace

bool VanillaNavigation::executable(std::uintptr_t address,std::size_t size) const noexcept {
    if (!size || !std::ranges::any_of(executable_regions_,[=](const auto& range){return range.contains(address,size);})) return false;
    // A resident page's protection comes from its own PTE in ~0.5us. VirtualQuery
    // instead walks every PTE to the end of Minecraft's ~300MB image region
    // (~1.5ms+ per call), which hitched the game UI thread on every click.
    using QueryWorkingSetEx = BOOL(WINAPI*)(HANDLE,PVOID,DWORD);
    static const auto query_working_set = reinterpret_cast<QueryWorkingSetEx>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32QueryWorkingSetEx")));
    if (query_working_set) {
        // Bits 0 Valid, 1-3 ShareCount, 4-14 Win32Protection (PSAPI_WORKING_SET_EX_BLOCK).
        struct Entry { PVOID address; ULONG_PTR attributes; };
        std::array<Entry,2> pages{{{reinterpret_cast<PVOID>(address&~std::uintptr_t{4095}),0},
                                   {reinterpret_cast<PVOID>((address+size-1)&~std::uintptr_t{4095}),0}}};
        if (query_working_set(GetCurrentProcess(),pages.data(),static_cast<DWORD>(sizeof(pages))) &&
            (pages[0].attributes&1) && (pages[1].attributes&1)) {
            return std::ranges::all_of(pages,[](const Entry& page) {
                const auto protection=static_cast<DWORD>((page.attributes>>4)&0x7FF);
                if (protection&(PAGE_GUARD|PAGE_NOACCESS)) return false;
                const auto base=protection&0xFF;
                return base==PAGE_EXECUTE || base==PAGE_EXECUTE_READ || base==PAGE_EXECUTE_READWRITE ||
                       base==PAGE_EXECUTE_WRITECOPY;
            });
        }
    }
    // Non-resident page: no PTE to read, so fall back to the full query.
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
        (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto protection=region.Protect&0xFF;
    return (protection==PAGE_EXECUTE || protection==PAGE_EXECUTE_READ || protection==PAGE_EXECUTE_READWRITE ||
            protection==PAGE_EXECUTE_WRITECOPY) &&
           memory::AddressRange{reinterpret_cast<std::uintptr_t>(region.BaseAddress),region.RegionSize}.contains(address,size);
}
bool VanillaNavigation::profile(bool include_update_entry) const noexcept {
    const auto b=image_.address;
    if (!b || !image_.contains(b+controller_table,0x68) || !image_.contains(b+scene_table,0x1C0) ||
        !executable(b+update_rva,27) || !executable(b+caller_rva,17) || !executable(b+exit_rva,18) ||
        !equals(b+controller_table,0x10,b+update_rva) || !equals(b+controller_table,0x18,b+0x4819AF0) ||
        !equals(b+controller_table,0x60,b+exit_rva) || !equals(b+scene_table,0x98,b+0x48576B0) ||
        !equals(b+app_table,0x20,b+0x834960) || !equals(b+app_table,0x28,b+0x834970) ||
        !bytes(b+caller_rva,{0x48,0x8B,0x4F,0x40,0x48,0x8B,0x01,0x48,0x8B,0x40,0x10,0xFF,0x15,0xCD,0x15,0x8C,0x0A}) ||
        !bytes(b+0x834970,{0x0F,0xB6,0x41,0x39,0xC3}) || !bytes(b+0x834960,{0xC6,0x41,0x39,0x01,0xC3}) ||
        !bytes(b+exit_rva,{0x55,0x56,0x57,0x48,0x81,0xEC,0xE0,0,0,0,0x48,0x8D,0xAC,0x24,0x80,0,0,0}) ||
        !bytes(b+0x4D4A730,{0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0xB0,0,0,0,0x48,0x8D,0xAC,0x24,0x80,0,0,0,
                           0x48,0xC7,0x45,0x28,0xFE,0xFF,0xFF,0xFF,0x48,0x8B,0x71,0x08,0x48,0x8B,0x9E,0xF0,0x0B,0,0}) ||
        !bytes(b+0x4D50E70,{0x55,0x56,0x57,0x53,0x48,0x81,0xEC,0x98,0,0,0,0x48,0x8D,0xAC,0x24,0x80,0,0,0,
                           0x48,0xC7,0x45,0x10,0xFE,0xFF,0xFF,0xFF,0x48,0x8B,0x71,0x08,0x48,0x8B,0x9E,0xF0,0x0B,0,0}) ||
        !bytes(b+0x22BAAC0,{0x48,0x8B,0x49,0x08,0x48,0x8B,0x01,0x48,0x8B,0x40,0x60,0x48,0x8B,0x15,0xD6,0x0F,0xE4,0x0C,0x48,0xFF,0xE2})) return false;
    if (include_update_entry && !bytes(b+update_rva,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,
        0x48,0x81,0xEC,0xA8,0x06,0,0,0x48,0x8D,0xAC,0x24,0x80,0,0,0})) return false;
    for (const auto& h:handlers)
        if (!image_.contains(b+h.table,24) || !executable(b+h.invoke,16) || !equals(b+h.table,0,b+0x791D0) ||
            !equals(b+h.table,8,b+0x76BC0) || !equals(b+h.table,0x10,b+h.invoke)) return false;
    return true;
}
bool VanillaNavigation::initialize(std::string& diagnostic) noexcept {
    try {
        std::lock_guard lock(mutex_);
        diagnostic.clear();
        if (initialized_) {diagnostic=terminal_diagnostic_;return ready_.load(std::memory_order_acquire);}
        initialized_=true;
        const bool image_ok=exact_host(diagnostic) && loaded_image(image_,executable_regions_,diagnostic);
        if (!image_ok || !profile(true)) {
            if (diagnostic.empty()) diagnostic=image_ok?"Native menu code/table profile mismatch; navigation disabled.":
                                                        "Native menu PE section layout mismatch; navigation disabled.";
            terminal_diagnostic_=diagnostic;
            return false;
        }
        terminal_diagnostic_.clear();
        ready_.store(true,std::memory_order_release);
        diagnostic="Native menu registered handlers ready at the verified controller update caller.";
        return true;
    } catch (...) {
        ready_.store(false,std::memory_order_release);
        try {diagnostic="Native menu initialization failed; navigation disabled.";} catch (...) {}
        return false;
    }
}
std::uintptr_t VanillaNavigation::update_hook_target() const noexcept {
    return ready_.load(std::memory_order_acquire)?image_.address+update_rva:0;
}
bool VanillaNavigation::has_pending() const noexcept {return pending_.load(std::memory_order_acquire);}
bool VanillaNavigation::request(NavigationAction action,std::uint64_t epoch) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!epoch || !ready_.load(std::memory_order_acquire) || pumping_.test(std::memory_order_acquire)) return false;
        const auto now=GetTickCount64();
        if (request_ && request_->epoch>=epoch && now-request_->tick<=expiry_ms) return false;
        if (dispatched_ && last_dispatch_epoch_==epoch && now-last_dispatch_tick_<debounce_ms) return false;
        request_=Request{action,epoch,now};
        pending_.store(true,std::memory_order_release);
        return true;
    } catch (...) {return false;}
}
bool VanillaNavigation::openPlay(std::uint64_t epoch) noexcept {return request(NavigationAction::Play,epoch);}
bool VanillaNavigation::openSettings(std::uint64_t epoch) noexcept {return request(NavigationAction::Settings,epoch);}
bool VanillaNavigation::quitGame(std::uint64_t epoch) noexcept {return request(NavigationAction::Quit,epoch);}
void VanillaNavigation::cancel(std::uint64_t retired_epoch) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (request_ && request_->epoch<=retired_epoch) {request_.reset();pending_.store(false,std::memory_order_release);}
    } catch (...) {}
}
void VanillaNavigation::stop() noexcept {
    ready_.store(false,std::memory_order_release);
    try {
        std::lock_guard lock(mutex_);
        request_.reset();pending_.store(false,std::memory_order_release);
        initialized_=false;dispatched_=false;terminal_diagnostic_.clear();
        // Profile storage remains valid until the host drains native callbacks.
    } catch (...) {}
}
bool VanillaNavigation::pump(void* native_controller,std::uintptr_t return_address,const MenuSessionIdentity& session,
                             const std::atomic<std::uint64_t>& epoch,const std::atomic_bool& stopping,
                             std::string& diagnostic) noexcept {
    if (!has_pending() || pumping_.test_and_set(std::memory_order_acquire)) return false;
    struct Exit {std::atomic_flag& flag;~Exit(){flag.clear(std::memory_order_release);}} exit{pumping_};
    try {
        diagnostic.clear();
        std::optional<Request> selected;
        {std::lock_guard lock(mutex_);selected=request_;request_.reset();pending_.store(false,std::memory_order_release);}
        if (!selected) return false;
        const auto h=std::ranges::find_if(handlers,[&](const Handler& value){return value.action==selected->action;});
        const auto fail=[&](const char* reason){diagnostic=std::string(h==handlers.end()?"Unknown":h->name)+" rejected: "+reason;return false;};
        if (!ready_.load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) return fail("shutdown.");
        if (selected->epoch!=epoch.load(std::memory_order_acquire)) return fail("menu session changed.");
        if (GetTickCount64()-selected->tick>expiry_ms) return fail("request expired before native UI update.");
        const auto b=image_.address, controller=reinterpret_cast<std::uintptr_t>(native_controller);
        if (return_address!=b+return_rva) return fail("unverified controller update caller.");
        if (!profile(false)) {
            ready_.store(false,std::memory_order_release);
            return fail("loaded code/table profile changed; native navigation disabled for this client lifecycle.");
        }
        if (!live_context(b,session,controller)) return fail("scene/ownership/input/quit context changed.");
        if (h==handlers.end()) return fail("unknown action.");
        Callable found;
        if (!find_callable(b,controller,*h,found)) return fail("registration missing, changed, duplicated or not one inspected handler.");
        if (!live_context(b,session,controller) || !unchanged(b,controller,*h,found)) return fail("context or callable changed before dispatch.");
        // Prepare diagnostics before a native handler can invalidate the scene.
        diagnostic=std::string(h->name)+" invoked through its existing native registration.";
        {
            std::lock_guard lock(mutex_);
            if (!ready_.load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) ||
                selected->epoch!=epoch.load(std::memory_order_acquire)) return fail("invalidated before dispatch.");
            last_dispatch_tick_=GetTickCount64();last_dispatch_epoch_=selected->epoch;dispatched_=true;
        }
        // These three inspected bodies ignore their incoming payload. Match
        // native scalar invoke ABI; never fabricate ScreenEvent/std::function.
        using Invoke=std::uint32_t (*)(void*,const std::uintptr_t*);
        const std::uintptr_t unused_payload=0;
        reinterpret_cast<Invoke>(b+h->invoke)(reinterpret_cast<void*>(found.begin),&unused_payload);
        // The handler may destroy the old scene/controller/vector immediately.
        // There is deliberately no native-object access after invocation.
        return true;
    } catch (...) {
        try {diagnostic="Native menu request failed; no further navigation attempted.";} catch (...) {}
        return false;
    }
}
} // namespace medicon::sdk
