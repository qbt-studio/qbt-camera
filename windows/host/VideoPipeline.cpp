#include "VideoPipeline.h"

#include <d3d11_4.h>

namespace {

// Largest rectangle with the source aspect ratio, centered in the target.
RECT Fit(const RECT& source, UINT targetWidth, UINT targetHeight) {
    const double sourceWidth = source.right - source.left;
    const double sourceHeight = source.bottom - source.top;
    if (sourceWidth <= 0 || sourceHeight <= 0) return {0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    const double scale = std::min(targetWidth / sourceWidth, targetHeight / sourceHeight);
    const LONG width = static_cast<LONG>(sourceWidth * scale) & ~1L;
    const LONG height = static_cast<LONG>(sourceHeight * scale) & ~1L;
    const LONG left = ((static_cast<LONG>(targetWidth) - width) / 2) & ~1L;
    const LONG top = ((static_cast<LONG>(targetHeight) - height) / 2) & ~1L;
    return {left, top, left + width, top + height};
}

} // namespace

HRESULT VideoPipeline::Initialize(HWND window) {
    window_ = window;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_));
    // The Media Foundation decoder uses the device from its own threads.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    RETURN_IF_FAILED(device_.As(&videoDevice_));
    RETURN_IF_FAILED(context_.As(&videoContext_));

    ComPtr<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(device_.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    RETURN_IF_FAILED(dxgiDevice->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    char adapterName[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
    qbt::Log("video: GPU %s", adapterName);

    ComPtr<IDXGIFactory2> factory;
    RETURN_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    RETURN_IF_FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapChain_));
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    swapChain_->GetDesc1(&desc);
    backBufferWidth_ = desc.Width;
    backBufferHeight_ = desc.Height;
    return S_OK;
}

HRESULT VideoPipeline::ResizeIfNeeded() {
    if (!resizePending_.exchange(false)) return S_OK;
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = std::max<LONG>(client.right - client.left, 16);
    const UINT height = std::max<LONG>(client.bottom - client.top, 16);
    if (width == backBufferWidth_ && height == backBufferHeight_) return S_OK;
    context_->ClearState();
    RETURN_IF_FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
    backBufferWidth_ = width;
    backBufferHeight_ = height;
    return S_OK;
}

HRESULT VideoPipeline::EnsureProcessor(Processor& processor, UINT inputWidth, UINT inputHeight, UINT outputWidth,
                                       UINT outputHeight) {
    if (processor.processor && processor.inputWidth == inputWidth && processor.inputHeight == inputHeight &&
        processor.outputWidth == outputWidth && processor.outputHeight == outputHeight)
        return S_OK;
    processor = {};
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {60, 1};
    desc.InputWidth = inputWidth;
    desc.InputHeight = inputHeight;
    desc.OutputFrameRate = {60, 1};
    desc.OutputWidth = outputWidth;
    desc.OutputHeight = outputHeight;
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    RETURN_IF_FAILED(videoDevice_->CreateVideoProcessorEnumerator(&desc, &processor.enumerator));
    RETURN_IF_FAILED(videoDevice_->CreateVideoProcessor(processor.enumerator.Get(), 0, &processor.processor));
    processor.inputWidth = inputWidth;
    processor.inputHeight = inputHeight;
    processor.outputWidth = outputWidth;
    processor.outputHeight = outputHeight;
    return S_OK;
}

HRESULT VideoPipeline::Blit(Processor& processor, const DecodedFrame& frame, ID3D11Texture2D* target, bool yuvTarget,
                            UINT targetWidth, UINT targetHeight) {
    D3D11_TEXTURE2D_DESC sourceDesc{};
    frame.texture->GetDesc(&sourceDesc);
    RETURN_IF_FAILED(EnsureProcessor(processor, sourceDesc.Width, sourceDesc.Height, targetWidth, targetHeight));
    ID3D11VideoProcessor* vp = processor.processor.Get();

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.ArraySlice = frame.subresource;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    RETURN_IF_FAILED(videoDevice_->CreateVideoProcessorInputView(frame.texture.Get(), processor.enumerator.Get(),
                                                                 &inputDesc, &inputView));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    RETURN_IF_FAILED(videoDevice_->CreateVideoProcessorOutputView(target, processor.enumerator.Get(), &outputDesc, &outputView));

    const RECT whole{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    const RECT destination = Fit(frame.visible, targetWidth, targetHeight);
    videoContext_->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &frame.visible);
    videoContext_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &destination);
    videoContext_->VideoProcessorSetOutputTargetRect(vp, TRUE, &whole);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    // iPhone video: BT.709, limited (16–235) range.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputSpace{};
    inputSpace.YCbCr_Matrix = 1;
    inputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(vp, 0, &inputSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputSpace{};
    D3D11_VIDEO_COLOR background{};
    if (yuvTarget) {
        outputSpace.YCbCr_Matrix = 1;
        outputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        background.YCbCr = {16.0f / 255.0f, 0.5f, 0.5f, 1.0f}; // black bars
    } else {
        outputSpace.RGB_Range = 0; // full range for the screen
        background.RGBA = {0.04f, 0.045f, 0.06f, 1.0f};
    }
    videoContext_->VideoProcessorSetOutputColorSpace(vp, &outputSpace);
    videoContext_->VideoProcessorSetOutputBackgroundColor(vp, yuvTarget ? TRUE : FALSE, &background);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    return videoContext_->VideoProcessorBlt(vp, outputView.Get(), 0, 1, &stream);
}

HRESULT VideoPipeline::Present(const DecodedFrame* frame) {
    RETURN_IF_FAILED(ResizeIfNeeded());
    ComPtr<ID3D11Texture2D> backBuffer;
    RETURN_IF_FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    if (frame) {
        RETURN_IF_FAILED(Blit(previewProcessor_, *frame, backBuffer.Get(), false, backBufferWidth_, backBufferHeight_));
    } else {
        ComPtr<ID3D11RenderTargetView> view;
        RETURN_IF_FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &view));
        const float idle[4] = {0.04f, 0.045f, 0.06f, 1.0f};
        context_->ClearRenderTargetView(view.Get(), idle);
    }
    return swapChain_->Present(0, 0); // no vsync wait: show the newest frame as early as possible
}

HRESULT VideoPipeline::RenderNV12(const DecodedFrame& frame, UINT width, UINT height,
                                  const std::function<void(const uint8_t*, UINT)>& consume) {
    if (!cameraTarget_ || cameraWidth_ != width || cameraHeight_ != height) {
        cameraTarget_.Reset();
        cameraStaging_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        RETURN_IF_FAILED(device_->CreateTexture2D(&desc, nullptr, &cameraTarget_));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        RETURN_IF_FAILED(device_->CreateTexture2D(&desc, nullptr, &cameraStaging_));
        cameraWidth_ = width;
        cameraHeight_ = height;
        qbt::Log("video: virtual camera output %ux%u", width, height);
    }
    RETURN_IF_FAILED(Blit(cameraProcessor_, frame, cameraTarget_.Get(), true, width, height));
    context_->CopyResource(cameraStaging_.Get(), cameraTarget_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    RETURN_IF_FAILED(context_->Map(cameraStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    consume(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch); // UV plane follows at RowPitch * height
    context_->Unmap(cameraStaging_.Get(), 0);
    return S_OK;
}
