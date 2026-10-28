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
        std::cout<<"P2 importer / UV / cache / bundled Neon extraction / loader fallback passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
