#include <medicon/sdk/BedrockReconnect.hpp>
#include <medicon/sdk/detail/ReconnectDispatch.hpp>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>

namespace medicon::sdk {
namespace {
// Minecraft 1.26.52.03. Evidence and ownership/ABI notes:
// docs/disconnect-recovery.md. The native retry consumes the complete saved
// destination (including NetherNet); it also consumes its NonOwnerPointer arg.
constexpr std::uintptr_t update_rva = 0x5DB4440, client_table = 0xE9731B0;
constexpr std::uintptr_t get_handler = 0x5DD4550, release_owner = 0x758E0;
constexpr std::uintptr_t get_scene_stack = 0x5DC6CA0, stack_table = 0xE8F7A40, push_scene = 0x4B23B00;
constexpr std::uintptr_t retry = 0xBD68A0, clear_screen = 0xBD38D0, get_router = 0x5DC6F40;
template<class T> bool read(std::uintptr_t at, std::size_t offset, T& value) noexcept {
    if (!at || offset > std::numeric_limits<std::uintptr_t>::max()-at ||
        sizeof(T)-1 > std::numeric_limits<std::uintptr_t>::max()-(at+offset)) return false;
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at+offset),&value,sizeof value,&count) && count==sizeof value;
}
bool bytes(std::uintptr_t at, std::initializer_list<unsigned char> expected) noexcept {
    std::array<unsigned char,64> actual{};
    SIZE_T count=0;
    return expected.size()<=actual.size() && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),
        actual.data(),expected.size(),&count) && count==expected.size() && std::equal(expected.begin(),expected.end(),actual.begin());
}
bool equal(std::uintptr_t at,std::size_t offset,std::uintptr_t expected) noexcept {
    std::uintptr_t actual=0;return read(at,offset,actual)&&actual==expected;
}
}

bool detail::dispatch_reconnect(const ReconnectCalls& calls,void* client,void* handler,void* factory,
                                std::uintptr_t stack,std::uintptr_t table,std::uintptr_t push) {
    if (!client || !handler || !factory || !stack || !calls.get_scene_stack || !calls.release_scene_stack || !calls.retry)
        return false;
    SceneStackOwner owner{};
    calls.get_scene_stack(client,&owner);
    std::uint8_t valid=0;
    if (owner.object!=stack || !read(owner.validity,0,valid) || valid!=1 ||
        !equal(owner.object,0,table) || !equal(table,0xC8,push)) {
        calls.release_scene_stack(&owner);
        return false;
    }
    // Native retry consumes this exact owner, including native reference counts.
    // A handler owner must never reach the SceneStack virtual pushScene call.
    calls.retry(handler,factory,&owner);
    return true;
}

bool BedrockReconnect::profile() const noexcept {
    const auto b=base_;
    return b && equal(b+client_table,0x728,b+0x5DC6A30) && equal(b+client_table,0x790,b+get_router) &&
        equal(b+client_table,0xC70,b+get_handler) && equal(b+0xE973F68,0,b+update_rva) &&
        equal(b+client_table,0x770,b+get_scene_stack) && equal(b+stack_table,0xC8,b+push_scene) &&
        equal(b+stack_table,44*8,b+0x4B28730) &&
        bytes(b+get_scene_stack,{0x56,0x48,0x83,0xEC,0x20,0x48,0x89,0xD6,0x48,0x8B,0x89,0xA8,0x01,0,0}) &&
        bytes(b+retry,{0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0x20,0x04,0,0}) &&
        bytes(b+retry+0x20,{0x4C,0x89,0xC7,0x80,0xB9,0x40,0x05,0,0,0x01}) &&
        bytes(b+get_handler+0x2F,{0x48,0x8B,0x9F,0xA0,0x0B,0,0}) &&
        bytes(b+release_owner,{0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x89,0xCE,0x0F,0x57,0xC0}) &&
        bytes(b+clear_screen+0x13,{0x80,0xB9,0x49,0x05,0,0,0x01}) &&
        bytes(b+0x5DC6A30,{0x48,0x8B,0x81,0x08,0x02,0,0,0xC3});
}
bool BedrockReconnect::initialize(std::string& diagnostic) noexcept {
    try {
        std::lock_guard lock(mutex_);
        base_=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        if (!read(base_,0,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew!=0x78 ||
            !read(base_,dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
            nt.FileHeader.TimeDateStamp!=0x6AB54E37 ||
            !((nt.OptionalHeader.SizeOfImage==0x12C02000 && nt.OptionalHeader.CheckSum==0x128DA293) ||
              (nt.OptionalHeader.SizeOfImage==0x12C01000 && nt.OptionalHeader.CheckSum==0x128CD801)) ||
            !profile() || !bytes(base_+update_rva,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,
                0x48,0x81,0xEC,0x48,0x06,0,0}) ||
            !bytes(base_+update_rva+0x30,{0x48,0x8D,0xB1,0x78,0xFF,0xFF,0xFF})) {
            diagnostic="Reconnect requires the inspected Minecraft 1.26.52.03 code profile.";
            ready_=false;return false;
        }
        ready_=menu_.prepare(diagnostic);
        if (ready_) diagnostic="Native disconnect observer and saved-destination retry ready.";
        return ready_;
    } catch (...) {return false;}
}
std::uintptr_t BedrockReconnect::update_target() const noexcept {
    std::lock_guard lock(mutex_);return ready_?base_+update_rva:0;
}
DisconnectState BedrockReconnect::snapshot() const {std::lock_guard lock(mutex_);return state_;}
bool BedrockReconnect::request(std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex_);
    const auto now=GetTickCount64();
    if (!ready_ || !state_.visible || !state_.available || generation!=state_.generation || requested_ ||
        (last_dispatch_ && now-last_dispatch_<750)) return false;
    requested_=generation;requested_at_=now;state_.error.clear();return true;
}
void BedrockReconnect::stop() noexcept {
    std::lock_guard lock(mutex_);ready_=false;requested_=0;state_.visible=false;state_.available=false;++state_.generation;
}
void BedrockReconnect::update(void* adjusted_client,void* current_client,std::string& diagnostic) {
    std::unique_lock lock(mutex_);
    if (!ready_) return;
    diagnostic.clear();
    const auto client=reinterpret_cast<std::uintptr_t>(current_client);
    if (client && reinterpret_cast<std::uintptr_t>(adjusted_client)!=client+0x88) return;
    if (!client || !equal(client,0,base_+client_table)) {
        if (state_.visible) ++state_.generation;
        state_.visible=false;state_.available=false;requested_=0;return;
    }
    std::uint8_t primary=1;
    std::uintptr_t handler=0,valid=0,factory=0;
    bool alive=read(client,0x678,primary) && primary==0 && read(client,0xBA0,handler) &&
        read(handler,8,valid) && read(valid,0,primary) && primary==1 && read(client,0x208,factory) && factory;
    MenuSessionIdentity scene{};
    std::string ignored;
    bool visible=alive && menu_.poll(current_client,ignored,&scene)==MenuScreenState::disconnected;
    std::uint8_t ore=0,saved=0;
    if (alive && read(handler,0x549,ore) && ore==1) {
        // Same native route test used by _handleDisconnectionScreenRequests.
        // Both calls execute on that original UI callback's thread.
        const auto router=reinterpret_cast<void*(*)(void*)>(base_+get_router)(current_client);
        visible=router && !reinterpret_cast<bool(*)(void*,void*)>(base_+clear_screen)(reinterpret_cast<void*>(handler),router);
    }
    const bool available=visible && read(handler,0x540,saved) && saved==1;
    if (state_.visible!=visible || (visible && (owner_!=handler || scene_!=scene))) {
        ++state_.generation;requested_=0;state_.error.clear();
        if (visible) diagnostic=available?"Reconnect: disconnect screen detected; saved destination ready.":
            "Reconnect: disconnect screen detected; no saved destination.";
    }
    state_.visible=visible;state_.available=available;owner_=handler;scene_=scene;
    if (!requested_) return;
    const auto wanted=std::exchange(requested_,0);
    if (!visible || wanted!=state_.generation || GetTickCount64()-requested_at_>1500) return;
    if (!available || !profile()) {
        state_.error="The previous server is no longer available. Join it from Play.";return;
    }
    // Match the official tryReconnect binding's callback: client vtable +770
    // returns NonOwnerPointer<SceneStack>. +C70 instead returns a handler owner;
    // its layout is the same but passing it here invokes the wrong vtable.
    std::uintptr_t game=0,stack=0,stack_valid=0;
    if (!read(client,0x1A8,game) || !read(game,0x1D70,stack) || !read(stack,8,stack_valid) ||
        !read(stack_valid,0,primary) || primary!=1 || !equal(stack,0,base_+stack_table)) {
        state_.error="The server screen changed. Try again.";return;
    }
    const detail::ReconnectCalls calls{
        reinterpret_cast<detail::ReconnectCalls::GetSceneStack>(base_+get_scene_stack),
        reinterpret_cast<detail::ReconnectCalls::ReleaseSceneStack>(base_+release_owner),
        reinterpret_cast<detail::ReconnectCalls::Retry>(base_+retry)};
    last_dispatch_=GetTickCount64();
    state_.available=false;
    diagnostic="Reconnect: native SceneStack retry dispatched (v2).";
    const auto generation=state_.generation,b=base_;
    lock.unlock(); // Native navigation may destroy scenes or reenter the renderer.
    if (!detail::dispatch_reconnect(calls,current_client,reinterpret_cast<void*>(handler),reinterpret_cast<void*>(factory),
                                    stack,b+stack_table,b+push_scene)) {
        lock.lock();
        if (state_.generation==generation) state_.error="Server screen changed before reconnect. Try again.";
        diagnostic="Reconnect: SceneStack owner mismatch; native retry cancelled.";
    }
}
}
