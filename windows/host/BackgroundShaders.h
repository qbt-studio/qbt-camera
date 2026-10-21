#pragma once

// HLSL compute shaders for background effects on the PC, compiled at startup with D3DCompile
// (d3dcompiler_47.dll ships with Windows). Same math as the iPhone's Core Image kernels, but at
// full resolution: the guided filter is applied per pixel of the 1080p/4K frame.
//
// Colors are the video processor's RGB output (gamma-encoded, 0…1).
inline constexpr char kBackgroundShaders[] = R"HLSL(
Texture2D<float4> gFull : register(t0);   // the picture, RGBA8
Texture2D<float4> gAux : register(t1);    // matte stream: [foreground | blur amount], gray
Texture2D<float4> gIn0 : register(t2);
Texture2D<float4> gIn1 : register(t3);
Texture2D<float4> gIn2 : register(t4);
Texture2D<float4> gIn3 : register(t5);
Texture2D<float4> gPrevious : register(t6);
RWTexture2D<float4> gOut0 : register(u0);
RWTexture2D<float4> gOut1 : register(u1);
RWTexture2D<float4> gOut2 : register(u2);
RWTexture2D<float4> gOut3 : register(u3);
Texture2D<float4> gImage : register(t7);  // the user's own backdrop picture
Texture3D<float4> gLut : register(t8);    // .cube color grade
SamplerState gLinear : register(s0);

cbuffer Parameters : register(b0) {
    float2 gOutSize;      // size of what this pass writes
    float2 gInvOutSize;
    float gRadius;        // box / gaussian radius in pixels
    float gSigma;
    float gEpsilon;
    float gKeep;          // temporal smoothing weight
    float4 gKey;          // chroma key color
    float gThreshold;
    float gSoftness;
    float gSpill;
    float gWrap;
    float4 gBackdropInner;
    float4 gBackdropOuter;
    float gBackdropKind;  // 0 radial, 1 vertical linear, 2 solid
    float gMode;          // 1 portrait, 2 replace, 3 chroma
    float gVertical;      // separable passes: 0 horizontal, 1 vertical
    float gAuxSplit;      // width of one half of the matte stream in UV (0.5)
    float4 gImageFit;     // backdrop picture: uv scale (xy) and offset (zw), "cover" fit
    float gLutStrength;   // 0 = no grade
    float gLutSize;
    float2 gPadding;
};

float3 Grade(float3 c) {
    if (gLutStrength <= 0.0) return c;
    float3 uvw = (saturate(c) * (gLutSize - 1.0) + 0.5) / gLutSize;
    return lerp(c, gLut.SampleLevel(gLinear, uvw, 0).rgb, gLutStrength);
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

bool Outside(uint2 id) { return id.x >= (uint)gOutSize.x || id.y >= (uint)gOutSize.y; }
float2 Uv(uint2 id) { return (id + 0.5) * gInvOutSize; }
float3 Guide(float3 c) { return sqrt(saturate(c)); }

// Half resolution: the 13 products the color guided filter needs, in four RGBA textures.
[numthreads(8, 8, 1)]
void Prepare(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    float2 uv = Uv(id.xy);
    float3 i = Guide(gFull.SampleLevel(gLinear, uv, 0).rgb);
    float edge = 0.5 * gAuxSplit / max(gOutSize.x, 1.0); // keep the two halves from bleeding together
    float p = gAux.SampleLevel(gLinear, float2(min(uv.x * gAuxSplit, gAuxSplit - edge), uv.y), 0).r;
    float blur = gAux.SampleLevel(gLinear, float2(max(gAuxSplit + uv.x * gAuxSplit, gAuxSplit + edge), uv.y), 0).r;
    gOut0[id.xy] = float4(i, p);
    gOut1[id.xy] = float4(i.r * i.r, i.r * i.g, i.r * i.b, i.g * i.g);
    gOut2[id.xy] = float4(i.g * i.b, i.b * i.b, i.r * p, i.g * p);
    gOut3[id.xy] = float4(i.b * p, blur, 0, 1);
}

// Separable box mean, gIn0 → gOut0.
[numthreads(8, 8, 1)]
void Box(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    int radius = (int)gRadius;
    int2 step = gVertical > 0.5 ? int2(0, 1) : int2(1, 0);
    int2 last = (int2)gOutSize - 1;
    float4 sum = 0;
    for (int k = -radius; k <= radius; ++k) sum += gIn0[clamp((int2)id.xy + step * k, 0, last)];
    gOut0[id.xy] = sum / (2 * radius + 1);
}

// Separable gaussian, gIn0 → gOut0.
[numthreads(8, 8, 1)]
void Gaussian(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    int radius = (int)gRadius;
    int2 step = gVertical > 0.5 ? int2(0, 1) : int2(1, 0);
    int2 last = (int2)gOutSize - 1;
    float4 sum = 0;
    float weights = 0;
    for (int k = -radius; k <= radius; ++k) {
        float w = exp(-(k * k) / (2.0 * gSigma * gSigma));
        sum += gIn0[clamp((int2)id.xy + step * k, 0, last)] * w;
        weights += w;
    }
    gOut0[id.xy] = sum / weights;
}

// a = (Σ + εI)⁻¹ · cov(I, p), b = mean(p) − a·mean(I) → (a, b).
[numthreads(8, 8, 1)]
void Coefficients(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    float4 mA = gIn0[id.xy], mB = gIn1[id.xy], mC = gIn2[id.xy], mD = gIn3[id.xy];
    float3 m = mA.rgb;
    float mp = mA.a;
    float rr = mB.r - m.r * m.r + gEpsilon;
    float rg = mB.g - m.r * m.g;
    float rb = mB.b - m.r * m.b;
    float gg = mB.a - m.g * m.g + gEpsilon;
    float gb = mC.r - m.g * m.b;
    float bb = mC.g - m.b * m.b + gEpsilon;
    float3 cov = float3(mC.b - m.r * mp, mC.a - m.g * mp, mD.r - m.b * mp);
    float c00 = gg * bb - gb * gb, c01 = rb * gb - rg * bb, c02 = rg * gb - rb * gg;
    float c11 = rr * bb - rb * rb, c12 = rb * rg - rr * gb, c22 = rr * gg - rg * rg;
    float det = rr * c00 + rg * c01 + rb * c02;
    float3 a = abs(det) < 1e-12 ? 0 : float3(c00 * cov.r + c01 * cov.g + c02 * cov.b,
                                             c01 * cov.r + c11 * cov.g + c12 * cov.b,
                                             c02 * cov.r + c12 * cov.g + c22 * cov.b) / det;
    gOut0[id.xy] = float4(a, mp - dot(a, m));
}

// Full resolution matte (alpha, blur amount), smoothed over time only where the edge stands still.
[numthreads(8, 8, 1)]
void Apply(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    float2 uv = Uv(id.xy);
    float3 i = Guide(gFull[id.xy].rgb);
    float4 ab = gIn0.SampleLevel(gLinear, uv, 0);
    float blur = gIn1.SampleLevel(gLinear, uv, 0).g;
    float alpha = smoothstep(0.04, 0.96, saturate(dot(ab.rgb, i) + ab.a));
    float4 previous = gPrevious[id.xy];
    float keep = gKeep * (1.0 - smoothstep(0.08, 0.25, abs(alpha - previous.r)));
    gOut0[id.xy] = float4(lerp(alpha, previous.r, keep), lerp(blur, previous.g, keep), 0, 1);
}

// Quarter resolution: background only, premultiplied by its weight (no halo, fills behind the person).
[numthreads(8, 8, 1)]
void BackgroundOnly(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    float2 uv = Uv(id.xy);
    float3 c = gFull.SampleLevel(gLinear, uv, 0).rgb;
    float w = 1.0 - gIn0.SampleLevel(gLinear, uv, 0).r;
    c *= 1.0 + 1.2 * smoothstep(0.85, 1.0, dot(c, kLuma)); // highlights bloom into round bokeh
    gOut0[id.xy] = float4(c * w, w);
}

float3 Unpremultiplied(float4 c, float3 fallback) { return c.a > 0.02 ? c.rgb / c.a : fallback; }

float3 Decontaminated(float3 color, float alpha, float3 background) {
    if (alpha > 0.98 || alpha < 0.02) return color;
    return saturate((color - (1.0 - alpha) * background) / alpha);
}

float3 Backdrop(uint2 id) {
    if (gBackdropKind > 2.5) return gImage.SampleLevel(gLinear, ((id + 0.5) * gInvOutSize) * gImageFit.xy + gImageFit.zw, 0).rgb;
    if (gBackdropKind > 1.5) return gBackdropInner.rgb;
    if (gBackdropKind > 0.5) return lerp(gBackdropInner.rgb, gBackdropOuter.rgb, (id.y + 0.5) * gInvOutSize.y);
    float2 center = float2(gOutSize.x * 0.5, gOutSize.y * 0.38);
    float t = saturate(length((float2)id - center) / (max(gOutSize.x, gOutSize.y) * 0.75));
    return lerp(gBackdropInner.rgb, gBackdropOuter.rgb, t);
}

// Final picture: portrait blur, replacement backdrop or chroma key.
[numthreads(8, 8, 1)]
void Composite(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    float2 uv = Uv(id.xy);
    float3 c = gFull[id.xy].rgb;
    float3 result = c;
    if (gMode > 2.5) {
        // Chroma key: background is where the chroma points the key's way (shadows too, skin never).
        float y = dot(c, kLuma);
        float2 chroma = float2(c.b - y, c.r - y);
        float keyY = dot(gKey.rgb, kLuma);
        float2 keyChroma = float2(gKey.b - keyY, gKey.r - keyY);
        float keyLength = max(length(keyChroma), 1e-3);
        float2 direction = keyChroma / keyLength;
        float along = dot(chroma, direction) / keyLength;
        float across = abs(chroma.x * direction.y - chroma.y * direction.x) / keyLength;
        float alpha = 1.0 - smoothstep(gThreshold, gThreshold + gSoftness, along - 1.5 * across);
        float3 subject = c;
        float cast = max(dot(chroma, direction), 0.0);
        if (cast > 0.0) {
            float2 fixedChroma = chroma - direction * cast * gSpill;
            float r = y + fixedChroma.y, b = y + fixedChroma.x;
            subject = saturate(float3(r, (y - kLuma.r * r - kLuma.b * b) / kLuma.g, b));
        }
        float3 backdrop = Backdrop(id.xy);
        result = lerp(backdrop, subject, alpha);
        result = lerp(result, backdrop, alpha * (1.0 - alpha) * 4.0 * gWrap * 0.5);
    } else {
        float4 matte = gPrevious[id.xy]; // (alpha, blur)
        float alpha = matte.r;
        float edge = alpha * (1.0 - alpha) * 4.0;
        if (gMode > 1.5) {
            float3 estimate = Unpremultiplied(gIn1.SampleLevel(gLinear, uv, 0), c);
            float3 backdrop = Backdrop(id.xy);
            result = lerp(backdrop, Decontaminated(c, alpha, estimate), alpha);
            result = lerp(result, backdrop, edge * gWrap);
        } else {
            float3 c3 = Unpremultiplied(gIn3.SampleLevel(gLinear, uv, 0), c);
            float3 c2 = Unpremultiplied(gIn2.SampleLevel(gLinear, uv, 0), c3);
            float3 c1 = Unpremultiplied(gIn1.SampleLevel(gLinear, uv, 0), c2);
            float t = matte.g * 2.0;
            float3 background = t < 1.0 ? lerp(c1, c2, t) : lerp(c2, c3, t - 1.0);
            result = lerp(background, Decontaminated(c, alpha, c1), alpha);
            result = lerp(result, c3, edge * gWrap);
        }
    }
    gOut0[id.xy] = float4(Grade(result), 1);
}

// No background effect, just the color grade.
[numthreads(8, 8, 1)]
void GradeOnly(uint3 id : SV_DispatchThreadID) {
    if (Outside(id.xy)) return;
    gOut0[id.xy] = float4(Grade(gFull[id.xy].rgb), 1);
}
)HLSL";
