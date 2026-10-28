#include <medicon/custom_model/ModelAsset.hpp>
#include <ufbx.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <tuple>
#include <stdexcept>
namespace medicon::custom_model {
namespace {
using Json=nlohmann::json;
using Bytes=std::vector<std::byte>;
constexpr std::string_view rig_version="medicon-rig-p3-v4-ufbx-0.20.1";
constexpr std::size_t max_bones=512,max_bindings=2048,max_vertices=500000,max_samples=4000000;
Affine convert(const ufbx_matrix& m,float scale) { Affine out;for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)out.v[c*4+r]=static_cast<float>(m.cols[c].v[r])*(c==3?scale:1);return out; }
Transform convert(const ufbx_transform& t,float scale) { return {{float(t.translation.x*scale),float(t.translation.y*scale),float(t.translation.z*scale)},
    {float(t.rotation.x),float(t.rotation.y),float(t.rotation.z),float(t.rotation.w)},{float(t.scale.x),float(t.scale.y),float(t.scale.z)}}; }
void finite(const Transform& t) { for(float f:t.translation)if(!std::isfinite(f)||std::abs(f)>100000)throw std::runtime_error("Invalid bone translation.");
    float length=0;for(float f:t.rotation){if(!std::isfinite(f))throw std::runtime_error("Invalid bone rotation.");length+=f*f;}
    if(std::abs(length-1)>.01F)throw std::runtime_error("Bone quaternion is not normalized.");
    for(float f:t.scale)if(!std::isfinite(f)||std::abs(f)<1e-8F||std::abs(f)>10000)throw std::runtime_error("Unsupported bone scale."); }
int find_bone(const RigAsset& rig,const std::string& name) { for(std::size_t i=0;i<rig.bones.size();++i)if(rig.bones[i].name==name)return static_cast<int>(i);return -1; }
int role(const RigAsset& rig,const Json& config,const char* key,std::initializer_list<const char*> names) {
    if(config.contains("bones")&&config["bones"].contains(key)){const auto name=config["bones"][key].get<std::string>();const int i=find_bone(rig,name);if(i<0)throw std::runtime_error("Configured bone is missing: "+name);return i;}
    for(const auto* name:names){int i=find_bone(rig,name);if(i>=0)return i;const std::string mix=std::string("mixamorig:")+name;i=find_bone(rig,mix);if(i>=0)return i;}return -1;
}
void resolve_roles(RigAsset& r,const Json& config) {
    auto& b=r.roles;
    b.hips=role(r,config,"hips",{"Hips","Splitter","Pelvis"});b.spine=role(r,config,"spine",{"Spine","Spine1"});
    b.neck=role(r,config,"neck",{"Neck"});b.head=role(r,config,"head",{"Head"});
    b.left_arm=role(r,config,"leftArm",{"LeftArm","L_Shoulder"});b.right_arm=role(r,config,"rightArm",{"RightArm","R_Shoulder"});
    b.left_elbow=role(r,config,"leftForeArm",{"LeftForeArm","L_Elbow"});b.right_elbow=role(r,config,"rightForeArm",{"RightForeArm","R_Elbow"});
    b.left_leg=role(r,config,"leftUpLeg",{"LeftUpLeg","L_Hip"});b.right_leg=role(r,config,"rightUpLeg",{"RightUpLeg","R_Hip"});
    b.left_knee=role(r,config,"leftLeg",{"LeftLeg","L_Knee"});b.right_knee=role(r,config,"rightLeg",{"RightLeg","R_Knee"});
    r.upper_mask.resize(r.bones.size());for(std::size_t i=0;i<r.bones.size();++i){int p=static_cast<int>(i);while(p>=0&&p!=b.spine)p=r.bones[p].parent;r.upper_mask[i]=p>=0;}
}
void delta(Transform& t,const Bone& bone,float pitch,float yaw,float roll) {
    const auto q=multiply(multiply(axis_angle({0,1,0},yaw),axis_angle({1,0,0},pitch)),axis_angle({0,0,1},roll));
    const auto w=bone.world_rotation;const Quaternion inv{-w[0],-w[1],-w[2],w[3]};
    t.rotation=multiply(t.rotation,multiply(multiply(inv,q),w));
}
Clip generated_clip(const RigAsset& r,Motion motion,float scale) {
    Clip c;c.name=std::string("generated:")+motion_name(motion);c.frames=31;c.duration=1;c.procedural=true;c.loop=motion!=Motion::attack;
    c.samples.resize(c.frames*r.bones.size());const auto& b=r.roles;
    std::vector<Affine> world(r.bones.size());for(std::size_t i=0;i<r.bones.size();++i){const auto local=matrix(r.bones[i].rest);world[i]=r.bones[i].parent<0?local:multiply(world[r.bones[i].parent],local);}
    for(unsigned f=0;f<c.frames;++f){auto* pose=c.samples.data()+f*r.bones.size();for(std::size_t i=0;i<r.bones.size();++i)pose[i]=r.bones[i].rest;
        const float t=f/30.0F,s=std::sin(t*6.28318530718F),l=std::max(0.0F,s),rr=std::max(0.0F,-s);
        float left_arm=0,right_arm=0,left_leg=0,right_leg=0,left_knee=0,right_knee=0,body_pitch=0,body_yaw=0,height=0;
        switch(motion){
        case Motion::idle:height=std::sin(t*6.28318530718F)*.006F;break;
        case Motion::walk:left_leg=-s*25;right_leg=s*25;left_arm=s*18;right_arm=-s*18;left_knee=l*28;right_knee=rr*28;height=std::abs(s)*.02F;break;
        case Motion::run:left_leg=-s*48;right_leg=s*48;left_arm=s*45;right_arm=-s*45;left_knee=l*65;right_knee=rr*65;body_pitch=8;height=std::abs(s)*.045F;break;
        case Motion::sneak_idle:case Motion::sneak:height=-.25F;body_pitch=20;left_leg=-30;right_leg=-30;left_knee=58;right_knee=58;
            if(motion==Motion::sneak){left_leg-=s*15;right_leg+=s*15;left_arm=s*10;right_arm=-s*10;left_knee+=l*15;right_knee+=rr*15;}break;
        case Motion::fall:left_leg=-15;right_leg=10;left_knee=25;right_knee=15;left_arm=-15;right_arm=-15;break;
        case Motion::swim:body_pitch=80;left_arm=s*65;right_arm=-s*65;left_leg=-s*20;right_leg=s*20;break;
        case Motion::sit:left_leg=-80;right_leg=-80;left_knee=85;right_knee=85;break;
        case Motion::glide:body_pitch=80;left_leg=-8;right_leg=-8;left_arm=-25;right_arm=-25;break;
        case Motion::sleep:body_pitch=90;break;
        case Motion::attack:right_arm=-95+115*std::sin(t*3.14159265359F);body_yaw=-18*std::sin(t*3.14159265359F);break;
        case Motion::use:right_arm=-95;left_arm=-55;break;default:break;}
        const auto apply=[&](int bone,float pitch,float yaw,float roll){if(bone>=0)delta(pose[bone],r.bones[bone],pitch,yaw,roll);};
        apply(b.left_arm,left_arm,0,-75);apply(b.right_arm,right_arm,0,75);
        apply(b.left_elbow,motion==Motion::run?-55:motion==Motion::use?-70:-10,0,0);
        apply(b.right_elbow,motion==Motion::run?-55:motion==Motion::use?-100:motion==Motion::attack?-35:-10,0,0);
        apply(b.left_leg,left_leg,0,0);apply(b.right_leg,right_leg,0,0);apply(b.left_knee,left_knee,0,0);apply(b.right_knee,right_knee,0,0);
        if(motion==Motion::swim||motion==Motion::glide||motion==Motion::sleep)apply(b.hips,body_pitch,0,0);else apply(b.spine,body_pitch,body_yaw,0);
        if(b.hips>=0){const int parent=r.bones[b.hips].parent;auto inv=parent>=0?inverse(world[parent]):Affine{};inv.v[12]=inv.v[13]=inv.v[14]=0;
            const auto local=point(inv,{0,height*scale,0});for(unsigned a=0;a<3;++a)pose[b.hips].translation[a]+=local[a];}
    }return c;
}
void strip_root_motion(const RigAsset& rig,Clip& c) {
    if(rig.roles.hips<0)return;std::vector<Affine> world(rig.bones.size());Float3 origin{};
    for(unsigned f=0;f<c.frames;++f){auto* p=c.samples.data()+f*rig.bones.size();
        for(std::size_t b=0;b<rig.bones.size();++b){const auto local=matrix(p[b]);world[b]=rig.bones[b].parent<0?local:multiply(world[rig.bones[b].parent],local);}
        const auto current=point(world[rig.roles.hips],{});if(f==0)origin=current;
        const Float3 offset{current[0]-origin[0],0,current[2]-origin[2]};const int parent=rig.bones[rig.roles.hips].parent;
        auto inv=parent<0?Affine{}:inverse(world[parent]);inv.v[12]=inv.v[13]=inv.v[14]=0;
        const auto local=point(inv,offset);for(unsigned a=0;a<3;++a)p[rig.roles.hips].translation[a]-=local[a];
    }
}
std::array<int,12> role_array(const Roles& r){return {r.hips,r.spine,r.neck,r.head,r.left_arm,r.right_arm,r.left_elbow,r.right_elbow,r.left_leg,r.right_leg,r.left_knee,r.right_knee};}
Roles to_roles(const std::array<int,12>& a){return {a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11]};}
template<class T> void append(Bytes& b,const std::vector<T>& values){const auto* p=reinterpret_cast<const std::byte*>(values.data());if(!values.empty())b.insert(b.end(),p,p+values.size()*sizeof(T));}
template<class T> void extract(std::span<const std::byte> body,std::size_t& at,std::vector<T>& out,std::size_t count,std::size_t maximum){
    if(count>maximum||at>body.size()||count>(body.size()-at)/sizeof(T))throw std::runtime_error("Invalid rig cache size.");out.resize(count);if(count)std::memcpy(out.data(),body.data()+at,count*sizeof(T));at+=count*sizeof(T);}
void write_cache(const std::filesystem::path& path,const RigAsset& r,const ModelAsset& m){
    Json j{{"version",rig_version},{"fbx",m.fbx_sha256},{"config",m.config_sha256},{"roles",role_array(r.roles)},{"upper",r.upper_mask},
        {"vertices",r.vertices.size()},{"bones",Json::array()},{"bindings",Json::array()},{"parts",Json::array()},{"clips",Json::array()},{"diagnostic",r.diagnostic}};
    for(const auto& b:r.bones)j["bones"].push_back({b.name,b.parent,b.rest.translation,b.rest.rotation,b.rest.scale,b.world_rotation});
    for(const auto& b:r.bindings)j["bindings"].push_back({b.bone,b.inverse_bind.v});
    Bytes body;append(body,r.vertices);for(const auto& p:r.part_indices){j["parts"].push_back(p.size());append(body,p);}
    for(const auto& c:r.clips){j["clips"].push_back({c.name,c.duration,c.frames,c.loop,c.procedural});append(body,c.samples);}
    j["body_sha256"]=sha256(body);const auto header=j.dump();const auto length=static_cast<std::uint32_t>(header.size());
    std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write("MMDLRIG3",8);out.write(reinterpret_cast<const char*>(&length),4);out.write(header.data(),header.size());out.write(reinterpret_cast<const char*>(body.data()),body.size());if(!out)throw std::runtime_error("Cannot write rig cache.");}
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish rig cache.");
}
std::shared_ptr<RigAsset> read_cache(const std::filesystem::path& path,const ModelAsset& m){try{
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>192*1024*1024)return {};
    std::ifstream in(path,std::ios::binary|std::ios::ate);const auto size=static_cast<std::size_t>(in.tellg());if(size<12)return {};in.seekg(0);
    Bytes all(size);if(!in.read(reinterpret_cast<char*>(all.data()),size)||std::memcmp(all.data(),"MMDLRIG3",8))return {};
    std::uint32_t length{};std::memcpy(&length,all.data()+8,4);if(length>4*1024*1024||length>size-12)return {};
    const auto j=Json::parse(reinterpret_cast<const char*>(all.data()+12),reinterpret_cast<const char*>(all.data()+12+length));
    if(j.at("version").get<std::string>()!=rig_version||j.at("fbx")!=m.fbx_sha256||j.at("config")!=m.config_sha256)return {};
    const auto body=std::span<const std::byte>(all).subspan(12+length);if(sha256(body)!=j.at("body_sha256").get<std::string>())return {};
    if(j.at("bones").empty()||j.at("bones").size()>max_bones||j.at("bindings").size()>max_bindings||j.at("clips").size()!=motion_count||j.at("parts").size()!=m.parts.size())return {};
    auto r=std::make_shared<RigAsset>();
    for(const auto& b:j.at("bones")){Bone bone{b.at(0).get<std::string>(),b.at(1).get<int>(),{b.at(2).get<Float3>(),b.at(3).get<Quaternion>(),b.at(4).get<Float3>()},b.at(5).get<Quaternion>()};finite(bone.rest);if(bone.parent<-1||bone.parent>=static_cast<int>(r->bones.size()))return {};r->bones.push_back(std::move(bone));}
    for(const auto& b:j.at("bindings")){Binding binding{b.at(0).get<std::uint32_t>(),Affine{b.at(1).get<std::array<float,16>>()}};if(binding.bone>=r->bones.size())return {};for(float f:binding.inverse_bind.v)if(!std::isfinite(f))return {};r->bindings.push_back(binding);}
    r->roles=to_roles(j.at("roles").get<std::array<int,12>>());for(int i:role_array(r->roles))if(i<-1||i>=static_cast<int>(r->bones.size()))return {};
    r->upper_mask=j.at("upper").get<std::vector<std::uint8_t>>();if(r->upper_mask.size()!=r->bones.size())return {};
    std::size_t at{};extract(body,at,r->vertices,j.at("vertices").get<std::size_t>(),max_vertices);
    for(const auto& v:r->vertices){float sum=0;for(unsigned k=0;k<4;++k){if(v.skin.binding[k]>=r->bindings.size()||!std::isfinite(v.skin.weight[k])||v.skin.weight[k]<0)return {};sum+=v.skin.weight[k];}if(std::abs(sum-1)>.0001F)return {};for(float f:v.position)if(!std::isfinite(f))return {};for(float f:v.normal)if(!std::isfinite(f))return {};}
    for(std::size_t i=0;i<m.parts.size();++i){r->part_indices.emplace_back();const auto count=j.at("parts").at(i).get<std::size_t>();if(count!=m.parts[i].vertices.size())return {};extract(body,at,r->part_indices.back(),count,4095);for(auto index:r->part_indices.back())if(index>=r->vertices.size())return {};}
    std::size_t total{};for(unsigned i=0;i<motion_count;++i){const auto& a=j.at("clips").at(i);auto& c=r->clips[i];c.name=a.at(0).get<std::string>();c.duration=a.at(1).get<float>();c.frames=a.at(2).get<std::uint32_t>();c.loop=a.at(3).get<bool>();c.procedural=a.at(4).get<bool>();
        if(!std::isfinite(c.duration)||c.duration<=0||c.duration>120||c.frames<2||c.frames>3601)return {};const auto n=static_cast<std::size_t>(c.frames)*r->bones.size();if(n>max_samples-total)return {};total+=n;extract(body,at,c.samples,n,max_samples);for(const auto& t:c.samples)finite(t);}
    if(at!=body.size())return {};r->diagnostic=j.value("diagnostic","");r->from_cache=true;return r;
}catch(...){return {};}}
}
void load_animation_asset(const std::filesystem::path& root,std::span<const std::byte> fbx,const std::string& config_text,ModelAsset& model,const void* loaded_scene) {
    const auto publish=[&](std::shared_ptr<RigAsset> rig){
        std::map<std::uint32_t,std::size_t> batches;
        for(std::size_t p=0;p<model.parts.size();++p){const auto& part=model.parts[p];auto found=batches.find(part.texture);std::size_t index{};
            if(found==batches.end()){index=rig->batches.size();rig->batches.push_back({part.texture,{}});batches.emplace(part.texture,index);}else index=found->second;
            auto& out=rig->batches[index].vertices;for(std::size_t v=0;v<part.vertices.size();++v)out.push_back({rig->part_indices[p][v],part.vertices[v].uv,part.vertices[v].color});}
        build_first_person_arms(*rig,model.scale);
        model.rig=rig;model.animation=std::make_shared<AnimationInstance>(*rig);
    };
    const auto cache=std::filesystem::weakly_canonical(root/L"cache"/L"model.rig.mmdl");
    auto root_part=root.begin(),cache_part=cache.begin();for(;root_part!=root.end();++root_part,++cache_part)
        if(cache_part==cache.end()||_wcsicmp(root_part->c_str(),cache_part->c_str()))throw std::runtime_error("Rig cache escapes the model directory.");
    if(auto cached=read_cache(cache,model)){publish(cached);return;}
    std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> owned(nullptr,ufbx_free_scene);auto* scene=static_cast<const ufbx_scene*>(loaded_scene);
    if(!scene){ufbx_load_opts opts{};opts.target_axes=ufbx_axes_right_handed_y_up;opts.target_unit_meters=1;opts.inherit_mode_handling=UFBX_INHERIT_MODE_HANDLING_COMPENSATE;opts.generate_missing_normals=true;opts.evaluate_skinning=true;opts.temp_allocator.memory_limit=256*1024*1024;opts.result_allocator.memory_limit=256*1024*1024;owned.reset(ufbx_load_memory(fbx.data(),fbx.size(),&opts,nullptr));scene=owned.get();}
    if(!scene)throw std::runtime_error("Cannot load FBX skeleton.");if(scene->skin_deformers.count==0)return;
    if(scene->nodes.count>max_bones)throw std::runtime_error("FBX skeleton exceeds 512 nodes.");
    const auto config=Json::parse(config_text);auto rig=std::make_shared<RigAsset>();std::map<const ufbx_node*,std::uint32_t> node_indices;std::vector<const ufbx_node*> nodes;
    const auto add_node=[&](auto&& self,const ufbx_node* node)->void{if(node_indices.contains(node))return;if(node->parent)self(self,node->parent);
        if(node->inherit_mode!=UFBX_INHERIT_MODE_NORMAL)throw std::runtime_error("Unsupported FBX scale inheritance for animation.");
        const auto t=convert(node->local_transform,model.scale);finite(t);const auto w=ufbx_matrix_to_transform(&node->node_to_world);
        node_indices[node]=static_cast<std::uint32_t>(rig->bones.size());nodes.push_back(node);rig->bones.push_back({std::string(node->name.data,node->name.length),node->parent?int(node_indices[node->parent]):-1,t,convert(w,model.scale).rotation});};
    for(auto* node:scene->nodes)add_node(add_node,node);resolve_roles(*rig,config);
    // Avatars can repeat the entire skeleton's clusters on every clothing mesh.
    // Reuse exactly identical bone/inverse-bind pairs before applying the budget.
    std::map<std::pair<std::uint32_t,std::array<float,16>>,std::uint16_t> binding_indices;
    const auto binding_for=[&](std::uint32_t bone,Affine inverse_bind){
        for(float f:inverse_bind.v)if(!std::isfinite(f))throw std::runtime_error("Invalid inverse bind matrix.");
        const auto key=std::pair(bone,inverse_bind.v);
        if(const auto found=binding_indices.find(key);found!=binding_indices.end())return found->second;
        if(rig->bindings.size()>=max_bindings)throw std::runtime_error("FBX exceeds unique skin binding budget.");
        const auto index=static_cast<std::uint16_t>(rig->bindings.size());
        rig->bindings.push_back({bone,inverse_bind});binding_indices.emplace(key,index);return index;
    };
    std::size_t part_index{};
    for(auto* node:scene->nodes){auto* mesh=node->mesh;if(!mesh||!mesh->num_triangles)continue;
        if(mesh->skin_deformers.count>1)throw std::runtime_error("Multiple skin deformers need explicit composition.");
        const auto* skin=mesh->skin_deformers.count?mesh->skin_deformers.data[0]:nullptr;
        const auto inverse_geometry=ufbx_matrix_invert(&node->geometry_to_world);std::vector<std::uint16_t> bindings;
        if(skin)for(auto* cluster:skin->clusters){if(!cluster->bone_node)throw std::runtime_error("Invalid FBX skin cluster.");
            const auto bind=ufbx_matrix_mul(&cluster->geometry_to_bone,&inverse_geometry);bindings.push_back(binding_for(node_indices.at(cluster->bone_node),convert(bind,model.scale)));}
        const auto rigid=binding_for(node_indices.at(node),convert(inverse_geometry,model.scale));
        std::vector<std::uint32_t> repaired_indices;std::vector<ufbx_vec3> repaired_normals;
        const auto bind_normal=[&](std::uint32_t index){
            auto normal=ufbx_get_vertex_vec3(&mesh->vertex_normal,index);
            const double length2=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
            if(std::isfinite(length2)&&length2>1e-24)return normal;
            if(repaired_indices.empty()){
                // Like ufbx's evaluated skinned normals, derive missing/zero
                // bind normals from topology and smoothing groups, off-thread.
                std::vector<ufbx_topo_edge> topology(mesh->num_indices);
                repaired_indices.resize(mesh->num_indices);
                ufbx_compute_topology(mesh,topology.data(),topology.size());
                const auto count=ufbx_generate_normal_mapping(mesh,topology.data(),topology.size(),repaired_indices.data(),repaired_indices.size(),false);
                repaired_normals.resize(count);
                ufbx_compute_normals(mesh,&mesh->vertex_position,repaired_indices.data(),repaired_indices.size(),repaired_normals.data(),repaired_normals.size());
            }
            return repaired_normals.at(repaired_indices.at(index));
        };
        std::map<std::tuple<std::uint32_t,float,float,float>,std::uint32_t> vertex_indices;
        std::map<std::uint32_t,std::vector<std::uint32_t>> materials;std::vector<std::uint32_t> tri(std::max<std::size_t>(3,mesh->max_face_triangles*3));
        const auto normals=ufbx_matrix_for_normals(&node->geometry_to_world);
        for(std::size_t f=0;f<mesh->faces.count;++f){const auto face=mesh->faces.data[f];if(face.num_indices<3)continue;const auto count=ufbx_triangulate_face(tri.data(),tri.size(),mesh,face);auto& indices=materials[mesh->face_material.count?mesh->face_material.data[f]:0];
            for(std::size_t k=0;k<count*3;++k){const auto index=tri[k],logical=mesh->vertex_indices.data[index];const auto normal=bind_normal(index);
                const auto key=std::tuple(logical,float(normal.x),float(normal.y),float(normal.z));auto found=vertex_indices.find(key);std::uint32_t vertex{};
                if(found!=vertex_indices.end())vertex=found->second;else{
                    if(rig->vertices.size()>=max_vertices)throw std::runtime_error("Too many skin vertices.");
                    const auto p=ufbx_transform_position(&node->geometry_to_world,ufbx_get_vertex_vec3(&mesh->vertex_position,index));auto n=ufbx_transform_direction(&normals,normal);const auto length=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);if(!std::isfinite(length)||length<1e-12)throw std::runtime_error("Invalid bind normal in "+std::string(node->name.data,node->name.length)+" at vertex "+std::to_string(index)+".");
                    SkinVertex v{{float(p.x*model.scale),float(p.y*model.scale),float(p.z*model.scale)},{float(n.x/length),float(n.y/length),float(n.z/length)},{{rigid,rigid,rigid,rigid},{1,0,0,0}}};
                    if(skin&&logical<skin->vertices.count){const auto weights=skin->vertices.data[logical];const auto take=std::min(weights.num_weights,4U);float sum=0;
                        if(take){v.skin.weight={};for(unsigned w=0;w<take;++w){const auto weight=skin->weights.data[weights.weight_begin+w];if(weight.cluster_index>=bindings.size()||!std::isfinite(weight.weight)||weight.weight<0)throw std::runtime_error("Invalid skin weight.");v.skin.binding[w]=bindings[weight.cluster_index];v.skin.weight[w]=float(weight.weight);sum+=v.skin.weight[w];}
                            if(sum>1e-12F)for(float& w:v.skin.weight)w/=sum;else v.skin={{rigid,rigid,rigid,rigid},{1,0,0,0}};}}
                    vertex=static_cast<std::uint32_t>(rig->vertices.size());rig->vertices.push_back(v);vertex_indices.emplace(key,vertex);
                }indices.push_back(vertex);
            }
        }
        for(auto& [material,indices]:materials){(void)material;for(std::size_t at=0;at<indices.size();at+=4095){const auto count=std::min<std::size_t>(4095,indices.size()-at);if(part_index>=model.parts.size()||model.parts[part_index].vertices.size()!=count)throw std::runtime_error("Rig/static mesh topology mismatch.");rig->part_indices.emplace_back(indices.begin()+at,indices.begin()+at+count);++part_index;}}
    }
    if(part_index!=model.parts.size()||rig->bindings.size()>max_bindings)throw std::runtime_error("Rig mesh binding mismatch.");
    std::size_t total_samples{};unsigned generated{};
    for(unsigned i=0;i<motion_count;++i){const auto motion=static_cast<Motion>(i);std::string wanted=motion_name(motion);
        if(config.contains("clips")&&config["clips"].contains(wanted))wanted=config["clips"][wanted].get<std::string>();
        const ufbx_anim_stack* stack{};for(auto* s:scene->anim_stacks){std::string name(s->name.data,s->name.length);if(name==wanted||_stricmp(name.c_str(),wanted.c_str())==0){stack=s;break;}}
        auto& clip=rig->clips[i];if(!stack){clip=generated_clip(*rig,motion,model.scale);++generated;}else{
            const double duration=stack->time_end-stack->time_begin;if(!std::isfinite(duration)||duration<=0||duration>120)throw std::runtime_error("Animation duration is invalid.");
            clip.name=wanted;clip.duration=float(duration);clip.frames=static_cast<std::uint32_t>(std::ceil(duration*30))+1;clip.loop=motion!=Motion::attack;
            if(clip.frames*rig->bones.size()>max_samples-total_samples)throw std::runtime_error("Animation exceeds sample budget.");clip.samples.reserve(clip.frames*rig->bones.size());
            for(unsigned f=0;f<clip.frames;++f)for(const auto* node:nodes){auto t=convert(ufbx_evaluate_transform(stack->anim,node,stack->time_begin+std::min(f/30.0,duration)),model.scale);finite(t);clip.samples.push_back(t);}strip_root_motion(*rig,clip);
        }total_samples+=clip.samples.size();if(total_samples>max_samples)throw std::runtime_error("Animation exceeds sample budget.");
    }
    rig->diagnostic="Bones "+std::to_string(rig->bones.size())+", skin vertices "+std::to_string(rig->vertices.size())+", generated clips "+std::to_string(generated)+"/12 (30 Hz; missing FBX clips replaced by basic motion).";
    try{write_cache(cache,*rig,model);}catch(const std::exception&){rig->diagnostic+=" Rig cache write unavailable.";}
    publish(rig);
}
}
