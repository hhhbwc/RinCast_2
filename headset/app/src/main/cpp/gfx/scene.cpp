#include "scene.h"

#include <cstring>
#include <vector>

#include "rc/platform.h"

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9  // GL_EXT_sRGB_write_control
#endif

namespace rv {

// Video into an sRGB layer image without the hardware's sRGB encoding, for as long as it's in
// scope (see m_rawSrgbWrites): the shaders' ToLayer() then leaves the gamma-encoded values alone.
struct RawSrgbWrites {
    explicit RawSrgbWrites(bool on) : on(on) {
        if (on) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    ~RawSrgbWrites() {
        if (on) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    bool on;
};

// Panel: a strip grid with u in [0,1] across, v in [0,1] down. The vertex
// shader bends it around a vertical cylinder centred `radius` behind the
// panel when uRadius > 0 (arc length == width).
static const char* kPanelVs = R"(
layout(location = 0) in vec2 aUv;
uniform mat4 uMvp;
uniform vec2 uSize;     // meters
uniform float uRadius;  // meters, 0 = flat
out vec2 vUv;
out vec2 vLocal;
void main() {
    vec2 local = vec2((aUv.x - 0.5) * uSize.x, (0.5 - aUv.y) * uSize.y);
    vec3 p = vec3(local, 0.0);
    if (uRadius > 0.0) {
        float a = local.x / uRadius;
        p = vec3(sin(a) * uRadius, local.y, uRadius - cos(a) * uRadius);
    }
    vUv = aUv;
    vLocal = local;
    gl_Position = uMvp * vec4(p, 1.0);
}
)";

static const char* kPanelFsCommon = R"(
uniform float uOpacity;
uniform float uBrightness;
uniform vec2 uSize;
uniform float uCorner;
uniform int uHole;
in vec2 vUv;
in vec2 vLocal;
out vec4 oColor;
float cornerMask() {
    vec2 q = abs(vLocal) - uSize * 0.5 + uCorner;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uCorner;
    return clamp(0.5 - d / max(fwidth(d), 1e-5), 0.0, 1.0);
}
)";

static const char* kPanel2DFs = R"(
uniform sampler2D uTex;
void main() {
    float m = cornerMask();
    if (uHole == 1) { if (m < 0.5) discard; oColor = vec4(0.0); return; }
    // UI textures are y-up (rendered via FBO): flip v.
    vec4 c = texture(uTex, vec2(vUv.x, 1.0 - vUv.y));
    // Clears what's behind by the canvas's coverage (blended with ZERO, ONE_MINUS_SRC_ALPHA).
    if (uHole == 2) { if (c.a < 0.02) discard; oColor = vec4(0.0, 0.0, 0.0, c.a); return; }
    oColor = vec4(PanelLinearToEye(c.rgb * uBrightness), c.a) * uOpacity * m;
    // Clear parts (around the canvas's rounded card) mustn't hide what's behind them.
    if (oColor.a < 0.02) discard;
}
)";

static const char* kPanelExtFs = R"(
uniform samplerExternalOES uTex;
uniform vec4 uCrop;  // visible part of the decoder buffer: offset.xy, scale.zw
void main() {
    float m = cornerMask();
    if (uHole == 1) { if (m < 0.5) discard; oColor = vec4(0.0); return; }
    // Video is gamma encoded; linearize only when eye buffers are sRGB targets.
    vec3 g = texture(uTex, uCrop.xy + vUv * uCrop.zw).rgb;
    vec3 rgb = PanelGammaToEye(g);
    oColor = vec4(rgb * uBrightness, 1.0) * uOpacity * m;
    if (oColor.a < 0.02) discard;
}
)";

static const char* kBeamVs = R"(
layout(location = 0) in vec4 aPos;   // xyz, t (0 at start, 1 at end)
layout(location = 1) in float aSide;
uniform mat4 uViewProj;
out float vT;
out float vSide;
void main() { vT = aPos.w; vSide = aSide; gl_Position = uViewProj * vec4(aPos.xyz, 1.0); }
)";
static const char* kBeamFs = R"(
uniform vec4 uStart;
uniform vec4 uEnd;
in float vT;
in float vSide;
out vec4 oColor;
void main() {
    vec4 c = mix(uStart, uEnd, vT);
    float edge = 1.0 - abs(vSide);
    c.a *= smoothstep(0.0, 0.6, edge);
    oColor = vec4(c.rgb * c.a, c.a);
}
)";

static const char* kDotVs = R"(
uniform mat4 uViewProj;
uniform vec3 uCenter;
uniform vec3 uRight;
uniform vec3 uUp;
out vec2 vP;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vP = c;
    gl_Position = uViewProj * vec4(uCenter + uRight * c.x + uUp * c.y, 1.0);
}
)";
static const char* kDotFs = R"(
uniform vec4 uColor;
in vec2 vP;
out vec4 oColor;
void main() {
    float r = length(vP);
    float core = 1.0 - smoothstep(0.55, 0.65, r);
    float ring = (1.0 - smoothstep(0.85, 1.0, r)) * 0.35;
    float a = max(core, ring) * uColor.a;
    vec3 col = mix(uColor.rgb, vec3(1.0), core * 0.3);
    oColor = vec4(col * a, a);
}
)";

static const char* kBlitVs = R"(
uniform vec4 uRect;   // u0 v0 u1 v1
out vec2 vUv;
void main() {
    // Fullscreen triangle: c is (0,0), (2,0), (0,2); the screen spans c = 0..1,
    // so c itself is the 0..1 coordinate across the viewport.
    vec2 c = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = mix(uRect.xy, uRect.zw, c);
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
)";
// Foveated encoding (rc::VrFoveationMetadata): where a point of the full picture is in the
// squeezed frame. uv over both eyes side by side, y from the top.
static const char* kFoveateFs = R"(
uniform int uFoveated;
uniform int uFoveaDebug;
uniform vec4 uFoveaStart;   // left eye x, y, right eye x, y
uniform vec3 uFoveaLayout;  // size, density x, density y
float FoveaAxis(float u, float a, float s) {
    float F = uFoveaLayout.x;
    float e = u < a ? u * s : u < a + F ? a * s + (u - a) : a * s + F + (u - a - F) * s;
    return e / (F + (1.0 - F) * s);
}
vec2 Foveate(vec2 uv, float eye) {
    if (uFoveated == 0) return uv;
    vec2 a = eye < 0.5 ? uFoveaStart.xy : uFoveaStart.zw;
    float x = FoveaAxis(uv.x * 2.0 - eye, a.x, uFoveaLayout.y);
    return vec2((x + eye) * 0.5, FoveaAxis(uv.y, a.y, uFoveaLayout.z));
}
vec3 ApplyFoveaDebug(vec2 fullUv, float eye, vec3 col) {
    if (uFoveaDebug == 0 || uFoveated == 0) return col;
    vec2 a = eye < 0.5 ? uFoveaStart.xy : uFoveaStart.zw;
    float F = uFoveaLayout.x;
    vec2 p = vec2(fullUv.x * 2.0 - eye, fullUv.y);
    bool inX = (p.x >= a.x && p.x <= a.x + F);
    bool inY = (p.y >= a.y && p.y <= a.y + F);
    float dx = min(abs(p.x - a.x), abs(p.x - (a.x + F)));
    float dy = min(abs(p.y - a.y), abs(p.y - (a.y + F)));
    bool border = (inX && dy < 0.003) || (inY && dx < 0.003);
    vec2 c = a + vec2(F * 0.5);
    bool cross = (abs(p.x - c.x) < 0.002 && abs(p.y - c.y) < 0.025) ||
                 (abs(p.y - c.y) < 0.002 && abs(p.x - c.x) < 0.025);
    if (border || cross) {
        return mix(col, vec3(0.1, 1.0, 0.3), 0.75);
    }
    return col;
}
)";
static const char* kColorGradeFs = R"(
uniform float uVibrance;
uniform float uDarkGamma;
vec3 ColorGrade(vec3 g) {
    if (uDarkGamma > 0.001) {
        float luma = dot(g, vec3(0.2126, 0.7152, 0.0722));
        float shadowWeight = (1.0 - luma) * (1.0 - luma);
        float gamma = 1.0 + uDarkGamma * shadowWeight;
        g = pow(max(g, vec3(0.0)), vec3(gamma));
    }
    if (uVibrance > 0.001) {
        float mx = max(g.r, max(g.g, g.b));
        float mn = min(g.r, min(g.g, g.b));
        float sat = mx - mn;
        float luma = dot(g, vec3(0.2126, 0.7152, 0.0722));
        float boost = 1.0 + uVibrance * (1.0 - sat);
        g = clamp(mix(vec3(luma), g, boost), 0.0, 1.0);
    }
    return g;
}
)";
static const char* kCasExtFs = R"(
// Adreno's ES 3.1 compiler rejects samplerExternalOES function parameters; keep uTex global.
uniform samplerExternalOES uTex;
uniform float uSharpen;
uniform vec2 uInvTexSize;
vec3 ApplyCasExtAt(vec2 tc, vec2 lo, vec2 hi, vec2 stepUv, float strength) {
    vec3 e = texture(uTex, tc).rgb;
    if (strength <= 0.001 || stepUv.x <= 0.0 || stepUv.y <= 0.0) return e;
    vec3 b = texture(uTex, clamp(tc + vec2(0.0, -stepUv.y), lo, hi)).rgb;
    vec3 d = texture(uTex, clamp(tc + vec2(-stepUv.x, 0.0), lo, hi)).rgb;
    vec3 f = texture(uTex, clamp(tc + vec2(stepUv.x, 0.0), lo, hi)).rgb;
    vec3 h = texture(uTex, clamp(tc + vec2(0.0, stepUv.y), lo, hi)).rgb;
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);
    amp = sqrt(amp);
    float peak = -1.0 / mix(8.0, 5.0, strength);
    vec3 w = amp * peak;
    vec3 outCol = (e + (b + d + f + h) * w) / (1.0 + 4.0 * w);
    return clamp(outCol, 0.0, 1.0);
}
vec3 ApplyCasExt(vec2 uv, vec4 crop, vec2 invSize, float strength) {
    float eye = step(0.5, uv.x);
    vec2 lo = crop.xy + vec2(eye * 0.5 + 0.5 * invSize.x, 0.5 * invSize.y) * crop.zw;
    vec2 hi = crop.xy + vec2(eye * 0.5 + 0.5 - 0.5 * invSize.x, 1.0 - 0.5 * invSize.y) * crop.zw;
    return ApplyCasExtAt(crop.xy + uv * crop.zw, lo, hi, invSize * crop.zw, strength);
}
)";
static const char* kCas2DFs = R"(
uniform float uSharpen;
uniform vec2 uInvTexSize;
vec3 ApplyCas2D(sampler2D tex, vec2 uv, vec2 invSize, float strength) {
    vec3 e = texture(tex, uv).rgb;
    if (strength <= 0.001 || invSize.x <= 0.0 || invSize.y <= 0.0) return e;
    float eye = step(0.5, uv.x);
    vec2 lo = vec2(eye * 0.5 + 0.5 * invSize.x, 0.5 * invSize.y);
    vec2 hi = vec2(eye * 0.5 + 0.5 - 0.5 * invSize.x, 1.0 - 0.5 * invSize.y);
    vec3 b = texture(tex, clamp(uv + vec2(0.0, -invSize.y), lo, hi)).rgb;
    vec3 d = texture(tex, clamp(uv + vec2(-invSize.x, 0.0), lo, hi)).rgb;
    vec3 f = texture(tex, clamp(uv + vec2(invSize.x, 0.0), lo, hi)).rgb;
    vec3 h = texture(tex, clamp(uv + vec2(0.0, invSize.y), lo, hi)).rgb;
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);
    amp = sqrt(amp);
    float peak = -1.0 / mix(8.0, 5.0, strength);
    vec3 w = amp * peak;
    vec3 outCol = (e + (b + d + f + h) * w) / (1.0 + 4.0 * w);
    return clamp(outCol, 0.0, 1.0);
}
)";
static const char* kBlitExtFs = R"(
uniform vec4 uCrop;  // visible part of the decoder buffer: offset.xy, scale.zw
in vec2 vUv;
out vec4 oColor;

// Layer swapchains are sRGB: ToLayer() linearizes the gamma-encoded video, or with sRGB writes
// off leaves it as it is.
void main() {
    vec2 uv = Foveate(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x));
    vec3 g = ApplyCasExt(uv, uCrop, uInvTexSize, uSharpen);
    g = ColorGrade(g);
    g = ApplyFoveaDebug(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x), g);
    oColor = vec4(ToLayer(g), 1.0);
}
)";
// The video as it is, gamma-encoded: the upscaler's input. External textures can't be gathered.
static const char* kBlitExtGammaFs = R"(
uniform samplerExternalOES uTex;
uniform vec4 uCrop;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec2 uv = Foveate(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x));
    vec3 c = texture(uTex, uCrop.xy + uv * uCrop.zw).rgb;
    c = ApplyFoveaDebug(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x), c);
    oColor = vec4(c, 1.0);
}
)";
// Snapdragon Game Super Resolution 1, RGBA mode, from Qualcomm's sgsr1_shader_mobile.frag.
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. SPDX-License-Identifier: BSD-3-Clause
// Adapted: names, and the result goes through ToLayer() for the sRGB swapchain.
// GatherEye coordinates: (base+1.0)/width centers the gather between texels so
// u*W-0.5 = base+0.5 keeps 0.5 texel of float32 margin off the floor() edge; and
// textureGather order is .x=(i,j+1) .y=(i+1,j+1) .z=(i+1,j) .w=(i,j), so the left
// column is .x/.w — the left boundary replicates .w, not .z.
static const char* kGsrFs = R"(
precision mediump float;
uniform highp vec4 uViewport;  // 1/width, 1/height, width, height
uniform mediump sampler2D uTex;
in highp vec2 vUv;
out vec4 oColor;
float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, float std) {
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
highp vec4 GatherEye(highp vec2 uv, float eye) {
    highp float width = uViewport.z;
    highp float lo = eye * 0.5 * width;
    highp float hi = lo + 0.5 * width;
    highp float footprint = uv.x * width - 0.5;
    highp float base = floor(footprint);
    base = clamp(base, lo, max(lo, hi - 2.0));
    vec4 g = textureGather(uTex, vec2((base + 1.0) / width, uv.y), 1);
    if (footprint < lo) return vec4(g.x, g.x, g.w, g.w);
    if (footprint >= hi - 1.0) return vec4(g.y, g.y, g.z, g.z);
    return g;
}
void main() {
    const int mode = 1;
    const float edgeThreshold = 8.0 / 255.0;
    const float edgeSharpness = 2.0;
    vec4 color;
    color.xyz = textureLod(uTex, vUv, 0.0).xyz;
    highp vec2 imgCoord = (vUv * uViewport.zw) + vec2(-0.5, 0.5);
    highp vec2 imgCoordPixel = floor(imgCoord);
    highp vec2 coord = imgCoordPixel * uViewport.xy;
    vec2 pl = imgCoord - imgCoordPixel;
    float eye = step(0.5, vUv.x);
    vec4 left = GatherEye(coord, eye);
    float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y) + abs(color[mode] - left.z);
    if (edgeVote > edgeThreshold) {
        coord.x += uViewport.x;
        vec4 right = GatherEye(coord + vec2(uViewport.x, 0.0), eye);
        vec4 upDown;
        upDown.xy = GatherEye(coord + vec2(0.0, -uViewport.y), eye).wz;
        upDown.zw = GatherEye(coord + vec2(0.0, uViewport.y), eye).yx;
        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color[mode] - mean;
        float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) + abs(right.x) + abs(right.y) + abs(right.z) +
                    abs(right.w) + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);
        float std = 2.181818 / max(sum, 1e-5);
        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
        aWY += weightY(pl.x, pl.y, left.z, std);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, std);
        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(edgeSharpness * finalY, minY, maxY);
        float deltaY = clamp(finalY - color.w, -23.0 / 255.0, 23.0 / 255.0);
        color.xyz = clamp(color.xyz + deltaY, 0.0, 1.0);
    }
    highp vec3 g = ColorGrade(color.xyz);
    oColor = vec4(ToLayer(g), 1.0);
}
)";
static const char* kKeptFs = R"(
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec3 g = ApplyCas2D(uTex, vUv, uInvTexSize, uSharpen);
    g = ColorGrade(g);
    oColor = vec4(ToLayer(g), 1.0);
}
)";
// The kept picture has its top at v = 0; so does the motion field, whose y points down.
static const char* kKeptMovedFs = R"(
uniform sampler2D uTex;
uniform mediump sampler2D uMotion;
uniform vec2 uScale;
uniform vec2 uField;
uniform vec2 uMotionHalfTexel;
uniform vec2 uHalfTexel;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    float eye = step(0.5, vUv.x);
    vec2 motionUv = uv * uField;
    motionUv.x = clamp(motionUv.x, eye * 0.5 + uMotionHalfTexel.x, 0.5 + eye * 0.5 - uMotionHalfTexel.x);
    motionUv.y = clamp(motionUv.y, uMotionHalfTexel.y, 1.0 - uMotionHalfTexel.y);
    vec2 m = texture(uMotion, motionUv).rg;
    vec2 mStep = 2.0 * uMotionHalfTexel;
    vec2 mLo = vec2(eye * 0.5 + uMotionHalfTexel.x, uMotionHalfTexel.y);
    vec2 mHi = vec2(0.5 + eye * 0.5 - uMotionHalfTexel.x, 1.0 - uMotionHalfTexel.y);
    vec2 mL = texture(uMotion, clamp(motionUv - vec2(mStep.x, 0.0), mLo, mHi)).rg;
    vec2 mR = texture(uMotion, clamp(motionUv + vec2(mStep.x, 0.0), mLo, mHi)).rg;
    vec2 mU = texture(uMotion, clamp(motionUv - vec2(0.0, mStep.y), mLo, mHi)).rg;
    vec2 mD = texture(uMotion, clamp(motionUv + vec2(0.0, mStep.y), mLo, mHi)).rg;
    float jump = max(max(length(mR - m), length(mL - m)), max(length(mD - m), length(mU - m)));
    float damp = mix(1.0, 0.15, smoothstep(0.016, 0.048, jump));
    uv -= (m * damp) * uScale;
    uv.x = clamp(uv.x, eye * 0.5, 0.5 + eye * 0.5);
    uv.y = clamp(uv.y, 0.0, 1.0);
    uv.x = clamp(uv.x, eye * 0.5 + uHalfTexel.x, 0.5 + eye * 0.5 - uHalfTexel.x);
    uv.y = clamp(uv.y, uHalfTexel.y, 1.0 - uHalfTexel.y);
    vec3 g = uSharpen > 0.001
        ? ApplyCas2D(uTex, vec2(uv.x, 1.0 - uv.y), uInvTexSize, uSharpen)
        : texture(uTex, vec2(uv.x, 1.0 - uv.y)).rgb;
    g = ColorGrade(g);
    oColor = vec4(ToLayer(g), 1.0);
}
)";
// The decoder insets only the OUTER edge beside padding.  With one-sided padding the
// seam sits 0.25 texel off crop.x+0.5*crop.z, so the inner seam needs 1.5 half-texels
// to reach the true first in-eye column.  eyeMid keeps clamp() well-formed when
// uCrop.z degenerates toward 0 (eyeMin > eyeMax would be undefined).
static const char* kExternalMovedFs = R"(
uniform vec4 uCrop;
uniform mediump sampler2D uMotion;
uniform vec2 uScale;
uniform vec2 uField;
uniform vec2 uMotionHalfTexel;
uniform vec2 uHalfTexel;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    float eye = step(0.5, vUv.x);
    vec2 motionUv = uv * uField;
    motionUv.x = clamp(motionUv.x, eye * 0.5 + uMotionHalfTexel.x, 0.5 + eye * 0.5 - uMotionHalfTexel.x);
    motionUv.y = clamp(motionUv.y, uMotionHalfTexel.y, 1.0 - uMotionHalfTexel.y);
    vec2 m = texture(uMotion, motionUv).rg;
    vec2 mStep = 2.0 * uMotionHalfTexel;
    vec2 mLo = vec2(eye * 0.5 + uMotionHalfTexel.x, uMotionHalfTexel.y);
    vec2 mHi = vec2(0.5 + eye * 0.5 - uMotionHalfTexel.x, 1.0 - uMotionHalfTexel.y);
    vec2 mL = texture(uMotion, clamp(motionUv - vec2(mStep.x, 0.0), mLo, mHi)).rg;
    vec2 mR = texture(uMotion, clamp(motionUv + vec2(mStep.x, 0.0), mLo, mHi)).rg;
    vec2 mU = texture(uMotion, clamp(motionUv - vec2(0.0, mStep.y), mLo, mHi)).rg;
    vec2 mD = texture(uMotion, clamp(motionUv + vec2(0.0, mStep.y), mLo, mHi)).rg;
    float jump = max(max(length(mR - m), length(mL - m)), max(length(mD - m), length(mU - m)));
    float damp = mix(1.0, 0.15, smoothstep(0.016, 0.048, jump));
    uv -= (m * damp) * uScale;
    uv.x = clamp(uv.x, eye * 0.5, 0.5 + eye * 0.5);
    uv.y = clamp(uv.y, 0.0, 1.0);
    vec2 sampleUv = uCrop.xy + Foveate(uv, eye) * uCrop.zw;
    vec2 cropMin = uCrop.xy + eye * 0.5 * uCrop.zw;
    vec2 cropMax = cropMin + 0.5 * uCrop.zw;
    float eyeMin = cropMin.x + eye * 1.5 * uHalfTexel.x;
    float eyeMax = cropMax.x - (1.0 - eye) * 1.5 * uHalfTexel.x;
    float eyeMid = 0.5 * (cropMin.x + cropMax.x);
    sampleUv.x = clamp(sampleUv.x, min(eyeMin, eyeMid), max(eyeMax, eyeMid));
    sampleUv.y = clamp(sampleUv.y, uCrop.y, uCrop.y + uCrop.w);
    vec2 casLo = vec2(min(eyeMin, eyeMid), uCrop.y);
    vec2 casHi = vec2(max(eyeMax, eyeMid), uCrop.y + uCrop.w);
    vec3 g = uSharpen > 0.001
        ? ApplyCasExtAt(sampleUv, casLo, casHi, uInvTexSize * uCrop.zw, uSharpen)
        : texture(uTex, sampleUv).rgb;
    g = ColorGrade(g);
    g = ApplyFoveaDebug(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x), g);
    oColor = vec4(ToLayer(g), 1.0);
}
)";
static const char* kBlit2DFs = R"(
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

SceneRenderer::~SceneRenderer() { Shutdown(); }

void SceneRenderer::Shutdown() {
    if (m_beamVao) {
        glDeleteVertexArrays(1, &m_beamVao);
        m_beamVao = 0;
    }
    if (m_beamVbo) {
        glDeleteBuffers(1, &m_beamVbo);
        m_beamVbo = 0;
    }
    if (m_gridVao) {
        glDeleteVertexArrays(1, &m_gridVao);
        m_gridVao = 0;
    }
    if (m_gridVbo) {
        glDeleteBuffers(1, &m_gridVbo);
        m_gridVbo = 0;
    }
}

bool SceneRenderer::Init(bool srgbSwapchain) {
    m_beamVao = 0;
    m_beamVbo = 0;
    m_gridVao = 0;
    m_gridVbo = 0;
    const std::string panelColor = srgbSwapchain
        ? "vec3 PanelLinearToEye(vec3 lin) { return lin; }\n"
          "vec3 PanelGammaToEye(vec3 g) { return mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)); }\n"
        : "vec3 PanelLinearToEye(vec3 lin) { vec3 c = max(lin, vec3(0.0)); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }\n"
          "vec3 PanelGammaToEye(vec3 g) { return g; }\n";
    bool ok = m_panel2D.Build(kPanelVs, (std::string(kPanelFsCommon) + panelColor + kPanel2DFs).c_str(), "panel2d");
    // GLSL requires #extension before any other statement: build the source with it first.
    const std::string extFs = std::string("#extension GL_OES_EGL_image_external_essl3 : require\n") +
                              kPanelFsCommon + panelColor + kPanelExtFs;
    ok &= m_panelExt.Build(kPanelVs, extFs.c_str(), "panelExt");
    ok &= m_beam.Build(kBeamVs, kBeamFs, "beam");
    ok &= m_dot.Build(kDotVs, kDotFs, "dot");
    // The video is gamma encoded and the layer images are sRGB. With sRGB writes switched off
    // (GL_EXT_sRGB_write_control) or on non-sRGB Swapchain formats (e.g. GL_RGBA8 fallback),
    // its values go in as they are; otherwise every pixel is decoded
    // to linear (three pow()s) for the hardware to encode right back, for the same bytes.
    const char* glExtensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    m_rawSrgbWrites = glExtensions && std::strstr(glExtensions, "GL_EXT_sRGB_write_control");
    const std::string toLayer = (!srgbSwapchain || m_rawSrgbWrites)
        ? "vec3 ToLayer(vec3 g) { return g; }\n"
        : "vec3 ToLayer(vec3 g) { return mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)); }\n";
    const std::string ext = "#extension GL_OES_EGL_image_external_essl3 : require\n";
    const std::string blitExt = ext + toLayer + kColorGradeFs + kCasExtFs + kFoveateFs + kBlitExtFs;
    const std::string blitExtGamma = ext + kFoveateFs + kBlitExtGammaFs;
    // Optional: without it, upscaling just stays off.
    m_upscalerReady = m_blitExtGamma.Build(kBlitVs, blitExtGamma.c_str(), "blitExtGamma");
    m_upscalerReady = m_gsr.Build(kBlitVs, (toLayer + kColorGradeFs + kGsrFs).c_str(), "gsr") && m_upscalerReady;
    m_upscalerReady = m_kept.Build(kBlitVs, (toLayer + kColorGradeFs + kCas2DFs + kKeptFs).c_str(), "kept") && m_upscalerReady;
    m_upscalerReady = m_keptMoved.Build(kBlitVs, (toLayer + kColorGradeFs + kCas2DFs + kKeptMovedFs).c_str(), "keptMoved") && m_upscalerReady;
    m_externalMovedReady = m_externalMoved.Build(kBlitVs, (ext + toLayer + kColorGradeFs + kCasExtFs + kFoveateFs + kExternalMovedFs).c_str(), "externalMoved");
    if (!m_externalMovedReady) {
        RC_LOGW("Optional externalMoved shader failed to build, frame reprojection will degrade safely");
    }
    ok &= m_blitExt.Build(kBlitVs, blitExt.c_str(), "blitExt");
    ok &= m_blit2D.Build(kBlitVs, kBlit2DFs, "blit2d");

    // Grid: (segments + 1) x 2 vertices as a triangle strip.
    std::vector<float> verts;
    for (int i = 0; i <= m_gridSegments; i++) {
        const float u = float(i) / m_gridSegments;
        verts.insert(verts.end(), {u, 0.f, u, 1.f});
    }
    glGenVertexArrays(1, &m_gridVao);
    glGenBuffers(1, &m_gridVbo);
    glBindVertexArray(m_gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return ok;
}

void SceneRenderer::Panel(const PanelDraw& p) {
    if (p.opacity <= 0.001f && !p.holePunch) return;
    const Program& prog = p.source == PanelSource::External ? m_panelExt : m_panel2D;
    prog.Use();
    const Mat4 mvp = m_viewProj * Mat4::FromPose(p.pose);
    glUniformMatrix4fv(prog.Uniform("uMvp"), 1, GL_FALSE, mvp.m);
    glUniform2f(prog.Uniform("uSize"), p.width, p.height);
    glUniform1f(prog.Uniform("uRadius"), p.radiusM);
    glUniform1f(prog.Uniform("uOpacity"), p.opacity);
    glUniform1f(prog.Uniform("uBrightness"), p.brightness);
    glUniform1f(prog.Uniform("uCorner"), p.cornerRadius);
    glUniform1i(prog.Uniform("uHole"), !p.holePunch ? 0 : p.holeFromAlpha ? 2 : 1);
    if (p.source == PanelSource::External) glUniform4fv(prog.Uniform("uCrop"), 1, p.crop);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(p.source == PanelSource::External ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, p.texture);
    glUniform1i(prog.Uniform("uTex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (p.holePunch) glBlendFunc(p.holeFromAlpha ? GL_ZERO : GL_ONE, p.holeFromAlpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ZERO);
    // Panels hide each other per pixel, whatever order they come in. One fading in or out
    // doesn't hide what's drawn after it.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(p.opacity >= 0.98f || p.holePunch ? GL_TRUE : GL_FALSE);
    glBindVertexArray(m_gridVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, (m_gridSegments + 1) * 2);
    glBindVertexArray(0);
}

void SceneRenderer::Beam(const Vec3& from, const Vec3& to, float width, const Color& start, const Color& end) {
    const Vec3 dir = Normalize(to - from);
    const Vec3 toEye = Normalize(m_eye - (from + to) * 0.5f);
    const Vec3 side = Normalize(Cross(dir, toEye)) * (width * 0.5f);
    const float verts[] = {
        from.x - side.x, from.y - side.y, from.z - side.z, 0, -1,
        from.x + side.x, from.y + side.y, from.z + side.z, 0, 1,
        to.x - side.x,   to.y - side.y,   to.z - side.z,   1, -1,
        to.x + side.x,   to.y + side.y,   to.z + side.z,   1, 1,
    };
    if (!m_beamVao) {
        glGenVertexArrays(1, &m_beamVao);
        glGenBuffers(1, &m_beamVbo);
        glBindVertexArray(m_beamVao);
        glBindBuffer(GL_ARRAY_BUFFER, m_beamVbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(4 * sizeof(float)));
    }
    glBindVertexArray(m_beamVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_beamVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    m_beam.Use();
    glUniformMatrix4fv(m_beam.Uniform("uViewProj"), 1, GL_FALSE, m_viewProj.m);
    glUniform4f(m_beam.Uniform("uStart"), start.r, start.g, start.b, start.a);
    glUniform4f(m_beam.Uniform("uEnd"), end.r, end.g, end.b, end.a);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void SceneRenderer::Dot(const Vec3& position, const Vec3& normal, float radius, const Color& c) {
    const Vec3 n = Normalize(normal);
    const Vec3 ref = std::fabs(n.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 right = Normalize(Cross(ref, n)) * radius;
    const Vec3 up = Normalize(Cross(n, right)) * radius;
    m_dot.Use();
    glUniformMatrix4fv(m_dot.Uniform("uViewProj"), 1, GL_FALSE, m_viewProj.m);
    const Vec3 center = position + n * 0.002f;
    glUniform3f(m_dot.Uniform("uCenter"), center.x, center.y, center.z);
    glUniform3f(m_dot.Uniform("uRight"), right.x, right.y, right.z);
    glUniform3f(m_dot.Uniform("uUp"), up.x, up.y, up.z);
    glUniform4f(m_dot.Uniform("uColor"), c.r, c.g, c.b, c.a);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void SceneRenderer::ControllerOrb(const Pose& aim, const Color& c) {
    const Vec3 toEye = Normalize(m_eye - aim.position);
    Dot(aim.position, toEye, 0.012f, c);
}

// Before a draw that covers the whole bound image: what it held isn't needed. Without this a
// tiled GPU, as the headsets have, first loads the old picture into every tile it is about to
// draw over, and for the game's picture that is millions of pixels read for nothing in the
// part of the frame the compositor waits for.
static void DiscardTarget() {
    const GLenum colour = GL_COLOR_ATTACHMENT0;
    glDisable(GL_SCISSOR_TEST);
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &colour);
}

// The squeeze of a foveated VR frame, or none.
static void SetFoveation(const Program& p, const VideoFoveation* f, bool debug = false) {
    glUniform1i(p.Uniform("uFoveated"), f ? 1 : 0);
    glUniform1i(p.Uniform("uFoveaDebug"), (f && debug) ? 1 : 0);
    if (!f) return;
    glUniform4f(p.Uniform("uFoveaStart"), f->start[0][0], f->start[0][1], f->start[1][0], f->start[1][1]);
    glUniform3f(p.Uniform("uFoveaLayout"), f->size, f->density[0], f->density[1]);
}

void SceneRenderer::BlitExternal(GLuint dstFbo, GLuint texture, int width, int height, const float* crop, float u0, float v0,
                                 float u1, float v1, const VideoFoveation* foveation,
                                 float sharpen, float vibrance, float darkGamma, int srcWidth, int srcHeight) {
    static const float kFull[4] = {0, 0, 1, 1};
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    if (u0 == 0 && v0 == 0 && u1 == 1 && v1 == 1) DiscardTarget();
    RawSrgbWrites raw(m_rawSrgbWrites);
    m_blitExt.Use();
    glUniform4f(m_blitExt.Uniform("uRect"), u0, v0, u1, v1);
    glUniform4fv(m_blitExt.Uniform("uCrop"), 1, crop ? crop : kFull);
    SetFoveation(m_blitExt, foveation, m_foveaDebug);
    const float tw = srcWidth > 0 ? float(srcWidth) : float(width);
    const float th = srcHeight > 0 ? float(srcHeight) : float(height);
    glUniform2f(m_blitExt.Uniform("uInvTexSize"), tw > 0 ? 1.0f / tw : 0.0f, th > 0 ? 1.0f / th : 0.0f);
    glUniform1f(m_blitExt.Uniform("uSharpen"), sharpen);
    glUniform1f(m_blitExt.Uniform("uVibrance"), vibrance);
    glUniform1f(m_blitExt.Uniform("uDarkGamma"), darkGamma);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glUniform1i(m_blitExt.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::KeepVideo(GLuint texture, const float* crop, int width, int height, const VideoFoveation* foveation) {
    static const float kFull[4] = {0, 0, 1, 1};
    if (rc::NeedsTextureResize(m_gsrWidth, m_gsrHeight, width, height)) {
        if (m_gsrFbo) glDeleteFramebuffers(1, &m_gsrFbo);
        if (m_gsrTexture) glDeleteTextures(1, &m_gsrTexture);
        glGenTextures(1, &m_gsrTexture);
        glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &m_gsrFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_gsrFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gsrTexture, 0);
        m_gsrWidth = width;
        m_gsrHeight = height;
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(EmptyVao());
    glBindFramebuffer(GL_FRAMEBUFFER, m_gsrFbo);
    glViewport(0, 0, width, height);
    DiscardTarget();
    m_blitExtGamma.Use();
    glUniform4f(m_blitExtGamma.Uniform("uRect"), 0, 0, 1, 1);
    glUniform4fv(m_blitExtGamma.Uniform("uCrop"), 1, crop ? crop : kFull);
    SetFoveation(m_blitExtGamma, foveation, m_foveaDebug);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glUniform1i(m_blitExtGamma.Uniform("uTex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::DrawKeptUpscaled(GLuint dstFbo, int dstW, int dstH) {
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    DiscardTarget();
    RawSrgbWrites raw(m_rawSrgbWrites);
    m_gsr.Use();
    glUniform4f(m_gsr.Uniform("uRect"), 0, 0, 1, 1);
    glUniform4f(m_gsr.Uniform("uViewport"), 1.f / m_gsrWidth, 1.f / m_gsrHeight, float(m_gsrWidth), float(m_gsrHeight));
    glUniform1f(m_gsr.Uniform("uVibrance"), m_vrVibrance);
    glUniform1f(m_gsr.Uniform("uDarkGamma"), m_vrDarkGamma);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
    glUniform1i(m_gsr.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::DrawKept(GLuint dstFbo, int dstW, int dstH) {
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    DiscardTarget();
    RawSrgbWrites raw(m_rawSrgbWrites);
    m_kept.Use();
    glUniform4f(m_kept.Uniform("uRect"), 0, 0, 1, 1);
    glUniform2f(m_kept.Uniform("uInvTexSize"), m_gsrWidth > 0 ? 1.f / m_gsrWidth : 0.f, m_gsrHeight > 0 ? 1.f / m_gsrHeight : 0.f);
    glUniform1f(m_kept.Uniform("uSharpen"), m_vrSharpen);
    glUniform1f(m_kept.Uniform("uVibrance"), m_vrVibrance);
    glUniform1f(m_kept.Uniform("uDarkGamma"), m_vrDarkGamma);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
    glUniform1i(m_kept.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool SceneRenderer::DrawKeptMoved(GLuint dstFbo, int dstW, int dstH, float scaleX, float scaleY, float fieldU, float fieldV) {
    if (!dstFbo || !m_upscalerReady || !m_gsrTexture || !m_motionTexture) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    DiscardTarget();
    RawSrgbWrites raw(m_rawSrgbWrites);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    m_keptMoved.Use();
    glUniform4f(m_keptMoved.Uniform("uRect"), 0, 0, 1, 1);
    glUniform2f(m_keptMoved.Uniform("uInvTexSize"), m_gsrWidth > 0 ? 1.f / m_gsrWidth : 0.f, m_gsrHeight > 0 ? 1.f / m_gsrHeight : 0.f);
    glUniform1f(m_keptMoved.Uniform("uSharpen"), m_vrSharpen);
    glUniform1f(m_keptMoved.Uniform("uVibrance"), m_vrVibrance);
    glUniform1f(m_keptMoved.Uniform("uDarkGamma"), m_vrDarkGamma);
    glUniform2f(m_keptMoved.Uniform("uScale"), scaleX, scaleY);
    glUniform2f(m_keptMoved.Uniform("uField"), fieldU, fieldV);
    glUniform2f(m_keptMoved.Uniform("uMotionHalfTexel"),
                m_motionWidth > 0 ? 0.5f / float(m_motionWidth) : 0.f,
                m_motionHeight > 0 ? 0.5f / float(m_motionHeight) : 0.f);
    glUniform2f(m_keptMoved.Uniform("uHalfTexel"),
                m_gsrWidth > 0 ? 0.5f / float(m_gsrWidth) : 0.f,
                m_gsrHeight > 0 ? 0.5f / float(m_gsrHeight) : 0.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
    glUniform1i(m_keptMoved.Uniform("uTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_motionTexture);
    glUniform1i(m_keptMoved.Uniform("uMotion"), 1);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

void SceneRenderer::RememberExternal(GLuint texture, int width, int height, const float* crop,
                                      const VideoFoveation* foveation) {
    static const float kFull[4] = {0, 0, 1, 1};
    m_externalTexture = texture;
    m_externalWidth = width;
    m_externalHeight = height;
    std::memcpy(m_externalCrop, crop ? crop : kFull, sizeof(m_externalCrop));
    m_externalFoveated = foveation != nullptr;
    if (foveation) m_externalFoveation = *foveation;
}

void SceneRenderer::ClearExternal() {
    RememberExternal(0, 0, 0, nullptr, nullptr);
    m_externalFoveation = {};
}

bool SceneRenderer::DrawExternalMoved(GLuint dstFbo, int dstW, int dstH, float scaleX, float scaleY, float fieldU, float fieldV) {
    if (!dstFbo || !m_externalMovedReady || !m_externalTexture || !m_motionTexture) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    DiscardTarget();
    RawSrgbWrites raw(m_rawSrgbWrites);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    m_externalMoved.Use();
    glUniform4f(m_externalMoved.Uniform("uRect"), 0, 0, 1, 1);
    glUniform4fv(m_externalMoved.Uniform("uCrop"), 1, m_externalCrop);
    SetFoveation(m_externalMoved, m_externalFoveated ? &m_externalFoveation : nullptr, m_foveaDebug);
    glUniform2f(m_externalMoved.Uniform("uInvTexSize"), m_externalWidth > 0 ? 1.f / m_externalWidth : 0.f,
                m_externalHeight > 0 ? 1.f / m_externalHeight : 0.f);
    glUniform1f(m_externalMoved.Uniform("uSharpen"), m_vrSharpen);
    glUniform1f(m_externalMoved.Uniform("uVibrance"), m_vrVibrance);
    glUniform1f(m_externalMoved.Uniform("uDarkGamma"), m_vrDarkGamma);
    glUniform2f(m_externalMoved.Uniform("uScale"), scaleX, scaleY);
    glUniform2f(m_externalMoved.Uniform("uField"), fieldU, fieldV);
    glUniform2f(m_externalMoved.Uniform("uMotionHalfTexel"),
                m_motionWidth > 0 ? 0.5f / float(m_motionWidth) : 0.f,
                m_motionHeight > 0 ? 0.5f / float(m_motionHeight) : 0.f);
    glUniform2f(m_externalMoved.Uniform("uHalfTexel"),
                m_externalWidth > 0 ? 0.5f / float(m_externalWidth) : 0.f,
                m_externalHeight > 0 ? 0.5f / float(m_externalHeight) : 0.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_externalTexture);
    glUniform1i(m_externalMoved.Uniform("uTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_motionTexture);
    glUniform1i(m_externalMoved.Uniform("uMotion"), 1);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

void SceneRenderer::UploadMotion(const int8_t* grid, int width, int height) {
    if (rc::NeedsTextureResize(m_motionWidth, m_motionHeight, width, height)) {
        if (m_motionTexture) glDeleteTextures(1, &m_motionTexture);
        glGenTextures(1, &m_motionTexture);
        glBindTexture(GL_TEXTURE_2D, m_motionTexture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8_SNORM, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_motionWidth = width;
        m_motionHeight = height;
    }
    glBindTexture(GL_TEXTURE_2D, m_motionTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RG, GL_BYTE, grid);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// The same, faded: premultiplied canvas times `alpha` over cleared black, for a window that is
// a compositor layer while it fades in or out.
void SceneRenderer::Blit2DAlpha(GLuint texture, int width, int height, float alpha) {
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendColor(0, 0, 0, alpha);
    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    m_blit2D.Use();
    glUniform4f(m_blit2D.Uniform("uRect"), 0, 0, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(m_blit2D.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

void SceneRenderer::Blit2D(GLuint texture, int width, int height, float u0, float v0, float u1, float v1) {
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    m_blit2D.Use();
    glUniform4f(m_blit2D.Uniform("uRect"), u0, v0, u1, v1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(m_blit2D.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace rv
