#include <medicon/gui/LiquidGlass.hpp>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void success(HRESULT hr,const char* what){if(FAILED(hr))throw std::runtime_error(what);}
template<class T> void result(ID3D11DeviceContext* context,ID3D11Query* query,T& value) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;){const auto hr=context->GetData(query,&value,sizeof(value),0);if(hr==S_OK)return;success(hr,"GPU query failed");
        if(std::chrono::steady_clock::now()>limit)throw std::runtime_error("GPU query timed out");Sleep(1);}
}
void profile(std::ostream& out){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf());
    bool warp=false;if(FAILED(hr)){warp=true;success(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf()),"Create graphics device");}
    ComPtr<IDXGIDevice> dxgi;success(device.As(&dxgi),"Query DXGI device");ComPtr<IDXGIAdapter> adapter;success(dxgi->GetAdapter(adapter.GetAddressOf()),"Get adapter");DXGI_ADAPTER_DESC desc{};success(adapter->GetDesc(&desc),"Get adapter description");
    char name[1024]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);
    out<<"# GPU: "<<name<<"; route D3D11; WARP="<<warp<<"; offscreen, 3 warmups + 10 measured frames; no game/FPS claim\n";
    out<<"width,height,scale,panes,cpu_mean_us,cpu_max_us,gpu_mean_us,gpu_max_us,draws\n";
    for(const auto size:{std::pair{1920u,1080u},std::pair{2560u,1440u}})for(float scale:{1.F,1.5F})for(int count:{1,5,20}){
        D3D11_TEXTURE2D_DESC texture_desc{};texture_desc.Width=size.first;texture_desc.Height=size.second;texture_desc.MipLevels=texture_desc.ArraySize=1;
        texture_desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture_desc.SampleDesc.Count=1;texture_desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture;success(device->CreateTexture2D(&texture_desc,nullptr,texture.GetAddressOf()),"Create profile target");
        ComPtr<ID3D11RenderTargetView> view;success(device->CreateRenderTargetView(texture.Get(),nullptr,view.GetAddressOf()),"Create profile view");
        medicon::gui::LiquidGlassD3D11 glass;
        std::vector<medicon::gui::LiquidGlassFrame> panes(static_cast<size_t>(count));
        for(int i=0;i<count;++i){auto& pane=panes[i];pane.center={180.F*scale+(i%5)*220.F*scale,100.F*scale+(i/5)*150.F*scale};pane.size={180.F*scale,90.F*scale};pane.scale=scale;pane.radius=24*scale;}
        double cpu=0,gpu=0,max_cpu=0,max_gpu=0;
        for(int frame=0;frame<13;++frame){
            const float background[]{.2F,.3F,.5F,1};context->ClearRenderTargetView(view.Get(),background);
            const auto query=[&](D3D11_QUERY kind){ComPtr<ID3D11Query> q;D3D11_QUERY_DESC d{kind,0};success(device->CreateQuery(&d,q.GetAddressOf()),"Create timing query");return q;};
            auto disjoint=query(D3D11_QUERY_TIMESTAMP_DISJOINT),start=query(D3D11_QUERY_TIMESTAMP),end=query(D3D11_QUERY_TIMESTAMP),pipeline=query(D3D11_QUERY_PIPELINE_STATISTICS);
            context->Begin(disjoint.Get());context->Begin(pipeline.Get());context->End(start.Get());
            const auto before=std::chrono::steady_clock::now();std::string error;
            if(!glass.render(device.Get(),context.Get(),texture.Get(),view.Get(),std::span<const medicon::gui::LiquidGlassFrame>(panes),error))throw std::runtime_error(error);
            const auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-before).count();
            context->End(end.Get());context->End(pipeline.Get());context->End(disjoint.Get());context->Flush();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};UINT64 first=0,last=0;D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            result(context.Get(),disjoint.Get(),timing);result(context.Get(),start.Get(),first);result(context.Get(),end.Get(),last);result(context.Get(),pipeline.Get(),stats);
            if(timing.Disjoint||!timing.Frequency)throw std::runtime_error("Unreliable GPU timestamp");
            if(stats.IAVertices!=static_cast<UINT64>((5+count)*3))throw std::runtime_error("Batch duplicated the shared downsample/blur passes");
            const double gpu_us=static_cast<double>(last-first)*1e6/static_cast<double>(timing.Frequency);
            if(frame>=3){cpu+=elapsed;gpu+=gpu_us;max_cpu=std::max(max_cpu,elapsed);max_gpu=std::max(max_gpu,gpu_us);}
        }
        out<<size.first<<','<<size.second<<','<<scale<<','<<count<<','<<cpu/10<<','<<max_cpu<<','<<gpu/10<<','<<max_gpu<<','<<5+count<<'\n';
        // Resources from a resized/recreated target are released by the same existing renderer.
        glass.reset();if(glass.loaded())throw std::runtime_error("Native resources survived reset");
    }
}
}
int main(int argc,char** argv){try{
    if(argc>1){const std::filesystem::path path=argv[1];std::filesystem::create_directories(path.parent_path());std::ofstream out(path);out<<std::fixed<<std::setprecision(3);profile(out);if(!out)throw std::runtime_error("Cannot save performance record");std::cout<<"Saved "<<path.string()<<'\n';}
    else profile(std::cout);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
