#include <medicon/custom_model/Animation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace medicon::custom_model {
namespace {
void arm_rotation(Transform& pose,Quaternion rest_world,float pitch,float yaw,float roll) noexcept {
    const Quaternion inv{-rest_world[0],-rest_world[1],-rest_world[2],rest_world[3]};
    const auto delta=multiply(multiply(axis_angle({0,1,0},yaw),axis_angle({1,0,0},pitch)),axis_angle({0,0,1},roll));
    pose.rotation=multiply(pose.rotation,multiply(multiply(inv,delta),rest_world));
}
}

void build_first_person_arms(RigAsset& rig,float scale) {
    rig.first_person={};
    const int left=rig.roles.left_arm,right=rig.roles.right_arm;
    if(left<0||right<0||left==right||static_cast<std::size_t>(left)>=rig.bones.size()||
        static_cast<std::size_t>(right)>=rig.bones.size()||!std::isfinite(scale)||scale<=0)return;
    auto& arms=rig.first_person;arms.scale=scale;
    arms.binding_sides.resize(rig.bindings.size());
    for(std::size_t b=0;b<rig.bindings.size();++b){
        int bone=static_cast<int>(rig.bindings[b].bone);
        for(std::size_t depth=0;depth<rig.bones.size()&&bone>=0&&static_cast<std::size_t>(bone)<rig.bones.size();++depth){
            if(bone==left){arms.binding_sides[b]=1;break;}
            if(bone==right){arms.binding_sides[b]=2;break;}
            bone=rig.bones[bone].parent;
        }
    }
    std::vector<Affine> world(rig.bones.size());
    for(std::size_t b=0;b<rig.bones.size();++b){const auto local=matrix(rig.bones[b].rest);
        world[b]=rig.bones[b].parent<0?local:multiply(world[rig.bones[b].parent],local);}
    for(unsigned side=0;side<2;++side){const auto root=point(world[side?right:left],{});
        for(unsigned a=0;a<3;++a)arms.root_to_origin[side].v[12+a]=-root[a];}

    std::vector<std::uint8_t> sides(rig.vertices.size());
    for(std::size_t v=0;v<rig.vertices.size();++v){float weights[2]{};
        for(unsigned k=0;k<4;++k){const auto& skin=rig.vertices[v].skin;
            if(skin.binding[k]>=arms.binding_sides.size())continue;
            if(const auto side=arms.binding_sides[skin.binding[k]])weights[side-1]+=skin.weight[k];}
        // Keep only an arm-dominated vertex. Head/torso/legs cannot enter the
        // view mesh through a shared body material or a low-weight influence.
        if(weights[0]>=.7F&&weights[0]>weights[1])sides[v]=1;
        else if(weights[1]>=.7F&&weights[1]>weights[0])sides[v]=2;
    }
    std::vector<std::uint32_t> remap(rig.vertices.size(),std::numeric_limits<std::uint32_t>::max());
    for(const auto& batch:rig.batches){AnimationBatch out;out.texture=batch.texture;
        for(std::size_t t=0;t+2<batch.vertices.size();t+=3){const auto a=batch.vertices[t].skin_index,b=batch.vertices[t+1].skin_index,c=batch.vertices[t+2].skin_index;
            if(a>=sides.size()||b>=sides.size()||c>=sides.size()||!sides[a]||sides[a]!=sides[b]||sides[a]!=sides[c])continue;
            for(unsigned corner=0;corner<3;++corner){auto vertex=batch.vertices[t+corner];auto& index=remap[vertex.skin_index];
                if(index==std::numeric_limits<std::uint32_t>::max()){
                    auto skin=rig.vertices[vertex.skin_index];float sum{};
                    for(unsigned k=0;k<4;++k){if(skin.skin.binding[k]>=arms.binding_sides.size()||arms.binding_sides[skin.skin.binding[k]]!=sides[vertex.skin_index])skin.skin.weight[k]=0;sum+=skin.skin.weight[k];}
                    for(float& weight:skin.skin.weight)weight/=sum;
                    index=static_cast<std::uint32_t>(arms.vertices.size());arms.vertices.push_back(skin);
                }
                vertex.skin_index=index;out.vertices.push_back(vertex);
            }
        }
        if(!out.vertices.empty())arms.batches.push_back(std::move(out));
    }
}

void AnimationInstance::update_first_person(const RigAsset& rig,const AnimationInput& input,float seconds,bool animate) noexcept {
    const auto& arms=rig.first_person;
    if(!arms.ready()||first_person_output_.size()!=arms.vertices.size())return;
    if(!initialized_||!first_person_||identity_!=input.identity||world_identity_!=input.world_identity){
        reset();initialized_=true;first_person_=true;identity_=input.identity;world_identity_=input.world_identity;
    }
    const float dt=std::isfinite(seconds)?std::clamp(seconds,0.0F,.1F):0;
    if(animate)time_+=dt;
    const bool attack=animate&&input.attack_progress>0&&input.attack_progress<1;
    const bool use=animate&&input.using_item;
    const float swing=attack?std::sin(input.attack_progress*3.14159265358979323846F):0;
    motion_=attack?Motion::attack:use?Motion::use:Motion::idle;
    for(std::size_t b=0;b<rig.bones.size();++b)pose_[b]=rig.bones[b].rest;
    arm_rotation(pose_[rig.roles.left_arm],rig.bones[rig.roles.left_arm].world_rotation,-80,0,-78);
    arm_rotation(pose_[rig.roles.right_arm],rig.bones[rig.roles.right_arm].world_rotation,use?-88:-80-24*swing,16*swing,use?90:78);
    for(std::size_t b=0;b<rig.bones.size();++b){const auto local=matrix(pose_[b]);
        world_[b]=rig.bones[b].parent<0?local:multiply(world_[rig.bones[b].parent],local);}
    const float bob=animate?std::sin(time_*6.28318530718F)*.004F:0;
    std::array<Affine,2> placement;
    for(unsigned side=0;side<2;++side){Transform view;
        // View-model layout in metres, relative to the camera. Keep its size
        // stable when model.json changes the third-person character scale.
        view.translation={side?-.22F:.22F,-.20F+bob,.08F};
        view.scale.fill(.8F/arms.scale);
        placement[side]=multiply(matrix(view),arms.root_to_origin[side]);
    }
    for(std::size_t b=0;b<rig.bindings.size();++b){const auto side=arms.binding_sides[b];
        if(!side)continue;
        palette_[b].position=multiply(multiply(placement[side-1],world_[rig.bindings[b].bone]),rig.bindings[b].inverse_bind);
        palette_[b].normal=normal_matrix(palette_[b].position);
    }
    skin_vertices(arms.vertices,palette_,first_person_output_);
}

} // namespace medicon::custom_model
