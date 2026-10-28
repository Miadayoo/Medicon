#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace medicon::custom_model {
using Float3=std::array<float,3>;
using Quaternion=std::array<float,4>;
struct Transform { Float3 translation{};Quaternion rotation{0,0,0,1};Float3 scale{1,1,1}; };
// Column-major matrix with padded columns for SSE2. No game ABI types.
struct alignas(16) Affine { std::array<float,16> v{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; };
[[nodiscard]] Quaternion multiply(Quaternion a,Quaternion b) noexcept;
[[nodiscard]] Quaternion slerp(Quaternion a,Quaternion b,float t) noexcept;
[[nodiscard]] Quaternion axis_angle(Float3 axis,float degrees) noexcept;
[[nodiscard]] Float3 rotate(Quaternion q,Float3 v) noexcept;
[[nodiscard]] Affine matrix(const Transform& t) noexcept;
[[nodiscard]] Affine multiply(const Affine& a,const Affine& b) noexcept;
[[nodiscard]] Affine inverse(const Affine& a);
[[nodiscard]] Affine normal_matrix(const Affine& a) noexcept;
[[nodiscard]] Float3 point(const Affine& a,Float3 p) noexcept;
[[nodiscard]] Transform blend(const Transform& a,const Transform& b,float t) noexcept;
struct Bone { std::string name;int parent{-1};Transform rest;Quaternion world_rotation{0,0,0,1}; };
struct Binding { std::uint32_t bone{};Affine inverse_bind; };
struct Weights { std::array<std::uint16_t,4> binding{};std::array<float,4> weight{1,0,0,0}; };
struct SkinVertex { Float3 position{},normal{};Weights skin; };
struct SkinOutput { Float3 position{},normal{}; };
struct SkinMatrix { Affine position,normal; };
struct AnimatedVertex { std::uint32_t skin_index{};std::array<float,2> uv{};std::uint32_t color{0xFFFFFFFF}; };
struct AnimationBatch { std::uint32_t texture{};std::vector<AnimatedVertex> vertices; };
enum class Motion : unsigned { idle,walk,run,sneak_idle,sneak,fall,swim,sit,glide,sleep,attack,use,count };
inline constexpr unsigned motion_count=static_cast<unsigned>(Motion::count);
[[nodiscard]] const char* motion_name(Motion motion) noexcept;
struct Clip { std::string name;float duration{1};std::uint32_t frames{};bool loop{true},procedural{};std::vector<Transform> samples; };
struct Roles { int hips{-1},spine{-1},neck{-1},head{-1},left_arm{-1},right_arm{-1},left_elbow{-1},right_elbow{-1},left_leg{-1},right_leg{-1},left_knee{-1},right_knee{-1}; };
struct FirstPersonArms {
    std::vector<SkinVertex> vertices; // arm-only, arm weights renormalized
    std::vector<AnimationBatch> batches; // original material/UVs, whole triangles
    std::vector<std::uint8_t> binding_sides; // 0=excluded, 1=left, 2=right
    std::array<Affine,2> root_to_origin; // translation only: rest shoulder -> origin
    float scale{1};
    [[nodiscard]] bool ready() const noexcept { return !vertices.empty() && !batches.empty(); }
};
struct RigAsset {
    std::vector<Bone> bones;
    std::vector<Binding> bindings;
    std::vector<SkinVertex> vertices;
    std::vector<std::vector<std::uint32_t>> part_indices;
    std::vector<AnimationBatch> batches; // worker-built per material, immutable
    std::array<Clip,motion_count> clips;
    std::vector<std::uint8_t> upper_mask;
    Roles roles;
    FirstPersonArms first_person;
    std::string diagnostic;
    bool from_cache{};
};
struct AnimationInput {
    std::uint64_t identity{},world_identity{};
    Float3 world_position{};
    float speed{},vertical_speed{},head_yaw{},pitch{},attack_progress{-1},death_seconds{};
    bool valid{},sprinting{},sneaking{},on_ground{true},using_item{},riding{},swimming{},gliding{},sleeping{},hurt{},dead{};
};
// All vectors allocated by the import worker before asset publication.
// One render callback owns update() under BoxRenderer's existing try-lock.
class AnimationInstance {
public:
    explicit AnimationInstance(const RigAsset& rig);
    void reset() noexcept;
    void update(const RigAsset& rig,const AnimationInput& input,float seconds) noexcept;
    void update_first_person(const RigAsset& rig,const AnimationInput& input,float seconds,bool animate) noexcept;
    [[nodiscard]] std::span<const SkinOutput> output() const noexcept { return output_; }
    [[nodiscard]] std::span<const SkinOutput> first_person_output() const noexcept { return first_person_output_; }
    [[nodiscard]] Motion motion() const noexcept { return motion_; }
    [[nodiscard]] float fade() const noexcept { return fade_; }
    [[nodiscard]] float overlay() const noexcept { return upper_weight_; }
    std::span<const Transform> pose() const noexcept { return pose_; }
private:
    std::vector<Transform> pose_,from_,target_,upper_pose_,upper_from_;
    std::vector<Affine> world_;
    std::vector<SkinMatrix> palette_;
    std::vector<SkinOutput> output_,first_person_output_;
    std::uint64_t identity_{},world_identity_{};
    Motion motion_{Motion::idle};
    float time_{},gait_{},fade_{1},upper_weight_{},layer_phase_{},layer_fade_{1},death_elapsed_{};
    Motion layer_motion_{Motion::use};
    bool initialized_{},first_person_{};
};
// Worker only, after full-model batches are built (also on rig-cache hits).
// Uses skeleton ancestry and skin weights, never positions or material names.
void build_first_person_arms(RigAsset& rig,float scale);
void sample_clip(const Clip& clip,std::size_t bones,float phase,std::span<Transform> output) noexcept;
// Runtime SIMD LBS; caller supplies already sized output and palettes.
void skin_vertices(std::span<const SkinVertex> input,std::span<const SkinMatrix> palette,std::span<SkinOutput> output) noexcept;
void skin_vertices_scalar(std::span<const SkinVertex> input,std::span<const SkinMatrix> palette,std::span<SkinOutput> output) noexcept;
struct ModelAsset;
// Worker-only rig extraction / 30Hz baking / .rig.mmdl cache.
void load_animation_asset(const std::filesystem::path& root,std::span<const std::byte> fbx,
    const std::string& config_text,ModelAsset& model,const void* loaded_scene=nullptr);
}
