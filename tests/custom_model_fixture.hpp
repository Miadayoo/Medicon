#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <Windows.h>
namespace model_fixture {
inline void text(const std::filesystem::path& path,const std::string& value) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<value;
}
// Owned minimal FBX 7.4 fixture: one UV-mapped triangle + external diffuse PNG.
inline std::filesystem::path create(const char* suffix) {
    auto root=std::filesystem::temp_directory_path()/(std::string("medicon-p2-")+std::to_string(GetCurrentProcessId())+suffix);
    std::filesystem::create_directories(root/L"textures");
    text(root/L"model.fbx",R"FBX(; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: { Version: 1000
 Properties70: {
 P: "UpAxis", "int", "Integer", "",1
 P: "UpAxisSign", "int", "Integer", "",1
 P: "FrontAxis", "int", "Integer", "",2
 P: "FrontAxisSign", "int", "Integer", "",-1
 P: "CoordAxis", "int", "Integer", "",0
 P: "CoordAxisSign", "int", "Integer", "",1
 P: "UnitScaleFactor", "double", "Number", "",100
 }
}
Objects: {
 Geometry: 100, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
  LayerElementNormal: 0 { Version: 101
   Name: ""
   MappingInformationType: "ByPolygonVertex"
   ReferenceInformationType: "Direct"
   Normals: *9 { a: 0,0,1,0,0,1,0,0,1 }
  }
  LayerElementUV: 0 { Version: 101
   Name: "UVMap"
   MappingInformationType: "ByPolygonVertex"
   ReferenceInformationType: "Direct"
   UV: *6 { a: 0,0,1,0,0,1 }
  }
  LayerElementMaterial: 0 { Version: 101
   Name: ""
   MappingInformationType: "AllSame"
   ReferenceInformationType: "IndexToDirect"
   Materials: *1 { a: 0 }
  }
  Layer: 0 { Version: 100
   LayerElement: { Type: "LayerElementNormal"
    TypedIndex: 0 }
   LayerElement: { Type: "LayerElementUV"
    TypedIndex: 0 }
   LayerElement: { Type: "LayerElementMaterial"
    TypedIndex: 0 }
  }
 }
 Model: 200, "Model::Triangle", "Mesh" { Version: 232 }
 Material: 300, "Material::Diffuse", "" { Version: 102
  ShadingModel: "lambert"
  Properties70: { P: "DiffuseColor", "Color", "", "A",1,1,1 }
 }
 Texture: 400, "Texture::Diffuse", "" { Type: "TextureVideoClip"
  Version: 202
  TextureName: "Texture::Diffuse"
  FileName: "textures/triangle.png"
  RelativeFilename: "textures/triangle.png"
  ModelUVTranslation: 0,0
  ModelUVScaling: 1,1
 }
 Video: 500, "Video::Diffuse", "Clip" { Type: "Clip"
  Filename: "textures/triangle.png"
  RelativeFilename: "textures/triangle.png"
 }
}
Connections: {
 C: "OO",100,200
 C: "OO",200,0
 C: "OO",300,200
 C: "OP",400,300,"DiffuseColor"
 C: "OO",500,400
}
)FBX");
    // Valid one-pixel PNG; this fixture is our own test data.
    constexpr unsigned char png[]{137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,
        0,0,0,11,73,68,65,84,120,156,99,96,0,2,0,0,5,0,1,165,246,69,64,0,0,0,0,73,69,78,68,174,66,96,130};
    std::ofstream out(root/L"textures"/L"triangle.png",std::ios::binary);out.write(reinterpret_cast<const char*>(png),sizeof png);
    return root;
}
}
