#include "medicon/gui/LiquidGlass.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <medicon/gui/Accessibility.hpp>

namespace medicon::gui {
namespace {
using Microsoft::WRL::ComPtr;
struct PublishedFrame { std::vector<LiquidGlassFrame> frames; int number = -1; bool ready=false; };
thread_local std::unordered_map<ImGuiContext*, PublishedFrame> published;

bool check(HRESULT result, const char* operation, std::string& error) {
    if (SUCCEEDED(result)) return true;
    char message[200]{};
    std::snprintf(message, sizeof message, "Liquid Glass: %s failed (HRESULT 0x%08lX).", operation,
                  static_cast<unsigned long>(result));
    error = message;
    return false;
}

// The optics are ported from YMM4-LiquidGlass (MIT, Copyright (c) 2026
// namakemono-san; see THIRD_PARTY_NOTICES.md). A slab of clear glass lies on the
// scene. x runs from 0 at the outline to 1 where the bezel meets the flat top,
// and the bezel falls away as h = 1 - sqrt(1 - (1-x)^2). The scene is displaced
// by h * dh/dx: strongly at the rim with a long tail inward, and not at all on
// the flat, which therefore shows the scene pixel for pixel. Red and blue are
// displaced by different amounts for the prism. The rim squeezes a lot of scene
// into a few pixels and its highlight is a hairline, so the bezel is shaded at
// eight subpixel positions. Medicon adds the elastic waist, and absorption that
// deepens over bright scenes so the white digits stay legible.
constexpr const char* shader_source = R"hlsl(
cbuffer Glass : register(b0) {
    float4 shape;  // center, half size
    float4 form;   // corner radius, waist, bezel width, displacement reach
    float4 optics; // light direction on screen, lens, energy
    float4 region; // origin and extent of the captured scene
    float4 kernel; // blur direction in UV, UV per pixel of the target
    float4 misc;   // UV per pixel of the capture, opacity, scale
};
Texture2D sharp : register(t0);
Texture2D soft : register(t1);
SamplerState linearClamp : register(s0);
struct Varying { float4 pos : SV_POSITION; };
Varying QuadVS(uint id : SV_VertexID) {
    Varying o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2,-2) + float2(-1,1),0,1);
    return o;
}
float4 DownPS(Varying i) : SV_TARGET {
    return float4(sharp.SampleLevel(linearClamp,i.pos.xy*kernel.zw,0).rgb,1);
}
float4 BlurPS(Varying i) : SV_TARGET {
    float2 uv = i.pos.xy*kernel.zw;
    float3 c = sharp.SampleLevel(linearClamp,uv,0).rgb*.2270270270;
    c += (sharp.SampleLevel(linearClamp,uv+kernel.xy*1.3846153846,0).rgb
        + sharp.SampleLevel(linearClamp,uv-kernel.xy*1.3846153846,0).rgb)*.3162162162;
    c += (sharp.SampleLevel(linearClamp,uv+kernel.xy*3.2307692308,0).rgb
        + sharp.SampleLevel(linearClamp,uv-kernel.xy*3.2307692308,0).rgb)*.0702702703;
    return float4(c,1);
}
float smoothMin(float a, float b, float k) {
    float h = saturate(.5+.5*(b-a)/k);
    return lerp(b,a,h)-k*h*(1-h);
}
float smoothMax(float a, float b, float k) {
    float h = saturate(.5+.5*(a-b)/k);
    return lerp(b,a,h)+k*h*(1-h);
}
// A rounded box with smoothed corner joins. The waist bends its height along x:
// the elastic deformation.
float pane(float2 p) {
    float along = saturate(abs(p.x)/shape.z);
    p.y /= 1+form.y*(1-along*along);
    float k = 5*misc.w;
    float2 q = abs(p)-shape.zw+form.x;
    return smoothMin(smoothMax(q.x,q.y,k),0,k*.5)+length(float2(smoothMax(q.x,0,k),smoothMax(q.y,0,k)))-form.x;
}
float field(float2 p) { return saturate(-pane(p)/form.z); }
float heightAt(float x) { return 1-sqrt(saturate(1-(1-x)*(1-x))); }
float2 sceneUv(float2 world) {
    return (clamp(world,region.xy,region.xy+region.zw)-region.xy)*misc.xy;
}
float3 scene(float2 world) { return sharp.SampleLevel(linearClamp,sceneUv(world),0).rgb; }
static const float3 tint = float3(.70,.74,.82);
static const float aberration = .1;
// The standard eight-sample pattern, in pixels.
static const float2 subpixel[8] = {
    float2(1,-3)/16, float2(-1,3)/16, float2(5,1)/16, float2(-3,-5)/16,
    float2(-5,5)/16, float2(-7,-1)/16, float2(3,7)/16, float2(7,-7)/16};
// Premultiplied glass and its coverage at one point.
float4 shade(float2 world, float density) {
    float2 p = world-shape.xy;
    float lens = optics.z, energy = optics.w;
    float cover = saturate(.5-pane(p)*2);
    if (cover <= 0) return 0;

    float bezel = form.z;
    float x = field(p);
    float height = heightAt(x);
    float slope = (heightAt(saturate(x+.03))-height)/.03;
    float e = max(bezel*.08,1);
    float2 gradient = float2(field(p+float2(e,0))-field(p-float2(e,0)),field(p+float2(0,e))-field(p-float2(0,e)));
    float size = length(gradient);
    float2 outward = size > .000001 ? -gradient/size : 0;
    // Fades where the bezels of opposite sides meet, which hides that seam.
    float settled = saturate(size/(2*e/bezel));
    float travel = height*slope*form.w*lens*(1+.3*energy)*settled;
    float2 shift = outward*clamp(travel,-shape.w*2,shape.w*2);
    float3 color = float3(scene(world+shift*(1-aberration)).r,scene(world+shift).g,scene(world+shift*(1+aberration)).b);

    // Beer-Lambert: the path through the glass shortens toward the rim.
    color *= exp(-(1-tint)*density*3.5*(1-height));

    // The outermost wall is steep, so Fresnel reflection makes the highlight a
    // hairline along the outline, on the side of the light and wrapped to the far one.
    float u = 1-x;
    float wall = min(u/sqrt(max(1-u*u,.0001)),8);
    float3 n = normalize(float3(wall*outward,1));
    float fresnel = pow(1-saturate(n.z),5);
    float facing = dot(outward,optics.xy);
    float lobe = saturate(facing)*saturate(facing)+saturate(-facing)*saturate(-facing)*.5;
    color += fresnel*lobe*(1.9+energy);
    color += .06*energy;
    return float4(color*cover,cover);
}
float4 GlassPS(Varying i) : SV_TARGET {
    float2 world = i.pos.xy;
    float2 p = world-shape.xy;
    float scale = misc.w;
    float d = pane(p);
    float fall = saturate(1-pane(p-float2(0,6*scale))/(20*scale));
    float shadow = fall*fall*.30;
    if (d > 1) return float4(0,0,0,shadow*misc.z);

    float brightness = dot(soft.SampleLevel(linearClamp,sceneUv(world),0).rgb,float3(.2126,.7152,.0722));
    float density = (.16+.42*smoothstep(.30,.90,brightness))*optics.z;
    float4 glass = 0;
    if (-d > form.z+1) {
        glass = shade(world,density);
    } else {
        for (int k = 0; k < 8; ++k) glass += shade(world+subpixel[k],density);
        glass /= 8;
    }
    // Interleaved gradient noise: the soft falloffs would band at 8 bits.
    float grain = frac(52.9829189*frac(dot(i.pos.xy,float2(.06711056,.00583715))));
    return float4(glass.rgb+(grain-.5)/255*glass.a,glass.a+shadow*(1-glass.a))*misc.z;
}
)hlsl";

struct Bytecode {
    ComPtr<ID3DBlob> quad, down, blur, glass;
    std::string error;
};

Bytecode compile_shaders() {
    Bytecode code;
    const auto compile = [&](const char* name, const char* profile, ComPtr<ID3DBlob>& result) {
        ComPtr<ID3DBlob> messages;
        const auto hr = D3DCompile(shader_source, std::strlen(shader_source), "LiquidGlass", nullptr, nullptr, name, profile,
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                   result.GetAddressOf(), messages.GetAddressOf());
        if (check(hr, name, code.error)) return true;
        if (messages) code.error += " " + std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
        return false;
    };
    (void)(compile("QuadVS", "vs_4_0", code.quad) && compile("DownPS", "ps_4_0", code.down) &&
           compile("BlurPS", "ps_4_0", code.blur) && compile("GlassPS", "ps_4_0", code.glass));
    return code;
}

// The glass shader takes over 100 ms to compile and a window resize rebuilds the
// renderer, so it compiles once. Intentionally process-lifetime: COM must not be
// released from DllMain.
const Bytecode& shaders() {
    static const Bytecode* compiled = new Bytecode(compile_shaders());
    return *compiled;
}

struct alignas(16) Parameters {
    float shape[4]{}, form[4]{}, optics[4]{}, region[4]{}, kernel[4]{}, misc[4]{};
};

struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> source;
    UINT width = 0, height = 0;

    bool create(ID3D11Device* device, UINT w, UINT h, std::string& error) {
        if (texture && width == w && height == h) return true;
        *this = {};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (!check(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()), "create blur buffer", error) ||
            !check(device->CreateRenderTargetView(texture.Get(), nullptr, target.GetAddressOf()), "create blur target", error) ||
            !check(device->CreateShaderResourceView(texture.Get(), nullptr, source.GetAddressOf()), "create blur view", error))
            return false;
        width = w; height = h;
        return true;
    }
};
} // namespace

bool publish_liquid_glass(const LiquidGlassFrame& frame) {
    if(auto* context=ImGui::GetCurrentContext()) {
        auto& state=published[context];
        if(state.number!=ImGui::GetFrameCount()) { state.frames.clear(); state.number=ImGui::GetFrameCount(); }
        // Keep one slot available to native HUD widgets regardless of script order.
        if(state.frames.size()>=(frame.owner.empty()?64u:63u)) return false;
        state.frames.push_back(frame); return true;
    }
    return false;
}
const LiquidGlassFrame* current_liquid_glass() {
    auto frames=current_liquid_glass_frames();
    return frames.empty()?nullptr:&frames.back();
}
std::span<const LiquidGlassFrame> current_liquid_glass_frames() {
    const auto found = published.find(ImGui::GetCurrentContext());
    if(found==published.end() || found->second.number!=ImGui::GetFrameCount()) return {};
    return found->second.frames;
}
void remove_liquid_glass(std::string_view owner,std::uint64_t generation) {
    if(auto found=published.find(ImGui::GetCurrentContext());found!=published.end())
        std::erase_if(found->second.frames,[&](const auto& pane){return pane.owner==owner&&pane.generation==generation;});
}
bool liquid_glass_warm() {
    if(accessibility_preferences.high_contrast) return true;
    const auto found=published.find(ImGui::GetCurrentContext());
    return found!=published.end()&&found->second.ready;
}
bool liquid_glass_ready() {
    return liquid_glass_warm()&&!accessibility_preferences.reduced_transparency&&!accessibility_preferences.high_contrast;
}
void forget_liquid_glass(ImGuiContext* context) { published.erase(context); }

struct LiquidGlassD3D11::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> commands;
    ComPtr<ID3D11VertexShader> quadVS;
    ComPtr<ID3D11PixelShader> downPS, blurPS, glassPS;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> over, replace;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> capture;
    ComPtr<ID3D11ShaderResourceView> captureView;
    Surface blur[2];
    UINT captureWidth = 0, captureHeight = 0;
    DXGI_FORMAT captureFormat = DXGI_FORMAT_UNKNOWN;
    Parameters p{};
    bool loaded = false;

    bool create_shaders(const Bytecode& code, std::string& error) {
        if (!code.error.empty()) { error = code.error; return false; }
        const auto pixel = [&](const ComPtr<ID3DBlob>& blob, ComPtr<ID3D11PixelShader>& shader) {
            return check(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.GetAddressOf()), "create pixel shader", error);
        };
        return check(device->CreateVertexShader(code.quad->GetBufferPointer(), code.quad->GetBufferSize(), nullptr, quadVS.GetAddressOf()), "create quad shader", error) &&
               pixel(code.down, downPS) && pixel(code.blur, blurPS) && pixel(code.glass, glassPS);
    }

    bool initialize(ID3D11Device* requested, std::string& error) {
        device = requested;
        if (!check(device->CreateDeferredContext(0, commands.GetAddressOf()), "create isolated command context", error)) return false;
        if (!create_shaders(shaders(), error)) return false;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Parameters); buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!check(device->CreateBuffer(&buffer, nullptr, constants.GetAddressOf()), "create constants", error)) return false;
        D3D11_BLEND_DESC blend{};
        auto& target = blend.RenderTarget[0];
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (!check(device->CreateBlendState(&blend, replace.GetAddressOf()), "create replace blend", error)) return false;
        target.BlendEnable = TRUE;
        target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (!check(device->CreateBlendState(&blend, over.GetAddressOf()), "create premultiplied blend", error)) return false;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
        raster.ScissorEnable = TRUE; raster.DepthClipEnable = TRUE;
        if (!check(device->CreateRasterizerState(&raster, rasterizer.GetAddressOf()), "create rasterizer", error)) return false;
        D3D11_DEPTH_STENCIL_DESC stencil{};
        stencil.DepthEnable = FALSE;
        if (!check(device->CreateDepthStencilState(&stencil, depth.GetAddressOf()), "create depth state", error)) return false;
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.ComparisonFunc = D3D11_COMPARISON_NEVER; sample.MaxLOD = D3D11_FLOAT32_MAX;
        return check(device->CreateSamplerState(&sample, sampler.GetAddressOf()), "create sampler", error);
    }

    // A swap-chain format the back buffer can be copied into. The pane breathes
    // every frame, so the extent grows in blocks instead of following it.
    bool ensure_capture(const D3D11_TEXTURE2D_DESC& back, UINT width, UINT height, std::string& error) {
        width = std::max(width, captureWidth);
        height = std::max(height, captureHeight);
        if (capture && captureWidth == width && captureHeight == height && captureFormat == back.Format) return true;
        capture.Reset(); captureView.Reset();
        captureWidth = width; captureHeight = height; captureFormat = back.Format;
        D3D11_TEXTURE2D_DESC desc = back;
        desc.Width = width; desc.Height = height;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
        desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0; desc.MipLevels = 1; desc.ArraySize = 1;
        if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return check(device->CreateTexture2D(&desc, nullptr, capture.GetAddressOf()), "create scene capture", error) &&
               check(device->CreateShaderResourceView(capture.Get(), nullptr, captureView.GetAddressOf()), "create scene capture view", error);
    }

    bool draw(ID3D11RenderTargetView* target, float width, float height, ID3D11PixelShader* shader,
              ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second, bool blended, std::string& error) {
        ID3D11ShaderResourceView* unbound[2]{};
        commands->PSSetShaderResources(0, 2, unbound);
        commands->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, width, height, 0.0F, 1.0F};
        commands->RSSetViewports(1, &viewport);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (!check(commands->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map), "upload constants", error)) return false;
        std::memcpy(map.pData, &p, sizeof p);
        commands->Unmap(constants.Get(), 0);
        ID3D11Buffer* buffer = constants.Get();
        commands->PSSetConstantBuffers(0, 1, &buffer);
        ID3D11ShaderResourceView* sources[2]{first, second};
        commands->PSSetShader(shader, nullptr, 0);
        commands->PSSetShaderResources(0, 2, sources);
        commands->OMSetBlendState(blended ? over.Get() : replace.Get(), nullptr, 0xFFFFFFFF);
        commands->Draw(3, 0);
        return true;
    }

    bool render(ID3D11DeviceContext* immediate, ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* target,
                std::span<const LiquidGlassFrame> frames, std::string& error) {
        if(frames.empty()) return true;
        D3D11_TEXTURE2D_DESC back{};
        backBuffer->GetDesc(&back);
        // Flip-model swap chains are never multisampled; the digits still draw.
        if (back.SampleDesc.Count > 1) return true;
        const auto extent = [](float pixels) { return (static_cast<UINT>(std::max(pixels, 2.0F)) + 63U) & ~63U; };
        float left=static_cast<float>(back.Width),top=static_cast<float>(back.Height),right=0,bottom=0,s=1;
        // One union of the same native capture regions. Never capture already composited panes.
        for(const auto& frame:frames) {
            s=std::max(s,frame.scale);
            const float width=static_cast<float>(extent(frame.size.x+120*frame.scale));
            const float height=static_cast<float>(extent(frame.size.y+120*frame.scale));
            left=std::min(left,std::floor(frame.center.x-width*.5F)); top=std::min(top,std::floor(frame.center.y-height*.5F));
            right=std::max(right,std::floor(frame.center.x-width*.5F)+width); bottom=std::max(bottom,std::floor(frame.center.y-height*.5F)+height);
        }
        const auto clamped = [](float value, UINT limit) { return static_cast<UINT>(std::clamp(value, 0.0F, static_cast<float>(limit))); };
        const D3D11_BOX box{clamped(left,back.Width),clamped(top,back.Height),0,clamped(right,back.Width),clamped(bottom,back.Height),1};
        if(box.right<=box.left || box.bottom<=box.top) return true;
        const UINT width=extent(static_cast<float>(box.right-box.left)),height=extent(static_cast<float>(box.bottom-box.top));
        if (!ensure_capture(back, width, height, error) ||
            !blur[0].create(device.Get(), captureWidth / 2, captureHeight / 2, error) ||
            !blur[1].create(device.Get(), captureWidth / 2, captureHeight / 2, error)) return false;

        commands->ClearState();
        commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->RSSetState(rasterizer.Get());
        commands->OMSetDepthStencilState(depth.Get(), 0);
        commands->VSSetShader(quadVS.Get(), nullptr, 0);
        ID3D11SamplerState* samplers[]{sampler.Get()};
        commands->PSSetSamplers(0, 1, samplers);
        commands->CopySubresourceRegion(capture.Get(), 0, 0, 0, 0, backBuffer, 0, &box);

        const float halfWidth = static_cast<float>(blur[0].width), halfHeight = static_cast<float>(blur[0].height);
        D3D11_RECT clip{0, 0, static_cast<LONG>(blur[0].width), static_cast<LONG>(blur[0].height)};
        commands->RSSetScissorRects(1, &clip);
        p.kernel[2] = 1.0F / halfWidth; p.kernel[3] = 1.0F / halfHeight;
        if (!draw(blur[0].target.Get(), halfWidth, halfHeight, downPS.Get(), captureView.Get(), nullptr, false, error)) return false;
        const float step = std::max(1.0F, s);
        for (int i = 0; i < 2; ++i) {
            p.kernel[0] = step / halfWidth; p.kernel[1] = 0.0F;
            if (!draw(blur[1].target.Get(), halfWidth, halfHeight, blurPS.Get(), blur[0].source.Get(), nullptr, false, error)) return false;
            p.kernel[0] = 0.0F; p.kernel[1] = step / halfHeight;
            if (!draw(blur[0].target.Get(), halfWidth, halfHeight, blurPS.Get(), blur[1].source.Get(), nullptr, false, error)) return false;
        }

        for(const auto& frame:frames) {
        const float halfSizeX = frame.size.x * 0.5F, halfSizeY = frame.size.y * 0.5F;
        const float radius = frame.radius<0 ? std::min(halfSizeX,halfSizeY) : std::clamp(frame.radius,0.F,std::min(halfSizeX,halfSizeY));
        p.shape[0] = frame.center.x; p.shape[1] = frame.center.y; p.shape[2] = halfSizeX; p.shape[3] = halfSizeY;
        p.form[0] = radius; p.form[1] = frame.waist;
        // Past the corner radius the bezels of opposite sides would meet on the flat.
        p.form[2] = std::max(.001F,radius * 0.85F); p.form[3] = p.form[2] * 1.43F;
        p.optics[0] = std::cos(frame.light); p.optics[1] = std::sin(frame.light);
        p.optics[2] = std::clamp(frame.lens, 0.0F, 1.0F); p.optics[3] = std::clamp(frame.energy, 0.0F, 1.0F);
        p.region[0] = static_cast<float>(box.left); p.region[1] = static_cast<float>(box.top);
        p.region[2] = static_cast<float>(box.right - box.left); p.region[3] = static_cast<float>(box.bottom - box.top);
        p.misc[0] = 1.0F / static_cast<float>(captureWidth); p.misc[1] = 1.0F / static_cast<float>(captureHeight);
        p.misc[2] = std::clamp(frame.opacity, 0.0F, 1.0F); p.misc[3] = frame.scale;
        const float margin=60*frame.scale;
        clip = {static_cast<LONG>(clamped(frame.center.x-halfSizeX-margin,back.Width)),static_cast<LONG>(clamped(frame.center.y-halfSizeY-margin,back.Height)),
                static_cast<LONG>(clamped(std::ceil(frame.center.x+halfSizeX+margin),back.Width)),static_cast<LONG>(clamped(std::ceil(frame.center.y+halfSizeY+margin),back.Height))};
        commands->RSSetScissorRects(1, &clip);
        if (!draw(target, static_cast<float>(back.Width), static_cast<float>(back.Height), glassPS.Get(),
                  captureView.Get(), blur[0].source.Get(), true, error)) return false;
        }

        ID3D11ShaderResourceView* unbound[2]{};
        commands->PSSetShaderResources(0, 2, unbound);
        commands->OMSetRenderTargets(0, nullptr, nullptr);
        ComPtr<ID3D11CommandList> list;
        if (!check(commands->FinishCommandList(FALSE, list.GetAddressOf()), "finish commands", error)) return false;
        immediate->ExecuteCommandList(list.Get(), TRUE);
        return check(device->GetDeviceRemovedReason(), "execute commands", error);
    }
};

LiquidGlassD3D11::LiquidGlassD3D11() = default;
LiquidGlassD3D11::~LiquidGlassD3D11() { reset(); }
void LiquidGlassD3D11::reset() { impl_.reset(); if(auto f=published.find(context_);f!=published.end()) f->second.ready=false; }
bool LiquidGlassD3D11::loaded() const noexcept { return impl_ && impl_->loaded; }

bool LiquidGlassD3D11::render(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer,
                              ID3D11RenderTargetView* target, const LiquidGlassFrame& frame, std::string& error) {
    return render(device,context,backBuffer,target,std::span<const LiquidGlassFrame>(&frame,1),error);
}
bool LiquidGlassD3D11::render(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* backBuffer,
                              ID3D11RenderTargetView* target,std::span<const LiquidGlassFrame> frames,std::string& error) {
    context_=ImGui::GetCurrentContext();
    if(accessibility_preferences.high_contrast) {
        if(context_) published[context_].ready=false;
        return true;
    }
    if (!device || !context || !backBuffer || !target) { error = "Liquid Glass requires a complete D3D11 render target."; return false; }
    if (!impl_ || impl_->device.Get() != device) {
        impl_ = std::make_unique<Impl>();
        if (!impl_->initialize(device, error)) { impl_.reset(); return false; }
    }
    if (!impl_->render(context, backBuffer, target, frames, error)) {
        // Drop a partially recorded command list: it may otherwise retain a
        // swap-chain resource through ResizeBuffers.
        reset();
        return false;
    }
    impl_->loaded = true;
    if(context_) published[context_].ready=true;
    return true;
}

} // namespace medicon::gui
