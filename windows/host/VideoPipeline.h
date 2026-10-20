#pragma once
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <functional>

#include "Common.h"

// Owns the D3D11 device (shared with the decoder), the preview swap chain and the GPU video
// processor that scales/converts decoded frames for the preview and for the virtual camera.
class VideoPipeline {
public:
    HRESULT Initialize(HWND window);
    ID3D11Device* Device() const { return device_.Get(); }

    /// Called from the UI thread on WM_SIZE; the render thread resizes before the next frame.
    void RequestResize() { resizePending_ = true; }

    /// Shows a frame letterboxed in the window, or a dark idle screen when `frame` is null.
    HRESULT Present(const DecodedFrame* frame);

    /// Renders the frame letterboxed into a width x height NV12 image and hands its bytes
    /// (Y rows then interleaved UV rows, `rowPitch` apart) to `consume`.
    HRESULT RenderNV12(const DecodedFrame& frame, UINT width, UINT height,
                       const std::function<void(const uint8_t* data, UINT rowPitch)>& consume);

private:
    struct Processor {
        UINT inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0;
        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        ComPtr<ID3D11VideoProcessor> processor;
    };

    HRESULT EnsureProcessor(Processor& processor, UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight);
    HRESULT Blit(Processor& processor, const DecodedFrame& frame, ID3D11Texture2D* target, bool yuvTarget,
                 UINT targetWidth, UINT targetHeight);
    HRESULT ResizeIfNeeded();

    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<IDXGISwapChain1> swapChain_;
    std::atomic<bool> resizePending_{false};
    UINT backBufferWidth_ = 0;
    UINT backBufferHeight_ = 0;

    Processor previewProcessor_;
    Processor cameraProcessor_;
    ComPtr<ID3D11Texture2D> cameraTarget_;
    ComPtr<ID3D11Texture2D> cameraStaging_;
    UINT cameraWidth_ = 0;
    UINT cameraHeight_ = 0;
};
