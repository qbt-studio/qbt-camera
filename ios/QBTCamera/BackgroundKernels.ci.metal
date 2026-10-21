// Core Image kernels for QBT background effects (compiled with -fcikernel, linked with -cikernel).
// Everything runs on the GPU inside the Core Image graph built in BackgroundEngine.swift.

#include <CoreImage/CoreImage.h>
using namespace metal;

constant float3 kLuma = float3(0.2126, 0.7152, 0.0722); // BT.709

extern "C" {
namespace coreimage {

// Depth → (blur amount, in focus). Depth in meters; holes (0, NaN, inf) count as far away.
// Blur grows with the distance from the focus plane, faster behind it than in front, like a lens.
float4 qbtDepthMap(sample_t depth, float focus) {
    float d = depth.r;
    if (!(d > 0.05) || isinf(d)) d = 100.0;
    float behind = (d - focus) / (focus * 0.55 + 0.25);
    float inFront = (focus - d) / (focus * 0.6 + 0.2);
    float blur = clamp(max(behind, inFront * 0.6), 0.0, 1.0);
    float inFocus = 1.0 - smoothstep(0.10, 0.32, blur);
    return float4(blur, inFocus, 0.0, 1.0);
}

// Coarse foreground: the person, plus whatever stands in the focus plane (a mug, a guitar).
// The person mask is a couple of frames old; depth is current. Where depth sees the far wall,
// the old mask is wrong (the head already moved away), so depth vetoes it: no trails.
// `nearPerson` (the person mask spread wide) limits depth-only foreground to things touching the
// person — what they hold — so a curtain at the same distance doesn't count as the subject.
float4 qbtCoarseMatte(sample_t person, sample_t depthMap, sample_t nearPerson, float useDepth) {
    float p = person.r;
    if (useDepth < 0.5) return float4(p, 1.0, 0.0, 1.0);
    float blur = depthMap.r;
    float nearSubject = 1.0 - smoothstep(0.35, 0.65, blur);
    float attached = smoothstep(0.03, 0.15, nearPerson.r);
    float fg = max(p * nearSubject, depthMap.g * nearSubject * attached);
    return float4(fg, blur, 0.0, 1.0);
}

// Guided filter, step 1: pack I, p, I², I·p so one box blur gives all four means.
float4 qbtGuidePrep(sample_t color, sample_t coarse) {
    float i = dot(color.rgb, kLuma);
    float p = coarse.r;
    return float4(i, p, i * i, i * p);
}

// Guided filter, step 2: per-pixel linear model alpha ≈ a·I + b.
float4 qbtGuideCoefficients(sample_t means, float epsilon) {
    float varianceI = max(means.b - means.r * means.r, 0.0);
    float covariance = means.a - means.r * means.g;
    float a = covariance / (varianceI + epsilon);
    float b = means.g - a * means.r;
    return float4(a, b, 0.0, 1.0);
}

// Guided filter, step 3: the matte follows the real edges of this frame (hair, fingers),
// then blends with the previous frame so edges don't shimmer. Output: (alpha, blur amount).
float4 qbtGuideApply(sample_t color, sample_t coefficients, sample_t coarse, sample_t previous, float keepPrevious) {
    float i = dot(color.rgb, kLuma);
    float alpha = clamp(coefficients.r * i + coefficients.g, 0.0, 1.0);
    alpha = smoothstep(0.15, 0.85, alpha); // crisp, like a real cut-out
    float blur = coarse.g;
    // Smooth only small flicker along a still edge; anything that really moved takes the new
    // value at once, so a moving head never leaves a ghost behind.
    float change = abs(alpha - previous.r);
    float keep = keepPrevious * (1.0 - smoothstep(0.08, 0.25, change));
    alpha = mix(alpha, previous.r, keep);
    blur = mix(blur, previous.g, keep);
    return float4(alpha, blur, 0.0, 1.0);
}

// ---- Color guided filter (He et al.): the matte follows edges in color, not just brightness,
// so blond hair against a beige wall separates even when both are equally bright.
// Guide = color in a perceptual (square-root) scale; means of the 13 needed products are packed
// into four RGBA images so four box blurs give them all.

static float3 qbtGuide(float3 c) { return sqrt(max(c, 0.0)); }

float4 qbtGuideColorA(sample_t color, sample_t coarse) {
    return float4(qbtGuide(color.rgb), coarse.r);
}

float4 qbtGuideColorB(sample_t color) {
    float3 i = qbtGuide(color.rgb);
    return float4(i.r * i.r, i.r * i.g, i.r * i.b, i.g * i.g);
}

float4 qbtGuideColorC(sample_t color, sample_t coarse) {
    float3 i = qbtGuide(color.rgb);
    float p = coarse.r;
    return float4(i.g * i.b, i.b * i.b, i.r * p, i.g * p);
}

float4 qbtGuideColorD(sample_t color, sample_t coarse) {
    float3 i = qbtGuide(color.rgb);
    return float4(i.b * coarse.r, 0.0, 0.0, 1.0);
}

// a = (Σ + εI)⁻¹ · cov(I, p), b = mean(p) − a·mean(I); packed as (a.r, a.g, a.b, b).
float4 qbtGuideColorCoefficients(sample_t mA, sample_t mB, sample_t mC, sample_t mD, float epsilon) {
    float3 m = mA.rgb;
    float mp = mA.a;
    float rr = mB.r - m.r * m.r + epsilon;
    float rg = mB.g - m.r * m.g;
    float rb = mB.b - m.r * m.b;
    float gg = mB.a - m.g * m.g + epsilon;
    float gb = mC.r - m.g * m.b;
    float bb = mC.g - m.b * m.b + epsilon;
    float3 cov = float3(mC.b - m.r * mp, mC.a - m.g * mp, mD.r - m.b * mp);
    // Inverse of the symmetric 3×3 covariance via cofactors.
    float c00 = gg * bb - gb * gb;
    float c01 = rb * gb - rg * bb;
    float c02 = rg * gb - rb * gg;
    float c11 = rr * bb - rb * rb;
    float c12 = rb * rg - rr * gb;
    float c22 = rr * gg - rg * rg;
    float det = rr * c00 + rg * c01 + rb * c02;
    if (abs(det) < 1e-12) return float4(0.0, 0.0, 0.0, mp);
    float3 a = float3(c00 * cov.r + c01 * cov.g + c02 * cov.b,
                      c01 * cov.r + c11 * cov.g + c12 * cov.b,
                      c02 * cov.r + c12 * cov.g + c22 * cov.b) / det;
    float b = mp - dot(a, m);
    return float4(a, b);
}

// Refined (alpha, blur), blended with the previous frame only where the edge stands still.
float4 qbtGuideColorApply(sample_t color, sample_t coefficients, sample_t coarse, sample_t previous, float keepPrevious) {
    float3 i = qbtGuide(color.rgb);
    float alpha = clamp(dot(coefficients.rgb, i) + coefficients.a, 0.0, 1.0);
    alpha = smoothstep(0.04, 0.96, alpha);
    float blur = coarse.g;
    float change = abs(alpha - previous.r);
    float keep = keepPrevious * (1.0 - smoothstep(0.08, 0.25, change));
    alpha = mix(alpha, previous.r, keep);
    blur = mix(blur, previous.g, keep);
    return float4(alpha, blur, 0.0, 1.0);
}

// Background only, premultiplied by its weight: blurring it never smears the person into the
// background (no halo), and dividing by the blurred weight fills the space behind the person.
float4 qbtBackgroundOnly(sample_t color, sample_t matte) {
    float w = 1.0 - matte.r;
    // Lift highlights a little before blurring: bright spots bloom into round "bokeh" like glass.
    float l = dot(color.rgb, kLuma);
    float3 c = color.rgb * (1.0 + 1.6 * smoothstep(0.72, 1.0, l));
    return float4(c * w, w);
}

static float3 unpremultiplied(float4 c, float3 fallback) {
    return c.a > 0.02 ? c.rgb / c.a : fallback;
}

// Edge pixels mix the person with the old background (the wall glows through hair).
// Solve color = alpha·F + (1−alpha)·B for the person's own color F, B being the estimated
// background behind that pixel: the fringe disappears on any new background.
static float3 decontaminated(float3 color, float alpha, float3 background) {
    if (alpha > 0.98 || alpha < 0.02) return color;
    float3 foreground = (color - (1.0 - alpha) * background) / alpha;
    return clamp(foreground, 0.0, 1.0);
}

// Portrait: sharp subject over a background whose blur follows the depth map,
// plus a touch of "light wrap" so the edge sits in the scene instead of looking cut out.
float4 qbtPortrait(sample_t color, sample_t matte, sample_t level1, sample_t level2, sample_t level3, float wrap) {
    float alpha = matte.r;
    float t = matte.g * 2.0;
    float3 c3 = unpremultiplied(level3, color.rgb);
    float3 c2 = unpremultiplied(level2, c3);
    float3 c1 = unpremultiplied(level1, c2);
    float3 background = t < 1.0 ? mix(c1, c2, t) : mix(c2, c3, t - 1.0);
    float3 subject = decontaminated(color.rgb, alpha, c1);
    float3 result = mix(background, subject, alpha);
    float edge = alpha * (1.0 - alpha) * 4.0;
    result = mix(result, c3, edge * wrap);
    return float4(result, 1.0);
}

// Replacement backdrop behind the subject, with light wrap from the backdrop.
// `estimate` is the old background (person removed and filled in), used to clean the edge.
float4 qbtReplace(sample_t color, sample_t matte, sample_t backdrop, sample_t estimate, float wrap) {
    float alpha = matte.r;
    float3 subject = decontaminated(color.rgb, alpha, unpremultiplied(estimate, color.rgb));
    float3 result = mix(backdrop.rgb, subject, alpha);
    float edge = alpha * (1.0 - alpha) * 4.0;
    result = mix(result, backdrop.rgb, edge * wrap);
    return float4(result, 1.0);
}

// Chroma key in YCbCr: a pixel is background when its chroma points the key's way.
// Works for shadows on the screen (same hue, less saturated) and keeps skin (opposite hue).
// Spill: the key-colored cast on hair and skin edges is removed from the foreground.
float4 qbtChromaKey(sample_t color, sample_t backdrop, float3 key, float threshold, float softness, float spill, float wrap) {
    float3 c = color.rgb;
    float y = dot(c, kLuma);
    float2 chroma = float2(c.b - y, c.r - y);
    float keyY = dot(key, kLuma);
    float2 keyChroma = float2(key.b - keyY, key.r - keyY);
    float keyLength = max(length(keyChroma), 1e-3);
    float2 direction = keyChroma / keyLength;
    float along = dot(chroma, direction) / keyLength;
    float across = abs(chroma.x * direction.y - chroma.y * direction.x) / keyLength;
    float keyness = along - 1.5 * across;
    float alpha = 1.0 - smoothstep(threshold, threshold + softness, keyness);

    float3 despilled = c;
    float cast = max(dot(chroma, direction), 0.0);
    if (cast > 0.0) {
        float2 fixedChroma = chroma - direction * cast * spill;
        float r = y + fixedChroma.y;
        float b = y + fixedChroma.x;
        float g = (y - kLuma.r * r - kLuma.b * b) / kLuma.g;
        despilled = clamp(float3(r, g, b), 0.0, 1.0);
    }
    float3 result = mix(backdrop.rgb, despilled, alpha);
    float edge = alpha * (1.0 - alpha) * 4.0;
    result = mix(result, backdrop.rgb, edge * wrap * 0.5);
    return float4(result, 1.0);
}

}
}
