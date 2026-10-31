#include "medicon/gui/EdgeLit.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace medicon::gui {
namespace {
using Microsoft::WRL::ComPtr;
struct PublishedFrame { EdgeLitFrame frame; int number = -1; };
thread_local std::unordered_map<ImGuiContext*, PublishedFrame> published;
float clamp_alpha(float value) { return std::clamp(value, 0.0F, 1.0F); }
ImVec4 alpha(ImVec4 value, float opacity) { value.w = clamp_alpha(opacity); return value; }
bool check(HRESULT result, const char* operation, std::string& error) {
    if (SUCCEEDED(result)) return true;
    char message[200]{};
    std::snprintf(message, sizeof message, "Edge-lit: %s failed (HRESULT 0x%08lX).", operation,
        static_cast<unsigned long>(result));
    error = message;
    return false;
}

// Every shader produces premultiplied RGBA. Light is composited with ONE /
// INV_SRC_ALPHA, never an additive blend. Convolution filters RGBA together.
constexpr const char* shader_source = R"hlsl(
cbuffer Params : register(b0) {
    float4 view; // viewport width/height in physical pixels, world origin x/y
    float4 rect;
    float4 corners; // top-left, bottom-left, top-right, bottom-right
    float4 colorA;
    float4 colorB;
    float4 opts; // shape mode, opacity, reflection alpha, world units per pixel
    float4 roi; // world origin and physical extent of the region
    float4 screen; // full frame width/height, blur dx/dy
    float4 blur; // paired sample count, center weight, reserved
    float4 weights[49];
};
Texture2D image : register(t0);
SamplerState linearBorder : register(s0);
SamplerState linearClamp : register(s1);
struct Varying { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
Varying QuadVS(uint id : SV_VertexID) {
    Varying o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2,-2) + float2(-1,1),0,1);
    o.uv = p; o.color = 1; return o;
}
Varying TextVS(float2 pos : POSITION, float2 uv : TEXCOORD0, float4 color : COLOR0) {
    Varying o;
    float2 p = (pos - view.zw) / (view.xy * opts.w);
    o.pos = float4(p * float2(2,-2) + float2(-1,1),0,1);
    o.uv = uv; o.color = color; return o;
}
float2 world(float4 p) { return p.xy * opts.w + view.zw; }
float coverage(float2 p) {
    float2 center = (rect.xy + rect.zw) * .5;
    float2 halfSize = (rect.zw - rect.xy) * .5;
    float r = p.x < center.x ? (p.y < center.y ? corners.x : corners.y)
                                    : (p.y < center.y ? corners.z : corners.w);
    r = min(r, min(halfSize.x, halfSize.y));
    float2 q = abs(p-center)-halfSize+r;
    float d = length(max(q,0)) + min(max(q.x,q.y),0)-r;
    return saturate(.5-d / opts.w);
}
float4 ShapePS(Varying i) : SV_TARGET {
    float2 p = world(i.pos);
    float cover = coverage(p);
    if (cover <= 0) discard;
    float t = saturate((p.y-rect.y)/max(.001,rect.w-rect.y));
    float4 c = lerp(colorA,colorB,t);
    if (opts.x == 1) { c.rgb=lerp(colorA.rgb,colorB.rgb,saturate((t-.15)/.70)); c.a *= min(saturate(t/.15),saturate((1-t)/.15)); }
    if (opts.x == 2) { c.rgb=lerp(colorA.rgb,colorB.rgb,saturate((t-.1)/.80)); c.a *= min(saturate(t/.1),saturate((1-t)/.1)); }
    if (opts.x == 4) c.a *= 1-abs(2*t-1);
    c.a = saturate(c.a * opts.y) * cover;
    return float4(c.rgb * c.a, c.a);
}
float4 BlurPS(Varying i) : SV_TARGET {
    float2 uv = i.pos.xy / view.xy;
    float4 c = image.SampleLevel(linearBorder,uv,0) * blur.y;
    [loop] for (int k=0;k<(int)blur.x;++k) {
        float2 offset = float2(screen.z,screen.w) * weights[k].y;
        c += (image.SampleLevel(linearBorder,uv+offset,0)
            + image.SampleLevel(linearBorder,uv-offset,0)) * weights[k].x;
    }
    return c;
}
float4 CompositePS(Varying i) : SV_TARGET {
    float2 p = world(i.pos);
    float2 uv = (p-roi.xy)/roi.zw;
    return image.SampleLevel(linearBorder,uv,0) * opts.y;
}
float4 CapturePS(Varying i) : SV_TARGET {
    return image.SampleLevel(linearClamp,world(i.pos)/screen.xy,0);
}
float4 PlatePS(Varying i) : SV_TARGET {
    float2 p=world(i.pos);
    float cover=coverage(p);
    if (cover<=0) discard;
    float3 bg=image.SampleLevel(linearClamp,(p-roi.xy)/roi.zw,0).rgb;
    float gray=dot(bg,float3(.2126,.7152,.0722));
    bg=saturate(lerp(gray.xxx,bg,1.3));
    float3 plate=lerp(bg,float3(10,11,17)/255,.66);
    float d=(rect.z-p.x)/opts.z;
    float reflect=d<=38 ? lerp(1,.35,saturate(d/38)) : .35*saturate((104-d)/66);
    float a=saturate(colorA.a*reflect);
    plate=lerp(plate,colorA.rgb,a);
    float opacity=cover*opts.y;
    return float4(plate*opacity,opacity);
}
float4 TextPS(Varying i) : SV_TARGET {
    float a=image.SampleLevel(linearClamp,i.uv,0).a*i.color.a;
    return float4(i.color.rgb*a,a);
}
)hlsl";

struct alignas(16) Parameters {
    float view[4]{}, rect[4]{}, corners[4]{}, colorA[4]{}, colorB[4]{}, opts[4]{}, roi[4]{}, screen[4]{}, blur[4]{};
    float weights[196]{};
};
void put(float (&out)[4], ImVec4 value) { out[0]=value.x; out[1]=value.y; out[2]=value.z; out[3]=value.w; }
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> source;
    UINT width=0,height=0;
    bool create(ID3D11Device* device,UINT w,UINT h,std::string& error) {
        if (texture && width==w && height==h) return true;
        *this={};
        D3D11_TEXTURE2D_DESC d{};
        d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1; d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count=1; d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if (!check(device->CreateTexture2D(&d,nullptr,texture.GetAddressOf()),"create effect buffer",error) ||
            !check(device->CreateRenderTargetView(texture.Get(),nullptr,target.GetAddressOf()),"create effect target",error) ||
            !check(device->CreateShaderResourceView(texture.Get(),nullptr,source.GetAddressOf()),"create effect view",error)) return false;
        width=w; height=h; return true;
    }
};
}

void publish_edge_lit_frame(EdgeLitFrame frame) {
    if (auto* context=ImGui::GetCurrentContext()) published[context]={std::move(frame),ImGui::GetFrameCount()};
}
const EdgeLitFrame* current_edge_lit_frame() {
    const auto found=published.find(ImGui::GetCurrentContext());
    if (found==published.end() || found->second.number!=ImGui::GetFrameCount()) return nullptr;
    return &found->second.frame;
}
void forget_edge_lit_frame(ImGuiContext* context) { published.erase(context); forget_liquid_glass(context); }

struct EdgeLitD3D11::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> commands;
    ComPtr<ID3D11VertexShader> quadVS,textVS;
    ComPtr<ID3D11PixelShader> shapePS,blurPS,compositePS,capturePS,platePS,textPS;
    ComPtr<ID3D11Buffer> constants,vertices,indices;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11BlendState> blend,replace,unionBlend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> borderSampler,clampSampler;
    ComPtr<ID3D11Texture2D> snapshot;
    ComPtr<ID3D11ShaderResourceView> snapshotView,fontView;
    Surface full[2],half[2],glass;
    UINT snapshotWidth=0,snapshotHeight=0;
    DXGI_FORMAT snapshotFormat=DXGI_FORMAT_UNKNOWN;
    ImFontAtlas* atlas=nullptr;
    UINT vertexCapacity=0,indexCapacity=0;
    ImDrawList glyphs{ImGui::GetDrawListSharedData()};
    Parameters p{};
    float roiX=0,roiY=0,roiWidth=0,roiHeight=0;
    UINT frameWidth=0,frameHeight=0;

    bool initialize(ID3D11Device* requested,std::string& error) {
        device=requested;
        if (!check(device->CreateDeferredContext(0,commands.GetAddressOf()),"create isolated command context",error)) return false;
        auto compile=[&](const char* name,const char* profile,ComPtr<ID3DBlob>& result) {
            ComPtr<ID3DBlob> messages;
            const auto hr=D3DCompile(shader_source,std::strlen(shader_source),"EdgeLit",nullptr,nullptr,name,profile,
                D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,result.GetAddressOf(),messages.GetAddressOf());
            if (!check(hr,name,error)) {
                if(messages) error += " " + std::string(static_cast<const char*>(messages->GetBufferPointer()),messages->GetBufferSize());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> bytecode;
        if (!compile("QuadVS","vs_4_0",bytecode) || !check(device->CreateVertexShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,quadVS.GetAddressOf()),"create quad shader",error)) return false;
        bytecode.Reset();
        if (!compile("TextVS","vs_4_0",bytecode) || !check(device->CreateVertexShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,textVS.GetAddressOf()),"create text vertex shader",error)) return false;
        const D3D11_INPUT_ELEMENT_DESC layout[]{
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,static_cast<UINT>(offsetof(ImDrawVert,pos)),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,static_cast<UINT>(offsetof(ImDrawVert,uv)),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,static_cast<UINT>(offsetof(ImDrawVert,col)),D3D11_INPUT_PER_VERTEX_DATA,0}};
        if(!check(device->CreateInputLayout(layout,3,bytecode->GetBufferPointer(),bytecode->GetBufferSize(),inputLayout.GetAddressOf()),"create glyph input layout",error)) return false;
        const std::pair<const char*,ComPtr<ID3D11PixelShader>*> pixels[]{
            {"ShapePS",&shapePS},{"BlurPS",&blurPS},{"CompositePS",&compositePS},
            {"CapturePS",&capturePS},{"PlatePS",&platePS},{"TextPS",&textPS}};
        for(const auto& item:pixels) {
            bytecode.Reset();
            if(!compile(item.first,"ps_4_0",bytecode) || !check(device->CreatePixelShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,item.second->GetAddressOf()),item.first,error)) return false;
        }
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=sizeof(Parameters); buffer.Usage=D3D11_USAGE_DYNAMIC;
        buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(!check(device->CreateBuffer(&buffer,nullptr,constants.GetAddressOf()),"create effect constants",error)) return false;
        D3D11_BLEND_DESC b{}; b.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(!check(device->CreateBlendState(&b,replace.GetAddressOf()),"create replace blend",error)) return false;
        auto& r=b.RenderTarget[0]; r.BlendEnable=TRUE; r.SrcBlend=D3D11_BLEND_ONE; r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOp=D3D11_BLEND_OP_ADD; r.SrcBlendAlpha=D3D11_BLEND_ONE; r.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; r.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        if(!check(device->CreateBlendState(&b,blend.GetAddressOf()),"create premultiplied source-over blend",error)) return false;
        // MAX is only used to rasterize a coverage union into an offscreen mask.
        // Its blurred result is still composited onto the scene by source-over.
        r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_MAX;
        r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ONE;
        if(!check(device->CreateBlendState(&b,unionBlend.GetAddressOf()),"create shadow union mask blend",error)) return false;
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
        raster.ScissorEnable=TRUE; raster.DepthClipEnable=TRUE;
        if(!check(device->CreateRasterizerState(&raster,rasterizer.GetAddressOf()),"create effect rasterizer",error)) return false;
        D3D11_DEPTH_STENCIL_DESC z{}; z.DepthEnable=FALSE;
        if(!check(device->CreateDepthStencilState(&z,depth.GetAddressOf()),"create effect depth state",error)) return false;
        D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;
        sample.ComparisonFunc=D3D11_COMPARISON_NEVER; sample.MaxLOD=D3D11_FLOAT32_MAX;
        if(!check(device->CreateSamplerState(&sample,borderSampler.GetAddressOf()),"create transparent-border sampler",error)) return false;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        return check(device->CreateSamplerState(&sample,clampSampler.GetAddressOf()),"create background sampler",error);
    }

    void viewport(ID3D11RenderTargetView* target,UINT w,UINT h,float x,float y,float units=1) {
        ID3D11ShaderResourceView* empty=nullptr; commands->PSSetShaderResources(0,1,&empty);
        commands->OMSetRenderTargets(1,&target,nullptr);
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(w),static_cast<float>(h),0,1};
        commands->RSSetViewports(1,&viewport);
        D3D11_RECT scissor{0,0,static_cast<LONG>(w),static_cast<LONG>(h)}; commands->RSSetScissorRects(1,&scissor);
        p.view[0]=static_cast<float>(w); p.view[1]=static_cast<float>(h); p.view[2]=x; p.view[3]=y; p.opts[3]=units;
        p.roi[0]=roiX; p.roi[1]=roiY; p.roi[2]=roiWidth; p.roi[3]=roiHeight;
        p.screen[0]=static_cast<float>(frameWidth); p.screen[1]=static_cast<float>(frameHeight);
    }
    void viewport(Surface& target,float units) { viewport(target.target.Get(),target.width,target.height,roiX,roiY,units); }
    void scissor(float left,float top,float right,float bottom) {
        const float unit=p.opts[3];
        const auto x=[&](float value) { return static_cast<LONG>(std::clamp(value,0.0F,p.view[0])); };
        const auto y=[&](float value) { return static_cast<LONG>(std::clamp(value,0.0F,p.view[1])); };
        D3D11_RECT clip{x(std::floor((left-p.view[2])/unit)),y(std::floor((top-p.view[3])/unit)),
                        x(std::ceil((right-p.view[2])/unit)),y(std::ceil((bottom-p.view[3])/unit))};
        commands->RSSetScissorRects(1,&clip);
    }
    void whole_scissor() {
        D3D11_RECT clip{0,0,static_cast<LONG>(p.view[0]),static_cast<LONG>(p.view[1])};
        commands->RSSetScissorRects(1,&clip);
    }
    bool upload(std::string& error) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if(!check(commands->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map),"upload effect constants",error)) return false;
        std::memcpy(map.pData,&p,sizeof p); commands->Unmap(constants.Get(),0);
        ID3D11Buffer* buffer=constants.Get(); commands->VSSetConstantBuffers(0,1,&buffer); commands->PSSetConstantBuffers(0,1,&buffer);
        return true;
    }
    bool quad(ID3D11PixelShader* shader,ID3D11ShaderResourceView* source,std::string& error,bool over=true,bool maskUnion=false) {
        if(!upload(error)) return false;
        commands->OMSetBlendState(maskUnion?unionBlend.Get():(over?blend.Get():replace.Get()),nullptr,0xFFFFFFFF);
        commands->IASetInputLayout(nullptr); commands->VSSetShader(quadVS.Get(),nullptr,0);
        commands->PSSetShader(shader,nullptr,0); commands->PSSetShaderResources(0,1,&source);
        commands->Draw(3,0); return true;
    }
    bool shape(float left,float top,float right,float bottom,ImVec4 a,ImVec4 b,
               float tl,float bl,float tr,float br,int mode,std::string& error) {
        if(right<=left || bottom<=top) return true;
        p.rect[0]=left; p.rect[1]=top; p.rect[2]=right; p.rect[3]=bottom;
        p.corners[0]=tl; p.corners[1]=bl; p.corners[2]=tr; p.corners[3]=br;
        put(p.colorA,a); put(p.colorB,b); p.opts[0]=static_cast<float>(mode); p.opts[1]=1;
        scissor(left-p.opts[3],top-p.opts[3],right+p.opts[3],bottom+p.opts[3]);
        const bool result=quad(shapePS.Get(),nullptr,error,true,mode==5);
        whole_scissor();
        return result;
    }
    bool gaussian(Surface* pool,float sigma,std::string& error) {
        const int radius=static_cast<int>(std::ceil(3*sigma));
        if(radius<=0) return true;
        if(radius>96) { error="Edge-lit Gaussian radius exceeds supported scale range."; return false; }
        std::array<double,97> weights{};
        weights[0]=1;
        double sum=1;
        for(int i=1;i<=radius;++i) {
            weights[i]=std::exp(-(i*i)/(2.0*sigma*sigma));
            sum+=2*weights[i];
        }
        // A linear sample at the weighted midpoint is exactly the sum of two
        // neighboring Gaussian taps. Keep the center tap separate and sample
        // each pair symmetrically: roughly half as many texture reads without
        // changing the kernel, including its transparent-border behavior.
        int pairs=0;
        for(int i=1;i<=radius;i+=2,++pairs) {
            const double second=i+1<=radius?weights[i+1]:0;
            const double combined=weights[i]+second;
            p.weights[pairs*4]=static_cast<float>(combined/sum);
            p.weights[pairs*4+1]=static_cast<float>(i+second/combined);
        }
        p.blur[0]=static_cast<float>(pairs);
        p.blur[1]=static_cast<float>(1/sum);
        const float units=roiWidth/pool[0].width;
        viewport(pool[1],units); p.screen[2]=1.0F/pool[0].width; p.screen[3]=0;
        if(!quad(blurPS.Get(),pool[0].source.Get(),error,false)) return false;
        viewport(pool[0],units); p.screen[2]=0; p.screen[3]=1.0F/pool[1].height;
        return quad(blurPS.Get(),pool[1].source.Get(),error,false);
    }
    bool composite(ID3D11RenderTargetView* target,Surface& source,float opacity,std::string& error) {
        viewport(target,frameWidth,frameHeight,0,0); p.opts[1]=clamp_alpha(opacity);
        scissor(roiX-1,roiY-1,roiX+roiWidth+1,roiY+roiHeight+1);
        const bool result=quad(compositePS.Get(),source.source.Get(),error);
        whole_scissor();
        return result;
    }
    void clear(Surface& surface) { const float transparent[4]{}; commands->ClearRenderTargetView(surface.target.Get(),transparent); }
    Surface* pool(float sigma) { return sigma>=8 ? half : full; }
    float units(float sigma) { return sigma>=8 ? 2.0F : 1.0F; }

    bool ensure_snapshot(ID3D11Texture2D* backBuffer,std::string& error) {
        D3D11_TEXTURE2D_DESC d{}; backBuffer->GetDesc(&d);
        frameWidth=d.Width; frameHeight=d.Height;
        if(snapshot && snapshotWidth==d.Width && snapshotHeight==d.Height && snapshotFormat==d.Format) return true;
        snapshot.Reset(); snapshotView.Reset(); snapshotWidth=d.Width; snapshotHeight=d.Height; snapshotFormat=d.Format;
        d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags=0; d.MiscFlags=0;
        d.SampleDesc.Count=1; d.SampleDesc.Quality=0; d.MipLevels=1; d.ArraySize=1;
        if(d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS) d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        if(d.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS) d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        if(!check(device->CreateTexture2D(&d,nullptr,snapshot.GetAddressOf()),"create background capture",error)) return false;
        return check(device->CreateShaderResourceView(snapshot.Get(),nullptr,snapshotView.GetAddressOf()),"create background capture view",error);
    }
    bool ensure_font(std::string& error) {
        auto* wanted=ImGui::GetIO().Fonts;
        if(fontView && atlas==wanted) return true;
        unsigned char* pixels=nullptr; int width=0,height=0; wanted->GetTexDataAsRGBA32(&pixels,&width,&height);
        D3D11_TEXTURE2D_DESC d{}; d.Width=static_cast<UINT>(width); d.Height=static_cast<UINT>(height);
        d.MipLevels=1; d.ArraySize=1; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_IMMUTABLE; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels,static_cast<UINT>(width*4),0}; ComPtr<ID3D11Texture2D> texture;
        if(!check(device->CreateTexture2D(&d,&data,texture.GetAddressOf()),"upload native-resolution font atlas",error)) return false;
        fontView.Reset();
        if(!check(device->CreateShaderResourceView(texture.Get(),nullptr,fontView.GetAddressOf()),"create font atlas view",error)) return false;
        atlas=wanted; return true;
    }
    void begin_text() {
        glyphs._ResetForNewFrame();
        glyphs.Flags|=ImDrawListFlags_AllowVtxOffset;
        glyphs.PushTextureID(atlas->TexID);
    }
    void text(const EdgeLitText& text,const EdgeLitRow& row,ImVec2 offset,ImVec4 color) {
        if(!text.font || text.value.empty() || color.w<=0) return;
        glyphs.PushClipRect(ImVec2(row.left,row.top),ImVec2(row.right,row.bottom));
        const ImVec2 desired(text.pos.x+offset.x,text.pos.y+offset.y);
        const ImVec2 integral(std::floor(desired.x),std::floor(desired.y));
        const int first=glyphs.VtxBuffer.Size;
        glyphs.AddText(text.font,text.size,integral,
            ImGui::ColorConvertFloat4ToU32(color),text.value.c_str(),nullptr,0);
        // ImFont::RenderText snaps its origin; restore fractional animation on
        // the completed mesh while retaining sharp rasterized glyphs.
        for(int i=first;i<glyphs.VtxBuffer.Size;++i) {
            auto& vertex=glyphs.VtxBuffer[i];
            vertex.pos.x+=desired.x-integral.x;
            vertex.pos.y+=desired.y-integral.y;
        }
        glyphs.PopClipRect();
    }
    bool draw_text(std::string& error) {
        // Reuse a single mesh and upload once per text layer, while retaining
        // each row's clipping rectangle in its draw command.
        auto& list=glyphs;
        list.PopTextureID();
        if(list.VtxBuffer.empty() || list.IdxBuffer.empty()) return true;
        auto allocate=[&](ComPtr<ID3D11Buffer>& buffer,UINT& capacity,UINT bytes,UINT bind) {
            if(buffer && capacity>=bytes) return true;
            buffer.Reset(); capacity=bytes+4096;
            D3D11_BUFFER_DESC d{}; d.ByteWidth=capacity; d.Usage=D3D11_USAGE_DYNAMIC; d.BindFlags=bind; d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            return check(device->CreateBuffer(&d,nullptr,buffer.GetAddressOf()),"allocate glyph mesh",error);
        };
        const UINT vbytes=static_cast<UINT>(list.VtxBuffer.Size*sizeof(ImDrawVert));
        const UINT ibytes=static_cast<UINT>(list.IdxBuffer.Size*sizeof(ImDrawIdx));
        if(!allocate(vertices,vertexCapacity,vbytes,D3D11_BIND_VERTEX_BUFFER) || !allocate(indices,indexCapacity,ibytes,D3D11_BIND_INDEX_BUFFER)) return false;
        D3D11_MAPPED_SUBRESOURCE map{};
        if(!check(commands->Map(vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map),"upload glyph vertices",error)) return false;
        std::memcpy(map.pData,list.VtxBuffer.Data,vbytes); commands->Unmap(vertices.Get(),0);
        if(!check(commands->Map(indices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map),"upload glyph indices",error)) return false;
        std::memcpy(map.pData,list.IdxBuffer.Data,ibytes); commands->Unmap(indices.Get(),0);
        if(!upload(error)) return false;
        const UINT stride=sizeof(ImDrawVert),zero=0; ID3D11Buffer* v=vertices.Get();
        commands->IASetInputLayout(inputLayout.Get()); commands->IASetVertexBuffers(0,1,&v,&stride,&zero);
        commands->IASetIndexBuffer(indices.Get(),sizeof(ImDrawIdx)==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT,0);
        commands->VSSetShader(textVS.Get(),nullptr,0); commands->PSSetShader(textPS.Get(),nullptr,0);
        ID3D11ShaderResourceView* font=fontView.Get(); commands->PSSetShaderResources(0,1,&font);
        commands->OMSetBlendState(blend.Get(),nullptr,0xFFFFFFFF);
        for(const auto& cmd:list.CmdBuffer) if(cmd.ElemCount) {
            scissor(cmd.ClipRect.x,cmd.ClipRect.y,cmd.ClipRect.z,cmd.ClipRect.w);
            commands->DrawIndexed(cmd.ElemCount,cmd.IdxOffset,static_cast<INT>(cmd.VtxOffset));
        }
        whole_scissor();
        return true;
    }

    bool render(ID3D11DeviceContext* immediate,ID3D11Texture2D* backBuffer,ID3D11RenderTargetView* target,const EdgeLitFrame& frame,std::string& error) {
        if(!ensure_snapshot(backBuffer,error) || !ensure_font(error)) return false;
        const float s=frame.scale;
        float left=frame.screenWidth-12*s;
        for(const auto& row:frame.rows) left=std::min(left,row.left);
        roiX=std::floor(left-54*s); roiY=std::floor(frame.top-68*s);
        roiWidth=std::ceil((frame.screenWidth+76*s-roiX)/2)*2;
        roiHeight=std::ceil((frame.top+frame.height+80*s-roiY)/2)*2;
        // Animated row widths/heights used to recreate five render targets on
        // almost every frame. Grow in small blocks and reuse capacity across
        // normal animation; shrink only after a dimension falls to half size,
        // so reducing HUD scale/row count does not retain a huge blur surface.
        // Use that same extent for every pass so UVs remain unscaled.
        const auto capacity=[](float desired,UINT current) {
            const auto pixels=static_cast<UINT>(std::max(2.0F,desired));
            const auto bucket=(pixels+63U)&~63U;
            return bucket<=current/2U?bucket:std::max(current,bucket);
        };
        const auto w=capacity(roiWidth,full[0].width),h=capacity(roiHeight,full[0].height);
        roiWidth=static_cast<float>(w); roiHeight=static_cast<float>(h);
        for(auto& surface:full) if(!surface.create(device.Get(),w,h,error)) return false;
        for(auto& surface:half) if(!surface.create(device.Get(),w/2,h/2,error)) return false;
        const float glassSigma=10*s;
        const auto glassW=glassSigma>=8?w/2:w,glassH=glassSigma>=8?h/2:h;
        if(!glass.create(device.Get(),glassW,glassH,error)) return false;
        commands->ClearState(); commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->RSSetState(rasterizer.Get()); commands->OMSetDepthStencilState(depth.Get(),0);
        ID3D11SamplerState* samplers[]{borderSampler.Get(),clampSampler.Get()}; commands->PSSetSamplers(0,2,samplers);
        const float right=frame.screenWidth-9*s, bottom=frame.top+frame.height;
        // 1. Opaque union mask avoids doubled opacity where expanded rows overlap.
        if(frame.shadow>0) {
            auto* buffers=pool(11*s); const float unit=units(11*s); clear(buffers[0]); viewport(buffers[0],unit);
            for(const auto& row:frame.rows) {
                const ImVec4 c{2.0F/255,3.0F/255,10.0F/255,1};
                if(!shape(row.left-11*s,row.top, row.right-5*s,row.bottom+6*s,c,c,0,0,0,0,5,error)) return false;
            }
            if(!gaussian(buffers,11*s/unit,error) || !composite(target,buffers[0],.62F*frame.shadow,error)) return false;
        }
        // 2. World bloom is present before the glass samples its background.
        if(frame.glow>0) {
            auto* buffers=pool(16*s); const float unit=units(16*s); clear(buffers[0]); viewport(buffers[0],unit);
            if(!shape(frame.screenWidth-25*s,frame.top-16*s,frame.screenWidth+15*s,bottom+16*s,
                alpha(frame.accentA,.42F*frame.glow),alpha(frame.accentB,.42F*frame.glow),0,0,0,0,1,error) ||
                !gaussian(buffers,16*s/unit,error) || !composite(target,buffers[0],1,error)) return false;
        }
        // Capture the completed shadow+bloom frame, not a previously rendered plate.
        ID3D11ShaderResourceView* empty=nullptr; commands->PSSetShaderResources(0,1,&empty); commands->OMSetRenderTargets(0,nullptr,nullptr);
        D3D11_TEXTURE2D_DESC backDesc{}; backBuffer->GetDesc(&backDesc);
        if(backDesc.SampleDesc.Count>1) commands->ResolveSubresource(snapshot.Get(),0,backBuffer,0,snapshotFormat);
        else {
            // Only the pixels sampled by CapturePS are needed. Keep the full
            // snapshot coordinates (and edge clamping) while avoiding a full
            // screen copy for a small HUD on every game frame.
            const auto clamp_x=[&](float value) { return static_cast<UINT>(std::clamp(value,0.0F,static_cast<float>(frameWidth))); };
            const auto clamp_y=[&](float value) { return static_cast<UINT>(std::clamp(value,0.0F,static_cast<float>(frameHeight))); };
            const D3D11_BOX region{clamp_x(roiX),clamp_y(roiY),0,
                clamp_x(roiX+roiWidth),clamp_y(roiY+roiHeight),1};
            if(region.right>region.left && region.bottom>region.top)
                commands->CopySubresourceRegion(snapshot.Get(),0,region.left,region.top,0,backBuffer,0,&region);
        }
        auto* glassBuffers=pool(glassSigma); const float glassUnits=units(glassSigma); viewport(glassBuffers[0],glassUnits);
        if(!quad(capturePS.Get(),snapshotView.Get(),error,false) || !gaussian(glassBuffers,glassSigma/glassUnits,error)) return false;
        commands->PSSetShaderResources(0,1,&empty); commands->OMSetRenderTargets(0,nullptr,nullptr);
        commands->CopyResource(glass.texture.Get(),glassBuffers[0].texture.Get());
        // 3. Per-row frosted glass and the tube's diminishing reflected light.
        viewport(target,frameWidth,frameHeight,0,0);
        for(const auto& row:frame.rows) {
            p.rect[0]=row.left; p.rect[1]=row.top; p.rect[2]=row.right; p.rect[3]=row.bottom;
            p.corners[0]=row.roundTopLeft; p.corners[1]=row.roundBottomLeft; p.corners[2]=p.corners[3]=0;
            put(p.colorA,alpha(row.color,(.36F+.24F*row.lighting)*frame.glow));
            p.opts[1]=clamp_alpha(row.opacity); p.opts[2]=s;
            scissor(row.left-1,row.top-1,row.right+1,row.bottom+1);
            if(!quad(platePS.Get(),glass.source.Get(),error)) return false;
        }
        whole_scissor();
        // 4. Gaussian mode halos, then Gaussian contact shadows, then sharp glyphs.
        if(frame.glow>0) {
            clear(full[0]); viewport(full[0],1);
            begin_text();
            for(const auto& row:frame.rows) text(row.mode,row,{0,0},alpha(row.color,
                clamp_alpha((.60F+.30F*row.lighting)*frame.glow*row.mode.color.w)));
            if(!draw_text(error) || !gaussian(full,3.5F*s,error) || !composite(target,full[0],1,error)) return false;
        }
        if(frame.shadow>0) {
            clear(full[0]); viewport(full[0],1);
            begin_text();
            for(const auto& row:frame.rows) {
                text(row.name,row,{-s,s},{0,0,0,clamp_alpha(.55F*frame.shadow*row.name.color.w)});
                text(row.mode,row,{-s,s},{0,0,0,clamp_alpha(.50F*frame.shadow*row.mode.color.w)});
            }
            if(!draw_text(error) || !gaussian(full,.5F*s,error) || !composite(target,full[0],1,error)) return false;
        }
        viewport(target,frameWidth,frameHeight,0,0);
        begin_text();
        for(const auto& row:frame.rows) {
            text(row.name,row,{0,0},row.name.color);
            text(row.mode,row,{0,0},row.mode.color);
        }
        if(!draw_text(error)) return false;
        // 5a. Tube glow. 5b/5c remain unchanged when glow is zero.
        if(frame.glow>0) {
            clear(full[0]); viewport(full[0],1);
            if(!shape(frame.screenWidth-12*s,frame.top-3*s,frame.screenWidth-3*s,bottom+3*s,
                alpha(frame.accentA,.95F*frame.glow),alpha(frame.accentB,.95F*frame.glow),5*s,5*s,5*s,5*s,2,error) ||
               !gaussian(full,4*s,error) || !composite(target,full[0],1,error)) return false;
        }
        viewport(target,frameWidth,frameHeight,0,0);
        if(!shape(right,frame.top,right+3*s,bottom,alpha(frame.accentA,1),alpha(frame.accentB,1),2*s,2*s,2*s,2*s,0,error) ||
           !shape(frame.screenWidth-8*s,frame.top+2*s,frame.screenWidth-7*s,bottom-2*s,{1,1,1,.85F},{1,1,1,.55F},0,0,0,0,0,error)) return false;
        // 6. Each independent travelling pulse is bounded by the CPU state machine.
        if(frame.glow>0 && !frame.pulses.empty()) {
            for(const auto& pulse:frame.pulses) {
                clear(full[0]); viewport(full[0],1);
                const float cx=frame.screenWidth-7.5F*s;
                ImVec4 c=pulse.color; c.x+=(1-c.x)*.35F; c.y+=(1-c.y)*.35F; c.z+=(1-c.z)*.35F;
                c.w=clamp_alpha(.9F*frame.glow*pulse.opacity);
                if(!shape(cx-9.5F*s,pulse.centerY-20*s,cx+9.5F*s,pulse.centerY+20*s,c,c,10*s,10*s,10*s,10*s,0,error)) return false;
                if(!gaussian(full,6*s,error) || !composite(target,full[0],1,error)) return false;
            }
            viewport(target,frameWidth,frameHeight,0,0);
            for(const auto& pulse:frame.pulses) {
                const auto white=ImVec4(1,1,1,clamp_alpha(frame.glow*pulse.opacity));
                if(!shape(right,pulse.centerY-14*s,right+3*s,pulse.centerY+14*s,white,white,0,0,0,0,4,error)) return false;
            }
        }
        commands->PSSetShaderResources(0,1,&empty); commands->OMSetRenderTargets(0,nullptr,nullptr);
        ComPtr<ID3D11CommandList> list;
        if(!check(commands->FinishCommandList(FALSE,list.GetAddressOf()),"finish effect commands",error)) return false;
        immediate->ExecuteCommandList(list.Get(),TRUE);
        return check(device->GetDeviceRemovedReason(),"execute effect commands",error);
    }
};

EdgeLitD3D11::EdgeLitD3D11()=default;
EdgeLitD3D11::~EdgeLitD3D11()=default;
void EdgeLitD3D11::reset() { impl_.reset(); glass_.reset(); }
void EdgeLitD3D11::invalidate_fonts() { if(impl_) { impl_->fontView.Reset(); impl_->atlas=nullptr; } }
bool EdgeLitD3D11::render(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* backBuffer,ID3D11RenderTargetView* target,std::string& error) {
    const auto* frame=current_edge_lit_frame();
    const auto* pane=current_liquid_glass();
    if(frame && frame->height>0) {
        if(!device || !context || !backBuffer || !target) { error="Edge-lit requires a complete D3D11 render target."; return false; }
        if(!impl_ || impl_->device.Get()!=device) {
            impl_=std::make_unique<Impl>();
            if(!impl_->initialize(device,error)) { impl_.reset(); return false; }
        }
        if (!impl_->render(context,backBuffer,target,*frame,error)) {
            // Discard any partially recorded command list before a retry; it may
            // otherwise retain a swap-chain resource through ResizeBuffers.
            impl_.reset();
            return false;
        }
    }
    if(pane) return glass_.render(device,context,backBuffer,target,*pane,error);
    if(glass_.loaded() || !device || !context || !backBuffer || !target) return true;
    // Load at injection, while nothing is on screen to hitch: D3D11On12 builds its
    // pipeline at the first draw, so one real, fully transparent pane goes through.
    D3D11_TEXTURE2D_DESC back{}; backBuffer->GetDesc(&back);
    LiquidGlassFrame unseen;
    unseen.center={back.Width*.5F,back.Height*.5F};
    unseen.size={120,56};
    unseen.lens=unseen.opacity=0;
    return glass_.render(device,context,backBuffer,target,unseen,error);
}
} // namespace medicon::gui
