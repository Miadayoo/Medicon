#include <medicon/sdk/CustomModelBox.hpp>
#include <medicon/sdk/CustomModelP1Patterns.hpp>
#include <medicon/sdk/CustomModelRender.hpp>
#include <medicon/memory/pattern.hpp>
#include "custom_model_fixture.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace medicon::sdk::custom_model;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T get(const void* base, std::size_t offset) {
    T result{}; std::memcpy(&result,static_cast<const std::byte*>(base)+offset,sizeof(T)); return result;
}
template<class T> void put(void* base, std::size_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(base)+offset,&value,sizeof(value));
}
BoxMatrix identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
bool close(float a,float b) { return std::fabs(a-b)<0.0001F; }
void resolution_test() {
    namespace profile = bedrock_1_26_60_5;
    namespace pattern = p1_native;
    constexpr std::uintptr_t base = 0x140000000, code_rva = 0x1000, group_rva = 0x8000, key_rva = 0x9000;
    const std::array patterns{
        pattern::world_render_pattern, pattern::mesh_immediate_pattern, pattern::mesh_view_pattern,
        pattern::tess_begin_pattern, pattern::tess_vertex_pattern, pattern::interpolated_position_pattern,
        pattern::riding_offset_pattern, pattern::body_yaw_pattern, pattern::height_offset_pattern,
        pattern::material_lookup_pattern, pattern::material_release_pattern, pattern::material_anchor_pattern,
        pattern::mesh_submit_pattern, pattern::binding_copy_pattern, pattern::binding_release_pattern};
    std::vector<std::byte> code;
    std::array<std::size_t, patterns.size()> offsets{};
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const auto parsed = medicon::memory::Pattern::parse(patterns[i]);
        require(parsed.has_value(), "P1 native pattern parses");
        code.insert(code.end(), 16, std::byte{0xCC});
        offsets[i] = code.size();
        for (const auto byte : parsed->bytes())
            code.push_back(std::byte{byte.wildcard ? std::uint8_t{0xA5} : byte.value});
    }
    const auto anchor = offsets[11];
    const auto group_displacement = static_cast<std::int32_t>(group_rva - (code_rva + anchor + 7));
    const auto key_displacement = static_cast<std::int32_t>(key_rva - (code_rva + anchor + 14));
    put(code.data(), anchor + 3, group_displacement);
    put(code.data(), anchor + 10, key_displacement);
    const std::array regions{medicon::memory::MemoryRegion{code, base + code_rva}};
    std::string diagnostic;
    for (const auto size : {profile::official_image_size, profile::launcher_image_size}) {
        const auto targets = resolve_box_targets(profile::time_date_stamp, size, regions, {base, size}, diagnostic);
        require(targets && reinterpret_cast<std::uintptr_t>(targets.world) == base + code_rva + offsets[0] &&
                reinterpret_cast<std::uintptr_t>(targets.mesh) == base + code_rva + offsets[1] &&
                reinterpret_cast<std::uintptr_t>(targets.mesh_view) == base + code_rva + offsets[2] &&
                reinterpret_cast<std::uintptr_t>(targets.copy_binding) == base + code_rva + offsets[13] &&
                reinterpret_cast<std::uintptr_t>(targets.release_binding) == base + code_rva + offsets[14],
                "Stock and LeviLauncher resolve the same native draw and binding entries");
        require(reinterpret_cast<std::uintptr_t>(targets.material_group) == base + group_rva &&
                reinterpret_cast<std::uintptr_t>(targets.alpha_key) == base + key_rva &&
                reinterpret_cast<std::uintptr_t>(targets.entity_key) == base + key_rva + 0x90,
                "Both packagings keep the inspected RIP-relative material addresses");
        const auto saved = code[offsets[0]];
        code[offsets[0]] = std::byte{0xCC};
        require(!resolve_box_targets(profile::time_date_stamp, size, regions, {base, size}, diagnostic),
                "Neither packaging may enable rendering with a missing world entry");
        code[offsets[0]] = saved;
        const std::array duplicate{regions[0], medicon::memory::MemoryRegion{code, base + 0x10000}};
        require(!resolve_box_targets(profile::time_date_stamp, size, duplicate, {base, size}, diagnostic),
                "Neither packaging may enable rendering with ambiguous entries");
        require(!resolve_box_targets(0x6AB54E37, size, regions, {base, size}, diagnostic),
                "Matching native signatures cannot admit an older Minecraft build");
    }
    require(!resolve_box_targets(profile::time_date_stamp, 0x1383D000, regions, {base, 0x1383D000}, diagnostic),
            "An uninspected packaging is rejected");
}
void geometry_test() {
    const auto& box=test_box_vertices();
    float low[3]{10,10,10},high[3]{-10,-10,-10};
    for (const auto& v:box) {
        const float p[]{v.position.x,v.position.y,v.position.z};
        for (unsigned k=0;k<3;++k) { low[k]=std::min(low[k],p[k]); high[k]=std::max(high[k],p[k]); }
        require(v.uv[0]>=0 && v.uv[0]<=1 && v.uv[1]>=0 && v.uv[1]<=1,"UV bounds");
    }
    require(close(high[0]-low[0],1) && close(high[1]-low[1],2) && close(high[2]-low[2],1),"1x2x1 box");
    for (unsigned i=0;i<box.size();i+=3) {
        const auto a=box[i].position,b=box[i+1].position,c=box[i+2].position;
        const BoxVec3 ab{b.x-a.x,b.y-a.y,b.z-a.z},ac{c.x-a.x,c.y-a.y,c.z-a.z};
        const BoxVec3 cross{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
        const auto n=box[i].normal;
        require(cross.x*n.x+cross.y*n.y+cross.z*n.z>0,"Outward nondegenerate face triangles");
    }
    for (unsigned f=0;f<6;++f) for (unsigned g=f+1;g<6;++g) require(box[f*6].color!=box[g*6].color,"Six distinct colors");
    const auto matrix=box_world_matrix(identity(),{3,4,5},90);
    require(close(matrix[12],3)&&close(matrix[13],4)&&close(matrix[14],5)&&close(matrix[8],1)&&close(matrix[2],-1),"Native column-major T*Ry convention");
    // Sprinter's +Z forward with yawOffset=180 must follow Minecraft's yaw.
    for (const auto yaw : {0.0F,90.0F,180.0F,-90.0F}) {
        const auto model=model_world_matrix(identity(),{3,4,5},yaw,180);
        const auto radians=yaw*3.14159265358979323846F/180;
        require(close(model[8],-std::sin(radians)*BoxRenderer::body_scale)&&close(model[10],std::cos(radians)*BoxRenderer::body_scale),"Larger model faces the movement direction at all cardinal yaws");
        require(close(model[5],1.10F),"Body grows ten percent around its feet");
        require(close(model[12],3)&&close(model[13],4)&&close(model[14],5),"Facing correction preserves world translation");
    }
    const auto arms=first_person_world_matrix(identity(),90,30);
    require(close(arms[8],-std::cos(3.14159265358979323846F/6))&&close(arms[9],-.5F)&&close(arms[10],0),"Camera-relative arms follow interpolated head yaw and pitch");
    require(close(arms[12],0)&&close(arms[13],0)&&close(arms[14],0),"First-person view never adds player-feet translation");
}

void view_reader_test() {
    std::array<std::byte,0xE20> client{};
    std::array<std::uintptr_t,175> table{};
    std::array<std::byte,0x40> options{};
    std::array<std::uintptr_t,2> options_table{};
    std::array<std::byte,0x90> option{},override_option{};
    std::array<std::byte,0x240> impl{},override_impl{};
    std::array<std::byte,8> getter{std::byte{0x48},std::byte{0x8B},std::byte{0x81},std::byte{0x00},std::byte{0x0E},std::byte{0},std::byte{0},std::byte{0xC3}};
    const ViewProfile profile{reinterpret_cast<std::uintptr_t>(getter.data()),0x12340000};
    put(client.data(),0,table.data());table[172]=profile.client_options_getter;
    put(client.data(),0xE00,options.data());put(options.data(),0,options_table.data());options_table[1]=profile.option_getter;
    put(options.data(),0x28,option.data());put(option.data(),8,impl.data());put(impl.data(),0x1D4,6);
    const auto before=client;
    for(int value=0;value<3;++value){
        put(option.data(),0x10,std::array<int,4>{2,0,value,0});
        const auto view=read_client_view(client.data(),profile);
        require(view==static_cast<ViewMode>(1+value)&&third_person(view)==(value!=0),"Read native first/back/front setting without invoking native methods");
    }
    require(client==before,"Perspective reader never writes client state");
    put(impl.data(),0x238,override_option.data());put(override_option.data(),8,override_impl.data());
    put(override_impl.data(),0x1D4,5);put(override_option.data(),0x10,std::array<int,4>{2,0,0,0});
    require(read_client_view(client.data(),profile)==ViewMode::first_person,"Per-client option override takes precedence");
    put(override_impl.data(),0x238,option.data());
    require(read_client_view(client.data(),profile)==ViewMode::unknown,"Cyclic override chain is bounded");
    put(impl.data(),0x238,std::uintptr_t{});put(option.data(),0x18,3);
    require(read_client_view(client.data(),profile)==ViewMode::unknown,"Invalid perspective never draws a custom body");
    table[172]=0;
    require(read_client_view(client.data(),profile)==ViewMode::unknown,"Changed native getter fails validation");
    require(read_client_view(nullptr,profile)==ViewMode::unknown&&read_client_view(reinterpret_cast<void*>(UINTPTR_MAX-1),profile)==ViewMode::unknown,"Null and overflowing native pointers fail safely");
}

struct Fixture {
    std::array<std::byte,0xC0> context{},screen{},camera{},actor{},matrices{},table{};
    std::array<std::byte,0x2B0> tess{},old_tess{};
    std::array<std::byte,0x28> texture{},alpha{},entity{};
    std::array<std::byte,0x148> bindings{};
    std::array<std::uintptr_t,1> map{};
    BoxMatrix top=identity(),submitted{};
    std::array<BoxVertex,36> vertices{};
    unsigned begins{},count{},submits{},materials{},releases{},pose_reads{},binding_copies{},binding_releases{},expected_vertices{36};
    unsigned mesh_ends{},mesh_destroys{},mesh_draws{},builder_clears{},texture_requests{};
    std::array<std::byte,32> texture_data{},gpu{},old_gpu{};
    std::array<std::byte,40> metadata{};
    BoxRenderer renderer;
};
Fixture* live{};
void fake_world(void*,void*,const void*,void*) {}
void fake_view(void*,void*,const void*,const void*,std::uint16_t,const void*) {}
void fake_lower(void*,void*,const void*,void*,std::uintptr_t,std::uintptr_t,const void*,std::uintptr_t,void*) {}
void fake_binding_copy(std::intptr_t tag,void** destination,const void* source) {
    require(tag==2 && source==live->texture.data(),"Native retained binding copy slots");
    std::memcpy(*destination,source,40); ++live->binding_copies;
}
void fake_binding_release(void* object) {
    require(get<std::uint8_t>(object,0x20)==1,"Release native retained binding"); ++live->binding_releases;
}
BoxVec3* fake_position(void* actor,BoxVec3* out,float alpha) {
    require(actor==live->actor.data()&&alpha==.25F,"Position scalar ABI / native alpha");
    ++live->pose_reads; *out={101,51,201}; return out;
}
BoxVec3* fake_riding(BoxVec3* out,void* actor,float alpha) {
    require(actor==live->actor.data()&&alpha==.25F,"Riding sret physical slots"); *out={0,.5F,0}; return out;
}
float fake_yaw(void* actor,float alpha) {
    require(actor==live->actor.data()&&alpha==.25F,"Body yaw ABI"); return 90;
}
float fake_height(void* context) {
    require(context==live->actor.data()+8,"Native entity context adjustment"); return 1.5F;
}
void fake_begin(void* t,void* unused,std::uint8_t mode,int count,std::uint8_t indexed) {
    require(t==live->tess.data()&&!unused&&mode==2&&count==static_cast<int>(live->expected_vertices)&&!indexed,"All five begin ABI slots");
    ++live->begins; live->count=0;
    put(t,0x2A0,std::uint16_t{1}); put(t,8,mode); put(t,0x158,std::uint8_t{1});
    put(t,0x298,std::uint32_t{0}); put(t,0x170,std::uint64_t{36});
}
void fake_vertex(void* t,float x,float y,float z) {
    require(live->count<live->expected_vertices && !get<std::uint8_t>(t,0x210),"No stale transform");
    const auto translation=get<BoxVec3>(t,0x1CC),scale=get<BoxVec3>(t,0x1D8);
    require(translation.x==0&&translation.y==0&&translation.z==0&&scale.x==1&&scale.y==1&&scale.z==1,"No stale translation/scale");
    require(get<std::uint8_t>(t,0x188)&&get<std::uint8_t>(t,0x194)&&get<std::uint8_t>(t,0x1B4),"Normal, UV, color enabled");
    live->vertices[live->count++]={{x,y,z},get<BoxVec3>(t,0x178),get<std::array<float,2>>(t,0x18C),get<std::uint32_t>(t,0x1B0)};
    put(t,0x298,std::uint32_t{live->count});
}
void* fake_material(void* out,void* group,const void* key) {
    require(group==live && (key==live->alpha.data()||key==live->entity.data()),"Native material group/key");
    put(out,0,std::array<std::uintptr_t,2>{1,2}); ++live->materials; return out;
}
void fake_release(void* ptr) {
    require(get<std::uintptr_t>(ptr,8)==2,"Native control block released once"); ++live->releases;
}
void fake_submit(void* screen,void* t,const void* material,const void* texture,const void* metadata) {
    require(screen==live->screen.data()&&t==live->tess.data()&&texture&&metadata,"Forward all borrowed native arguments");
    require(get<std::array<std::byte,40>>(texture,0)==live->texture&&get<std::array<std::byte,40>>(metadata,0)==live->metadata,"Borrowed or retained native values match source");
    require(get<std::uintptr_t>(material,0)==1&&live->count==36,"Native output handle and complete mesh");
    live->submitted=live->top; ++live->submits;
    put(t,0x298,std::uint32_t{0}); put(t,0x2A0,std::uint8_t{0});
}
void initialize(Fixture& f) {
    live=&f;
    put(f.context.data(),0x28,f.screen.data()); put(f.context.data(),0xA8,f.camera.data()); put(f.context.data(),0xB0,.25F);
    // 1.26.60 camera origin starts at +20. Poison the former +28 window so
    // an old layout cannot silently draw at a plausible but wrong position.
    put(f.camera.data(),0x20,BoxVec3{100,50,200});
    put(f.camera.data(),0x2C,std::numeric_limits<float>::quiet_NaN());
    put(f.screen.data(),0x18,f.matrices.data());
    // Native 1.26.60 draw reads ScreenContext+B0. The old B8 field is an
    // unrelated resource whose bytes can look like a permanently busy builder.
    put(f.screen.data(),0xB0,f.tess.data());
    put(f.screen.data(),0xB8,f.old_tess.data());
    put(f.old_tess.data(),0x298,std::uint32_t{15120});
    put(f.old_tess.data(),0x255,std::uint8_t{5});
    put(f.bindings.data(),0x140,std::uintptr_t{1});
    f.map[0]=reinterpret_cast<std::uintptr_t>(f.top.data());
    put(f.matrices.data(),0x48,f.map.data()); put(f.matrices.data(),0x50,std::uintptr_t{1});
    put(f.matrices.data(),0x58,std::uintptr_t{0}); put(f.matrices.data(),0x60,std::uintptr_t{1});
    put(f.actor.data(),0,f.table.data()); put(f.table.data(),0x68,&fake_yaw);
    put(f.tess.data(),0,std::uint8_t{17}); put(f.tess.data(),8,std::uint8_t{7});
    put(f.tess.data(),0x158,std::uint8_t{3}); put(f.tess.data(),0x168,std::uint8_t{4});
    put(f.tess.data(),0x170,std::uint64_t{81}); put(f.tess.data(),0x20C,std::uint32_t{43});
    put(f.tess.data(),0x2A0,std::uint16_t{0x100}); // Indexed flag can remain set on an idle tessellator.
    put(f.tess.data(),0x210,std::uint8_t{1});
    for (unsigned i=0x178;i<0x1E4;++i) f.tess[i]=static_cast<std::byte>(i&255);
    put(f.alpha.data(),0,std::uint64_t{0x0A4F54B100E83415ULL});
    const char* alpha="entity_alphatest"; put(f.alpha.data(),8,alpha);
    put(f.alpha.data(),0x18,std::uint64_t{16}); put(f.alpha.data(),0x20,std::uint64_t{31});
    put(f.entity.data(),0,std::uint64_t{0x03AAE082A9C0D218ULL}); std::memcpy(f.entity.data()+8,"entity",7);
    put(f.entity.data(),0x18,std::uint64_t{6}); put(f.entity.data(),0x20,std::uint64_t{15});
    f.renderer.configure({fake_world,fake_submit,fake_view,fake_begin,fake_vertex,fake_position,fake_riding,fake_yaw,
        fake_height,fake_material,fake_release,&f,f.alpha.data(),f.entity.data(),fake_lower,fake_binding_copy,fake_binding_release});
    // The GPU pair moved from TextureData+8 to +0. A ready decoy at the old
    // offset must not admit a pending texture or transfer its references.
    put(f.texture_data.data(),0,f.gpu.data());
    put(f.texture_data.data(),8,f.old_gpu.data());
    put(f.old_gpu.data(),0x12,std::uint8_t{1});
    f.renderer.set_ready(true);
}
void lifecycle_test() {
    Fixture f; initialize(f);
    const auto saved=f.tess;
    // OFF and calls outside a native world scope never inspect native inputs.
    f.renderer.after_submit(nullptr,reinterpret_cast<void*>(1),nullptr,nullptr,fake_submit);
    { BoxRenderer::WorldScope world(f.renderer,reinterpret_cast<void*>(1),reinterpret_cast<void*>(1)); }
    require(f.pose_reads==0&&f.begins==0,"OFF avoids native getters/state");
    f.renderer.set_options(true,0,.5F);
    f.renderer.after_submit(f.screen.data(),f.tess.data(),f.texture.data(),&f.metadata,fake_submit);
    require(f.begins==0,"No context retained after scope");
    {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data());
        f.renderer.after_submit(f.screen.data(),f.tess.data(),nullptr,&f.metadata,fake_submit);
        require(f.begins==0,"Missing native texture fails without mutation");
        f.renderer.after_submit(f.screen.data(),f.tess.data(),f.texture.data(),&f.metadata,fake_submit);
        f.renderer.after_submit(f.screen.data(),f.tess.data(),f.texture.data(),&f.metadata,fake_submit);
    }
    require(f.submits==1&&f.materials==1&&f.releases==1&&f.pose_reads==1,"One draw per scope, balanced native ownership");
    require(f.tess==saved&&f.top==identity(),"Restore all changed scalar Tessellator state and model matrix");
    require(get<std::uint8_t>(f.matrices.data(),0x78)==1,"Invalidate derived matrices after restoring model matrix");
    require(close(f.submitted[12],1)&&close(f.submitted[13],0)&&close(f.submitted[14],1),"Native interpolated position + riding - camera - height");
    require((f.vertices[0].color&255)==127,"Vertex brightness multiplication");
    auto frame=f.renderer.take_frame();
    require(frame.drawn==1&&frame.failures[static_cast<unsigned>(BoxFailure::texture)]==1,"Diagnostic failure is observable");
    require(f.renderer.take_frame().drawn==0,"Frame counters drain");
    f.renderer.set_options(true,1,1);
    {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data());
        // Nested invalid scopes must not borrow the outer world's context.
        { BoxRenderer::WorldScope nested(f.renderer,nullptr,nullptr);
          f.renderer.after_submit(f.screen.data(),f.tess.data(),f.texture.data(),&f.metadata,fake_submit); }
        f.renderer.after_submit(f.screen.data(),f.tess.data(),f.texture.data(),&f.metadata,fake_submit);
    }
    require(f.submits==2&&f.releases==2,"Native entity key and nested scope restoration");
    {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data());
        // The lower native entry consumes its texture array, so draw before
        // forwarding it, while the first binding remains borrowed and valid.
        f.renderer.before_mesh_submit(f.screen.data()+0x10,f.bindings.data(),&f.metadata,fake_submit);
        require(f.submits==3&&f.tess==saved,"Lower MeshSubmit uses the proven idle ScreenContext tessellator");
    }
    f.renderer.set_ready(false);
    { BoxRenderer::WorldScope world(f.renderer,reinterpret_cast<void*>(1),reinterpret_cast<void*>(1)); }
    require(f.pose_reads==3,"Unready renderer never dereferences native inputs");
}
void retained_binding_test() {
    Fixture f; initialize(f);
    put(f.texture.data(),0x20,std::uint8_t{1});
    f.renderer.set_options(true,0,1);
    // Outside the world scope only a native-owned resource reference is kept.
    f.renderer.after_submit(nullptr,nullptr,f.texture.data(),f.metadata.data(),fake_submit);
    require(f.renderer.texture_ready()&&f.binding_copies==1&&!f.pose_reads,"Retain texture, never actor/context");
    for (unsigned i=0;i<2;++i) {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data());
        f.renderer.finish_world(fake_submit);
    }
    require(f.submits==2&&f.binding_copies==1&&!f.binding_releases,"Direct world draw reuses immutable native texture reference");
    f.renderer.release_resources();
    f.renderer.release_resources();
    require(!f.renderer.texture_ready()&&f.binding_releases==1,"Release resource exactly once after callback drain");
}
void* fake_texture_get(void* group,void* output,const void* location,bool async,std::uint64_t optional,std::uint8_t mode) {
    require(group==live&&async&&!optional&&!mode,"Texture request uses borrowed group and native async branch");
    require(get<std::uint32_t>(location,0)==2,"Raw filesystem resource kind");
    const auto* path=get<const char*>(location,8);
    require(std::string_view(path).ends_with("textures/triangle.png"),"Worker supplies stable absolute PNG path");
    put(output,0,live->texture_data.data());++live->texture_requests;return output;
}
std::uint16_t fake_view_id() { return 42; }
void fake_mesh_end(void* tess,void* mesh,int mode,const void* label,std::uint16_t view) {
    require(tess==live->tess.data()&&live->count==3&&mode==2&&view==42&&get<std::size_t>(label,8)==18,"Native mesh finalization physical ABI");
    put(mesh,0,std::uint64_t{123});++live->mesh_ends;
}
void fake_builder_clear(void* builder) { require(builder==live->tess.data()+8,"Clear only native builder");++live->builder_clears; }
void fake_mesh_destroy(void* mesh) { require(get<std::uint64_t>(mesh,0)==123,"Destroy same opaque native Mesh exactly once");++live->mesh_destroys; }
void fake_mesh_draw(void* mesh,void* context,const void* material,const void* texture,std::uintptr_t a,std::uintptr_t b,const void* metadata) {
    require(get<std::uint64_t>(mesh,0)==123&&context==live->screen.data()+0x10&&!a&&!b,"Cached mesh submission slots");
    require(get<std::uintptr_t>(material,0)==1&&get<std::uint8_t>(texture,0x20)==1&&get<void*>(texture,0)==live->texture_data.data(),"Own PNG binding and native material");
    require(get<std::uint8_t>(metadata,0x20)==0,"Captured native empty metadata");++live->mesh_draws;
}
void static_model_test(bool stock_available) {
    const auto root=model_fixture::create(stock_available?"native":"native-no-stock");medicon::custom_model::ModelRegistry registry;registry.start(root);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (registry.status()==medicon::custom_model::ModelRegistry::Status::loading&&std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    require(static_cast<bool>(registry.snapshot()),"Native fixture CPU model loads");
    Fixture f;initialize(f);f.expected_vertices=3;const auto saved=f.tess;
    f.renderer.configure_static({fake_texture_get,fake_mesh_end,fake_mesh_draw,fake_mesh_destroy,fake_builder_clear,fake_view_id},registry);
    f.renderer.set_static_mode(true,false);f.renderer.set_options(true,0,1);
    put(f.texture.data(),0x20,std::uint8_t{1});
    if (stock_available) f.renderer.after_submit(nullptr,nullptr,f.texture.data(),f.metadata.data(),fake_submit);
    else require(!f.renderer.texture_ready(),"Custom PNG fixture has no stock texture submission");
    f.renderer.observe_texture_group(&f,fake_texture_get);
    require(f.texture_requests==1&&f.renderer.static_progress().textures==0&&f.binding_releases==1,"Pending current GPU releases temporary references even if the former +8 field looks ready");
    put(f.gpu.data(),0x12,std::uint8_t{1});
    const auto texture_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!f.renderer.static_progress().textures&&std::chrono::steady_clock::now()<texture_deadline) f.renderer.observe_texture_group(&f,fake_texture_get);
    require(f.texture_requests==2&&f.renderer.static_progress().textures==1,"Native owned texture retained when GPU ready");
    // First-person warmup must complete without displaying the full model.
    for (unsigned i=0;i<2;++i) {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data(),ViewMode::first_person);f.renderer.finish_world(fake_submit);
        require(!world.drawn(),"Warmup never suppresses the stock model before a custom draw succeeds");
        require(!world.matches_player(1,2),"Missing native identity cannot match a previously captured player");
    }
    require(f.renderer.static_progress().ready&&f.mesh_ends==1&&f.builder_clears==1&&!f.mesh_draws&&f.tess==saved,"Prepare cached mesh without suppressing vanilla or leaving draw state");
    f.renderer.set_static_mode(true,true);
    for (const auto view : {ViewMode::first_person,ViewMode::unknown}) {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data(),view);f.renderer.finish_world(fake_submit);
        require(f.renderer.static_progress().ready&&!f.mesh_draws&&f.tess==saved,"First person / unknown hides full body without losing readiness or mutating state");
    }
    for (const auto view : {ViewMode::third_person,ViewMode::third_person_front,ViewMode::third_person}) {
        BoxRenderer::WorldScope world(f.renderer,f.context.data(),f.actor.data(),view);f.renderer.finish_world(fake_submit);
        require(world.drawn(),"Only a completed custom draw may replace the stock model");
    }
    require(f.mesh_draws==3&&f.mesh_ends==1&&f.begins==1&&f.tess==saved&&f.top==identity(),"Steady frames reuse native mesh and restore state");
    f.renderer.set_options(false,0,1);
    { BoxRenderer::WorldScope world(f.renderer,reinterpret_cast<void*>(1),reinterpret_cast<void*>(1));f.renderer.finish_world(fake_submit); }
    require(f.mesh_draws==3,"OFF never dereferences native context");
    f.renderer.release_resources();f.renderer.release_resources();registry.stop();
    require(f.mesh_destroys==1&&f.binding_releases==(stock_available?3U:2U),"Only references actually acquired are released once after drain");
}
void model_switch_test() {
    namespace cm=medicon::custom_model;
    const auto first=model_fixture::create("switch-first"),second=model_fixture::create("switch-second");
    model_fixture::text(second/L"model.json",R"({"scale":2,"yawOffset":180})");
    cm::ModelRegistry registry;registry.start_catalog({first,second},{});
    const auto wait=[&]{
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(registry.status()==cm::ModelRegistry::Status::loading&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        require(registry.status()==cm::ModelRegistry::Status::ready,"Switch fixture CPU model ready");
    };wait();
    Fixture f;initialize(f);f.expected_vertices=3;
    f.renderer.configure_static({fake_texture_get,fake_mesh_end,fake_mesh_draw,fake_mesh_destroy,fake_builder_clear,fake_view_id},registry);
    f.renderer.set_static_mode(true,true);f.renderer.set_options(true,0,1);
    put(f.texture.data(),0x20,std::uint8_t{1});put(f.gpu.data(),0x12,std::uint8_t{1});
    f.renderer.after_submit(nullptr,nullptr,f.texture.data(),f.metadata.data(),fake_submit);
    f.renderer.observe_texture_group(&f,fake_texture_get);
    const auto world=[&]{BoxRenderer::WorldScope scope(f.renderer,f.context.data(),f.actor.data(),ViewMode::third_person);f.renderer.finish_world(fake_submit);};
    world();world();require(f.renderer.static_progress().ready&&f.mesh_ends==1,"First model native resources ready");
    registry.select(cm::BuiltinModel::chocolat);
    require(!f.renderer.static_progress().ready&&!f.renderer.first_person_ready(),"A model change immediately invalidates readiness for vanilla suppression");wait();
    f.renderer.observe_texture_group(&f,fake_texture_get);
    require(f.texture_requests==1,"Old generation cannot accept a new model's PNG request");
    world();
    require(f.mesh_destroys==1&&f.mesh_ends==2&&!f.renderer.static_progress().ready,"World callback replaces old mesh and restarts texture warmup");
    f.renderer.observe_texture_group(&f,fake_texture_get);world();
    require(f.renderer.static_progress().ready&&f.texture_requests==2,"Selected model prepares its own PNG and becomes drawable");
    f.renderer.release_resources();registry.stop();
    require(f.mesh_destroys==2&&f.binding_releases==3,"Each generation releases its custom handles once and reuses the stock binding");
}
void resource_location_test() {
    const auto location=raw_texture_location("hello");
    require(get<std::uint32_t>(location.data(),0)==2&&get<std::uint64_t>(location.data(),0x18)==5&&get<std::uint64_t>(location.data(),0x20)==15,"Short native input envelope");
    require(std::memcmp(location.data()+8,"hello\0",6)==0&&get<std::uint64_t>(location.data(),0x28)==0xA430D84680AABD0BULL,"Native FNV-1a golden hash");
    require(get<std::uint64_t>(location.data(),0x30)==(((0xCBF29CE484222325ULL^2)*0x100000001B3ULL)^0xA430D84680AABD0BULL),"Native combined resource hash includes filesystem type");
}

void rotations_pitch_test() {
    struct Request { void* actor{}; float pitch{-45}; bool enabled{true}; } request;
    int actor{}, other{};
    request.actor = &actor;
    BoxRenderer renderer;
    renderer.set_render_pitch_provider([](void* target, float& pitch, void* owner) noexcept {
        const auto& current = *static_cast<Request*>(owner);
        if (!current.enabled || current.actor != target) return false;
        pitch = current.pitch;
        return true;
    }, &request);
    require(renderer.render_pitch(&actor, ViewMode::third_person, 12) == -45 &&
            renderer.render_pitch(&actor, ViewMode::third_person_front, 12) == -45,
            "Both third-person views must use Rotations' rendered pitch");
    require(renderer.render_pitch(&actor, ViewMode::first_person, 12) == 12 &&
            renderer.render_pitch(&actor, ViewMode::unknown, 12) == 12 &&
            renderer.render_pitch(&other, ViewMode::third_person, 12) == 12,
            "Rotations changed first-person arms, an unknown view or a different actor");
    request.enabled = false;
    require(renderer.render_pitch(&actor, ViewMode::third_person, 12) == 12,
            "Turning Rotations off left the model pitch override active");
    request.enabled = true;
    request.pitch = std::numeric_limits<float>::quiet_NaN();
    require(renderer.render_pitch(&actor, ViewMode::third_person, 12) == 12,
            "CustomModel accepted a non-finite Rotations pitch");
}
}
int main() {
    try { resolution_test(); geometry_test(); view_reader_test(); lifecycle_test(); retained_binding_test(); resource_location_test(); rotations_pitch_test(); static_model_test(true); static_model_test(false); model_switch_test(); std::cout<<"Model packaging / geometry / native view / ABI / restoration / ownership / character switch passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
