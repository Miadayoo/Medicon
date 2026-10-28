#include <medicon/custom_model/ModelAsset.hpp>
#include <medicon/custom_model/BundledModel.hpp>

#include <ufbx.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>

namespace medicon::custom_model {
namespace {
using Json=nlohmann::json;
constexpr std::size_t max_file=128*1024*1024, max_vertices=500000, chunk_vertices=4095;
constexpr std::string_view importer="ufbx-0.20.1-13b5df8e89eff0ce898f7c37fa0716e3bc186573-p2-v2";
using Bytes=std::vector<std::byte>;
Bytes read_file(const std::filesystem::path& path,std::size_t maximum=max_file) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if (!in) throw std::runtime_error("Cannot read model asset: "+path.filename().string());
    const auto size=in.tellg();
    if (size<0 || static_cast<std::uint64_t>(size)>maximum) throw std::runtime_error("Model asset exceeds size limit.");
    Bytes bytes(static_cast<std::size_t>(size)); in.seekg(0);
    if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Model asset read was incomplete.");
    return bytes;
}
void atomic_file(const std::filesystem::path& path,std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temp=path;temp+=L".tmp";
    { std::ofstream out(temp,std::ios::binary|std::ios::trunc);
      if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
          throw std::runtime_error("Cannot write model cache."); }
    if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish model cache.");
}
std::span<const std::byte> as_bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()),text.size()};
}
bool inside(const std::filesystem::path& root,const std::filesystem::path& path) {
    auto r=root.begin(),p=path.begin();
    for (;r!=root.end();++r,++p) if (p==path.end() || _wcsicmp(r->c_str(),p->c_str())) return false;
    return p!=path.end();
}
std::filesystem::path checked(const std::filesystem::path& root,const std::filesystem::path& path) {
    const auto resolved=std::filesystem::weakly_canonical(path);
    if (!inside(root,resolved)) throw std::runtime_error("Texture path escapes the model directory.");
    return resolved;
}
std::uint32_t be32(const std::byte* p) {
    return (std::to_integer<std::uint32_t>(p[0])<<24)|(std::to_integer<std::uint32_t>(p[1])<<16)|
        (std::to_integer<std::uint32_t>(p[2])<<8)|std::to_integer<std::uint32_t>(p[3]);
}
TextureAsset texture_info(const std::filesystem::path& path) {
    const auto bytes=read_file(path,32*1024*1024);
    constexpr unsigned char signature[]{137,80,78,71,13,10,26,10};
    if (bytes.size()<33 || std::memcmp(bytes.data(),signature,8) || std::memcmp(bytes.data()+12,"IHDR",4))
        throw std::runtime_error("Expected a PNG diffuse texture: "+path.filename().string());
    const auto w=be32(bytes.data()+16),h=be32(bytes.data()+20);
    if (!w||!h||w>8192||h>8192) throw std::runtime_error("Texture dimensions are unsupported.");
    const auto utf8=path.generic_u8string();
    if (utf8.size()>4096) throw std::runtime_error("Texture path is too long.");
    return {path,sha256(bytes),w,h,std::string(utf8.begin(),utf8.end())};
}
std::filesystem::path texture_path(const std::filesystem::path& root,const ufbx_texture* texture) {
    if (!texture) throw std::runtime_error("FBX material has no diffuse texture.");
    if (texture->type!=UFBX_TEXTURE_FILE) {
        if (texture->file_textures.count!=1) throw std::runtime_error("Layered/procedural diffuse texture needs an explicit file texture.");
        texture=texture->file_textures.data[0];
    }
    if (texture->content.size) {
        const auto bytes=std::span(static_cast<const std::byte*>(texture->content.data),texture->content.size);
        if (bytes.size()>32*1024*1024) throw std::runtime_error("Embedded texture exceeds size limit.");
        const auto path=checked(root,root/L"cache"/std::filesystem::path(sha256(bytes)+".png"));
        if (!std::filesystem::exists(path)) atomic_file(path,bytes);
        return path;
    }
    std::string reference(texture->relative_filename.data,texture->relative_filename.length);
    if (reference.empty()) reference.assign(texture->filename.data,texture->filename.length);
    std::replace(reference.begin(),reference.end(),'\\','/');
    const auto relative=std::filesystem::u8path(reference);
    if (!relative.empty()&&!relative.is_absolute()) {
        const auto candidate=std::filesystem::weakly_canonical(root/relative);
        if (inside(root,candidate)&&std::filesystem::is_regular_file(candidate)) return candidate;
    }
    // The provided FBX records its exporter directory. Resolve its exact
    // filename in the staged textures directory, never open arbitrary paths.
    const auto candidate=checked(root,root/L"textures"/relative.filename());
    if (!std::filesystem::is_regular_file(candidate)) throw std::runtime_error("Missing FBX diffuse texture: "+relative.filename().string());
    return candidate;
}
bool finite(const Vertex& v) {
    for (float f:v.position) if (!std::isfinite(f)||std::abs(f)>10000) return false;
    for (float f:v.normal) if (!std::isfinite(f)) return false;
    for (float f:v.uv) if (!std::isfinite(f)||std::abs(f)>10000) return false;
    return true;
}
void read_exact(std::istream& in,void* data,std::size_t size) {
    if (size&&!in.read(static_cast<char*>(data),static_cast<std::streamsize>(size))) throw std::runtime_error("Truncated model cache.");
}
std::shared_ptr<ModelAsset> read_cache(const std::filesystem::path& root,const std::string& fbx_hash,const std::string& config_hash) {
    try {
        const auto path=checked(root,root/L"cache"/L"model.mmdl");
        if (!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>max_file) return {};
        std::ifstream in(path,std::ios::binary);std::array<char,8> magic{};std::uint32_t length{};
        read_exact(in,magic.data(),8);read_exact(in,&length,4);
        if (magic!=std::array<char,8>{'M','M','D','L','P','2',0,1}||length>1024*1024) return {};
        std::string header(length,'\0');read_exact(in,header.data(),header.size());
        const auto j=Json::parse(header);
        if (j.at("importer").get<std::string>()!=importer || j.at("fbx_sha256").get<std::string>()!=fbx_hash || j.at("config_sha256").get<std::string>()!=config_hash) return {};
        auto model=std::make_shared<ModelAsset>();model->fbx_sha256=fbx_hash;model->config_sha256=config_hash;
        model->scale=j.at("scale").get<float>();model->yaw_offset=j.at("yaw_offset").get<float>();
        model->minimum=j.at("minimum").get<std::array<float,3>>();model->maximum=j.at("maximum").get<std::array<float,3>>();
        if (!std::isfinite(model->scale)||model->scale<=0||model->scale>10||!std::isfinite(model->yaw_offset)||std::abs(model->yaw_offset)>36000) return {};
        for (unsigned axis=0;axis<3;++axis) if (!std::isfinite(model->minimum[axis])||!std::isfinite(model->maximum[axis])||model->minimum[axis]>model->maximum[axis]) return {};
        if (j.at("textures").size()>32 || j.at("parts").size()>128) return {};
        for (const auto& item:j.at("textures")) {
            auto texture=texture_info(checked(root,root/std::filesystem::u8path(item.at("path").get<std::string>())));
            if (texture.sha256!=item.at("sha256").get<std::string>()) return {};
            model->textures.push_back(std::move(texture));
        }
        std::size_t total{};Bytes body;
        for (const auto& item:j.at("parts")) {
            const auto count=item.at("vertices").get<std::size_t>();
            if (!count||count%3||count>chunk_vertices||count>max_vertices-total) return {};
            MeshPart part;part.name=item.at("name").get<std::string>();part.texture=item.at("texture").get<std::uint32_t>();
            if (part.texture>=model->textures.size()) return {};
            part.vertices.resize(count);read_exact(in,part.vertices.data(),count*sizeof(Vertex));
            if (!std::all_of(part.vertices.begin(),part.vertices.end(),finite)) return {};
            const auto* begin=reinterpret_cast<const std::byte*>(part.vertices.data());body.insert(body.end(),begin,begin+count*sizeof(Vertex));
            total+=count;model->parts.push_back(std::move(part));
        }
        if (!total || sha256(body)!=j.at("vertices_sha256").get<std::string>() || in.peek()!=EOF) return {};
        model->triangle_count=total/3;model->from_cache=true;return model;
    } catch (...) { return {}; } // Invalid/stale caches are rebuilt from source.
}
void write_cache(const std::filesystem::path& root,const ModelAsset& model) {
    Json j{{"importer",importer},{"fbx_sha256",model.fbx_sha256},{"config_sha256",model.config_sha256},
        {"scale",model.scale},{"yaw_offset",model.yaw_offset},{"minimum",model.minimum},{"maximum",model.maximum},
        {"textures",Json::array()},{"parts",Json::array()}};
    for (const auto& texture:model.textures) {
        const auto utf8=texture.path.lexically_relative(root).generic_u8string();
        j["textures"].push_back({{"path",std::string(utf8.begin(),utf8.end())},{"sha256",texture.sha256}});
    }
    Bytes body;
    for (const auto& part:model.parts) {
        j["parts"].push_back({{"name",part.name},{"texture",part.texture},{"vertices",part.vertices.size()}});
        const auto* begin=reinterpret_cast<const std::byte*>(part.vertices.data());body.insert(body.end(),begin,begin+part.vertices.size()*sizeof(Vertex));
    }
    j["vertices_sha256"]=sha256(body);const auto header=j.dump();const auto length=static_cast<std::uint32_t>(header.size());
    Bytes output;output.resize(12+header.size());const char magic[8]{'M','M','D','L','P','2',0,1};
    std::memcpy(output.data(),magic,8);std::memcpy(output.data()+8,&length,4);std::memcpy(output.data()+12,header.data(),header.size());
    output.insert(output.end(),body.begin(),body.end());atomic_file(checked(root,root/L"cache"/L"model.mmdl"),output);
}
}

std::string sha256(std::span<const std::byte> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};std::array<unsigned char,32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA-256 unavailable.");
    bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    if (ok&&!bytes.empty()) ok=bytes.size()<=ULONG_MAX && BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),static_cast<ULONG>(bytes.size()),0)>=0;
    if (ok) ok=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    if (hash) BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if (!ok) throw std::runtime_error("SHA-256 failed.");
    constexpr char hex[]="0123456789abcdef";std::string text(64,'0');
    for (unsigned i=0;i<32;++i) { text[i*2]=hex[digest[i]>>4];text[i*2+1]=hex[digest[i]&15]; }return text;
}

std::shared_ptr<const ModelAsset> load_model(const std::filesystem::path& directory,std::string& diagnostic) {
    try {
        const auto root=std::filesystem::canonical(directory);
        const auto fbx=read_file(checked(root,root/L"model.fbx"));
        const auto config_path=checked(root,root/L"model.json");
        if (!std::filesystem::exists(config_path)) {
            const std::string defaults="{\n  \"scale\": 1.0,\n  \"yawOffset\": 180.0\n}\n";
            atomic_file(config_path,as_bytes(defaults));
        }
        const auto config=read_file(config_path,65536);const auto config_json=Json::parse(reinterpret_cast<const char*>(config.data()),reinterpret_cast<const char*>(config.data()+config.size()));
        const auto fbx_hash=sha256(fbx),config_hash=sha256(config);
        const std::string config_text(reinterpret_cast<const char*>(config.data()),config.size());
        if (auto cached=read_cache(root,fbx_hash,config_hash)) {
            load_animation_asset(root,fbx,config_text,*cached);
            diagnostic=cached->rig?cached->rig->diagnostic:"Static FBX cache ready (no skeleton).";return cached;
        }
        const float scale=config_json.value("scale",1.0F),yaw=config_json.value("yawOffset",180.0F);
        if (!std::isfinite(scale)||scale<=0||scale>10||!std::isfinite(yaw)||std::abs(yaw)>36000)
            throw std::runtime_error("model.json scale/yawOffset is invalid.");
        ufbx_load_opts opts{};opts.target_axes=ufbx_axes_right_handed_y_up;opts.target_unit_meters=1.0;
        opts.inherit_mode_handling=UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
        opts.evaluate_skinning=true;opts.load_external_files=false;opts.generate_missing_normals=true;
        opts.temp_allocator.memory_limit=256*1024*1024;opts.result_allocator.memory_limit=256*1024*1024;
        ufbx_error error{};
        std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> scene(ufbx_load_memory(fbx.data(),fbx.size(),&opts,&error),ufbx_free_scene);
        if (!scene) { char text[1024]{};ufbx_format_error(text,sizeof text,&error);throw std::runtime_error(text); }
        auto model=std::make_shared<ModelAsset>();model->fbx_sha256=fbx_hash;model->config_sha256=config_hash;
        model->scale=scale;model->yaw_offset=yaw;model->minimum.fill(std::numeric_limits<float>::max());model->maximum.fill(std::numeric_limits<float>::lowest());
        std::map<std::filesystem::path,std::uint32_t> texture_indices;
        std::size_t total{};
        for (auto* node:scene->nodes) {
            auto* mesh=node->mesh;if (!mesh||!mesh->num_triangles) continue;
            if (!mesh->vertex_uv.exists) throw std::runtime_error("FBX mesh has no UV coordinates.");
            if (mesh->num_triangles>(max_vertices-total)/3) throw std::runtime_error("FBX exceeds static mesh vertex limit.");
            std::vector<std::uint32_t> triangles(std::max<std::size_t>(3,mesh->max_face_triangles*3));
            std::map<std::uint32_t,std::vector<Vertex>> by_material;
            const auto normal_matrix=ufbx_matrix_for_normals(&node->geometry_to_world);
            for (std::size_t face_index=0;face_index<mesh->faces.count;++face_index) {
                const auto face=mesh->faces.data[face_index];if (face.num_indices<3) continue;
                const auto material=mesh->face_material.count?mesh->face_material.data[face_index]:0;
                auto& vertices=by_material[material];const auto count=ufbx_triangulate_face(triangles.data(),triangles.size(),mesh,face);
                for (std::size_t k=0;k<count*3;++k) {
                    const auto index=triangles[k];auto p=ufbx_get_vertex_vec3(&mesh->skinned_position,index);
                    auto n=ufbx_get_vertex_vec3(&mesh->skinned_normal,index);const auto uv=ufbx_get_vertex_vec2(&mesh->vertex_uv,index);
                    if (mesh->skinned_is_local) { p=ufbx_transform_position(&node->geometry_to_world,p);n=ufbx_transform_direction(&normal_matrix,n); }
                    const auto length=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
                    if (length<=1e-12) throw std::runtime_error("FBX contains a zero normal.");
                    Vertex v{{static_cast<float>(p.x*scale),static_cast<float>(p.y*scale),static_cast<float>(p.z*scale)},
                        {static_cast<float>(n.x/length),static_cast<float>(n.y/length),static_cast<float>(n.z/length)},
                        {static_cast<float>(uv.x),static_cast<float>(1.0-uv.y)},0xFFFFFFFF};
                    if (!finite(v)) throw std::runtime_error("FBX contains invalid vertex data.");
                    for (unsigned axis=0;axis<3;++axis) { model->minimum[axis]=std::min(model->minimum[axis],v.position[axis]);model->maximum[axis]=std::max(model->maximum[axis],v.position[axis]); }
                    vertices.push_back(v);++total;
                }
            }
            for (auto& [material_index,vertices]:by_material) {
                if (vertices.empty()) continue;
                if (material_index>=node->materials.count) throw std::runtime_error("FBX material index is invalid.");
                const auto* material=node->materials.data[material_index];
                const auto* diffuse=material->pbr.base_color.texture?material->pbr.base_color.texture:material->fbx.diffuse_color.texture;
                const std::string material_name(material->name.data,material->name.length);
                // Unity avatars often retain PSD/exporter paths in the FBX.
                // Explicit material-to-PNG bindings stay inside this model's root.
                const auto overrides=config_json.find("textures");
                const auto path=overrides!=config_json.end()&&overrides->contains(material_name)
                    ? checked(root,root/std::filesystem::u8path(overrides->at(material_name).get<std::string>()))
                    : texture_path(root,diffuse);
                auto found=texture_indices.find(path);std::uint32_t texture{};
                if (found==texture_indices.end()) {
                    if (model->textures.size()>=32) throw std::runtime_error("Too many FBX diffuse textures.");
                    texture=static_cast<std::uint32_t>(model->textures.size());model->textures.push_back(texture_info(path));texture_indices.emplace(path,texture);
                } else texture=found->second;
                for (std::size_t offset=0;offset<vertices.size();offset+=chunk_vertices) {
                    if (model->parts.size()>=128) throw std::runtime_error("Too many static mesh chunks.");
                    const auto count=std::min(chunk_vertices,vertices.size()-offset);
                    MeshPart part{std::string(material->name.data,material->name.length),texture,{}};
                    part.vertices.assign(vertices.begin()+offset,vertices.begin()+offset+count);model->parts.push_back(std::move(part));
                }
            }
        }
        if (!total||total%3) throw std::runtime_error("FBX has no supported triangles.");
        model->triangle_count=total/3;
        load_animation_asset(root,fbx,config_text,*model,scene.get());
        try { write_cache(root,*model);diagnostic="Static FBX imported; cache written."; }
        catch (const std::exception&) { diagnostic="Static FBX imported; cache write unavailable."; }
        if(model->rig)diagnostic+=" "+model->rig->diagnostic;
        return model;
    } catch (const std::exception& e) { diagnostic=e.what();return {}; }
}

ModelRegistry::~ModelRegistry() { stop(); }
void ModelRegistry::start(std::filesystem::path directory, std::filesystem::path bundled_cache) {
    stop();status_.store(Status::loading,std::memory_order_release);
    worker_=std::jthread([this,directory=std::move(directory),bundled_cache=std::move(bundled_cache)](std::stop_token stop) {
        std::string diagnostic;auto model=load_model(directory,diagnostic);
        if (!model && !bundled_cache.empty() && !stop.stop_requested()) {
            const auto external_error=diagnostic;
            std::string bundle_diagnostic;
            const auto bundled=materialize_bundled_neon(bundled_cache,bundle_diagnostic);
            if (!bundled.empty() && !stop.stop_requested()) {
                model=load_model(bundled,diagnostic);
                diagnostic=bundle_diagnostic+". "+diagnostic;
            } else diagnostic=bundle_diagnostic;
            diagnostic="External model unavailable ("+external_error+"). "+diagnostic;
        }
        if (stop.stop_requested()) return;
        { std::lock_guard lock(mutex_);diagnostic_=std::move(diagnostic); }
        retained_=model;std::atomic_store_explicit(&current_,std::move(model),std::memory_order_release);
        status_.store(retained_?Status::ready:Status::failed,std::memory_order_release);
    });
}
void ModelRegistry::stop() {
    catalog_active_.store(false,std::memory_order_release);
    if (worker_.joinable()) { worker_.request_stop();worker_.join(); }
    std::atomic_store_explicit(&current_,std::shared_ptr<const ModelAsset>{},std::memory_order_release);retained_.reset();status_.store(Status::idle,std::memory_order_release);
    retained_models_={};
}
void ModelRegistry::start_catalog(std::array<std::filesystem::path,builtin_model_count> directories,
    std::filesystem::path bundled_cache,BuiltinModel initial) {
    stop();
    { std::lock_guard lock(mutex_);requested_model_=std::min(static_cast<unsigned>(initial),builtin_model_count-1);requested_revision_=0;diagnostic_.clear(); }
    status_.store(Status::loading,std::memory_order_release);
    catalog_active_.store(true,std::memory_order_release);
    worker_=std::jthread([this,directories=std::move(directories),bundled_cache=std::move(bundled_cache)](std::stop_token stop){
        std::uint64_t served=UINT64_MAX;
        std::array<std::string,builtin_model_count> diagnostics;
        while (!stop.stop_requested()) {
            unsigned selected{};std::uint64_t revision{};
            { std::unique_lock lock(mutex_);
              if (!wake_.wait(lock,stop,[&]{return requested_revision_!=served;})) return;
              selected=requested_model_;revision=requested_revision_; }
            auto model=retained_models_[selected];
            auto& diagnostic=diagnostics[selected];
            if (!model) {
                model=load_model(directories[selected],diagnostic);
                if (!model&&!stop.stop_requested()) {
                    const auto external_error=diagnostic;
                    std::string bundle_diagnostic;
                    const auto directory=materialize_bundled_model(static_cast<BuiltinModel>(selected),bundled_cache,bundle_diagnostic);
                    if (!directory.empty()&&!stop.stop_requested()) {
                        model=load_model(directory,diagnostic);diagnostic=bundle_diagnostic+". "+diagnostic;
                    } else diagnostic=bundle_diagnostic;
                    diagnostic="External model unavailable ("+external_error+"). "+diagnostic;
                }
                // Keep even an obsolete completed request alive off the render
                // thread; it can be selected again without importing another FBX.
                retained_models_[selected]=model;
            }
            if (stop.stop_requested()) return;
            { std::lock_guard lock(mutex_);
              served=revision;
              if (requested_revision_!=revision) continue;
              diagnostic_=std::string(builtin_model_name(static_cast<BuiltinModel>(selected)))+": "+diagnostic;
              std::atomic_store_explicit(&current_,std::move(model),std::memory_order_release);
              status_.store(retained_models_[selected]?Status::ready:Status::failed,std::memory_order_release); }
        }
    });
}
bool ModelRegistry::select(BuiltinModel model) noexcept {
    const auto selected=static_cast<unsigned>(model);
    if (selected>=builtin_model_count||!catalog_active_.load(std::memory_order_acquire)) return false;
    { std::lock_guard lock(mutex_);
      if (selected==requested_model_) return false;
      requested_model_=selected;++requested_revision_;
      std::atomic_store_explicit(&current_,std::shared_ptr<const ModelAsset>{},std::memory_order_release);
      status_.store(Status::loading,std::memory_order_release); }
    wake_.notify_all();return true;
}
std::string ModelRegistry::diagnostic() const { std::lock_guard lock(mutex_);return diagnostic_; }
}
