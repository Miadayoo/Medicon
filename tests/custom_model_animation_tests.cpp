#include <medicon/custom_model/ModelAsset.hpp>
#include <medicon/sdk/CustomModelAnimation.hpp>
#include "custom_model_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
std::atomic_size_t allocations{};
void* operator new(std::size_t n){allocations.fetch_add(1);if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p)noexcept{::operator delete(p);}
namespace {
using namespace medicon::custom_model;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool close(float a,float b,float epsilon=2e-4F){return std::abs(a-b)<epsilon;}
template<class T> void put(void* p,std::size_t at,const T& value){std::memcpy(static_cast<std::byte*>(p)+at,&value,sizeof value);}
template<class T> T get(const void* p,std::size_t at){T value{};std::memcpy(&value,static_cast<const std::byte*>(p)+at,sizeof value);return value;}
void simd_test(){
    std::array<SkinMatrix,8> palette{};
    for(unsigned i=0;i<palette.size();++i){Transform t;t.translation={float(i),float(i)/3,-float(i)};t.rotation=axis_angle({1,2,3},i*17.0F);t.scale={1+i*.1F,1+i*.03F,1+i*.2F};palette[i].position=matrix(t);palette[i].normal=normal_matrix(palette[i].position);
        const auto identity=multiply(palette[i].position,inverse(palette[i].position));for(unsigned c=0;c<16;++c)require(close(identity.v[c],c%5==0?1:0),"Affine inverse under nonuniform scale and rotation");}
    std::vector<SkinVertex> input(10000);std::vector<SkinOutput> scalar(input.size()),simd(input.size());
    for(std::size_t i=0;i<input.size();++i){auto& v=input[i];v.position={float(i%31)*.03F,float(i%43)*.04F,float(i%27)*.05F};v.normal={0,1,0};v.skin.binding={std::uint16_t(i%8),std::uint16_t((i+1)%8),std::uint16_t((i+3)%8),std::uint16_t((i+5)%8)};v.skin.weight={.4F,.3F,.2F,.1F};}
    skin_vertices_scalar(input,palette,scalar);skin_vertices(input,palette,simd);
    for(std::size_t i=0;i<input.size();++i)for(unsigned a=0;a<3;++a)require(close(scalar[i].position[a],simd[i].position[a])&&close(scalar[i].normal[a],simd[i].normal[a]),"SIMD agrees with scalar reference for four influences");
    const auto before=allocations.load();const auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<200;++i)skin_vertices(input,palette,simd);
    const double cost=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/200;
    require(allocations.load()==before,"Steady skinning allocates no heap");std::cout<<"10,000 vertices / four influences SIMD: "<<cost<<" ms\n";
}
RigAsset test_rig(){
    RigAsset r;r.bones={{"Hips",-1,{},{}},{"Spine",0,{},{}},{"Leg",0,{},{}}};for(auto& b:r.bones)b.world_rotation={0,0,0,1};
    r.roles.hips=0;r.roles.spine=1;r.upper_mask={0,1,0};r.bindings={{0,{}}};r.vertices={{{0,1,0},{0,1,0},{}}};
    for(unsigned m=0;m<motion_count;++m){auto& c=r.clips[m];c.name=motion_name(static_cast<Motion>(m));c.frames=31;c.duration=1;c.loop=m!=static_cast<unsigned>(Motion::attack);c.samples.resize(31*3);
        for(unsigned f=0;f<31;++f){c.samples[f*3+1].translation[0]=float(m);c.samples[f*3+2].translation[0]=float(m);if(m==static_cast<unsigned>(Motion::attack))c.samples[f*3+1].translation[0]=f/30.0F*100;}}
    return r;
}
void animator_test(){auto rig=test_rig();AnimationInstance instance(rig);AnimationInput in;in.valid=true;in.identity=1;in.world_identity=2;
    instance.update(rig,in,.016F);in.speed=4.3F;instance.update(rig,in,.016F);
    require(instance.motion()==Motion::walk&&instance.fade()<.2F,"Walking enters a bounded 150ms crossfade");
    for(unsigned i=0;i<12;++i)instance.update(rig,in,.016F);require(instance.fade()==1,"Crossfade settles within 0.2 seconds");
    const float leg=instance.pose()[2].translation[0];in.attack_progress=.5F;for(unsigned i=0;i<12;++i)instance.update(rig,in,.016F);
    require(close(instance.pose()[1].translation[0],50)&&close(instance.pose()[2].translation[0],leg),"Attack progress drives clip directly; spine mask preserves lower locomotion");
    in.attack_progress=-1;in.using_item=true;for(unsigned i=0;i<12;++i)instance.update(rig,in,.016F);require(close(instance.pose()[1].translation[0],11),"Use layer replaces upper body");
    in.using_item=false;in.sneaking=true;instance.update(rig,in,.016F);require(instance.motion()==Motion::sneak,"Sneak input has priority over speed");
    in.on_ground=false;instance.update(rig,in,.016F);require(instance.motion()==Motion::fall,"Airborne input selects fall/jump posture");
    in.swimming=true;instance.update(rig,in,.016F);require(instance.motion()==Motion::swim,"Swimming overrides airborne posture");
    in.swimming=false;in.gliding=true;instance.update(rig,in,.016F);require(instance.motion()==Motion::glide,"Gliding state");
    in.gliding=false;in.on_ground=true;in.sneaking=false;in.speed=0;in.identity=99;instance.update(rig,in,.016F);require(instance.motion()==Motion::idle&&instance.fade()==1,"New identity discards prior animation state");
    const auto before=allocations.load();for(unsigned i=0;i<120;++i){in.speed=i%2?5.7F:0;in.attack_progress=(i%30)/30.0F;instance.update(rig,in,1.0F/60);}
    require(allocations.load()==before,"Animator transitions, masks and skinning reuse all buffers");
}
void first_person_test(){
    RigAsset rig;
    rig.bones={{"Hips",-1,{},{}},{"Spine",0,{},{}},{"Head",1,{{0,1.8F,0}},{0,0,0,1}},
        {"LeftArm",1,{{.2F,1.6F,0}},{0,0,0,1}},{"LeftHand",3,{{.5F,0,0}},{0,0,0,1}},
        {"RightArm",1,{{-.2F,1.6F,0}},{0,0,0,1}},{"RightHand",5,{{-.5F,0,0}},{0,0,0,1}}};
    for(auto& bone:rig.bones)bone.world_rotation={0,0,0,1};
    rig.roles.hips=0;rig.roles.spine=1;rig.roles.head=2;rig.roles.left_arm=3;rig.roles.right_arm=5;
    rig.upper_mask.assign(rig.bones.size(),0);
    std::vector<Affine> world(rig.bones.size());
    for(std::size_t b=0;b<rig.bones.size();++b){const auto local=matrix(rig.bones[b].rest);world[b]=rig.bones[b].parent<0?local:multiply(world[rig.bones[b].parent],local);}
    for(const unsigned bone:{0U,3U,4U,5U,6U,2U})rig.bindings.push_back({bone,inverse(world[bone])});
    AnimationBatch batch;batch.texture=2;
    for(unsigned triangle=0;triangle<3;++triangle)for(unsigned corner=0;corner<3;++corner){
        SkinVertex vertex;vertex.position={triangle==0?.6F:triangle==1?-.6F:0,(triangle==2?1.8F:1.6F)+(corner==1?.02F:0),corner==2?.02F:0};vertex.normal={1,0,0};
        vertex.skin.binding={std::uint16_t(triangle==0?2:triangle==1?4:5),0,0,0};vertex.skin.weight=triangle==0?std::array<float,4>{.8F,.2F,0,0}:std::array<float,4>{1,0,0,0};
        rig.vertices.push_back(vertex);batch.vertices.push_back({static_cast<std::uint32_t>(rig.vertices.size()-1),{float(corner),float(triangle)},0xFFFFFFFF});
    }
    batch.vertices.insert(batch.vertices.end(),{batch.vertices[0],batch.vertices[3],batch.vertices[6]}); // Mixed arm/head triangle.
    rig.batches.push_back(batch);
    for(unsigned m=0;m<motion_count;++m){auto& clip=rig.clips[m];clip.frames=2;clip.duration=1;
        for(unsigned f=0;f<2;++f)for(const auto& bone:rig.bones)clip.samples.push_back(bone.rest);}
    build_first_person_arms(rig,1);
    require(rig.first_person.ready()&&rig.first_person.vertices.size()==6&&rig.first_person.batches.size()==1&&rig.first_person.batches[0].vertices.size()==6,"Arm-only whole triangles exclude head and mixed arm/head geometry");
    require(rig.first_person.batches[0].texture==2&&rig.first_person.batches[0].vertices[2].uv==batch.vertices[2].uv,"Arm mesh keeps original texture and UVs");
    for(const auto& vertex:rig.first_person.vertices){float sum=0;
        for(unsigned k=0;k<4;++k){sum+=vertex.skin.weight[k];if(vertex.skin.weight[k]>0)require(rig.first_person.binding_sides[vertex.skin.binding[k]]!=0,"Non-arm influences removed from view mesh");}
        require(close(sum,1),"Arm weights renormalized after removing torso influences");}
    AnimationInstance instance(rig);AnimationInput input;input.valid=true;input.identity=42;
    instance.update_first_person(rig,input,1.0F/60,false);
    const auto idle=std::vector<SkinOutput>(instance.first_person_output().begin(),instance.first_person_output().end());
    for(const auto& vertex:idle)require(vertex.position[1]<0&&vertex.position[2]>.05F,"Custom arms are placed below and in front of the camera");
    require(idle[0].position[0]>0&&idle[3].position[0]<0,"Left/right arms remain on their corresponding screen sides");
    input.attack_progress=.5F;instance.update_first_person(rig,input,1.0F/60,true);
    require(instance.motion()==Motion::attack&&!close(instance.first_person_output()[3].position[2],idle[3].position[2]),"Native swing phase animates custom first-person arm");
    input.attack_progress=-1;input.using_item=true;instance.update_first_person(rig,input,1.0F/60,true);
    require(instance.motion()==Motion::use,"Use state affects first-person custom arms");
    instance.update(rig,input,1.0F/60);
    require(instance.output().size()==rig.vertices.size(),"Third-person switch restores full-model animation output");
    const auto before=allocations.load();
    for(unsigned i=0;i<120;++i){instance.update_first_person(rig,input,1.0F/60,i%2);instance.update(rig,input,1.0F/60);}
    require(allocations.load()==before,"First/third-person switching and arm skinning reuse worker-allocated buffers");
    auto missing=test_rig();build_first_person_arms(missing,1);require(!missing.first_person.ready(),"Missing arm mapping preserves native fallback instead of showing full body");
}
void bulk_test(){
    std::array<std::byte,0x2B0> tess{};std::array<Float3,3> positions{};std::array<std::array<float,4>,3> normals{};std::array<std::uint32_t,3> colors{};std::array<std::array<float,2>,3> uvs{};
    const auto vector=[&](std::size_t offset,auto& values){const auto begin=reinterpret_cast<std::uintptr_t>(values.data());put(tess.data(),offset,std::array<std::uintptr_t,3>{begin,begin+sizeof(values[0]),begin+sizeof(values)});};
    vector(0x10,positions);vector(0x28,normals);vector(0x70,colors);vector(0xA0,uvs);
    std::array<SkinOutput,3> output{{{{1,2,3},{0,1,0}},{{-1,5,2},{1,0,0}},{{3,0,-4},{0,0,1}}}};
    AnimationBatch batch;batch.vertices={AnimatedVertex{2,{.1F,.2F},0xFFFFFFFF},AnimatedVertex{0,{.3F,.4F},0xFFFFFFFF},AnimatedVertex{1,{.5F,.6F},0xFFFFFFFF}};
    require(medicon::sdk::custom_model::fill_animated_tessellator(tess.data(),output,batch,true),"Fill native-reserved vectors in one batch");
    require(positions[0]==output[2].position&&normals[1][1]==1&&colors[2]==0xFF6060FF&&uvs[2]==batch.vertices[2].uv,"Bulk positions/normals/UV/hurt color match indices");
    require(get<std::uint32_t>(tess.data(),0x298)==3&&get<Float3>(tess.data(),0x130)==Float3{-1,0,-4}&&get<Float3>(tess.data(),0x13C)==Float3{3,5,3},"Bulk native vertex count and animated bounds");
    vector(0x10,positions);put(tess.data(),0x20,reinterpret_cast<std::uintptr_t>(positions.data())+12);const auto saved=tess;
    require(!medicon::sdk::custom_model::fill_animated_tessellator(tess.data(),output,batch,false)&&saved==tess,"Insufficient native capacity rejects without partial state mutation");
}
void input_test(){
    struct Pool{std::array<std::byte,0x80> storage{};std::array<std::uint32_t,2048> sparse{};std::array<std::uint32_t*,1> sparse_pages{};std::array<std::uint32_t,1> packed{7};std::array<std::byte,128*36> data{};std::array<void*,1> pages{};};
    std::array<Pool,7> pools{};std::array<std::byte,7*32> nodes{};std::array<std::byte,0x80> registry{};std::array<std::byte,0x500> actor{};
    constexpr std::array<std::uint32_t,7> hashes{0xC67426F3,0x1B5D5238,0x75DF36B7,0xBABE7211,0x18F957AF,0xD7A3585C,0xC29078A0};
    const auto native_vector=[&](void* object,std::size_t offset,auto* first,std::size_t bytes){const auto address=reinterpret_cast<std::uintptr_t>(first);put(object,offset,std::array<std::uintptr_t,3>{address,address+bytes,address+bytes});};
    for(unsigned i=0;i<pools.size();++i){auto& p=pools[i];p.sparse.fill(0xFFFFFFFF);p.sparse[7]=0;p.sparse_pages[0]=p.sparse.data();p.pages[0]=p.data.data();
        native_vector(p.storage.data(),8,p.sparse_pages.data(),8);native_vector(p.storage.data(),0x20,p.packed.data(),4);native_vector(p.storage.data(),0x50,p.pages.data(),8);
        put(nodes.data(),i*32+8,hashes[i]);put(nodes.data(),i*32+16,p.storage.data());}
    native_vector(registry.data(),0x68,nodes.data(),nodes.size());put(actor.data(),0x10,registry.data());put(actor.data(),0x18,std::uint32_t{7});
    put(pools[0].data.data(),0,std::array<std::uint64_t,2>{1ULL<<3,0});
    put(pools[1].data.data(),0,std::array<float,9>{100,50,200,99,50,200,.2F,.1F,0});
    put(pools[2].data.data(),0,std::array<float,4>{30,40,10,20});put(pools[3].data.data(),0,std::array<float,2>{30,10});
    put(pools[4].data.data(),0,std::uint64_t{42});put(pools[5].data.data(),0,int{5});
    const auto before=actor;const auto input=medicon::sdk::custom_model::read_animation_input(actor.data(),.5F,15);
    require(input.valid&&input.identity==42&&input.sprinting&&input.on_ground&&input.hurt&&!input.dead,"Read current EnTT animation state without native mutations");
    require(close(input.speed,4)&&close(input.vertical_speed,2)&&close(input.head_yaw,-5)&&close(input.pitch,20),"Render alpha, head/body delta and native tick velocity units");
    require(actor==before,"Animation input reader is read-only");put(actor.data(),0x18,std::uint32_t{8});
    require(!medicon::sdk::custom_model::read_animation_input(actor.data(),.5F,15).valid,"Stale entity identity falls back without accepting old components");
}
void rig_import_test(){
    const auto root=model_fixture::create("rig");std::ifstream in(root/L"model.fbx",std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});in.close();
    const auto objects=text.find("Objects: {")+std::strlen("Objects: {");
    const std::string identity="1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1";
    std::string extra="\n Model: 600, \"Model::Hips\", \"LimbNode\" { Version: 232 }\n Deformer: 700, \"Deformer::Skin\", \"Skin\" { Version: 101 }\n";
    for(unsigned b=0;b<5;++b){extra+=" Model: "+std::to_string(610+b)+", \"Model::Bone"+std::to_string(b)+"\", \"LimbNode\" { Version: 232 }\n";
        extra+=" Deformer: "+std::to_string(710+b)+", \"SubDeformer::Bone"+std::to_string(b)+"\", \"Cluster\" { Version: 100\n Indexes: *3 { a: 0,1,2 }\n Weights: *3 { a: "+std::to_string(5-b)+","+std::to_string(5-b)+","+std::to_string(5-b)+" }\n Transform: *16 { a: "+identity+" }\n TransformLink: *16 { a: "+identity+" }\n }\n";}
    text.insert(objects,extra);const auto connections=text.find("Connections: {")+std::strlen("Connections: {");extra="\n C: \"OO\",600,0\n C: \"OO\",700,100\n";
    for(unsigned b=0;b<5;++b)extra+=" C: \"OO\","+std::to_string(610+b)+",600\n C: \"OO\","+std::to_string(710+b)+",700\n C: \"OO\","+std::to_string(610+b)+","+std::to_string(710+b)+"\n";
    text.insert(connections,extra);model_fixture::text(root/L"model.fbx",text);std::string diagnostic;auto model=load_model(root,diagnostic);if(!model)throw std::runtime_error(diagnostic);
    require(model->rig&&model->rig->vertices.size()==3&&model->rig->clips[0].frames==31,"Rig extraction and 30Hz generated clips");
    const auto& weights=model->rig->vertices[0].skin.weight;require(close(weights[0],5.0F/14)&&close(weights[3],2.0F/14),"Keep strongest four influences and renormalize after dropping fifth");
    model=load_model(root,diagnostic);require(model&&model->rig&&model->rig->from_cache,"Rig binary cache restores skeleton, weights and clips");
    model_fixture::text(root/L"cache"/L"model.rig.mmdl","broken");model=load_model(root,diagnostic);require(model&&model->rig&&!model->rig->from_cache,"Corrupt rig cache rebuilds without losing static geometry");
    // An authored translation track must be sampled, then stripped of its
    // horizontal root motion instead of moving the rendered actor a second time.
    text.insert(text.find("Objects: {")+std::strlen("Objects: {"),R"FBX(
 AnimationStack: 800, "AnimStack::walk", "" {
  Properties70: {
   P: "LocalStart", "KTime", "Time", "",0
   P: "LocalStop", "KTime", "Time", "",46186158000
   P: "ReferenceStart", "KTime", "Time", "",0
   P: "ReferenceStop", "KTime", "Time", "",46186158000
  }
 }
 AnimationLayer: 801, "AnimLayer::BaseLayer", "" { }
 AnimationCurveNode: 802, "AnimCurveNode::T", "" {
  Properties70: {
   P: "d|X", "Number", "", "A",0
   P: "d|Y", "Number", "", "A",0
   P: "d|Z", "Number", "", "A",0
  }
 }
 AnimationCurve: 803, "AnimCurve::TX", "" {
  Default: 0
  KeyVer: 4008
  KeyTime: *2 { a: 0,46186158000 }
  KeyValueFloat: *2 { a: 0,3 }
  KeyAttrFlags: *2 { a: 24836,24836 }
  KeyAttrDataFloat: *8 { a: 0,0,0,0,0,0,0,0 }
  KeyAttrRefCount: *2 { a: 1,1 }
 }
)FBX");
    text.insert(text.find("Connections: {")+std::strlen("Connections: {"),R"FBX(
 C: "OO",801,800
 C: "OO",802,801
 C: "OP",802,600,"Lcl Translation"
 C: "OP",803,802,"d|X"
)FBX");
    model_fixture::text(root/L"model.fbx",text);model=load_model(root,diagnostic);if(!model)throw std::runtime_error(diagnostic);
    const auto& clip=model->rig->clips[static_cast<unsigned>(Motion::walk)];require(!clip.procedural&&clip.frames==31,"Authored FBX clip baked at 30Hz");
    const auto hips=static_cast<std::size_t>(model->rig->roles.hips),bones=model->rig->bones.size();
    require(close(clip.samples[hips].translation[0],clip.samples[30*bones+hips].translation[0]),"Remove authored horizontal hip root motion");
}
}
int main(){try{simd_test();animator_test();first_person_test();bulk_test();input_test();rig_import_test();std::cout<<"P3 LBS, custom arms, state/layers, allocation, bulk bridge and rig cache passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
