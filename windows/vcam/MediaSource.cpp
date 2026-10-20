#include "MediaSource.h"

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activateAttributes) {
    RETURN_IF_FAILED(MFCreateEventQueue(&events_));
    RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 4));
    if (activateAttributes) RETURN_IF_FAILED(activateAttributes->CopyAllItems(attributes_.Get()));

    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaStream>(&stream_, this, 0ul));
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(stream_->GetStreamDescriptor(&streamDescriptor));
    IMFStreamDescriptor* descriptors[] = {streamDescriptor.Get()};
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descriptors, &descriptor_));
    RETURN_IF_FAILED(descriptor_->SelectStream(0));
    qbt::Log("vcam: media source created");
    return S_OK;
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> events;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        events = events_;
    }
    return events->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->QueueEventParamVar(type, extendedType, status, value);
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    RETURN_HR_IF_NULL(E_POINTER, characteristics);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() {
    return MF_E_INVALID_STATE_TRANSITION; // live source
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                const PROPVARIANT* startPosition) {
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    RETURN_HR_IF_NULL(E_POINTER, startPosition);
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    if (startPosition->vt != VT_EMPTY && startPosition->vt != VT_I8) return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    DWORD count = 0;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
    for (DWORD index = 0; index < count; ++index) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> streamDescriptor;
        RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(index, &selected, &streamDescriptor));
        DWORD id = 0;
        RETURN_IF_FAILED(streamDescriptor->GetStreamIdentifier(&id));
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;

        if (selected) {
            const bool wasActive = stream_->IsActive();
            ComPtr<IMFMediaTypeHandler> handler;
            RETURN_IF_FAILED(streamDescriptor->GetMediaTypeHandler(&handler));
            ComPtr<IMFMediaType> type;
            RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
            IMFMediaStream* stream = stream_.Get();
            RETURN_IF_FAILED(events_->QueueEventParamUnk(wasActive ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, stream));
            RETURN_IF_FAILED(stream_->Start(type.Get()));
        } else if (stream_->IsActive()) {
            RETURN_IF_FAILED(stream_->Stop());
        }
    }

    PROPVARIANT time;
    PropVariantInit(&time);
    time.vt = VT_I8;
    time.hVal.QuadPart = MFGetSystemTime();
    return events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (stream_->IsActive()) RETURN_IF_FAILED(stream_->Stop());
    return events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        shutdown_ = true;
        events_->Shutdown();
        stream = std::move(stream_);
    }
    if (stream) stream->Shutdown();
    qbt::Log("vcam: media source shut down");
    return S_OK;
}

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    ComPtr<IMFStreamDescriptor> descriptor;
    RETURN_IF_FAILED(stream_->GetStreamDescriptor(&descriptor));
    return descriptor.CopyTo(attributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    return S_OK; // frames are delivered in system memory
}

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (object) *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
