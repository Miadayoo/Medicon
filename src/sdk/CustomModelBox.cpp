#include <medicon/sdk/CustomModelBox.hpp>
#include <medicon/sdk/CustomModelP1Patterns.hpp>
#include <medicon/sdk/CustomModelRender.hpp>
#include <medicon/sdk/CustomModelAnimation.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace medicon::sdk::custom_model {
namespace {
namespace pattern = p1_native;
template<class T> bool read(std::uintptr_t address, T& result) noexcept {
    SIZE_T count{};
    return address && address <= UINTPTR_MAX - sizeof(T) &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                          &result, sizeof(result), &count) && count == sizeof(result);
}
template<class T> void write(std::uintptr_t address, const T& value) noexcept {
    // Only validated, borrowed native draw state; never actor/world state.
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
bool finite(const BoxMatrix& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); });
}
bool finite(BoxVec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool matrix_top(std::uintptr_t owner, std::uintptr_t& top, BoxMatrix& result) noexcept {
    // DDR 0x14558DBxx and MeshSubmit 0x144B047AF..48A6 read the
    // native world deque this way. Blocks hold one 64-byte column-major matrix.
    std::array<std::uintptr_t, 4> state{};
    if (!read(owner + 0x48, state)) return false;
    const auto [map, count, offset, size] = state;
    if (!map || count < 1 || count > 65536 || (count & (count - 1)) || !size || size > count) return false;
    return read(map + (((offset + size - 1) & (count - 1)) * 8), top) && top && read(top, result) && finite(result);
}
bool key_valid(std::uintptr_t address, bool entity) noexcept {
    // Native HashedString: hash +0, string storage +8, length +0x18,
    // capacity +0x20; game constructs/destroys it. No DLL STL crosses ABI.
    std::array<std::uint64_t, 5> key{};
    if (!read(address, key)) return false;
    if (entity) return key[0] == 0x03AAE082A9C0D218ULL && key[3] == 6 && key[4] == 15 &&
        std::memcmp(&key[1], "entity\0", 7) == 0;
    std::array<char, 17> text{};
    return key[0] == 0x0A4F54B100E83415ULL && key[3] == 16 && key[4] >= 16 &&
        read(key[1], text) && std::memcmp(text.data(), "entity_alphatest\0", 17) == 0;
}
// Restore scalar attributes only. Native vertex vectors/mesh ownership remain
// native-owned. Offsets are all read/written in begin/vertex/submit evidence.
struct TessState {
    std::array<std::byte,24> bounds{};
    std::array<std::byte, 0x6C> attributes{}; // +178..1E3: normal/UV/color/translation/scale
    std::uint64_t hint{};
    std::uint32_t count{}, index_count{};
    std::uint16_t end_flags{}, begin_flags{};
    std::uint8_t changed{}, primitive{}, format{}, extra{}, transform{};
    bool capture(std::uintptr_t t) noexcept {
        return read(t+0x130,bounds) && read(t+0x178, attributes) && read(t+0x170, hint) && read(t+0x298, count) &&
            read(t+0x20C, index_count) && read(t+0x254, end_flags) && read(t+0x2A0, begin_flags) &&
            read(t, changed) && read(t+8, primitive) && read(t+0x158, format) &&
            read(t+0x168, extra) && read(t+0x210, transform);
    }
    void restore(std::uintptr_t t) const noexcept {
        write(t+0x130,bounds);write(t+0x178, attributes); write(t+0x170, hint); write(t+0x298, count);
        write(t+0x20C, index_count); write(t+0x254, end_flags); write(t+0x2A0, begin_flags);
        write(t, changed); write(t+8, primitive); write(t+0x158, format);
        write(t+0x168, extra); write(t+0x210, transform);
    }
};
struct RestoreDrawState {
    std::uintptr_t tess, top, owner;
    const TessState& previous;
    const BoxMatrix& matrix;
    ~RestoreDrawState() {
        previous.restore(tess);
        write(top, matrix);
        // MeshSubmit can compute/copy derived matrices. Restoring the model
        // matrix must invalidate those caches, even if dirty was zero before.
        write(owner+0x78, std::uint8_t{1});
    }
};
}

const std::array<BoxVertex, 36>& test_box_vertices() noexcept {
    static constexpr auto vertices = [] {
        std::array<BoxVertex, 36> out{};
        struct Face { std::array<BoxVec3, 4> p; BoxVec3 n; std::uint32_t color; };
        // Outward triangle winding; 0..1 UVs on every face. Color packing is
        // passed verbatim like the native +1B0 packed uint32 attribute.
        constexpr std::array<Face, 6> faces{{
            {{{{-.5F,0,.5F},{.5F,0,.5F},{.5F,2,.5F},{-.5F,2,.5F}}}, {0,0,1}, 0xFF4040FF},
            {{{{.5F,0,-.5F},{-.5F,0,-.5F},{-.5F,2,-.5F},{.5F,2,-.5F}}}, {0,0,-1}, 0xFF40FF40},
            {{{{.5F,0,.5F},{.5F,0,-.5F},{.5F,2,-.5F},{.5F,2,.5F}}}, {1,0,0}, 0xFFFF4040},
            {{{{-.5F,0,-.5F},{-.5F,0,.5F},{-.5F,2,.5F},{-.5F,2,-.5F}}}, {-1,0,0}, 0xFF40FFFF},
            {{{{-.5F,2,.5F},{.5F,2,.5F},{.5F,2,-.5F},{-.5F,2,-.5F}}}, {0,1,0}, 0xFFFF40FF},
            {{{{-.5F,0,-.5F},{.5F,0,-.5F},{.5F,0,.5F},{-.5F,0,.5F}}}, {0,-1,0}, 0xFFFFFF40}
        }};
        constexpr std::array<unsigned, 6> indices{0,1,2,0,2,3};
        constexpr std::array<std::array<float,2>,4> uv{{{0,1},{1,1},{1,0},{0,0}}};
        unsigned i{};
        for (const auto& f : faces) for (auto k : indices) out[i++] = {f.p[k], f.n, uv[k], f.color};
        return out;
    }();
    return vertices;
}

BoxMatrix box_world_matrix(const BoxMatrix& base, BoxVec3 position, float yaw) noexcept {
    const auto angle = (180.0F - yaw) * (3.14159265358979323846F / 180.0F);
    const float c = std::cos(angle), s = std::sin(angle);
    BoxMatrix out = base;
    for (unsigned row=0; row<4; ++row) {
        out[row] = base[row]*c - base[8+row]*s;
        out[8+row] = base[row]*s + base[8+row]*c;
        out[12+row] = base[row]*position.x + base[4+row]*position.y + base[8+row]*position.z + base[12+row];
    }
    return out;
}

BoxMatrix model_world_matrix(const BoxMatrix& base, BoxVec3 position, float body_yaw, float yaw_offset) noexcept {
    // model.json yawOffset is additive to the native 180-bodyYaw basis.
    // The previous composition subtracted another 180 degrees, reversing the
    // supplied Sprinter model (whose configured yawOffset is 180).
    return box_world_matrix(base, position, body_yaw - yaw_offset);
}

BoxMatrix first_person_world_matrix(const BoxMatrix& base,float head_yaw,float pitch) noexcept {
    // Arm geometry is authored into camera-relative +Z-forward space by the
    // worker/Animator. No player-feet translation or third-person yawOffset.
    const auto yaw=model_world_matrix(base,{},head_yaw,180);
    const float angle=pitch*3.14159265358979323846F/180,c=std::cos(angle),s=std::sin(angle);
    auto out=yaw;
    for(unsigned row=0;row<4;++row){out[4+row]=yaw[4+row]*c+yaw[8+row]*s;out[8+row]=yaw[8+row]*c-yaw[4+row]*s;}
    return out;
}

BoxTargets::operator bool() const noexcept {
    return world && mesh && mesh_view && begin && vertex && position && riding && yaw && height &&
        material && release && material_group && alpha_key && entity_key && submit && copy_binding && release_binding;
}

BoxTargets resolve_box_targets(std::uint32_t timestamp, std::uint32_t image_size,
    std::span<const memory::MemoryRegion> regions, memory::AddressRange image, std::string& diagnostic) {
    if (timestamp != 0x6AB54E37 || image_size != 0x12C02000) {
        diagnostic = "P1 withheld: executable profile differs from bounded IDA evidence."; return {};
    }
    using D = memory::SignatureDefinition;
    using R = memory::SignatureResolution;
    constexpr auto version = bedrock_1_26_52_3::file_version;
    constexpr std::array definitions{
        D{"world", pattern::world_render_pattern,R::direct,0,0,version,false},
        D{"mesh", pattern::mesh_immediate_pattern,R::direct,0,0,version,false},
        D{"mesh_view", pattern::mesh_view_pattern,R::direct,0,0,version,false},
        D{"begin", pattern::tess_begin_pattern,R::direct,0,0,version,false},
        D{"vertex", pattern::tess_vertex_pattern,R::direct,0,0,version,false},
        D{"position", pattern::interpolated_position_pattern,R::direct,0,0,version,false},
        D{"riding", pattern::riding_offset_pattern,R::direct,0,0,version,false},
        D{"yaw", pattern::body_yaw_pattern,R::direct,0,0,version,false},
        D{"height", pattern::height_offset_pattern,R::direct,0,0,version,false},
        D{"material", pattern::material_lookup_pattern,R::direct,0,0,version,false},
        D{"release", pattern::material_release_pattern,R::direct,0,0,version,false},
        D{"group", pattern::material_anchor_pattern,R::relative32,3,0,version,false},
        // Second LEA in the same proven callsite, RIP = match +14.
        D{"key", pattern::material_anchor_pattern,R::relative32,10,0,version,false},
        D{"submit", pattern::mesh_submit_pattern,R::direct,0,0,version,false},
        D{"copy_binding", pattern::binding_copy_pattern,R::direct,0,0,version,false},
        D{"release_binding", pattern::binding_release_pattern,R::direct,0,0,version,false}};
    memory::SignatureRegistry registry;
    registry.scan(definitions, regions, image);
    std::array<std::uintptr_t, definitions.size()> a{};
    for (unsigned i=0; i<a.size(); ++i) {
        const auto* r = registry.find(definitions[i].name);
        if (!r || !r->address) {
            diagnostic = "P1 " + std::string(definitions[i].name) + ": " +
                std::string(r ? memory::to_string(r->status) : "no result"); return {};
        }
        a[i] = *r->address;
    }
    // +0x90 is observed in p1_live_material_keys.json, not a guessed native
    // layout. Validate both full keys at each draw before calling lookup.
    if (!image.contains(a[12], 40) || !image.contains(a[12]+0x90, 40)) return {};
    diagnostic = "P1 world + mesh wrappers and native pose/material entries uniquely resolved; visual result unverified.";
    return {reinterpret_cast<WorldRenderFn>(a[0]), reinterpret_cast<MeshImmediateFn>(a[1]),
        reinterpret_cast<MeshImmediateViewFn>(a[2]), reinterpret_cast<TessBeginFn>(a[3]),
        reinterpret_cast<TessVertexFn>(a[4]), reinterpret_cast<InterpolatedPositionFn>(a[5]),
        reinterpret_cast<RidingOffsetFn>(a[6]), reinterpret_cast<BodyYawFn>(a[7]),
        reinterpret_cast<HeightOffsetFn>(a[8]), reinterpret_cast<MaterialLookupFn>(a[9]),
        reinterpret_cast<MaterialReleaseFn>(a[10]), reinterpret_cast<void*>(a[11]),
        reinterpret_cast<const void*>(a[12]), reinterpret_cast<const void*>(a[12]+0x90),reinterpret_cast<MeshSubmitFn>(a[13]),
        reinterpret_cast<BindingCopyFn>(a[14]),reinterpret_cast<BindingReleaseFn>(a[15])};
}

BoxTargets locate_box_targets(std::string& diagnostic) {
    // Called only after the P0 locate() file-version gate passed, on bootstrap.
    const auto module = memory::current_process_module(&diagnostic);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!module || !read(module->image.address, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        !module->image.contains(module->image.address + dos.e_lfanew, sizeof(nt)) ||
        !read(module->image.address + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return {};
    return resolve_box_targets(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage,
        module->executable_regions, module->image, diagnostic);
}

const char* box_failure_name(BoxFailure failure) noexcept {
    switch (failure) {
    case BoxFailure::none:return "none"; case BoxFailure::context:return "context";
    case BoxFailure::matrix:return "matrix"; case BoxFailure::pose:return "pose";
    case BoxFailure::texture:return "texture"; case BoxFailure::tessellator:return "tessellator";
    case BoxFailure::material:return "material"; default:return "unknown";
    }
}
thread_local BoxRenderer::WorldScope* BoxRenderer::current_{};
struct BoxRenderer::StaticState {
    std::atomic_bool configured{};
    StaticTargets native;
    medicon::custom_model::ModelRegistry* registry{};
    // TessEnd output is 632 bytes in the bounded native caller/output stores.
    // Overprovisioned opaque, aligned storage; never copied or interpreted as
    // a DLL class. Only the native destructor releases the constructed object.
    struct alignas(16) Mesh { std::array<std::byte,1024> bytes{}; bool constructed{}; };
    struct alignas(16) Texture { std::array<std::byte,40> bytes{}; bool owned{}; };
    std::array<Mesh,128> meshes;
    std::array<Texture,32> textures;
    std::atomic_uint uploaded{},loaded{};
    std::atomic_bool ready{};
    std::atomic_bool first_person_ready{};
    std::atomic<std::uint64_t> requests{},ticks{};
    std::atomic_flag upload_lock=ATOMIC_FLAG_INIT, request_lock=ATOMIC_FLAG_INIT;
    std::uint64_t next_request{};
    unsigned request_index{};
    std::atomic<std::uint64_t> animation_frames{},animation_ticks{},animation_draw_ticks{},animation_read_failures{},bulk_batches{},fallback_batches{};
    std::atomic_uint animation_motion{};
    std::atomic_bool animation_input_ready{};
    LARGE_INTEGER animation_last{};
};
BoxRenderer::BoxRenderer() : static_(std::make_unique<StaticState>()) {}
BoxRenderer::~BoxRenderer() = default; // Runtime explicitly releases after callback drain.
void BoxRenderer::configure_static(StaticTargets targets, medicon::custom_model::ModelRegistry& registry) {
    static_->native=targets;static_->registry=&registry;
    static_->configured.store(true,std::memory_order_release);
}
void BoxRenderer::set_static_mode(bool value,bool visible) noexcept {
    static_visible_.store(visible,std::memory_order_release);static_mode_.store(value,std::memory_order_release);
}
StaticProgress BoxRenderer::static_progress() const noexcept {
    if (!static_) return {};
    return {static_->uploaded.load(std::memory_order_acquire),static_->loaded.load(std::memory_order_acquire),
        static_->ready.load(std::memory_order_acquire),static_->requests.load(std::memory_order_relaxed),static_->ticks.load(std::memory_order_relaxed)};
}
bool BoxRenderer::first_person_ready() const noexcept {
    return static_->first_person_ready.load(std::memory_order_acquire);
}
BoxRenderer::AnimationFrame BoxRenderer::take_animation_frame() noexcept {
    auto& s=*static_;return {s.animation_frames.exchange(0),s.animation_ticks.exchange(0),s.animation_draw_ticks.exchange(0),s.animation_read_failures.exchange(0),
        s.bulk_batches.exchange(0),s.fallback_batches.exchange(0),s.animation_motion.load(),s.animation_input_ready.load()};
}
void BoxRenderer::observe_texture_group(void* group,TextureGetFn original) noexcept {
    auto* state=static_.get();
    if (!enabled()||!static_mode_.load(std::memory_order_acquire)||!state||!state->configured.load(std::memory_order_acquire)||!state->registry||!group||!original) return;
    const auto model=state->registry->snapshot();
    if (!model||state->loaded.load(std::memory_order_acquire)>=model->textures.size()) return;
    if (state->request_lock.test_and_set(std::memory_order_acquire)) return;
    struct Unlock { std::atomic_flag& flag;~Unlock(){flag.clear(std::memory_order_release);} } unlock{state->request_lock};
    const auto now=GetTickCount64();if (now<state->next_request) return;state->next_request=now+100;
    const auto index=state->request_index++%static_cast<unsigned>(model->textures.size());
    auto& texture=state->textures[index];if (texture.owned) return;
    const auto input=raw_texture_location(model->textures[index].native_path);
    alignas(16) std::array<std::byte,40> output{};
    // 141D0D57F takes the async branch for flag=1. Optional/mode=0 match
    // native caller 1407EFF07. Only this callback's borrowed TextureGroup is
    // used; no native context/group/actor pointer is retained across calls.
    original(group,output.data(),input.data(),true,0,0);
    output[0x20]=std::byte{1}; // Native TexturePtr (two ref pairs) binding tag.
    state->requests.fetch_add(1,std::memory_order_relaxed);
    std::uintptr_t data{},gpu{};std::uint8_t flags{};
    std::memcpy(&data,output.data(),sizeof data);
    // 1407EFExx and MeshSubmit case 1: TextureData +8 is GPU resource;
    // native caller tests its +12 bit 0 before using the texture.
    const bool ready=data&&read(data+8,gpu)&&gpu&&read(gpu+0x12,flags)&&(flags&1);
    if (ready) {
        // Transfer the native-owned reference pairs without copying refs.
        texture.bytes=output;texture.owned=true;state->loaded.fetch_add(1,std::memory_order_release);
    } else native_.release_binding(output.data());
}
void BoxRenderer::set_options(bool enabled, unsigned material, float brightness) noexcept {
    material_.store(std::min(material,1U), std::memory_order_relaxed);
    brightness_.store(std::isfinite(brightness) ? std::clamp(brightness,0.05F,1.0F) : 1.0F, std::memory_order_relaxed);
    enabled_.store(enabled, std::memory_order_release);
    if(!enabled)static_->first_person_ready.store(false,std::memory_order_release);
}
void BoxRenderer::fail(BoxFailure why) noexcept { failures_[static_cast<unsigned>(why)].fetch_add(1, std::memory_order_relaxed); }
BoxRenderer::WorldScope::WorldScope(BoxRenderer& owner, void* context, void* actor, ViewMode view) noexcept
    : owner_(owner), previous_(current_), view_(view) {
    owner_.worlds_.fetch_add(1, std::memory_order_relaxed);
    owner_.view_.store(view, std::memory_order_relaxed);
    if (owner_.enabled()) valid_ = owner_.prepare(*this, context, actor);
    if (!valid_&&owner_.static_mode_.load(std::memory_order_acquire)&&owner_.static_)
        owner_.static_->ready.store(false,std::memory_order_release);
    if(!valid_||view!=ViewMode::first_person||!animation_input_.valid)
        owner_.static_->first_person_ready.store(false,std::memory_order_release);
    current_ = this;
}
BoxRenderer::WorldScope::~WorldScope() { current_ = previous_; }
bool BoxRenderer::prepare(WorldScope& scope, void* context, void* actor) noexcept {
    const auto c = reinterpret_cast<std::uintptr_t>(context);
    const auto a = reinterpret_cast<std::uintptr_t>(actor);
    std::uintptr_t camera{}, table{}, yaw{};
    float alpha{};
    BoxVec3 origin{};
    // BaseActorRenderContext offsets are read in native camera/dispatcher;
    // native dispatcher uses *context+A8 then +28 for camera origin, B0 alpha.
    if (!c || !read(c+0x28, scope.screen_) || !scope.screen_ || !read(c+0xA8, camera) ||
        !camera || !read(camera+0x28, origin) || !finite(origin) || !read(c+0xB0, alpha) ||
        !std::isfinite(alpha) || alpha < 0 || alpha > 1) { fail(BoxFailure::context); return false; }
    std::uintptr_t top{};
    BoxMatrix base{};
    if (!read(reinterpret_cast<std::uintptr_t>(scope.screen_)+0x18, scope.matrices_) || !scope.matrices_ ||
        !matrix_top(scope.matrices_,top,base)) { fail(BoxFailure::matrix); return false; }
    if (!a || !read(a,table) || !read(table+0x68,yaw) || yaw != reinterpret_cast<std::uintptr_t>(native_.yaw)) {
        fail(BoxFailure::pose); return false;
    }
    BoxVec3 position{}, offset{};
    native_.position(actor,&position,alpha);
    native_.riding(&offset,actor,alpha);
    const auto body_yaw = native_.yaw(actor,alpha);
    const auto height = native_.height(reinterpret_cast<void*>(a+8));
    position = {position.x+offset.x-origin.x, position.y+offset.y-origin.y-height, position.z+offset.z-origin.z};
    if (!finite(position) || !std::isfinite(body_yaw) || !std::isfinite(height)) { fail(BoxFailure::pose); return false; }
    scope.distance_squared_=position.x*position.x+position.y*position.y+position.z*position.z;
    scope.world_ = box_world_matrix(base,position,body_yaw);
    if (static_mode_.load(std::memory_order_acquire)&&static_&&static_->configured.load(std::memory_order_acquire)&&static_->registry) {
        if (const auto model=static_->registry->snapshot())
            scope.world_=model_world_matrix(base,position,body_yaw,model->yaw_offset);
        if(animation_enabled_.load(std::memory_order_acquire)||scope.view_==ViewMode::first_person){
            scope.animation_input_=read_animation_input(actor,alpha,body_yaw);
            scope.animation_input_.world_position={position.x+origin.x,position.y+origin.y,position.z+origin.z};
            if(scope.view_==ViewMode::first_person&&scope.animation_input_.valid)
                scope.world_=first_person_world_matrix(base,body_yaw-scope.animation_input_.head_yaw,scope.animation_input_.pitch);
        }
    }
    return finite(scope.world_);
}

void BoxRenderer::after_submit(void* screen, void* tessellator, const void* texture,
                               const void* metadata, MeshImmediateFn original) noexcept {
    submits_.fetch_add(1, std::memory_order_relaxed);
    observe_binding(texture,metadata);
    if (static_mode_.load(std::memory_order_acquire)) return;
    auto* scope = current_;
    if (!enabled() || !scope || &scope->owner_ != this || !scope->valid_ || scope->drawn_ || scope->drawing_ ||
        scope->screen_ != screen || !original) return;
    in_world_.fetch_add(1, std::memory_order_relaxed);
    const auto t = reinterpret_cast<std::uintptr_t>(tessellator);
    const auto texture_address = reinterpret_cast<std::uintptr_t>(texture);
    std::int8_t tag{-1};
    std::array<std::byte,0x28> binding{};
    if (!texture_address || !read(texture_address,binding) || !read(texture_address+0x20,tag) ||
        tag < 0 || tag > 3 || !metadata) { fail(BoxFailure::texture); return; }
    TessState saved{};
    std::uintptr_t top{};
    BoxMatrix matrix{};
    std::uint8_t alternative{};
    if (!read(scope->matrices_+0x70,alternative) || alternative || !matrix_top(scope->matrices_,top,matrix)) {
        fail(BoxFailure::matrix); return;
    }
    if (!t || !saved.capture(t) || saved.count || (saved.begin_flags & 255) || (saved.end_flags >> 8)) {
        fail(BoxFailure::tessellator); return;
    }
    const bool entity = material_.load(std::memory_order_relaxed) == 1;
    const auto* key = entity ? native_.entity_key : native_.alpha_key;
    if (!key_valid(reinterpret_cast<std::uintptr_t>(key),entity)) { fail(BoxFailure::material); return; }
    // Native MaterialPtr output is exactly two words in lookup/release. It is
    // initialized/destroyed by the game; never use a DLL std::shared_ptr here.
    struct alignas(16) Handle { std::array<std::uintptr_t,2> words{}; } material;
    native_.material(&material,native_.material_group,key);
    struct Release { MaterialReleaseFn fn; Handle& handle; ~Release() { fn(&handle); } } release{native_.release,material};
    if (!material.words[0]) { fail(BoxFailure::material); return; }
    LARGE_INTEGER start{},end{};
    QueryPerformanceCounter(&start);
    scope->drawing_ = true;
    {
        RestoreDrawState restore{t,top,scope->matrices_,saved,matrix};
        write(top,scope->world_); write(scope->matrices_+0x78,std::uint8_t{1});
        native_.begin(tessellator,nullptr,2,36,0); // RDX is unused; mode 2 groups triangles.
        write(t+0x210,std::uint8_t{0});
        write(t+0x1CC,BoxVec3{}); write(t+0x1D8,BoxVec3{1,1,1});
        for (auto flag : {0x168,0x1BA,0x1BE,0x1C4,0x1C9}) write(t+flag,std::uint8_t{0});
        const float brightness = brightness_.load(std::memory_order_relaxed);
        for (const auto& v : test_box_vertices()) {
            const std::array<float,4> normal{v.normal.x,v.normal.y,v.normal.z,0};
            write(t+0x178,normal); write(t+0x188,std::uint8_t{1});
            write(t+0x18C,v.uv); write(t+0x194,std::uint8_t{1});
            std::uint32_t color = 0xFF000000;
            for (unsigned shift : {0U,8U,16U}) color |= static_cast<std::uint32_t>(((v.color >> shift) & 255)*brightness) << shift;
            write(t+0x1B0,color); write(t+0x1B4,std::uint8_t{1});
            native_.vertex(tessellator,v.position.x,v.position.y,v.position.z);
        }
        // Stock source binding is only copied (including refs) by MeshImmediate;
        // caller still owns it here. The wrapper consumes our tessellator and
        // destroys its own native Mesh. Native submit copies the matrix.
        original(screen,tessellator,&material,texture,metadata);
    }
    scope->drawing_ = false;
    scope->drawn_ = true;
    drawn_.fetch_add(1, std::memory_order_relaxed);
    QueryPerformanceCounter(&end);
    ticks_.fetch_add(static_cast<std::uint64_t>(end.QuadPart-start.QuadPart),std::memory_order_relaxed);
}

void BoxRenderer::before_mesh_submit(void* render_context, const void* bindings,
                                     const void* metadata, MeshImmediateFn original) noexcept {
    native_submits_.fetch_add(1,std::memory_order_relaxed);
    std::uintptr_t binding_count{};
    if (enabled() && bindings && read(reinterpret_cast<std::uintptr_t>(bindings)+0x140,binding_count) && binding_count && binding_count<=8)
        observe_binding(bindings,metadata);
    auto* scope=current_;
    if (!enabled() || !scope || &scope->owner_!=this || !scope->valid_ || scope->drawn_ || scope->drawing_) return;
    scoped_submits_.fetch_add(1,std::memory_order_relaxed);
    std::uintptr_t matrices{};
    if (!read(reinterpret_cast<std::uintptr_t>(render_context)+8,matrices) || matrices!=scope->matrices_) {
        other_screen_.fetch_add(1,std::memory_order_relaxed); return;
    }
    // Native 0x144B045DD..4638: inline 40-byte texture-binding records,
    // count at +0x140. The body destroys them before return, so borrow BEFORE
    // forwarding the native submit. Metadata (slot 7) is copied, not moved.
    std::uintptr_t count{};
    void* tess{};
    if (!bindings || !read(reinterpret_cast<std::uintptr_t>(bindings)+0x140,count) || !count || count>8) {
        fail(BoxFailure::texture); return;
    }
    // Stock actor/item caller 0x14560200F loads the same ScreenContext +B8
    // Tessellator. A busy tessellator is rejected without touching its mesh.
    if (!read(reinterpret_cast<std::uintptr_t>(scope->screen_)+0xB8,tess) || !tess) {
        fail(BoxFailure::tessellator); return;
    }
    after_submit(scope->screen_,tess,bindings,metadata,original);
}

void BoxRenderer::observe_binding(const void* texture, const void* metadata) noexcept {
    if (!enabled() || texture_ready() || !native_.copy_binding || !texture || !metadata) return;
    std::int8_t tag{-1}, metadata_tag{-1};
    std::array<std::byte,40> empty_metadata{};
    if (!read(reinterpret_cast<std::uintptr_t>(texture)+0x20,tag) || tag<1 || tag>3 ||
        !read(reinterpret_cast<std::uintptr_t>(metadata)+0x20,metadata_tag) || metadata_tag!=0 ||
        !read(reinterpret_cast<std::uintptr_t>(metadata),empty_metadata)) return;
    if (texture_lock_.test_and_set(std::memory_order_acquire)) return;
    if (!texture_ready_.load(std::memory_order_relaxed)) {
        // Native 0x144B17B20 copy, destination-wrapper holds a pointer to its
        // 40-byte storage. Native switch table 0x14E8F7A08 maps source tags
        // 1/2/3 to copies retaining their shared resource references.
        void* destination=texture_.data();
        native_.copy_binding(tag+1,&destination,texture);
        // The metadata switch tables prove tag 0 is empty, copies only its
        // tag and destroys no payload. Copy this native value, no fake STL.
        metadata_=empty_metadata;
        texture_ready_.store(true,std::memory_order_release);
    }
    texture_lock_.clear(std::memory_order_release);
}

void BoxRenderer::finish_world(MeshImmediateFn original) noexcept {
    auto* scope=current_;
    if (!enabled() || !scope || &scope->owner_!=this || !scope->valid_ || scope->drawn_) return;
    if (static_mode_.load(std::memory_order_acquire)) { finish_static(*scope,original);return; }
    if (!texture_ready()) { fail(BoxFailure::texture); return; }
    void* tess{};
    if (!read(reinterpret_cast<std::uintptr_t>(scope->screen_)+0xB8,tess) || !tess) {
        fail(BoxFailure::tessellator); return;
    }
    // Borrow the immutable native-owned binding while this callback is alive.
    // The game wrapper copies it again for each submitted mesh. No context or
    // actor survives WorldScope; resource destruction waits for callback drain.
    after_submit(scope->screen_,tess,texture_.data(),metadata_.data(),original);
}

void BoxRenderer::finish_static(WorldScope& scope,MeshImmediateFn original) noexcept {
    auto* state=static_.get();if (!state||!state->configured.load(std::memory_order_acquire)||!state->native||!state->registry) return;
    const auto model=state->registry->snapshot();if (!model||model->parts.empty()||model->parts.size()>state->meshes.size()) return;
    if (state->upload_lock.test_and_set(std::memory_order_acquire)) return;
    struct Unlock { std::atomic_flag& flag;~Unlock(){flag.clear(std::memory_order_release);} } unlock{state->upload_lock};
    void* tess{};const auto screen=reinterpret_cast<std::uintptr_t>(scope.screen_);
    if (!read(screen+0xB8,tess)||!tess) { fail(BoxFailure::tessellator);return; }
    const auto t=reinterpret_cast<std::uintptr_t>(tess);
    TessState saved{};std::uintptr_t top{};BoxMatrix matrix{};std::uint8_t alternative{};
    if (!read(scope.matrices_+0x70,alternative)||alternative||!matrix_top(scope.matrices_,top,matrix)) { fail(BoxFailure::matrix);return; }
    if (!saved.capture(t)||saved.count||(saved.begin_flags&255)||(saved.end_flags>>8)) { fail(BoxFailure::tessellator);return; }
    LARGE_INTEGER start{};QueryPerformanceCounter(&start);
    struct Measure { StaticState& state;LARGE_INTEGER start;~Measure(){LARGE_INTEGER end{};QueryPerformanceCounter(&end);state.ticks.fetch_add(end.QuadPart-start.QuadPart,std::memory_order_relaxed);} } measure{*state,start};
    scope.drawing_=true;
    struct Drawing { bool& flag;~Drawing(){flag=false;} } drawing{scope.drawing_};
    RestoreDrawState restore{t,top,scope.matrices_,saved,matrix};
    const auto uploaded=state->uploaded.load(std::memory_order_relaxed);
    if (uploaded<model->parts.size()) {
        const auto& part=model->parts[uploaded];auto& mesh=state->meshes[uploaded];
        native_.begin(tess,nullptr,2,static_cast<int>(part.vertices.size()),0);
        write(t+0x210,std::uint8_t{0});write(t+0x1CC,BoxVec3{});write(t+0x1D8,BoxVec3{1,1,1});
        for (auto flag:{0x168,0x1BA,0x1BE,0x1C4,0x1C9}) write(t+flag,std::uint8_t{0});
        for (const auto& v:part.vertices) {
            write(t+0x178,std::array<float,4>{v.normal[0],v.normal[1],v.normal[2],0});write(t+0x188,std::uint8_t{1});
            write(t+0x18C,v.uv);write(t+0x194,std::uint8_t{1});write(t+0x1B0,v.color);write(t+0x1B4,std::uint8_t{1});
            native_.vertex(tess,v.position[0],v.position[1],v.position[2]);
        }
        struct View { const char* data;std::size_t size; } label{"Medicon static FBX",18};
        state->native.end(tess,mesh.bytes.data(),2,&label,state->native.view());mesh.constructed=true;
        // Same post-end reset as native 1446EEEEF..EEF62. Builder vectors are
        // cleared by the game, not resized/freed through DLL STL.
        write(t+0x298,std::uint32_t{0});write(t+0x255,std::uint8_t{0});write(t+0x2A0,std::uint8_t{0});write(t+0x1CA,std::uint8_t{0});
        state->native.clear(reinterpret_cast<void*>(t+8));write(t,std::uint8_t{0});write(t+0x170,std::uint64_t{0});
        for (auto flag:{0x1B4,0x188,0x1BA,0x168,0x194,0x1A0,0x1AC,0x1BE,0x1C9}) write(t+flag,std::uint8_t{0});
        state->uploaded.store(uploaded+1,std::memory_order_release);
        return; // At most 4095 vertices per world callback; keep vanilla visible.
    }
    if (!texture_ready()||state->loaded.load(std::memory_order_acquire)!=model->textures.size()) return;
    const bool entity=material_.load(std::memory_order_relaxed)==1;
    const auto* key=entity?native_.entity_key:native_.alpha_key;
    if (!key_valid(reinterpret_cast<std::uintptr_t>(key),entity)) { state->ready.store(false,std::memory_order_release);fail(BoxFailure::material);return; }
    alignas(16) std::array<std::uintptr_t,2> material{};
    native_.material(material.data(),native_.material_group,key);
    struct Release { MaterialReleaseFn fn;void* ptr;~Release(){fn(ptr);} } release{native_.release,material.data()};
    if (!material[0]) { state->ready.store(false,std::memory_order_release);fail(BoxFailure::material);return; }
    state->ready.store(true,std::memory_order_release);
    // Present activates suppression and custom drawing together after warmup.
    const bool first_person=scope.view_==ViewMode::first_person;
    if (!static_visible_.load(std::memory_order_acquire) || scope.view_==ViewMode::unknown ||
        (first_person&&(!model->rig||!model->rig->first_person.ready()||!model->animation||!original))) {
        // A missing arm mesh keeps native hands; never fall back to the full
        // P2 body in first person. Native resource warmup remains valid.
        state->first_person_ready.store(false,std::memory_order_release);
        state->animation_last = {};
        return;
    }
    write(top,scope.world_);write(scope.matrices_+0x78,std::uint8_t{1});
    const bool animate=animation_enabled_.load(std::memory_order_acquire);
    if((animate||first_person)&&model->rig&&model->animation&&original&&(first_person||scope.distance_squared_<=48.0F*48.0F)){
        const auto& input=scope.animation_input_;state->animation_input_ready.store(input.valid,std::memory_order_relaxed);
        if(!input.valid){state->animation_read_failures.fetch_add(1,std::memory_order_relaxed);
            if(first_person){state->first_person_ready.store(false,std::memory_order_release);return;}}
        else{
            LARGE_INTEGER now{},frequency{},skinned{},finished{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
            const float dt=state->animation_last.QuadPart?float(double(now.QuadPart-state->animation_last.QuadPart)/frequency.QuadPart):1.0F/60;
            state->animation_last=now;
            if(first_person)model->animation->update_first_person(*model->rig,input,dt,animate);
            else model->animation->update(*model->rig,input,dt);
            QueryPerformanceCounter(&skinned);
            state->animation_motion.store(static_cast<unsigned>(model->animation->motion()),std::memory_order_relaxed);
            const auto output=first_person?model->animation->first_person_output():model->animation->output();
            const auto& batches=first_person?model->rig->first_person.batches:model->rig->batches;
            for(const auto& batch:batches){
                native_.begin(tess,nullptr,2,static_cast<int>(batch.vertices.size()),0);
                write(t+0x210,std::uint8_t{0});write(t+0x1CC,BoxVec3{});write(t+0x1D8,BoxVec3{1,1,1});
                for(auto flag:{0x168,0x1BA,0x1BE,0x1C4,0x1C9})write(t+flag,std::uint8_t{0});
                const auto emit=[&](std::size_t i){const auto& v=batch.vertices[i];const auto& s=output[v.skin_index];
                    write(t+0x178,std::array<float,4>{s.normal[0],s.normal[1],s.normal[2],0});write(t+0x188,std::uint8_t{1});
                    write(t+0x18C,v.uv);write(t+0x194,std::uint8_t{1});write(t+0x1B0,input.hurt?0xFF6060FFU:v.color);write(t+0x1B4,std::uint8_t{1});
                    native_.vertex(tess,s.position[0],s.position[1],s.position[2]);};
                // One native seed reserves/initializes all active arrays using
                // begin's count hint. Verified native vector/capacity envelope
                // then allows a bulk fill rather than 60K native vertex calls.
                emit(0);
                if(fill_animated_tessellator(tess,output,batch,input.hurt))state->bulk_batches.fetch_add(1,std::memory_order_relaxed);
                else{for(std::size_t i=1;i<batch.vertices.size();++i)emit(i);state->fallback_batches.fetch_add(1,std::memory_order_relaxed);}
                const auto& texture=state->textures[batch.texture];original(scope.screen_,tess,material.data(),texture.bytes.data(),metadata_.data());
            }
            QueryPerformanceCounter(&finished);state->animation_frames.fetch_add(1,std::memory_order_relaxed);state->animation_ticks.fetch_add(skinned.QuadPart-now.QuadPart,std::memory_order_relaxed);
            state->animation_draw_ticks.fetch_add(finished.QuadPart-skinned.QuadPart,std::memory_order_relaxed);
            state->first_person_ready.store(first_person,std::memory_order_release);
            scope.drawn_=true;drawn_.fetch_add(1,std::memory_order_relaxed);return;
        }
    }
    for (unsigned i=0;i<model->parts.size();++i) {
        const auto& texture=state->textures[model->parts[i].texture];
        state->native.draw(state->meshes[i].bytes.data(),reinterpret_cast<void*>(screen+0x10),material.data(),texture.bytes.data(),0,0,metadata_.data());
    }
    scope.drawn_=true;drawn_.fetch_add(1,std::memory_order_relaxed);
}

void BoxRenderer::release_resources() noexcept {
    view_.store(ViewMode::unknown,std::memory_order_relaxed);
    if (static_) {
        static_->first_person_ready.store(false,std::memory_order_release);
        static_->ready.store(false,std::memory_order_release);
        for (auto& mesh:static_->meshes) if (mesh.constructed) { static_->native.destroy(mesh.bytes.data());mesh.constructed=false; }
        for (auto& texture:static_->textures) if (texture.owned) { native_.release_binding(texture.bytes.data());texture.owned=false; }
        static_->uploaded.store(0);static_->loaded.store(0);static_->requests.store(0);static_->ticks.store(0);
        static_->next_request=0;static_->request_index=0;static_->upload_lock.clear();static_->request_lock.clear();
        static_->animation_last={};(void)take_animation_frame();
    }
    if (texture_ready_.exchange(false,std::memory_order_acq_rel) && native_.release_binding)
        native_.release_binding(texture_.data());
    texture_={}; metadata_={};
    texture_lock_.clear(std::memory_order_release);
}

BoxFrame BoxRenderer::take_frame() noexcept {
    BoxFrame out{worlds_.exchange(0),submits_.exchange(0),in_world_.exchange(0),drawn_.exchange(0),ticks_.exchange(0),
        native_submits_.exchange(0),scoped_submits_.exchange(0),other_screen_.exchange(0),{}};
    for (unsigned i=0; i<failures_.size(); ++i) out.failures[i]=failures_[i].exchange(0);
    return out;
}
}
