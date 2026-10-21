#include "BackgroundCompositor.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "BackgroundShaders.h"

#pragma comment(lib, "d3dcompiler")

namespace {

UINT Groups(UINT size) { return (size + 7) / 8; }

// Backdrops, same colors as on the iPhone (inner/outer, kind: 0 radial, 1 vertical, 2 solid).
struct BackdropColors {
    float inner[3], outer[3];
    float kind;
};
constexpr BackdropColors kBackdrops[] = {
    {{0.30f, 0.33f, 0.38f}, {0.07f, 0.08f, 0.10f}, 0}, // studio
    {{0.20f, 0.45f, 0.95f}, {0.03f, 0.05f, 0.16f}, 0}, // QBT glow
    {{0.86f, 0.77f, 0.66f}, {0.52f, 0.42f, 0.33f}, 0}, // warm
    {{0.97f, 0.97f, 0.98f}, {0.84f, 0.85f, 0.87f}, 1}, // white
    {{0.02f, 0.02f, 0.03f}, {0.02f, 0.02f, 0.03f}, 2}, // black
    {{0.00f, 0.69f, 0.25f}, {0.00f, 0.69f, 0.25f}, 2}, // broadcast green for OBS
};

} // namespace

HRESULT BackgroundCompositor::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, Converter convert) {
    device_ = device;
    context_ = context;
    convert_ = std::move(convert);

    const auto compile = [&](const char* entry, ComPtr<ID3D11ComputeShader>& shader) -> HRESULT {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompile(kBackgroundShaders, sizeof(kBackgroundShaders) - 1, "BackgroundShaders", nullptr, nullptr,
                                      entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            qbt::Log("background: shader %s failed: %s", entry,
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            return hr;
        }
        return device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
    };
    RETURN_IF_FAILED(compile("Prepare", prepare_));
    RETURN_IF_FAILED(compile("Box", box_));
    RETURN_IF_FAILED(compile("Gaussian", gaussian_));
    RETURN_IF_FAILED(compile("Coefficients", coefficients_));
    RETURN_IF_FAILED(compile("Apply", apply_));
    RETURN_IF_FAILED(compile("BackgroundOnly", backgroundOnlyShader_));
    RETURN_IF_FAILED(compile("Composite", composite_));

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = (sizeof(Parameters) + 15) & ~15u;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    RETURN_IF_FAILED(device_->CreateBuffer(&buffer, nullptr, &constants_));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    RETURN_IF_FAILED(device_->CreateSamplerState(&sampler, &linear_));
    qbt::Log("background: GPU compositor ready");
    return S_OK;
}

HRESULT BackgroundCompositor::MakeTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, bool renderTarget) {
    target = {};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = std::max(width, 1u);
    desc.Height = std::max(height, 1u);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    RETURN_IF_FAILED(device_->CreateTexture2D(&desc, nullptr, &target.texture));
    RETURN_IF_FAILED(device_->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv));
    RETURN_IF_FAILED(device_->CreateUnorderedAccessView(target.texture.Get(), nullptr, &target.uav));
    target.width = desc.Width;
    target.height = desc.Height;
    return S_OK;
}

HRESULT BackgroundCompositor::EnsureTargets(UINT width, UINT height, UINT auxWidth, UINT auxHeight) {
    if (width != width_ || height != height_) {
        constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R8G8B8A8_UNORM, kFloat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const UINT halfWidth = width / 2, halfHeight = height / 2, quarterWidth = width / 4, quarterHeight = height / 4;
        RETURN_IF_FAILED(MakeTarget(full_, width, height, kColor, true));
        RETURN_IF_FAILED(MakeTarget(output_, width, height, kColor, true));
        for (Target& target : prepared_) RETURN_IF_FAILED(MakeTarget(target, halfWidth, halfHeight, kFloat, false));
        for (Target& target : means_) RETURN_IF_FAILED(MakeTarget(target, halfWidth, halfHeight, kFloat, false));
        RETURN_IF_FAILED(MakeTarget(halfScratch_, halfWidth, halfHeight, kFloat, false));
        RETURN_IF_FAILED(MakeTarget(coefficientsRaw_, halfWidth, halfHeight, kFloat, false));
        RETURN_IF_FAILED(MakeTarget(coefficientMeans_, halfWidth, halfHeight, kFloat, false));
        for (Target& target : mattes_) RETURN_IF_FAILED(MakeTarget(target, width, height, kFloat, false));
        RETURN_IF_FAILED(MakeTarget(backgroundOnly_, quarterWidth, quarterHeight, kFloat, false));
        RETURN_IF_FAILED(MakeTarget(quarterScratch_, quarterWidth, quarterHeight, kFloat, false));
        for (Target& target : levels_) RETURN_IF_FAILED(MakeTarget(target, quarterWidth, quarterHeight, kFloat, false));
        keyStaging_.Reset();
        width_ = width;
        height_ = height;
        havePreviousMatte_ = false;
        qbt::Log("background: compositing %ux%u on the GPU", width, height);
    }
    if (auxWidth && (auxWidth != auxWidth_ || auxHeight != auxHeight_)) {
        RETURN_IF_FAILED(MakeTarget(aux_, auxWidth, auxHeight, DXGI_FORMAT_R8G8B8A8_UNORM, true));
        auxWidth_ = auxWidth;
        auxHeight_ = auxHeight;
    }
    return S_OK;
}

void BackgroundCompositor::Run(ID3D11ComputeShader* shader, const Target& size, const Binding& binding, Parameters parameters) {
    parameters.outSize[0] = static_cast<float>(size.width);
    parameters.outSize[1] = static_cast<float>(size.height);
    parameters.invOutSize[0] = 1.0f / size.width;
    parameters.invOutSize[1] = 1.0f / size.height;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &parameters, sizeof(parameters));
        context_->Unmap(constants_.Get(), 0);
    }
    context_->CSSetShader(shader, nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->CSSetSamplers(0, 1, linear_.GetAddressOf());
    context_->CSSetShaderResources(0, 7, binding.inputs);
    context_->CSSetUnorderedAccessViews(0, 4, binding.outputs, nullptr);
    context_->Dispatch(Groups(size.width), Groups(size.height), 1);
    // Unbind so the next pass can read what this one wrote.
    ID3D11ShaderResourceView* noInputs[7] = {};
    ID3D11UnorderedAccessView* noOutputs[4] = {};
    context_->CSSetShaderResources(0, 7, noInputs);
    context_->CSSetUnorderedAccessViews(0, 4, noOutputs, nullptr);
}

void BackgroundCompositor::Separable(ID3D11ComputeShader* shader, const Target& input, Target& scratch, Target& output,
                                     Parameters parameters) {
    Binding horizontal;
    horizontal.inputs[2] = input.srv.Get();
    horizontal.outputs[0] = scratch.uav.Get();
    parameters.vertical = 0;
    Run(shader, scratch, horizontal, parameters);
    Binding vertical;
    vertical.inputs[2] = scratch.srv.Get();
    vertical.outputs[0] = output.uav.Get();
    parameters.vertical = 1;
    Run(shader, output, vertical, parameters);
}

void BackgroundCompositor::FillBackdrop(Parameters& parameters, int backdrop) const {
    const BackdropColors& colors = kBackdrops[std::clamp(backdrop, 0, static_cast<int>(std::size(kBackdrops)) - 1)];
    for (int i = 0; i < 3; ++i) {
        parameters.backdropInner[i] = colors.inner[i];
        parameters.backdropOuter[i] = colors.outer[i];
    }
    parameters.backdropKind = colors.kind;
}

void BackgroundCompositor::SampleKeyColor() {
    // The screen's color: the most saturated pixels along the top and the sides of the picture.
    if (!keyStaging_) {
        D3D11_TEXTURE2D_DESC desc{};
        full_.texture->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &keyStaging_))) return;
    }
    context_->CopyResource(keyStaging_.Get(), full_.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(keyStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    struct Sample {
        float saturation, r, g, b;
    };
    std::vector<Sample> samples;
    const auto add = [&](UINT x, UINT y) {
        const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4;
        const float r = pixel[0] / 255.0f, g = pixel[1] / 255.0f, b = pixel[2] / 255.0f;
        samples.push_back({std::max({r, g, b}) - std::min({r, g, b}), r, g, b});
    };
    for (UINT x = 0; x < width_; x += std::max(1u, width_ / 64)) {
        add(x, 2);
        add(x, 6);
    }
    for (UINT y = 0; y < height_ * 2 / 3; y += std::max(1u, height_ / 36)) {
        add(2, y);
        add(width_ - 3, y);
    }
    context_->Unmap(keyStaging_.Get(), 0);
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.saturation > b.saturation; });
    const size_t count = std::max<size_t>(8, samples.size() / 3);
    if (samples.empty() || samples.front().saturation < 0.12f) return; // no colored screen: keep the last key
    float sum[3] = {};
    for (size_t i = 0; i < std::min(count, samples.size()); ++i) {
        sum[0] += samples[i].r;
        sum[1] += samples[i].g;
        sum[2] += samples[i].b;
    }
    for (int i = 0; i < 3; ++i) key_[i] = sum[i] / std::min(count, samples.size());
    qbt::Log("background: key color %.2f %.2f %.2f", key_[0], key_[1], key_[2]);
}

HRESULT BackgroundCompositor::Process(const DecodedFrame& frame, const DecodedFrame* aux, const BackgroundSettings& settings,
                                      DecodedFrame& out) {
    if (!prepare_) return E_UNEXPECTED;
    const UINT width = static_cast<UINT>(frame.visible.right - frame.visible.left) & ~3u;
    const UINT height = static_cast<UINT>(frame.visible.bottom - frame.visible.top) & ~3u;
    const bool chroma = settings.mode == BackgroundSettings::Mode::Chroma;
    if (!chroma && !aux) return E_INVALIDARG;
    const UINT auxWidth = aux ? static_cast<UINT>(aux->visible.right - aux->visible.left) : 0;
    const UINT auxHeight = aux ? static_cast<UINT>(aux->visible.bottom - aux->visible.top) : 0;
    RETURN_IF_FAILED(EnsureTargets(width, height, auxWidth, auxHeight));
    RETURN_IF_FAILED(convert_(frame, full_.texture.Get(), width, height));

    Parameters parameters{};
    parameters.auxSplit = 0.5f;
    FillBackdrop(parameters, settings.backdrop);

    if (chroma) {
        if (keyGeneration_ != settings.keyGeneration) {
            keyGeneration_ = settings.keyGeneration;
            SampleKeyColor();
        }
        parameters.mode = 3;
        for (int i = 0; i < 3; ++i) parameters.key[i] = key_[i];
        parameters.threshold = 0.42f - 0.34f * settings.keySensitivity;
        parameters.softness = 0.06f + 0.3f * settings.keySoftness;
        parameters.spill = settings.keySpill;
        parameters.wrap = 0.3f;
        Binding binding;
        binding.inputs[0] = full_.srv.Get();
        binding.outputs[0] = output_.uav.Get();
        Run(composite_.Get(), output_, binding, parameters);
    } else {
        RETURN_IF_FAILED(convert_(*aux, aux_.texture.Get(), auxWidth_, auxHeight_));

        // 1. Guided filter statistics at half resolution.
        Binding prepare;
        prepare.inputs[0] = full_.srv.Get();
        prepare.inputs[1] = aux_.srv.Get();
        for (int i = 0; i < 4; ++i) prepare.outputs[i] = prepared_[i].uav.Get();
        Run(prepare_.Get(), prepared_[0], prepare, parameters);
        parameters.radius = std::max(4.0f, std::round(prepared_[0].width / 110.0f));
        for (int i = 0; i < 4; ++i) Separable(box_.Get(), prepared_[i], halfScratch_, means_[i], parameters);

        // 2. Per-pixel linear model, smoothed.
        Binding coefficients;
        for (int i = 0; i < 4; ++i) coefficients.inputs[2 + i] = means_[i].srv.Get();
        coefficients.outputs[0] = coefficientsRaw_.uav.Get();
        parameters.epsilon = 0.0004f;
        Run(coefficients_.Get(), coefficientsRaw_, coefficients, parameters);
        Separable(box_.Get(), coefficientsRaw_, halfScratch_, coefficientMeans_, parameters);

        // 3. Full-resolution matte against this very frame, steadied with the previous one.
        Target& matte = mattes_[currentMatte_];
        const Target& previous = mattes_[1 - currentMatte_];
        Binding apply;
        apply.inputs[0] = full_.srv.Get();
        apply.inputs[2] = coefficientMeans_.srv.Get();
        apply.inputs[3] = means_[3].srv.Get(); // blur amount in .g
        apply.inputs[6] = previous.srv.Get();
        apply.outputs[0] = matte.uav.Get();
        parameters.keep = havePreviousMatte_ ? 0.4f : 0.0f;
        Run(apply_.Get(), matte, apply, parameters);

        // 4. Background without the person at quarter resolution, blurred in levels.
        Binding backgroundOnly;
        backgroundOnly.inputs[0] = full_.srv.Get();
        backgroundOnly.inputs[2] = matte.srv.Get();
        backgroundOnly.outputs[0] = backgroundOnly_.uav.Get();
        Run(backgroundOnlyShader_.Get(), backgroundOnly_, backgroundOnly, parameters);
        const float unit = std::min(backgroundOnly_.width, backgroundOnly_.height) / 270.0f;
        const float amount = std::clamp(settings.blur, 0.05f, 1.0f);
        const bool replace = settings.mode == BackgroundSettings::Mode::Replace;
        const float sigmas[3] = {replace ? 3.0f : 1 + 3 * amount, 2 + 8 * amount, 4 + 18 * amount};
        const int levelCount = replace ? 1 : 3;
        for (int i = 0; i < levelCount; ++i) {
            parameters.sigma = sigmas[i] * unit;
            parameters.radius = std::min(120.0f, std::ceil(parameters.sigma * 3));
            Separable(gaussian_.Get(), backgroundOnly_, quarterScratch_, levels_[i], parameters);
        }

        // 5. The final picture.
        Binding composite;
        composite.inputs[0] = full_.srv.Get();
        for (int i = 0; i < 3; ++i) composite.inputs[3 + i] = levels_[i].srv.Get();
        composite.inputs[6] = matte.srv.Get();
        composite.outputs[0] = output_.uav.Get();
        parameters.mode = replace ? 2.0f : 1.0f;
        parameters.wrap = replace ? 0.12f : 0.25f;
        Run(composite_.Get(), output_, composite, parameters);

        currentMatte_ = 1 - currentMatte_;
        havePreviousMatte_ = true;
    }

    out = {};
    out.texture = output_.texture;
    out.subresource = 0;
    out.visible = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    out.rgb = true;
    return S_OK;
}
