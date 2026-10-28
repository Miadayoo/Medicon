#include <medicon/custom_model/Animation.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <emmintrin.h>
namespace medicon::custom_model {
namespace {
constexpr float pi=3.14159265358979323846F;
float dot(Quaternion a,Quaternion b) noexcept { float n=0;for(unsigned i=0;i<4;++i)n+=a[i]*b[i];return n; }
Quaternion normalized(Quaternion q) noexcept { const float n=std::sqrt(dot(q,q));if(n<1e-12F)return {0,0,0,1};for(auto&f:q)f/=n;return q; }
Float3 normalized(Float3 v) noexcept { const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n>1e-12F)for(auto&f:v)f/=n;return v; }
Transform sample(const Clip& c,std::size_t bones,std::size_t bone,float phase) noexcept {
    if(!c.frames||c.samples.size()!=c.frames*bones)return {};
    if(c.loop)phase-=std::floor(phase);else phase=std::clamp(phase,0.0F,1.0F);
    const float frame=phase*(c.frames-1);const auto a=static_cast<std::size_t>(frame),b=std::min(a+1,static_cast<std::size_t>(c.frames-1));
    return blend(c.samples[a*bones+bone],c.samples[b*bones+bone],frame-a);
}
void head_rotation(Transform& t,Quaternion world,float yaw,float pitch,float weight) noexcept {
    const Quaternion conjugate{-world[0],-world[1],-world[2],world[3]};
    const auto delta=multiply(axis_angle({0,1,0},yaw*weight),axis_angle({1,0,0},pitch*weight));
    t.rotation=normalized(multiply(t.rotation,multiply(multiply(conjugate,delta),world)));
}
}
Quaternion multiply(Quaternion a,Quaternion b) noexcept { return {
 a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
 a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]}; }
Quaternion axis_angle(Float3 axis,float degrees) noexcept { axis=normalized(axis);const float s=std::sin(degrees*pi/360);return {axis[0]*s,axis[1]*s,axis[2]*s,std::cos(degrees*pi/360)}; }
Quaternion slerp(Quaternion a,Quaternion b,float t) noexcept {
    float d=dot(a,b);if(d<0){for(auto&f:b)f=-f;d=-d;}
    float x=1-t,y=t;
    if(d<.9995F){const float angle=std::acos(std::clamp(d,-1.0F,1.0F)),s=std::sin(angle);x=std::sin((1-t)*angle)/s;y=std::sin(t*angle)/s;}
    Quaternion q{};for(unsigned i=0;i<4;++i)q[i]=a[i]*x+b[i]*y;return normalized(q);
}
Float3 rotate(Quaternion q,Float3 v) noexcept { const auto r=multiply(multiply(q,{v[0],v[1],v[2],0}),{-q[0],-q[1],-q[2],q[3]});return {r[0],r[1],r[2]}; }
Affine matrix(const Transform& t) noexcept {
    Affine a;for(unsigned c=0;c<3;++c){Float3 axis{};axis[c]=t.scale[c];const auto v=rotate(t.rotation,axis);for(unsigned r=0;r<3;++r)a.v[c*4+r]=v[r];}
    for(unsigned r=0;r<3;++r)a.v[12+r]=t.translation[r];return a;
}
Affine multiply(const Affine& a,const Affine& b) noexcept {
    Affine out;for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r){out.v[c*4+r]=0;for(unsigned k=0;k<4;++k)out.v[c*4+r]+=a.v[k*4+r]*b.v[c*4+k];}return out;
}
Affine normal_matrix(const Affine& a) noexcept {
    const auto& m=a.v;const float det=m[0]*(m[5]*m[10]-m[9]*m[6])-m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);
    if(!std::isfinite(det)||std::abs(det)<1e-15F)return {};
    Affine n;const float d=1/det;
    n.v[0]=(m[5]*m[10]-m[9]*m[6])*d;n.v[1]=(m[8]*m[6]-m[4]*m[10])*d;n.v[2]=(m[4]*m[9]-m[8]*m[5])*d;
    n.v[4]=(m[9]*m[2]-m[1]*m[10])*d;n.v[5]=(m[0]*m[10]-m[8]*m[2])*d;n.v[6]=(m[8]*m[1]-m[0]*m[9])*d;
    n.v[8]=(m[1]*m[6]-m[5]*m[2])*d;n.v[9]=(m[4]*m[2]-m[0]*m[6])*d;n.v[10]=(m[0]*m[5]-m[4]*m[1])*d;
    n.v[12]=n.v[13]=n.v[14]=0;return n;
}
Affine inverse(const Affine& a) {
    auto n=normal_matrix(a);Affine out;
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)out.v[c*4+r]=n.v[r*4+c];
    for(unsigned r=0;r<3;++r)out.v[12+r]=-(out.v[r]*a.v[12]+out.v[4+r]*a.v[13]+out.v[8+r]*a.v[14]);
    return out;
}
Float3 point(const Affine& a,Float3 p) noexcept { Float3 out{};for(unsigned r=0;r<3;++r)out[r]=a.v[r]*p[0]+a.v[4+r]*p[1]+a.v[8+r]*p[2]+a.v[12+r];return out; }
Transform blend(const Transform& a,const Transform& b,float t) noexcept { Transform out;for(unsigned i=0;i<3;++i){out.translation[i]=a.translation[i]+(b.translation[i]-a.translation[i])*t;out.scale[i]=a.scale[i]+(b.scale[i]-a.scale[i])*t;}out.rotation=slerp(a.rotation,b.rotation,t);return out; }
const char* motion_name(Motion m) noexcept { constexpr const char* names[]{"idle","walk","run","sneakIdle","sneak","fall","swim","sit","glide","sleep","attack","use"};const auto i=static_cast<unsigned>(m);return i<motion_count?names[i]:"unknown"; }
void sample_clip(const Clip& c,std::size_t bones,float phase,std::span<Transform> output) noexcept { for(std::size_t i=0;i<bones&&i<output.size();++i)output[i]=sample(c,bones,i,phase); }
AnimationInstance::AnimationInstance(const RigAsset& r) : pose_(r.bones.size()),from_(r.bones.size()),target_(r.bones.size()),upper_pose_(r.bones.size()),upper_from_(r.bones.size()),world_(r.bones.size()),palette_(r.bindings.size()),output_(r.vertices.size()),first_person_output_(r.first_person.vertices.size()) {}
void AnimationInstance::reset() noexcept { initialized_=first_person_=false;time_=gait_=upper_weight_=layer_phase_=death_elapsed_=0;fade_=layer_fade_=1;motion_=Motion::idle;layer_motion_=Motion::use; }
void AnimationInstance::update(const RigAsset& r,const AnimationInput& in,float dt) noexcept {
    dt=std::isfinite(dt)?std::clamp(dt,0.0F,.1F):0;
    if(!initialized_||first_person_||identity_!=in.identity||world_identity_!=in.world_identity){reset();identity_=in.identity;world_identity_=in.world_identity;initialized_=true;for(std::size_t b=0;b<r.bones.size();++b)pose_[b]=from_[b]=target_[b]=upper_pose_[b]=upper_from_[b]=r.bones[b].rest;}
    Motion next=Motion::idle;
    if(in.sleeping)next=Motion::sleep;else if(in.gliding)next=Motion::glide;else if(in.swimming)next=Motion::swim;else if(in.riding)next=Motion::sit;
    else if(!in.on_ground)next=Motion::fall;else if(in.sneaking)next=in.speed>.08F?Motion::sneak:Motion::sneak_idle;
    else if(in.speed>.08F)next=in.sprinting||in.speed>5?Motion::run:Motion::walk;
    if(next!=motion_){std::copy(target_.begin(),target_.end(),from_.begin());motion_=next;fade_=0;}
    time_+=dt;gait_=std::fmod(gait_+std::clamp(in.speed,0.0F,20.0F)*dt/(motion_==Motion::run?2.2F:1.6F),1.0F);fade_=std::min(1.0F,fade_+dt/.15F);
    const auto& clip=r.clips[static_cast<unsigned>(motion_)];
    const bool moving=motion_==Motion::walk||motion_==Motion::run||motion_==Motion::sneak;
    sample_clip(clip,r.bones.size(),moving?gait_:time_/clip.duration,pose_);
    const float run=std::clamp((in.speed-4.0F)/1.7F,0.0F,1.0F);
    for(std::size_t b=0;b<r.bones.size();++b){
        if(motion_==Motion::walk||motion_==Motion::run)pose_[b]=blend(sample(r.clips[1],r.bones.size(),b,gait_),sample(r.clips[2],r.bones.size(),b,gait_),in.sprinting?std::max(.65F,run):run);
        if(fade_<1)pose_[b]=blend(from_[b],pose_[b],fade_*fade_*(3-2*fade_));
        target_[b]=pose_[b];
    }
    const bool attack=std::isfinite(in.attack_progress)&&in.attack_progress>0&&in.attack_progress<1;
    const bool upper=(attack||in.using_item)&&!in.dead;
    upper_weight_=std::clamp(upper_weight_+(upper?1:-1)*dt/.15F,0.0F,1.0F);
    if(upper){const auto next_layer=attack?Motion::attack:Motion::use;
        if(next_layer!=layer_motion_){layer_motion_=next_layer;layer_fade_=0;std::copy(upper_pose_.begin(),upper_pose_.end(),upper_from_.begin());}
        layer_phase_=attack?in.attack_progress:time_/r.clips[static_cast<unsigned>(layer_motion_)].duration;
        layer_fade_=std::min(1.0F,layer_fade_+dt/.15F);
        const auto& layer=r.clips[static_cast<unsigned>(layer_motion_)];for(std::size_t b=0;b<r.bones.size();++b){const auto t=sample(layer,r.bones.size(),b,layer_phase_);upper_pose_[b]=layer_fade_<1?blend(upper_from_[b],t,layer_fade_):t;}}
    if(upper_weight_>0)for(std::size_t b=0;b<r.bones.size();++b)if(r.upper_mask[b])pose_[b]=blend(pose_[b],upper_pose_[b],upper_weight_);
    const float yaw=std::clamp(in.head_yaw,-65.0F,65.0F),pitch=std::clamp(in.pitch,-50.0F,50.0F);
    if(!in.dead&&r.roles.neck>=0)head_rotation(pose_[r.roles.neck],r.bones[r.roles.neck].world_rotation,yaw,pitch,r.roles.head>=0?.35F:1);
    if(!in.dead&&r.roles.head>=0)head_rotation(pose_[r.roles.head],r.bones[r.roles.head].world_rotation,yaw,pitch,r.roles.neck>=0?.65F:1);
    death_elapsed_=in.dead?death_elapsed_+dt:0;
    if(in.dead&&r.roles.hips>=0){const auto w=r.bones[r.roles.hips].world_rotation;const Quaternion inv{-w[0],-w[1],-w[2],w[3]};
        const float progress=std::clamp(std::max(in.death_seconds,death_elapsed_)/.5F,0.0F,1.0F);
        pose_[r.roles.hips].rotation=multiply(pose_[r.roles.hips].rotation,multiply(multiply(inv,axis_angle({0,0,1},80*progress)),w));}
    for(std::size_t b=0;b<r.bones.size();++b){const auto local=matrix(pose_[b]);world_[b]=r.bones[b].parent<0?local:multiply(world_[r.bones[b].parent],local);}
    for(std::size_t b=0;b<r.bindings.size();++b){palette_[b].position=multiply(world_[r.bindings[b].bone],r.bindings[b].inverse_bind);palette_[b].normal=normal_matrix(palette_[b].position);}
    skin_vertices(r.vertices,palette_,output_);
}
void skin_vertices_scalar(std::span<const SkinVertex> input,std::span<const SkinMatrix> palette,std::span<SkinOutput> output) noexcept {
    for(std::size_t i=0;i<input.size();++i){const auto& v=input[i];SkinOutput o{};
        for(unsigned k=0;k<4;++k)if(v.skin.weight[k]>0){const auto& m=palette[v.skin.binding[k]];const auto p=point(m.position,v.position),n=point(m.normal,v.normal);for(unsigned a=0;a<3;++a){o.position[a]+=p[a]*v.skin.weight[k];o.normal[a]+=n[a]*v.skin.weight[k];}}
        o.normal=normalized(o.normal);output[i]=o;
    }
}
void skin_vertices(std::span<const SkinVertex> input,std::span<const SkinMatrix> palette,std::span<SkinOutput> output) noexcept {
    for(std::size_t i=0;i<input.size();++i){const auto& v=input[i];__m128 p=_mm_setzero_ps(),n=p;
        for(unsigned k=0;k<4;++k)if(v.skin.weight[k]>0){const auto& m=palette[v.skin.binding[k]];const auto* a=m.position.v.data();const auto* b=m.normal.v.data();
            auto x=_mm_add_ps(_mm_load_ps(a+12),_mm_mul_ps(_mm_load_ps(a),_mm_set1_ps(v.position[0])));
            x=_mm_add_ps(x,_mm_mul_ps(_mm_load_ps(a+4),_mm_set1_ps(v.position[1])));x=_mm_add_ps(x,_mm_mul_ps(_mm_load_ps(a+8),_mm_set1_ps(v.position[2])));
            auto y=_mm_mul_ps(_mm_load_ps(b),_mm_set1_ps(v.normal[0]));y=_mm_add_ps(y,_mm_mul_ps(_mm_load_ps(b+4),_mm_set1_ps(v.normal[1])));y=_mm_add_ps(y,_mm_mul_ps(_mm_load_ps(b+8),_mm_set1_ps(v.normal[2])));
            const auto w=_mm_set1_ps(v.skin.weight[k]);p=_mm_add_ps(p,_mm_mul_ps(x,w));n=_mm_add_ps(n,_mm_mul_ps(y,w));
        }
        alignas(16) float pos[4],normal[4];_mm_store_ps(pos,p);_mm_store_ps(normal,n);output[i]={{pos[0],pos[1],pos[2]},normalized(Float3{normal[0],normal[1],normal[2]})};
    }
}
}
