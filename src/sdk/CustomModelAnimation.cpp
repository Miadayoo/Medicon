#include <medicon/sdk/CustomModelAnimation.hpp>
#include <medicon/sdk/detail/GameplayMemory.hpp>
#include <medicon/memory/signature_registry.hpp>
#include <medicon/memory/module_view.hpp>
#include <medicon/sdk/CustomModelP3Patterns.hpp>
#include <Windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
namespace medicon::sdk::custom_model {
namespace {
template<class T> bool read(std::uintptr_t address,T& out) noexcept { SIZE_T n{};return address&&address<=UINTPTR_MAX-sizeof(T)&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&out,sizeof out,&n)&&n==sizeof out; }
// EnttHashs.hpp + Actor/Components/{ActorDataFlag,StateVector,ActorRotation,
// ActorHeadRotation,ActorUniqueID,MobHurtTime}Component.hpp at pinned Solstice.
// Sparse/page layout is the existing, tested GameplayMemory bridge. Native
// ActorDataFlag pool lookup is also observed at 140226960..269F2. No assure,
// insertion, packet or native STL access. docs/custom_model_p3_report.md.
constexpr std::array<std::uint32_t,9> hashes{0xC67426F3,0x1B5D5238,0x75DF36B7,0xBABE7211,0x18F957AF,0xD7A3585C,0xC29078A0,0xB13F037A,0x710A62C4};
std::atomic_bool native_swing_profile{};
}
bool initialize_animation_reader(std::string& diagnostic) {
    const auto module=memory::current_process_module(&diagnostic);if(!module)return false;
    using D=memory::SignatureDefinition;using R=memory::SignatureResolution;
    constexpr std::array defs{D{"swing_update",p3_native::swing_update,R::direct,0,0,"1.26.52.3",false},D{"swing_previous",p3_native::swing_previous,R::direct,0,0,"1.26.52.3",false}};
    memory::SignatureRegistry registry;registry.scan(defs,module->executable_regions,module->image);
    for(const auto& d:defs){const auto* r=registry.find(d.name);if(!r||!r->address){diagnostic="P3 native swing field profile unavailable.";return false;}}
    native_swing_profile.store(true,std::memory_order_release);diagnostic="P3 read-only native swing profile ready.";return true;
}
medicon::custom_model::AnimationInput read_animation_input(void* actor,float alpha,float body_yaw) noexcept {
    medicon::custom_model::AnimationInput out;std::uintptr_t registry{};std::uint32_t entity{};
    const auto a=reinterpret_cast<std::uintptr_t>(actor);
    if(!read(a+0x10,registry)||!read(a+0x18,entity)||!registry||(entity&0x3FFFF)==0x3FFFF)return out;
    std::array<std::uintptr_t,3> packed{};
    if(!read(registry+0x68,packed)||packed[1]<packed[0]||packed[2]<packed[1]||(packed[1]-packed[0])%32||packed[2]-packed[0]>8192*32)return out;
    std::array<std::uintptr_t,hashes.size()> pools{};
    // Bounded stack chunks; no vector allocation and no cached storage pointer.
    struct Node{std::uintptr_t next;std::uint32_t hash,padding;std::uintptr_t data,control;};
    std::array<Node,32> chunk{};
    for(auto p=packed[0];p<packed[1];){const auto count=std::min<std::size_t>(chunk.size(),(packed[1]-p)/32);SIZE_T bytes{};
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),chunk.data(),count*32,&bytes)||bytes!=count*32)return out;
        for(std::size_t i=0;i<count;++i)for(std::size_t h=0;h<hashes.size();++h)if(chunk[i].hash==hashes[h])pools[h]=chunk[i].data;p+=count*32;
    }
    const auto component=[&](unsigned i,std::size_t size){return detail::gameplay_component(pools[i],entity,size);};
    std::array<std::uint64_t,2> flags{};std::array<float,9> state{};std::array<float,4> rotation{};std::array<float,2> head{};
    std::uint64_t identity{};int hurt{};
    if(!read(component(0,16),flags)||!read(component(1,36),state)||!read(component(2,16),rotation)||!read(component(4,8),identity))return out;
    if(!read(component(3,8),head))head={rotation[1],rotation[3]};
    for(float f:head)if(!std::isfinite(f)||std::abs(f)>36000000)return out;
    for(float f:state)if(!std::isfinite(f)||std::abs(f)>32000000)return out;
    for(float f:rotation)if(!std::isfinite(f)||std::abs(f)>36000000)return out;
    const auto flag=[&](unsigned i){return (flags[i/64]>>(i%64))&1;};
    out.valid=true;out.identity=identity;out.world_identity=registry;out.world_position={state[0],state[1],state[2]};
    out.speed=std::hypot(state[6],state[8])*20;out.vertical_speed=state[7]*20;
    out.sneaking=flag(1);out.sprinting=flag(3);out.using_item=flag(4);out.riding=flag(2);out.swimming=flag(57);out.gliding=flag(32);
    out.sleeping=flag(76)||detail::gameplay_contains(pools[7],entity);out.on_ground=detail::gameplay_contains(pools[6],entity);
    out.dead=detail::gameplay_contains(pools[8],entity);
    // Same native render alpha as position/body yaw; these are angle values,
    // never a second interpolation of the actor's render position.
    alpha=std::clamp(alpha,0.0F,1.0F);
    const float head_delta=std::remainder(head[0]-head[1],360.0F);out.head_yaw=std::remainder(body_yaw-(head[1]+head_delta*alpha),360.0F);
    out.pitch=rotation[2]+std::remainder(rotation[0]-rotation[2],360.0F)*alpha;
    if(read(component(5,4),hurt)&&hurt>=0&&hurt<=1000)out.hurt=hurt>0;
    if(native_swing_profile.load(std::memory_order_acquire)){
        float current{},previous{};std::uint8_t swinging{};
        // Native Actor tick 142077CDD copies +40C to +450. Native animation
        // update 1447722D0 divides swing tick by duration into +40C. Profile
        // scan confirms both exact bodies; old Solstice +43C is not used.
        if(read(a+0x40C,current)&&read(a+0x450,previous)&&read(a+0x434,swinging)&&swinging<=1&&
            std::isfinite(current)&&std::isfinite(previous)&&current>=0&&current<=1&&previous>=0&&previous<=1&&(swinging||current>0||previous>0)){
            float d=current-previous;if(d<0)d+=1;out.attack_progress=std::fmod(previous+d*alpha,1.0F);
        }
    }
    return out;
}
bool fill_animated_tessellator(void* tess,std::span<const medicon::custom_model::SkinOutput> skin,
    const medicon::custom_model::AnimationBatch& batch,bool hurt) noexcept {
    const auto t=reinterpret_cast<std::uintptr_t>(tess),count=batch.vertices.size();if(!count||count>500000)return false;
    constexpr std::array<std::size_t,4> offsets{0x10,0x28,0x70,0xA0},strides{12,16,4,8};
    std::array<std::array<std::uintptr_t,3>,4> vectors{};
    for(unsigned i=0;i<4;++i){if(!read(t+offsets[i],vectors[i]))return false;const auto& v=vectors[i];std::byte probe{};
        if(!v[0]||v[1]!=v[0]+strides[i]||v[2]<v[0]||v[2]-v[0]<count*strides[i]||!read(v[0],probe)||!read(v[0]+count*strides[i]-1,probe))return false;}
    for(const auto& v:batch.vertices)if(v.skin_index>=skin.size())return false;
    medicon::custom_model::Float3 minimum=skin[batch.vertices[0].skin_index].position,maximum=minimum;
    for(std::size_t i=0;i<count;++i){const auto& v=batch.vertices[i];const auto& s=skin[v.skin_index];
        std::memcpy(reinterpret_cast<void*>(vectors[0][0]+i*12),s.position.data(),12);
        const std::array<float,4> normal{s.normal[0],s.normal[1],s.normal[2],0};std::memcpy(reinterpret_cast<void*>(vectors[1][0]+i*16),normal.data(),16);
        const auto color=hurt?0xFF6060FFU:v.color;std::memcpy(reinterpret_cast<void*>(vectors[2][0]+i*4),&color,4);std::memcpy(reinterpret_cast<void*>(vectors[3][0]+i*8),v.uv.data(),8);
        for(unsigned a=0;a<3;++a){minimum[a]=std::min(minimum[a],s.position[a]);maximum[a]=std::max(maximum[a],s.position[a]);}
    }
    for(unsigned i=0;i<4;++i){const auto end=vectors[i][0]+count*strides[i];std::memcpy(reinterpret_cast<void*>(t+offsets[i]+8),&end,8);}
    const auto native_count=static_cast<std::uint32_t>(count);std::memcpy(reinterpret_cast<void*>(t+0x298),&native_count,4);
    std::memcpy(reinterpret_cast<void*>(t+0x130),minimum.data(),12);std::memcpy(reinterpret_cast<void*>(t+0x13C),maximum.data(),12);return true;
}
}
