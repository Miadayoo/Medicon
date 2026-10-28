#include <medicon/custom_model/ModelAsset.hpp>
#include <medicon/custom_model/BundledModel.hpp>
#include "custom_model_fixture.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        namespace cm=medicon::custom_model;
        const auto root=model_fixture::create("asset");std::string diagnostic;
        auto a=cm::load_model(root,diagnostic);if (!a) throw std::runtime_error(diagnostic);
        require(!a->from_cache&&a->triangle_count==1&&a->parts.size()==1&&a->textures.size()==1,"Import real FBX geometry/material mapping");
        require(a->maximum[0]==1&&a->maximum[1]==1&&a->textures[0].width==1,"Meters, bounds and PNG dimensions");
        require(a->parts[0].vertices[0].uv[1]==1&&a->parts[0].vertices[2].uv[1]==0,"PNG V orientation");
        auto b=cm::load_model(root,diagnostic);require(b&&b->from_cache&&b->fbx_sha256==a->fbx_sha256,"Cache roundtrip");
        model_fixture::text(root/L"cache"/L"model.mmdl","corrupt");
        b=cm::load_model(root,diagnostic);require(b&&!b->from_cache,"Corrupt cache rebuild");
        model_fixture::text(root/L"model.json","{\"scale\":2,\"yawOffset\":90}");
        b=cm::load_model(root,diagnostic);require(b&&!b->from_cache&&b->maximum[1]==2&&b->yaw_offset==90,"Config hash invalidates geometry cache");
        { std::ofstream out(root/L"model.fbx",std::ios::app);out<<"\n; content hash change\n"; }
        auto c=cm::load_model(root,diagnostic);require(c&&!c->from_cache&&c->fbx_sha256!=b->fbx_sha256,"FBX content hash invalidates cache");
        model_fixture::text(root/L"textures"/L"triangle.png","invalid");
        require(!cm::load_model(root,diagnostic),"Invalid texture fails to vanilla");
        cm::ModelRegistry registry;registry.start(root);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (registry.status()==cm::ModelRegistry::Status::loading&&std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        require(registry.status()==cm::ModelRegistry::Status::failed&&!registry.snapshot(),"Failed worker publishes no incomplete asset");registry.stop();
        const auto embedded=model_fixture::create("embedded");
        std::ifstream source(embedded/L"model.fbx",std::ios::binary);
        std::string fbx((std::istreambuf_iterator<char>(source)),{});source.close();
        const auto at=fbx.find("  Filename: \"textures/triangle.png\"");
        require(at!=std::string::npos,"Embedded fixture video node");
        fbx.insert(at,"  Content: ,\"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGNgAAIAAAUAAaX2RUAAAAAASUVORK5CYII=\"\n");
        model_fixture::text(embedded/L"model.fbx",fbx);
        model_fixture::text(embedded/L"textures"/L"triangle.png","external file must not be used");
        c=cm::load_model(embedded,diagnostic);if (!c) throw std::runtime_error(diagnostic);
        require(c->textures[0].path.parent_path().filename()==L"cache"&&c->textures[0].width==1,"Embedded PNG imported from FBX memory and staged for native Raw registration");
        registry.start(embedded);
        const auto ready_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (registry.status()==cm::ModelRegistry::Status::loading&&std::chrono::steady_clock::now()<ready_deadline) std::this_thread::yield();
        require(registry.status()==cm::ModelRegistry::Status::ready&&registry.snapshot(),"Worker atomically publishes complete immutable generation");registry.stop();
        const auto mapped=model_fixture::create("texture-overrides");
        std::filesystem::copy_file(mapped/L"textures"/L"triangle.png",mapped/L"textures"/L"mapped.png",std::filesystem::copy_options::overwrite_existing);
        model_fixture::text(mapped/L"textures"/L"triangle.png","obsolete exporter texture");
        model_fixture::text(mapped/L"model.json",R"({"textures":{"Diffuse":"textures/mapped.png"}})");
        auto mapped_model=cm::load_model(mapped,diagnostic);
        require(mapped_model&&mapped_model->textures[0].path.filename()==L"mapped.png","Explicit material mapping replaces obsolete exporter/PSD references");
        model_fixture::text(mapped/L"model.json",R"({"textures":{"Diffuse":"../../outside.png"}})");
        require(!cm::load_model(mapped,diagnostic)&&diagnostic.find("escapes")!=std::string::npos,"Material PNG override cannot escape the model root");
        // Use the actual resources linked into this EXE: no model beside the
        // executable and no dependency on a developer's Downloads directory.
        const auto bundle_cache=root/L"bundled-cache";
        const auto bundle=cm::materialize_bundled_neon(bundle_cache,diagnostic);
        if (bundle.empty()) throw std::runtime_error(diagnostic);
        auto neon=cm::load_model(bundle,diagnostic);if (!neon) throw std::runtime_error(diagnostic);
        require(neon->fbx_sha256=="8d036bc5e8ec3536e49a648113af2b281f9d5710b45deb3255cdd2f2994b2210"&&
            neon->textures.size()==3&&neon->triangle_count>1000,"Single-DLL pack imports real Neon geometry and all diffuse textures");
        require(neon->rig&&neon->rig->first_person.ready(),"Bundled rig provides third-person model and first-person arms");
        for (const auto& texture:neon->textures)
            require(std::filesystem::is_regular_file(texture.path)&&texture.path.is_absolute(),"Bundled textures are staged for native texture loading");
        const auto modified=std::filesystem::last_write_time(bundle/L"model.fbx");
        require(cm::materialize_bundled_neon(bundle_cache,diagnostic)==bundle&&
            std::filesystem::last_write_time(bundle/L"model.fbx")==modified,"Matching bundled cache is reused without rewriting");
        model_fixture::text(bundle/L"textures"/L"TP_Core_Eye_DF.png","corrupt");
        require(cm::materialize_bundled_neon(bundle_cache,diagnostic)==bundle,"Corrupt extracted file is repaired");
        neon=cm::load_model(bundle,diagnostic);
        require(neon&&neon->from_cache,"Repaired PNG matches validated importer cache");

        const auto wait_for_worker=[&] {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while (registry.status()==cm::ModelRegistry::Status::loading&&std::chrono::steady_clock::now()<end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        registry.start(root/L"no-external-model",bundle_cache);wait_for_worker();
        require(registry.status()==cm::ModelRegistry::Status::ready&&registry.snapshot()&&
            registry.snapshot()->fbx_sha256==neon->fbx_sha256,"Loader-style missing external directory uses embedded model");
        require(registry.diagnostic().find("Bundled Neon assets ready")!=std::string::npos,"Worker reports selected embedded source");registry.stop();
        registry.start(root,bundle_cache);wait_for_worker();
        require(registry.status()==cm::ModelRegistry::Status::ready&&registry.snapshot()&&
            registry.snapshot()->fbx_sha256==neon->fbx_sha256,"Invalid external model falls back to intact bundle");registry.stop();
        registry.start(embedded,bundle_cache);wait_for_worker();
        require(registry.status()==cm::ModelRegistry::Status::ready&&registry.snapshot()&&
            registry.snapshot()->triangle_count==1,"Valid external model override takes precedence over embedded default");registry.stop();
        require(cm::materialize_bundled_neon({},diagnostic).empty(),"Empty bundle cache path fails explicitly");
        const auto chocolat_directory=cm::materialize_bundled_model(cm::BuiltinModel::chocolat,bundle_cache,diagnostic);
        if (chocolat_directory.empty()) throw std::runtime_error(diagnostic);
        auto chocolat=cm::load_model(chocolat_directory,diagnostic);if (!chocolat) throw std::runtime_error(diagnostic);
        require(chocolat->fbx_sha256=="b4e05dc30e9e139418ce054f6b65bb6332849b201c2b546aa988e74e43300fdb"&&
            chocolat->textures.size()==5&&chocolat->maximum[1]>1.7F&&chocolat->maximum[1]<1.9F,"Bundled Chocolat uses five supplied PNGs and configured character scale");
        require(chocolat->rig&&chocolat->rig->first_person.ready()&&chocolat->rig->bindings.size()<2048,
            "Chocolat bone aliases provide arms and repeated clothing bindings fit the budget");
        const auto& chocolat_rig=*chocolat->rig;
        require(chocolat_rig.bones[chocolat_rig.roles.left_arm].name=="UpperArm.L"&&
            chocolat_rig.bones[chocolat_rig.roles.right_leg].name=="UpperLeg.R","Explicit Chocolat humanoid roles drive generated motion");
        for(unsigned pose=0;pose<3;++pose){
            cm::AnimationInput input;input.valid=true;input.identity=1;input.world_identity=1;
            input.attack_progress=pose==1?.5F:-1;input.using_item=pose==2;
            chocolat->animation->update_first_person(chocolat_rig,input,1.0F/60,true);
            const auto output=chocolat->animation->first_person_output();
            unsigned hands{},visible_hands{};
            for(std::size_t v=0;v<output.size();++v){
                for(float f:output[v].position)require(std::isfinite(f),"Chocolat first-person poses remain finite");
                bool hand=false;const auto& skin=chocolat_rig.first_person.vertices[v].skin;
                for(unsigned k=0;k<4;++k)if(skin.weight[k]>.3F){
                    int bone=static_cast<int>(chocolat_rig.bindings[skin.binding[k]].bone);
                    for(;bone>=0;bone=chocolat_rig.bones[bone].parent)
                        if(chocolat_rig.bones[bone].name=="Hand.L"||chocolat_rig.bones[bone].name=="Hand.R"){hand=true;break;}
                }
                if(!hand)continue;
                const auto& p=output[v].position;++hands;
                require(p[2]>.1F,"Chocolat hands stay in front of the first-person camera");
                constexpr float tangent=.70020754F; // FOV 70 degrees, 16:9
                if(std::abs(p[0]/p[2])<tangent*16/9&&std::abs(p[1]/p[2])<tangent)++visible_hands;
            }
            require(hands>0&&visible_hands>.9F*hands,"Chocolat hands are visible in idle, attack and use poses");
        }
        registry.start_catalog({root/L"missing-neon",root/L"missing-chocolat"},bundle_cache);wait_for_worker();
        const auto first_neon=registry.snapshot();require(first_neon&&first_neon->fbx_sha256==neon->fbx_sha256,"Catalog defaults to Neon");
        require(registry.select(cm::BuiltinModel::chocolat),"Catalog accepts character selection");wait_for_worker();
        require(registry.status()==cm::ModelRegistry::Status::ready&&registry.snapshot()->fbx_sha256==chocolat->fbx_sha256,"Catalog worker publishes selected Chocolat pack");
        registry.select(cm::BuiltinModel::neon);registry.select(cm::BuiltinModel::chocolat);registry.select(cm::BuiltinModel::neon);wait_for_worker();
        require(registry.status()==cm::ModelRegistry::Status::ready&&registry.snapshot()==first_neon,"Rapid selection returning to original model reuses its retained generation");
        require(!registry.select(cm::BuiltinModel::count),"Invalid catalog selection is rejected");registry.stop();
        std::cout<<"P2 importer / PNG mappings / cache / bundled Neon and Chocolat / catalog selection passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
