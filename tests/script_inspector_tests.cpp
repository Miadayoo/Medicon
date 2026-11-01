#include <medicon/scripts/ScriptServices.hpp>
#include <windows.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace medicon::scripts;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
std::string read(const fs::path& path) { std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}}; }
struct Image {
    std::string bytes=read(MEDICON_VALID_FIXTURE);
    template<class T> T get(size_t offset) const { check(offset<=bytes.size()&&sizeof(T)<=bytes.size()-offset,"Fixture read bounds");T value{};std::memcpy(&value,bytes.data()+offset,sizeof(value));return value; }
    template<class T> void put(size_t offset,const T& value) { check(offset<=bytes.size()&&sizeof(T)<=bytes.size()-offset,"Fixture write bounds");std::memcpy(bytes.data()+offset,&value,sizeof(value)); }
    size_t nt_offset() const {return static_cast<size_t>(get<IMAGE_DOS_HEADER>(0).e_lfanew);}
    IMAGE_NT_HEADERS64 nt() const {return get<IMAGE_NT_HEADERS64>(nt_offset());}
    size_t offset(uint32_t rva) const {
        for(size_t i=0;i<nt().FileHeader.NumberOfSections;++i) {
            const auto section=get<IMAGE_SECTION_HEADER>(nt_offset()+sizeof(IMAGE_NT_HEADERS64)+i*sizeof(IMAGE_SECTION_HEADER));
            if(rva>=section.VirtualAddress&&rva-section.VirtualAddress<section.SizeOfRawData)return section.PointerToRawData+rva-section.VirtualAddress;
        }
        throw std::runtime_error("Fixture RVA lookup");
    }
    size_t exports_offset() const {return offset(nt().OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);}
};
}
int main() {
    try {
        check(inspect_plugin(MEDICON_VALID_FIXTURE).api_version==400,"Registered DLL inspection");
        const auto data=read(MEDICON_UNREGISTERED_FIXTURE);
        IMAGE_DOS_HEADER dos{};std::memcpy(&dos,data.data(),sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};std::memcpy(&nt,data.data()+dos.e_lfanew,sizeof(nt));
        std::cout<<"Unregistered DLL export RVA="<<nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress<<'\n';
        bool refused=false;
        try { inspect_plugin(MEDICON_UNREGISTERED_FIXTURE); }
        catch(const std::exception& e) {
            std::cout<<e.what()<<'\n';refused=std::string(e.what()).find("MEDICON_MODULE")!=std::string::npos;
        }
        check(refused,"Unregistered DLL must diagnose missing MEDICON_MODULE instead of PE RVA");
        const auto temp=fs::temp_directory_path()/("medicon-pe-inspection-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".dll");
        struct Remove {fs::path file;~Remove(){std::error_code ignored;fs::remove(file,ignored);}} remove{temp};
        const auto rejects=[&](const Image& image,std::string_view expected){
            {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(image.bytes.data(),static_cast<std::streamsize>(image.bytes.size()));check(static_cast<bool>(out),"Save PE fixture");}
            try {inspect_plugin(temp);}catch(const std::exception& e){
                check(std::string_view(e.what()).find(expected)!=std::string_view::npos,e.what());return;
            }
            throw std::runtime_error("Invalid script DLL was accepted");
        };
        Image unnamed;auto exports=unnamed.get<IMAGE_EXPORT_DIRECTORY>(unnamed.exports_offset());exports.NumberOfNames=0;unnamed.put(unnamed.exports_offset(),exports);
        rejects(unnamed,"MEDICON_MODULE");
        Image incomplete;exports=incomplete.get<IMAGE_EXPORT_DIRECTORY>(incomplete.exports_offset());
        const auto first_name=incomplete.get<uint32_t>(incomplete.offset(exports.AddressOfNames));incomplete.bytes[incomplete.offset(first_name)]='x';
        rejects(incomplete,"MEDICON_MODULE");
        Image invalid;exports=invalid.get<IMAGE_EXPORT_DIRECTORY>(invalid.exports_offset());exports.AddressOfNames=0xfffffff0;invalid.put(invalid.exports_offset(),exports);
        rejects(invalid,"export name table RVA 0xfffffff0");
        Image truncated;auto header=truncated.nt();header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size=1;truncated.put(truncated.nt_offset(),header);
        rejects(truncated,"truncated export directory");
        std::cout<<"Script registration and malformed export diagnostics passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
