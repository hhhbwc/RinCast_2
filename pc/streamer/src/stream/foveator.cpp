#include "foveator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../gpu.h"
#include "rc/platform.h"

namespace rcs {

namespace {

// For each pixel of the squeezed frame, the place in the full one it shows: along each axis of
// an eye, [0, a) and [a + F, 1] are at density s, [a, a + F) at full density (rc::VrFoveationMetadata).
// Squeezed parts average the pixels they cover instead of picking one, or fine detail would shimmer.
const char* kShader = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) {
    float4 start;    // left eye x, y, right eye x, y
    float4 layout;   // F, density x, density y
    float4 texel;    // 1 / full frame width, 1 / height
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = c; return o;
}
// Full-picture position (0..1) of a squeezed position e, along one axis of an eye.
float Unsqueeze(float e, float a, float s) {
    float F = layout.x;
    float p = e * (F + (1 - F) * s);
    if (p < a * s) return p / s;
    if (p < a * s + F) return a + (p - a * s);
    return a + F + (p - a * s - F) / s;
}
float2 EyeUv(float2 p, float eye) {
    return float2(clamp(p.x, eye * 0.5 + 0.5 * texel.x, eye * 0.5 + 0.5 - 0.5 * texel.x), p.y);
}
float4 ps(V i) : SV_Target {
    float eye = i.uv.x < 0.5 ? 0 : 1;
    float2 a = eye < 0.5 ? start.xy : start.zw;
    float ux = Unsqueeze(i.uv.x * 2 - eye, a.x, layout.y);
    float uy = Unsqueeze(i.uv.y, a.y, layout.z);
    float2 uv = EyeUv(float2((ux + eye) * 0.5, uy), eye);
    // Taps a quarter of the covered stretch either side of the centre.
    float2 o = float2(ux < a.x || ux >= a.x + layout.x ? 0.25 * texel.x / layout.y : 0,
                      uy < a.y || uy >= a.y + layout.x ? 0.25 * texel.y / layout.z : 0);
    if (o.x == 0 && o.y == 0) return src.Sample(samp, uv);
    if (o.x == 0) return 0.5 * (src.Sample(samp, EyeUv(uv + float2(0, -o.y), eye)) +
                                 src.Sample(samp, EyeUv(uv + float2(0, o.y), eye)));
    if (o.y == 0) return 0.5 * (src.Sample(samp, EyeUv(uv + float2(-o.x, 0), eye)) +
                                 src.Sample(samp, EyeUv(uv + float2(o.x, 0), eye)));
    return 0.25 * (src.Sample(samp, EyeUv(uv + float2(-o.x, -o.y), eye)) +
                   src.Sample(samp, EyeUv(uv + float2(o.x, -o.y), eye)) +
                   src.Sample(samp, EyeUv(uv + float2(-o.x, o.y), eye)) +
                   src.Sample(samp, EyeUv(uv + float2(o.x, o.y), eye)));
}
)";

struct Params {
    float start[4];
    float layout[4];
    float texel[4];
};

}  // namespace

// Foveator::EncodedSize moved inline into foveator.h (TASK-COMBO-SWEEP-2026-11-21):
// the pure config builders and the offline sweep need it without the D3D side.

bool Foveator::Init(ID3D11Device* device, uint32_t eyeWidth, uint32_t eyeHeight, uint32_t encodedEyeWidth,
                    uint32_t encodedEyeHeight, float fovea, bool tenBit) {
    m_device = device;
    m_fovea = fovea;
    m_eyeWidth = eyeWidth;
    m_eyeHeight = eyeHeight;
    m_encodedWidth = encodedEyeWidth;
    m_encodedHeight = encodedEyeHeight;
    m_output.Reset();
    m_rtv.Reset();
    m_vs.Reset();
    m_ps.Reset();
    m_sampler.Reset();
    m_params.Reset();
    m_lastInput = nullptr;
    m_inputSrv.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = encodedEyeWidth * 2;
    td.Height = encodedEyeHeight;
    td.MipLevels = td.ArraySize = 1;
    td.Format = tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_output);
    if (FAILED(hr) || FAILED(device->CreateRenderTargetView(m_output.Get(), nullptr, &m_rtv))) {
        RC_LOGE("Foveated encoding: texture creation failed: %s", HResultString(hr).c_str());
        return false;
    }
    auto vs = CompileShader(kShader, "vs", "vs_5_0");
    auto ps = CompileShader(kShader, "ps", "ps_5_0");
    if (!vs || !ps) return false;
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &m_sampler);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &m_params)) && m_vs && m_ps && m_sampler;
}

bool Foveator::Squeeze(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, const float foveaStart[2][2]) {
    if (frame != m_lastInput) {
        m_inputSrv.Reset();
        m_lastInput = nullptr;
        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        // Shared VR textures are typeless; read the raw (gamma-encoded) values, as the colour
        // converter does.
        sd.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                    : desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS
                        ? DXGI_FORMAT_R10G10B10A2_UNORM
                    : desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS
                        ? DXGI_FORMAT_R16G16B16A16_FLOAT
                        : desc.Format;
        const HRESULT hr = m_device->CreateShaderResourceView(frame, &sd, &m_inputSrv);
        if (FAILED(hr)) {
            RC_LOGE("Foveated encoding: source view creation failed: %s", HResultString(hr).c_str());
            return false;
        }
        m_lastInput = frame;
    }
    const float F = m_fovea;
    Params p{};
    std::memcpy(p.start, foveaStart, sizeof(float) * 4);
    p.layout[0] = F;
    const float oneMinusF = std::max(1e-4f, 1.0f - F);
    p.layout[1] = std::clamp(m_eyeWidth ? (float(m_encodedWidth) / m_eyeWidth - F) / oneMinusF : 1.0f, 1e-3f, 1.0f);
    p.layout[2] = std::clamp(m_eyeHeight ? (float(m_encodedHeight) / m_eyeHeight - F) / oneMinusF : 1.0f, 1e-3f, 1.0f);
    p.texel[0] = m_eyeWidth ? 1.f / (m_eyeWidth * 2) : 0.f;
    p.texel[1] = m_eyeHeight ? 1.f / m_eyeHeight : 0.f;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const HRESULT mapHr = ctx->Map(m_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mapHr)) {
        RC_LOGE("Foveated encoding: parameter map failed: %s", HResultString(mapHr).c_str());
        return false;
    }
    std::memcpy(mapped.pData, &p, sizeof(p));
    ctx->Unmap(m_params.Get(), 0);

    ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, m_inputSrv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT vp{0, 0, float(m_encodedWidth * 2), float(m_encodedHeight), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    return true;
}

}  // namespace rcs
