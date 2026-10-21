#pragma once
#include <d3d11.h>

#include <functional>

#include "Common.h"

/// What the phone's background effect is set to (from its state messages).
struct BackgroundSettings {
    enum class Mode { Off, Portrait, Replace, Chroma };
    Mode mode = Mode::Off;
    float blur = 0.6f;          // portrait strength 0…1
    int backdrop = 0;           // studio, glow, warm, white, black, green
    float keySensitivity = 0.5f, keySoftness = 0.4f, keySpill = 0.7f;
    unsigned keyGeneration = 0; // bumped when the user asks to sample the screen color again
};

// Background effects on the PC's GPU: the phone sends the untouched picture plus a small matte stream
// (coarse foreground and depth-based blur amount); here a color guided filter refines the matte at full
// resolution against the picture itself, then portrait blur, a replacement backdrop or a chroma key
// is composited — all in D3D11 compute shaders.
class BackgroundCompositor {
public:
    /// Converts a decoded frame into an RGBA8 texture of the given size (the pipeline's video processor).
    using Converter = std::function<HRESULT(const DecodedFrame& frame, ID3D11Texture2D* target, UINT width, UINT height)>;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context, Converter convert);

    /// `aux` may be null for chroma key (it needs no matte). On success `out` is an RGB frame.
    HRESULT Process(const DecodedFrame& frame, const DecodedFrame* aux, const BackgroundSettings& settings, DecodedFrame& out);

private:
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        UINT width = 0, height = 0;
    };
    struct Parameters { // matches cbuffer Parameters in BackgroundShaders.h
        float outSize[2], invOutSize[2];
        float radius, sigma, epsilon, keep;
        float key[4];
        float threshold, softness, spill, wrap;
        float backdropInner[4], backdropOuter[4];
        float backdropKind, mode, vertical, auxSplit;
    };
    struct Binding {
        ID3D11ShaderResourceView* inputs[7] = {};   // t0…t6
        ID3D11UnorderedAccessView* outputs[4] = {}; // u0…u3
    };

    HRESULT MakeTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, bool renderTarget);
    HRESULT EnsureTargets(UINT width, UINT height, UINT auxWidth, UINT auxHeight);
    void Run(ID3D11ComputeShader* shader, const Target& size, const Binding& binding, Parameters parameters);
    void Separable(ID3D11ComputeShader* shader, const Target& input, Target& scratch, Target& output, Parameters parameters);
    void SampleKeyColor();
    void FillBackdrop(Parameters& parameters, int backdrop) const;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    Converter convert_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> linear_;
    ComPtr<ID3D11ComputeShader> prepare_, box_, gaussian_, coefficients_, apply_, backgroundOnlyShader_, composite_;

    UINT width_ = 0, height_ = 0, auxWidth_ = 0, auxHeight_ = 0;
    Target full_, aux_, output_;
    Target prepared_[4], means_[4], halfScratch_, coefficientsRaw_, coefficientMeans_;
    Target mattes_[2];
    int currentMatte_ = 0;
    bool havePreviousMatte_ = false;
    Target backgroundOnly_, quarterScratch_, levels_[3];
    ComPtr<ID3D11Texture2D> keyStaging_;

    float key_[3] = {0.0f, 0.69f, 0.25f};
    unsigned keyGeneration_ = ~0u;
};
