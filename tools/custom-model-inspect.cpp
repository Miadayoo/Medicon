#include <medicon/custom_model/ModelAsset.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>
int main(int argc,char**argv) {
    if (argc!=2) { std::cerr<<"Usage: MediconModelInspect <model-directory>\n";return 2; }
    std::string diagnostic;
    const auto asset=medicon::custom_model::load_model(std::filesystem::u8path(argv[1]),diagnostic);
    if (!asset) { std::cerr<<diagnostic<<'\n';return 1; }
    nlohmann::json j{{"diagnostic",diagnostic},{"from_cache",asset->from_cache},{"fbx_sha256",asset->fbx_sha256},
        {"triangles",asset->triangle_count},{"chunks",asset->parts.size()},{"minimum",asset->minimum},{"maximum",asset->maximum},
        {"scale",asset->scale},{"yawOffset",asset->yaw_offset},{"textures",nlohmann::json::array()},{"parts",nlohmann::json::array()}};
    for (const auto& t:asset->textures) j["textures"].push_back({{"path",t.path.string()},{"sha256",t.sha256},{"width",t.width},{"height",t.height}});
    for (const auto& p:asset->parts) j["parts"].push_back({{"name",p.name},{"texture",p.texture},{"vertices",p.vertices.size()}});
    if(asset->rig){const auto& r=*asset->rig;j["rig"]={{"bones",r.bones.size()},{"bindings",r.bindings.size()},{"vertices",r.vertices.size()},{"cache",r.from_cache},{"clips",nlohmann::json::array()}};
        for(const auto& c:r.clips)j["rig"]["clips"].push_back({{"name",c.name},{"frames",c.frames},{"procedural",c.procedural}});
        std::vector<medicon::custom_model::Affine> world(r.bones.size());std::vector<medicon::custom_model::SkinMatrix> palette(r.bindings.size());std::vector<medicon::custom_model::SkinOutput> output(r.vertices.size());
        for(std::size_t b=0;b<r.bones.size();++b){const auto local=medicon::custom_model::matrix(r.bones[b].rest);world[b]=r.bones[b].parent<0?local:medicon::custom_model::multiply(world[r.bones[b].parent],local);}
        for(std::size_t b=0;b<r.bindings.size();++b){palette[b].position=medicon::custom_model::multiply(world[r.bindings[b].bone],r.bindings[b].inverse_bind);palette[b].normal=medicon::custom_model::normal_matrix(palette[b].position);}
        medicon::custom_model::skin_vertices(r.vertices,palette,output);float max_error=0;
        for(std::size_t p=0;p<asset->parts.size();++p)for(std::size_t v=0;v<asset->parts[p].vertices.size();++v)for(unsigned a=0;a<3;++a)max_error=std::max(max_error,std::abs(output[r.part_indices[p][v]].position[a]-asset->parts[p].vertices[v].position[a]));
        j["rig"]["rest_max_error"]=max_error;
        medicon::custom_model::AnimationInput input;input.valid=true;input.speed=4.3F;
        for(unsigned i=0;i<10;++i)asset->animation->update(r,input,1.0F/60);
        const auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<200;++i)asset->animation->update(r,input,1.0F/60);
        j["rig"]["animation_cpu_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/200;
    }
    std::cout<<j.dump(2)<<'\n';return 0;
}
