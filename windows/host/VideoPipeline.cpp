#include "VideoPipeline.h"

#include <d3d11_4.h>

#include <cmath>

namespace {

// Preview card color: letterbox bars and the idle screen blend into the interface.
constexpr float kCard[4] = {0.039f, 0.047f, 0.071f, 1.0f};

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
    desc.Width = wantedWidth_;
    desc.Height = wantedHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    RETURN_IF_FAILED(factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_));
    backBufferWidth_ = desc.Width;
    backBufferHeight_ = desc.Height;

    // The preview visual floats above the window's own (Direct2D) content.
    RETURN_IF_FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&composition_)));
    RETURN_IF_FAILED(composition_->CreateTargetForHwnd(window, TRUE, &compositionTarget_));
    RETURN_IF_FAILED(composition_->CreateVisual(&visual_));
    RETURN_IF_FAILED(composition_->CreateRectangleClip(&clip_));
    RETURN_IF_FAILED(visual_->SetContent(swapChain_.Get()));
    RETURN_IF_FAILED(visual_->SetClip(clip_.Get()));
    RETURN_IF_FAILED(compositionTarget_->SetRoot(visual_.Get()));

    D2D1_FACTORY_OPTIONS options{};
    RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                                       reinterpret_cast<void**>(d2dFactory_.GetAddressOf())));
    RETURN_IF_FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_));
    RETURN_IF_FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_));
    RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())));
    return composition_->Commit();
}

void VideoPipeline::SetPreviewRect(const RECT& rect, float cornerRadius) {
    if (!visual_) return;
    const float width = static_cast<float>(std::max<LONG>(rect.right - rect.left, 16));
    const float height = static_cast<float>(std::max<LONG>(rect.bottom - rect.top, 16));
    visual_->SetOffsetX(static_cast<float>(rect.left));
    visual_->SetOffsetY(static_cast<float>(rect.top));
    clip_->SetLeft(0.0f);
    clip_->SetTop(0.0f);
    clip_->SetRight(width);
    clip_->SetBottom(height);
    clip_->SetTopLeftRadiusX(cornerRadius);
    clip_->SetTopLeftRadiusY(cornerRadius);
    clip_->SetTopRightRadiusX(cornerRadius);
    clip_->SetTopRightRadiusY(cornerRadius);
    clip_->SetBottomLeftRadiusX(cornerRadius);
    clip_->SetBottomLeftRadiusY(cornerRadius);
    clip_->SetBottomRightRadiusX(cornerRadius);
    clip_->SetBottomRightRadiusY(cornerRadius);
    // The buffer follows on the next Present (or RefreshIdle); the card behind has the same color meanwhile.
    wantedWidth_ = static_cast<UINT>(width);
    wantedHeight_ = static_cast<UINT>(height);
    composition_->Commit();
}

void VideoPipeline::SetIdleMessage(const std::wstring& title, const std::wstring& subtitle) {
    std::lock_guard lock(mutex_);
    idleTitle_ = title;
    idleSubtitle_ = subtitle;
}

void VideoPipeline::RefreshIdle() {
    bool idle;
    {
        std::lock_guard lock(mutex_);
        idle = showingIdle_;
    }
    if (idle) Present(nullptr);
}

HRESULT VideoPipeline::ResizeIfNeeded() {
    const UINT width = std::max<UINT>(wantedWidth_, 16);
    const UINT height = std::max<UINT>(wantedHeight_, 16);
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
        background.RGBA = {kCard[0], kCard[1], kCard[2], 1.0f};
    }
    videoContext_->VideoProcessorSetOutputColorSpace(vp, &outputSpace);
    videoContext_->VideoProcessorSetOutputBackgroundColor(vp, yuvTarget ? TRUE : FALSE, &background);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    return videoContext_->VideoProcessorBlt(vp, outputView.Get(), 0, 1, &stream);
}

HRESULT VideoPipeline::Present(const DecodedFrame* frame) {
    std::lock_guard lock(mutex_);
    if (!swapChain_) return E_UNEXPECTED;
    RETURN_IF_FAILED(ResizeIfNeeded());
    ComPtr<ID3D11Texture2D> backBuffer;
    RETURN_IF_FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    showingIdle_ = frame == nullptr;
    if (frame) {
        RETURN_IF_FAILED(Blit(previewProcessor_, *frame, backBuffer.Get(), false, backBufferWidth_, backBufferHeight_));
    } else {
        RETURN_IF_FAILED(DrawIdle(backBuffer.Get()));
    }
    return swapChain_->Present(0, 0); // no vsync wait: show the newest frame as early as possible
}

HRESULT VideoPipeline::DrawIdle(ID3D11Texture2D* backBuffer) {
    ComPtr<IDXGISurface> surface;
    RETURN_IF_FAILED(backBuffer->QueryInterface(IID_PPV_ARGS(&surface)));
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ComPtr<ID2D1Bitmap1> bitmap;
    RETURN_IF_FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap));
    d2dContext_->SetTarget(bitmap.Get());
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(kCard[0], kCard[1], kCard[2]));

    const float width = static_cast<float>(backBufferWidth_);
    const float height = static_cast<float>(backBufferHeight_);
    const float unit = std::max(0.5f, std::min(width / 900.0f, height / 520.0f)); // scales with the preview
    const float cx = width / 2, cy = height / 2 - 30 * unit;

    // Soft brand glow behind the logo.
    ComPtr<ID2D1GradientStopCollection> stops;
    const D2D1_GRADIENT_STOP glowStops[] = {{0.0f, D2D1::ColorF(0x2B7FFF, 0.22f)}, {1.0f, D2D1::ColorF(0x2B7FFF, 0.0f)}};
    d2dContext_->CreateGradientStopCollection(glowStops, 2, &stops);
    ComPtr<ID2D1RadialGradientBrush> glow;
    if (stops) {
        d2dContext_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), {}, 300 * unit, 220 * unit), stops.Get(), &glow);
    }
    if (glow) d2dContext_->FillRectangle(D2D1::RectF(0, 0, width, height), glow.Get());

    // QBT logo (rounded square outline with a corner badge, 100x100 viewBox).
    const float size = 72 * unit;
    const float x = cx - size / 2, y = cy - size / 2;
    const auto at = [size](float units) { return units * size / 100.0f; };
    ComPtr<ID2D1SolidColorBrush> brush;
    RETURN_IF_FAILED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0x2B7FFF), &brush));
    const D2D1_ROUNDED_RECT outline{D2D1::RectF(x + at(11), y + at(11), x + at(83), y + at(83)), at(24), at(24)};
    d2dContext_->DrawRoundedRectangle(outline, brush.Get(), at(10.5f));
    brush->SetColor(D2D1::ColorF(0x539BFF));
    d2dContext_->FillRoundedRectangle({D2D1::RectF(x + at(66), y + at(66), x + at(94), y + at(94)), at(9), at(9)}, brush.Get());

    const auto text = [&](const std::wstring& value, float top, float fontSize, DWRITE_FONT_WEIGHT weight, D2D1_COLOR_F color) {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, fontSize, L"ru-ru", &format)))
            return;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        brush->SetColor(color);
        d2dContext_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format.Get(),
                              D2D1::RectF(0, top, width, top + fontSize * 2), brush.Get());
    };
    text(idleTitle_, y + size + 22 * unit, 24 * unit, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::ColorF(0xFFFFFF));
    text(idleSubtitle_, y + size + 60 * unit, 15 * unit, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0xFFFFFF, 0.55f));

    const HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);
    return hr;
}

HRESULT VideoPipeline::RenderNV12(const DecodedFrame& frame, UINT width, UINT height,
                                  const std::function<void(const uint8_t*, UINT)>& consume) {
    std::lock_guard lock(mutex_);
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
