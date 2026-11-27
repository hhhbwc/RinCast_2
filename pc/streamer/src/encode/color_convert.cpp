#include "color_convert.h"

#include "../gpu.h"
#include "rc/platform.h"

namespace rcs {

// BT.709 limited range. Chroma is sampled at the centre of each 2x2 block
// (bilinear tap at the shared corner averages the four source pixels).
static const char* kShader = R"(
cbuffer Params : register(b0) { float2 invSize; float ditherAmp; float isStereo; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = c; return o;
}
static const float3 kY = float3(0.2126, 0.7152, 0.0722);
static const float3 kU = float3(-0.1146, -0.3854, 0.5);
static const float3 kV = float3(0.5, -0.4542, -0.0458);

float Ign(float2 pos) {
    return frac(52.9829189 * frac(dot(pos, float2(0.06711056, 0.00583715))));
}

float psY(V i) : SV_Target {
    float3 rgb = saturate(src.Sample(samp, i.uv).rgb);
    float y = dot(rgb, kY) * (219.0 / 255.0) + 16.0 / 255.0;
    if (ditherAmp > 0.0) {
        float n = (Ign(i.pos.xy) + Ign(i.pos.xy + float2(17.0, 31.0)) - 1.0) * 0.5;
        y = saturate(y + n * ditherAmp);
    }
    return y;
}
float2 psUV(V i) : SV_Target {
    float2 uv = i.uv;
    if (isStereo > 0.5) {
        float eye = step(0.5, uv.x);
        uv.x = clamp(uv.x, eye * 0.5 + 0.5 * invSize.x, eye * 0.5 + 0.5 - 0.5 * invSize.x);
    }
    float3 rgb = saturate(src.Sample(samp, uv).rgb);
    float2 chroma = float2(dot(rgb, kU), dot(rgb, kV)) * (224.0 / 255.0) + 128.0 / 255.0;
    if (ditherAmp > 0.0) {
        float nu = (Ign(i.pos.xy) + Ign(i.pos.xy + float2(17.0, 31.0)) - 1.0) * 0.5;
        float nv = (Ign(i.pos.xy + float2(13.0, 7.0)) + Ign(i.pos.xy + float2(29.0, 41.0)) - 1.0) * 0.5;
        chroma = saturate(chroma + float2(nu, nv) * ditherAmp);
    }
    return chroma;
}
)";

bool ColorConverter::Init(ID3D11Device* device, UINT width, UINT height, bool tenBit, bool isStereo) {
    // A47: a failed re-Init (resolution change / device loss) must not leave half
    // of the old object graph alive beside freshly created objects. Reset every
    // member up front so any FAILED return below leaves a single clean state.
    m_device = device;
    m_width = width;
    m_height = height;
    m_tenBit = tenBit;
    m_yuv.Reset();
    m_rtvY.Reset();
    m_rtvUV.Reset();
    m_vs.Reset();
    m_psY.Reset();
    m_psUV.Reset();
    m_sampler.Reset();
    m_raster.Reset();
    m_params.Reset();
    m_inputSrv.Reset();
    m_lastInput = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_yuv);
    if (FAILED(hr)) {
        RC_LOGE("Create YUV texture failed: %s", HResultString(hr).c_str());
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    if (FAILED(device->CreateRenderTargetView(m_yuv.Get(), &rd, &m_rtvY))) return false;
    rd.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device->CreateRenderTargetView(m_yuv.Get(), &rd, &m_rtvUV))) return false;

    auto vs = CompileShader(kShader, "vs", "vs_5_0");
    auto psY = CompileShader(kShader, "psY", "ps_5_0");
    auto psUV = CompileShader(kShader, "psUV", "ps_5_0");
    if (!vs || !psY || !psUV) return false;
    // A47: device loss / OOM makes these Create* fail; ignoring the HRESULT let
    // Init() report success and Convert() bind null objects. Check all of them.
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs))) return false;
    if (FAILED(device->CreatePixelShader(psY->GetBufferPointer(), psY->GetBufferSize(), nullptr, &m_psY))) return false;
    if (FAILED(device->CreatePixelShader(psUV->GetBufferPointer(), psUV->GetBufferSize(), nullptr, &m_psUV))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rs, &m_raster))) return false;

    const float paramsData[4] = {
        width ? 1.0f / float(width) : 0.0f,
        height ? 1.0f / float(height) : 0.0f,
        tenBit ? 0.0f : (1.0f / 255.0f),
        isStereo ? 1.0f : 0.0f,
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(paramsData);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initData{paramsData, 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &initData, &m_params))) return false;
    return true;
}

bool ColorConverter::Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* rgb) {
    // NOTE (A47): the upstream VR capture surface is a fixed 2-3 texture swapchain,
    // so a live texture is never freed/reallocated at the same address while it is
    // still the current input. A true ABA (new texture reusing an old address) would
    // still defeat this bare-pointer cache; if allocation ever becomes dynamic,
    // replace it with a map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> plus
    // an InvalidateInputCache() on texture rebuild. The failure-path inconsistency
    // below is fixed regardless.
    if (rgb != m_lastInput) {
        // A47: Reset the cached pointer together with the SRV. Otherwise a failed
        // CreateShaderResourceView below leaves m_lastInput on the OLD texture; the
        // next call passing that same pointer skips this block and binds an empty
        // m_inputSrv -> silent all-black frames.
        m_inputSrv.Reset();
        m_lastInput = nullptr;
        D3D11_TEXTURE2D_DESC desc;
        rgb->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        // Shared VR textures are typeless; read the raw (gamma-encoded) values.
        // A47: 10-bit / HDR typeless sources must be given a concrete UNORM view
        // or CreateShaderResourceView returns E_INVALIDARG.
        sd.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS      ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS    ? DXGI_FORMAT_B8G8R8A8_UNORM
                    : desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM
                    : desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS
                        ? DXGI_FORMAT_R16G16B16A16_FLOAT
                        : desc.Format;
        const HRESULT hr = m_device->CreateShaderResourceView(rgb, &sd, &m_inputSrv);
        if (FAILED(hr)) {
            RC_LOGE("Colour conversion: source view creation failed (format %u): %s", unsigned(desc.Format),
                    HResultString(hr).c_str());
            return false;
        }
        m_lastInput = rgb;
    }
    // The input may still be bound as a render target (e.g. by the capture
    // compositor); D3D11 would silently drop the SRV binding in that case.
    ctx->OMSetRenderTargets(1, m_rtvY.GetAddressOf(), nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, m_inputSrv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    ctx->RSSetState(m_raster.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);

    D3D11_VIEWPORT vp{0, 0, float(m_width), float(m_height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, m_rtvY.GetAddressOf(), nullptr);
    ctx->PSSetShader(m_psY.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    vp.Width = float(m_width / 2);
    vp.Height = float(m_height / 2);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, m_rtvUV.GetAddressOf(), nullptr);
    ctx->PSSetShader(m_psUV.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    return true;
}

}  // namespace rcs
