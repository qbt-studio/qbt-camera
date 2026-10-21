#pragma once
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "Common.h"

// Owns the D3D11 device (shared with the decoder), the preview swap chain and the GPU video
// processor that scales/converts decoded frames for the preview and for the virtual camera.
//
// The preview is a DirectComposition visual over the window: it gets real rounded corners
// (an anti-aliased clip) and sits on top of the Direct2D interface around it.
class VideoPipeline {
public:
    HRESULT Initialize(HWND window);
    ID3D11Device* Device() const { return device_.Get(); }

    /// UI thread: where the preview goes (client pixels) and how round its corners are.
    void SetPreviewRect(const RECT& rect, float cornerRadius);

    /// Puts another swap chain (the interface) under the video, in the same composition tree:
    /// the window's Mica backdrop covers anything that isn't in this topmost tree.
    HRESULT AddUnderlay(IUnknown* swapChain);

    /// Light or dark idle screen, to match the interface.
    void SetIdleTheme(bool dark);

    /// What the preview shows while no video comes in.
    void SetIdleMessage(const std::wstring& title, const std::wstring& subtitle);

    /// Shows a frame letterboxed in the preview, or the idle screen when `frame` is null.
    HRESULT Present(const DecodedFrame* frame);
    /// UI thread, after a resize: redraws the idle screen at the new size (frames do this themselves).
    void RefreshIdle();

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
    HRESULT DrawIdle(ID3D11Texture2D* backBuffer);

    std::mutex mutex_; // the render thread and the UI thread (idle redraws) share the context
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<IDXGISwapChain1> swapChain_;
    UINT backBufferWidth_ = 0;
    UINT backBufferHeight_ = 0;
    std::atomic<UINT> wantedWidth_{640};
    std::atomic<UINT> wantedHeight_{360};
    bool showingIdle_ = true;
    bool idleDark_ = true;

    ComPtr<IDCompositionDevice> composition_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> root_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDCompositionRectangleClip> clip_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory> dwrite_;
    std::wstring idleTitle_ = L"QBT Camera";
    std::wstring idleSubtitle_;

    Processor previewProcessor_;
    Processor cameraProcessor_;
    ComPtr<ID3D11Texture2D> cameraTarget_;
    ComPtr<ID3D11Texture2D> cameraStaging_;
    UINT cameraWidth_ = 0;
    UINT cameraHeight_ = 0;
};
