#pragma once
#include "FrameReader.h"
#include "Placeholder.h"

// The single video stream of the virtual camera. Apps (through Frame Server) call RequestSample;
// a delivery thread answers each request with the newest frame from the host, paced to the
// negotiated frame rate, or with the placeholder picture when there is no live video.
class MediaStream final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>> {
public:
    HRESULT RuntimeClassInitialize(IMFMediaSource* source, DWORD streamId);
    ~MediaStream() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    bool IsActive();
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();

private:
    void DeliveryLoop();
    HRESULT DeliverSample(IUnknown* token, UINT32 width, UINT32 height, UINT64 duration);

    std::mutex mutex_;
    std::condition_variable wake_;
    bool shutdown_ = false;
    bool active_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    ComPtr<IMFMediaSource> source_; // released in Shutdown to break the source <-> stream cycle
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    std::deque<ComPtr<IUnknown>> pendingTokens_;
    UINT32 width_ = 1920;
    UINT32 height_ = 1080;
    UINT64 frameDuration_ = 333333; // 100 ns units
    std::thread worker_;
    FrameReader reader_;
    Placeholder placeholder_;
};
