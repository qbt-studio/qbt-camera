#include "MediaStream.h"

namespace {

struct VideoFormat {
    UINT32 width;
    UINT32 height;
    UINT32 fps;
};

// First entry is the default; most call apps take it as is.
constexpr VideoFormat kFormats[] = {
    {1920, 1080, 30}, {1280, 720, 30}, {1920, 1080, 60}, {1280, 720, 60}, {3840, 2160, 30}, {640, 360, 30},
};

HRESULT CreateMediaType(const VideoFormat& format, IMFMediaType** result) {
    ComPtr<IMFMediaType> type;
    RETURN_IF_FAILED(MFCreateMediaType(&type));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RETURN_IF_FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, format.width, format.height));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, format.fps, 1));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, format.width));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, format.width * format.height * 3 / 2));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, format.width * format.height * 12 * format.fps));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    *result = type.Detach();
    return S_OK;
}

} // namespace

HRESULT MediaStream::RuntimeClassInitialize(IMFMediaSource* source, DWORD streamId) {
    source_ = source;
    RETURN_IF_FAILED(MFCreateEventQueue(&events_));

    std::vector<ComPtr<IMFMediaType>> types;
    std::vector<IMFMediaType*> rawTypes;
    for (const VideoFormat& format : kFormats) {
        ComPtr<IMFMediaType> type;
        RETURN_IF_FAILED(CreateMediaType(format, &type));
        rawTypes.push_back(type.Get());
        types.push_back(std::move(type));
    }
    RETURN_IF_FAILED(MFCreateStreamDescriptor(streamId, static_cast<DWORD>(rawTypes.size()), rawTypes.data(), &descriptor_));
    ComPtr<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(descriptor_->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(rawTypes[0]));

    // What Frame Server needs to treat this stream as a shareable color camera.
    RETURN_IF_FAILED(descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId));
    RETURN_IF_FAILED(descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));

    worker_ = std::thread([this] { DeliveryLoop(); });
    return S_OK;
}

MediaStream::~MediaStream() {
    Shutdown();
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> events;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        events = events_;
    }
    return events->GetEvent(flags, event); // may block: not under the lock
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->QueueEventParamVar(type, extendedType, status, value);
}

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    RETURN_HR_IF_NULL(E_POINTER, source);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return source_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    pendingTokens_.emplace_back(token);
    wake_.notify_one();
    return S_OK;
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    state_ = state;
    if (state != MF_STREAM_STATE_RUNNING) pendingTokens_.clear();
    wake_.notify_one();
    return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    RETURN_HR_IF_NULL(E_POINTER, state);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

bool MediaStream::IsActive() {
    std::lock_guard lock(mutex_);
    return active_;
}

HRESULT MediaStream::Start(IMFMediaType* type) {
    RETURN_HR_IF_NULL(E_POINTER, type);
    UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
    RETURN_IF_FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height));
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) || numerator == 0) {
        numerator = 30;
        denominator = 1;
    }
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        width_ = width;
        height_ = height;
        frameDuration_ = 10'000'000ull * denominator / numerator;
        active_ = true;
        state_ = MF_STREAM_STATE_RUNNING;
        wake_.notify_one();
    }
    qbt::Log("vcam: stream started %ux%u @ %u/%u", width, height, numerator, denominator);
    reader_.SetRequestedSize(width, height);

    PROPVARIANT time;
    PropVariantInit(&time);
    time.vt = VT_I8;
    time.hVal.QuadPart = MFGetSystemTime();
    return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &time);
}

HRESULT MediaStream::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        active_ = false;
        state_ = MF_STREAM_STATE_STOPPED;
        pendingTokens_.clear();
    }
    qbt::Log("vcam: stream stopped");
    return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
        pendingTokens_.clear();
        wake_.notify_all();
    }
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id())
            worker_.detach(); // last reference dropped on the delivery thread itself
        else
            worker_.join();
    }
    if (events_) events_->Shutdown();
    std::lock_guard lock(mutex_);
    source_.Reset();
}

void MediaStream::DeliveryLoop() {
    MFTIME lastDelivery = 0;
    for (;;) {
        UINT64 duration = 0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] {
                return shutdown_ || (!pendingTokens_.empty() && state_ == MF_STREAM_STATE_RUNNING);
            });
            if (shutdown_) return;
            duration = frameDuration_;
        }

        // Pace to the negotiated frame rate, but hand out a fresh frame from the phone the moment
        // it lands (as long as at least half a frame period has passed).
        const MFTIME due = lastDelivery + static_cast<MFTIME>(duration);
        for (;;) {
            const MFTIME now = MFGetSystemTime();
            if (now >= due) break;
            const DWORD waitMs = static_cast<DWORD>((due - now) / 10'000) + 1;
            if (reader_.WaitForFrame(waitMs) && MFGetSystemTime() >= lastDelivery + static_cast<MFTIME>(duration / 2)) break;
        }

        ComPtr<IUnknown> token;
        UINT32 width = 0, height = 0;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            if (pendingTokens_.empty() || state_ != MF_STREAM_STATE_RUNNING) continue;
            token = std::move(pendingTokens_.front());
            pendingTokens_.pop_front();
            width = width_;
            height = height_;
            duration = frameDuration_;
        }
        lastDelivery = MFGetSystemTime();
        const HRESULT hr = DeliverSample(token.Get(), width, height, duration);
        if (FAILED(hr)) qbt::Log("vcam: DeliverSample failed 0x%08lx", hr);
    }
}

HRESULT MediaStream::DeliverSample(IUnknown* token, UINT32 width, UINT32 height, UINT64 duration) {
    ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(MFCreate2DMediaBuffer(width, height, MFVideoFormat_NV12.Data1, FALSE, &buffer));
    ComPtr<IMF2DBuffer2> buffer2d;
    RETURN_IF_FAILED(buffer.As(&buffer2d));
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    BYTE* bufferStart = nullptr;
    DWORD bufferLength = 0;
    RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &bufferStart, &bufferLength));
    if (!reader_.CopyLatest(scanline0, pitch, width, height)) placeholder_.Draw(scanline0, pitch, width, height);
    buffer2d->Unlock2D();
    RETURN_IF_FAILED(buffer->SetCurrentLength(bufferLength));

    ComPtr<IMFSample> sample;
    RETURN_IF_FAILED(MFCreateSample(&sample));
    RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
    RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
    RETURN_IF_FAILED(sample->SetSampleDuration(static_cast<LONGLONG>(duration)));
    if (token) RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));

    ComPtr<IMFMediaEventQueue> events;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        events = events_;
    }
    return events->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}
