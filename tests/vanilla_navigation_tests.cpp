#include <medicon/sdk/VanillaNavigation.hpp>
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace medicon::sdk {
// Synthetic host admission exists only in this test executable. Shipping
// initialize() still requires the exact executable/version/PE/code profile.
struct VanillaNavigationTestAccess {
    static bool use_image(VanillaNavigation& n,memory::AddressRange image) {
        n.image_=image;n.executable_regions_={image};
        if (!n.profile(true)) return false;
        n.initialized_=true;n.ready_.store(true);return true;
    }
    static void expire(VanillaNavigation& n) {std::lock_guard lock(n.mutex_);n.request_->tick=GetTickCount64()-1501;}
};
}
namespace {
using medicon::sdk::VanillaNavigation;
using medicon::sdk::MenuSessionIdentity;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::atomic<unsigned> invoked{0};
void* expected_argument=nullptr;
bool argument_matches=false,block_native_after_call=false;
void* native_allocation=nullptr;
VanillaNavigation* reentrant_navigation=nullptr;
std::uint64_t reentrant_epoch=0;
bool reentrant_rejected=false;

std::uint32_t record(void* argument,const std::uintptr_t*) noexcept {
    argument_matches=argument==expected_argument;
    invoked.fetch_add(1);
    if(reentrant_navigation) reentrant_rejected=!reentrant_navigation->openPlay(reentrant_epoch);
    if(block_native_after_call) {DWORD old=0;VirtualProtect(native_allocation,0x10000,PAGE_NOACCESS,&old);}
    return 0x76543210u;
}
struct Fixture {
    static constexpr std::size_t image_size=0x1383B000;
    std::byte* image=static_cast<std::byte*>(VirtualAlloc(nullptr,image_size,MEM_RESERVE,PAGE_NOACCESS));
    std::byte* native=static_cast<std::byte*>(VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    VanillaNavigation navigation;
    MenuSessionIdentity session;
    std::atomic<std::uint64_t> epoch{10};
    std::atomic_bool stopping{false};
    std::string reason;
    static constexpr std::array<std::uint32_t,3> keys{3372022197u,4202751130u,116523271u};
    static constexpr std::array<std::uintptr_t,3> tables{0xF21BF40,0xF21C418,0xF2039E0};
    static constexpr std::array<std::uintptr_t,3> entries{0x4F5DE80,0x4F67590,0x26D7E80};
    Fixture() {
        require(image&&native,"Cannot allocate synthetic host.");
        try {
            observed_profile();
            session={at(0),at(0x1000),at(0x3000),at(0x6200),at(0x3100),at(0x6240),
                     at(0x3300),at(0x3400),at(0x3900),at(0x3940),at(0x4000)};
            put(0x1C8,session.game);put(0x228,session.factory);
            put(0x3008,session.factory_control);put(0x3018,session.client);put(0x6200,std::uint8_t{1});
            put(0x1000,base()+0xF091B70);put(0x2DF8,session.stack);
            put(0x1070,base()+0xF092470);put(0x1078,at(0x6280));put(0x6280,std::uint8_t{1});
            put(0x3108,session.stack_control);put(0x6240,std::uint8_t{1});
            put(0x3118,at(0x6400));put(0x3120,at(0x6480));put(0x6468,session.scene);
            put(0x3300,base()+0xF20EC60);put(0x3340,session.view);
            put(0x3440,session.controller);put(0x3448,at(0x62C0));put(0x62C8,std::int32_t{1});
            put(0x3450,session.definition_owner);put(0x3908,session.definition);
            put(0x4000,base()+0xF21A2E0);put(0x4BF0,at(0x6000));put(0x4D48,at(0x6000));
            put(0x4BF8,at(0x6300));put(0x4D50,at(0x6300));put(0x6308,std::int32_t{2});put(0x4C00,std::uint32_t{1});
            put(0x4828,at(0x6500));put(0x4830,std::uintptr_t{3});
            put(0x6500,at(0x6600));put(0x6508,at(0x6680));
            for(std::size_t i=0;i<3;++i) {
                const auto n=0x6600+i*64,c=0x6800+i*128;
                put(n,i==2?at(0x6500):at(n+64));put(n+8,i==0?at(0x6500):at(n-64));put(n+0x10,keys[i]);
                put(n+0x18,at(c));put(n+0x20,at(c+64));put(n+0x28,at(c+64));
                put(c,base()+tables[i]);put(c+8,session.controller);
            }
            require(medicon::sdk::VanillaNavigationTestAccess::use_image(navigation,{base(),image_size}),"Observed profile rejected.");
        } catch (...) {VirtualFree(image,0,MEM_RELEASE);VirtualFree(native,0,MEM_RELEASE);throw;}
    }
    ~Fixture(){DWORD old=0;VirtualProtect(native,0x10000,PAGE_READWRITE,&old);VirtualFree(native,0,MEM_RELEASE);VirtualFree(image,0,MEM_RELEASE);}
    std::uintptr_t base() const {return reinterpret_cast<std::uintptr_t>(image);}
    std::uintptr_t at(std::size_t offset) const {return reinterpret_cast<std::uintptr_t>(native+offset);}
    template<class T> void put(std::size_t offset,T value){require(offset+sizeof(value)<=0x10000,"Native fixture overflow.");std::memcpy(native+offset,&value,sizeof(value));}
    void image_put(std::uintptr_t rva,const void* data,std::size_t size) {
        const auto first=rva&~std::uintptr_t{4095},last=(rva+size+4095)&~std::uintptr_t{4095};
        require(rva+size<=image_size&&VirtualAlloc(image+first,last-first,MEM_COMMIT,PAGE_EXECUTE_READWRITE),"Image fixture overflow.");
        std::memcpy(image+rva,data,size);
        FlushInstructionCache(GetCurrentProcess(),image+rva,size);
    }
    void bytes(std::uintptr_t rva,std::initializer_list<std::uint8_t> bytes){image_put(rva,bytes.begin(),bytes.size());}
    void pointer(std::uintptr_t rva,std::uintptr_t target){const auto value=base()+target;image_put(rva,&value,sizeof(value));}
    void stub(std::uintptr_t rva,std::initializer_list<std::uint8_t> epilogue) {
        std::vector<std::uint8_t> code{0x48,0xB8}; // mov rax, record; call rax
        const auto target=reinterpret_cast<std::uintptr_t>(&record);
        const auto* p=reinterpret_cast<const std::uint8_t*>(&target);
        code.insert(code.end(),p,p+8);code.insert(code.end(),{0xFF,0xD0});
        code.insert(code.end(),epilogue.begin(),epilogue.end());
        image_put(rva,code.data(),code.size());
    }
    void observed_profile() {
        bytes(0x4F17340,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x18,0x07,0x00,0x00,0x48,0x8D,0xAC,0x24,0x80,0x00,0x00,0x00});
        bytes(0x4CB4638,{0x48,0x8B,0x4E,0x40,0x48,0x8B,0x01,0x48,0x8B,0x40,0x10,0xFF,0x15,0x27,0xA9,0xCE,0x0A});
        bytes(0x8D09C0,{0x0F,0xB6,0x41,0x39,0xC3});bytes(0x8D09B0,{0xC6,0x41,0x39,0x01,0xC3});
        bytes(0x4BB2960,{0x55,0x56,0x57,0x48,0x81,0xEC,0xE0,0x00,0x00,0x00,0x48,0x8D,0xAC,0x24,0x80,0x00,0x00,0x00});
        bytes(0x4F5DE80,{0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,0x48,0x8D,0xAC,0x24,0x80,0x00,0x00,0x00,0x48,0xC7,0x45,0x28,0xFE,0xFF,0xFF,0xFF,0x48,0x8B,0x71,0x08,0x48,0x8B,0x9E,0xF0,0x0B,0x00,0x00});
        bytes(0x4F67590,{0x55,0x56,0x57,0x53,0x48,0x81,0xEC,0x98,0x00,0x00,0x00,0x48,0x8D,0xAC,0x24,0x80,0x00,0x00,0x00,0x48,0xC7,0x45,0x10,0xFE,0xFF,0xFF,0xFF,0x48,0x8B,0x71,0x08,0x48,0x8B,0x9E,0xF0,0x0B,0x00,0x00});
        bytes(0x26D7E80,{0x48,0x8B,0x49,0x08,0x48,0x8B,0x01,0x48,0x8B,0x40,0x60,0x48,0x8B,0x15,0xDE,0x70,0x2C,0x0D,0x48,0xFF,0xE2});
        pointer(0xF21A2F0,0x4F17340);pointer(0xF21A2F8,0x5741A0);pointer(0xF21A340,0x4BB2960);
        pointer(0xF20ECF8,0x4CCF520);pointer(0xF092490,0x8D09B0);pointer(0xF092498,0x8D09C0);
        for(std::size_t i=0;i<3;++i) {pointer(tables[i],0x78090);pointer(tables[i]+8,0x768F0);pointer(tables[i]+16,entries[i]);}
        // Test-only native-shaped functions keep the verified prologues/ABI,
        // then call a local recorder; no Minecraft executable is loaded/called.
        stub(0x4F5DEA8,{0x48,0x81,0xC4,0xB0,0,0,0,0x5B,0x5F,0x5E,0x41,0x5E,0x5D,0xC3});
        stub(0x4F675B6,{0x48,0x81,0xC4,0x98,0,0,0,0x5B,0x5F,0x5E,0x5D,0xC3});
        stub(0x4BB2972,{0x48,0x81,0xC4,0xE0,0,0,0,0x5F,0x5E,0x5D,0xC3});
        bytes(0x10000,{0xFF,0xE0}); // fixture CFG dispatcher jumps to RAX
        pointer(0xF99EF70,0x10000);
    }
    bool request(unsigned i){switch(i){case 0:return navigation.openPlay(epoch.load());case 1:return navigation.openSettings(epoch.load());default:return navigation.quitGame(epoch.load());}}
    bool pump(std::uintptr_t caller=0){return navigation.pump(reinterpret_cast<void*>(session.controller),caller?caller:base()+0x4CB4649,session,epoch,stopping,reason);}
    void rejected(const char* label){const auto before=invoked.load();require(request(0),"Cannot queue rejection fixture.");require(!pump()&&!navigation.has_pending()&&!reason.empty()&&invoked.load()==before,label);}
};

void host_gate(){VanillaNavigation n;std::string why;require(!n.initialize(why)&&!why.empty()&&!n.update_hook_target(),"Unsupported host accepted.");require(!n.openPlay(1),"Unsupported host queued.");n.stop();require(!n.initialize(why),"Restart bypassed host gate.");}
void dispatch() {
    Fixture f;
    for(unsigned i=0;i<3;++i) {
        f.epoch.fetch_add(1);require(f.request(i),"Request refused.");require(!f.request(i),"Duplicate request admitted.");
        expected_argument=reinterpret_cast<void*>(i==2?f.session.controller:f.at(0x6800+i*128));argument_matches=false;
        reentrant_navigation=&f.navigation;reentrant_epoch=f.epoch.load();reentrant_rejected=false;
        const auto before=invoked.load();
        require(f.pump()&&invoked.load()==before+1&&argument_matches,"Registered callable dispatch failed.");
        require(reentrant_rejected&&!f.navigation.has_pending(),"Handler reentry stacked another action.");
        reentrant_navigation=nullptr;
        require(!f.request(i),"Same-session cooldown ignored.");
    }
    f.epoch.fetch_add(1);require(f.request(0),"Cannot queue destructive callback test.");
    expected_argument=reinterpret_cast<void*>(f.at(0x6800));native_allocation=f.native;block_native_after_call=true;
    require(f.pump(),"Pump touched invalidated native objects after invocation.");
    block_native_after_call=false;DWORD old=0;VirtualProtect(f.native,0x10000,PAGE_READWRITE,&old);
}
void queue_rules() {
    Fixture f;require(!f.navigation.openPlay(0),"Zero menu epoch admitted.");require(f.request(0),"Queue refused.");f.navigation.cancel(9);require(f.navigation.has_pending(),"Old epoch cancellation erased newer action.");
    f.navigation.cancel(10);require(!f.navigation.has_pending(),"Retired epoch was not cleared.");
    require(f.request(0),"Queue refused stale fixture.");f.epoch.fetch_add(1);require(!f.pump()&&!f.reason.empty(),"Stale epoch invoked.");
    require(f.request(0),"Queue refused expiry fixture.");medicon::sdk::VanillaNavigationTestAccess::expire(f.navigation);
    require(!f.pump()&&f.reason.find("expired")!=std::string::npos,"Expired request invoked.");
    require(f.request(0),"Queue refused caller fixture.");require(!f.pump(f.base()+0x483A4DC),"Unverified caller invoked.");
    require(f.request(0),"Queue refused epoch replacement fixture.");f.epoch.fetch_add(1);require(f.request(1),"New session could not replace an old pending request.");
    f.navigation.cancel(f.epoch.load()-1);require(f.navigation.has_pending(),"Bounded cancellation erased replacement request.");
    f.navigation.cancel();
    std::array<std::future<bool>,8> queued;
    for(auto& item:queued)item=std::async(std::launch::async,[&]{return f.request(0);});
    unsigned accepted=0;for(auto& item:queued)accepted+=item.get()?1u:0u;
    require(accepted==1&&f.navigation.has_pending(),"Concurrent callers stacked requests.");f.navigation.cancel();
    require(f.request(0),"Queue refused stop fixture.");f.navigation.stop();require(!f.navigation.has_pending()&&!f.request(1)&&!f.navigation.update_hook_target(),"Stop did not close admission.");
}
void rejected_contexts() {
    Fixture f;
    f.stopping.store(true);f.rejected("Shutdown invoked.");f.stopping.store(false);
    f.put(0x4000,f.base()+0xE900078);f.rejected("Wrong controller vtable invoked.");f.put(0x4000,f.base()+0xF21A2E0);
    f.put(0x3300,f.base()+0xE8E9088);f.rejected("Wrong scene vtable invoked.");f.put(0x3300,f.base()+0xF20EC60);
    f.put(0x62C8,std::int32_t{0});f.rejected("Dead controller owner invoked.");f.put(0x62C8,std::int32_t{1});
    f.put(0x4BF0,f.at(0x6010));f.rejected("Wrong model this invoked.");f.put(0x4BF0,f.at(0x6000));
    f.put(0x6308,std::int32_t{0});f.rejected("Dead model owner invoked.");f.put(0x6308,std::int32_t{2});
    f.put(0x3880,std::uint8_t{1});f.rejected("Input-disabled scene invoked.");f.put(0x3880,std::uint8_t{0});
    f.put(0x3885,std::uint8_t{1});f.rejected("Closing scene invoked.");f.put(0x3885,std::uint8_t{0});
    f.put(0x10A9,std::uint8_t{1});f.rejected("Quit-requested app invoked.");f.put(0x10A9,std::uint8_t{0});
    f.put(0x6280,std::uint8_t{0});f.rejected("Invalid IApp invoked.");f.put(0x6280,std::uint8_t{1});
    f.put(0x6468,std::uintptr_t{1});f.rejected("Changed top scene invoked.");f.put(0x6468,f.session.scene);
    f.put(0x6808,f.at(0x4010));f.rejected("Wrong callable capture invoked.");f.put(0x6808,f.session.controller);
    f.put(0x6800,f.base()+Fixture::tables[1]);f.rejected("Wrong callable operation table invoked.");f.put(0x6800,f.base()+Fixture::tables[0]);
    f.put(0x6620,f.at(0x6880));f.put(0x6628,f.at(0x6880));f.rejected("Multiple callbacks invoked as a single handler.");f.put(0x6620,f.at(0x6840));f.put(0x6628,f.at(0x6840));
    f.put(0x4830,std::uintptr_t{1025});f.rejected("Unbounded node list invoked.");f.put(0x4830,std::uintptr_t{3});
    f.put(0x6600,f.at(0x6600));f.rejected("Cyclic node list invoked.");f.put(0x6600,f.at(0x6640));
    f.put(0x6608,std::uintptr_t{1});f.rejected("Broken backward linkage invoked.");f.put(0x6608,f.at(0x6500));
    f.put(0x6618,std::uintptr_t{1});f.rejected("Unreadable callable invoked.");f.put(0x6618,f.at(0x6800));
    f.put(0x6618,std::numeric_limits<std::uintptr_t>::max()-7);f.rejected("Overflowing callable invoked.");f.put(0x6618,f.at(0x6800));
    f.bytes(Fixture::entries[0],{0x90});f.rejected("Changed native code invoked.");require(!f.request(0),"Code mismatch did not disable navigation.");f.bytes(Fixture::entries[0],{0x55});
    require(medicon::sdk::VanillaNavigationTestAccess::use_image(f.navigation,{f.base(),Fixture::image_size}),"Cannot rearm corrected synthetic profile.");
    f.pointer(Fixture::tables[0]+16,Fixture::entries[1]);f.rejected("Changed native invoke table invoked.");f.pointer(Fixture::tables[0]+16,Fixture::entries[0]);
    require(medicon::sdk::VanillaNavigationTestAccess::use_image(f.navigation,{f.base(),Fixture::image_size}),"Cannot rearm corrected synthetic table.");
    const auto page=Fixture::entries[0]&~std::uintptr_t{4095};
    DWORD old=0;require(VirtualProtect(f.image+page,4096,PAGE_READWRITE,&old),"Cannot change synthetic code protection.");
    f.rejected("Nonexecutable native entry invoked.");require(VirtualProtect(f.image+page,4096,old,&old),"Cannot restore synthetic code protection.");
    f.navigation.stop();f.bytes(0x4F17340,{0x90});
    require(!medicon::sdk::VanillaNavigationTestAccess::use_image(f.navigation,{f.base(),Fixture::image_size}),"Unverified update hook entry admitted.");
}
}
int main(){try{host_gate();dispatch();queue_rules();rejected_contexts();std::cout<<"Vanilla navigation queue, ABI fixture and fail-closed checks passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
