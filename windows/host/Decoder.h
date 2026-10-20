#pragma once
#include <mftransform.h>

#include "Common.h"

// Hardware H.264 decoding with the Windows Media Foundation decoder on our D3D11 device (DXVA).
// Output stays on the GPU as NV12 textures. Low-latency mode: one frame in, one frame out.
class H264Decoder {
public:
    HRESULT Initialize(ID3D11Device* device);
    /// S_OK with `frame` filled when a picture came out, S_FALSE when the decoder needs more data.
    HRESULT Decode(const uint8_t* data, size_t size, LONGLONG timeHns, DecodedFrame& frame);
    /// Drops queued data, e.g. after reconnecting to the phone.
    void Flush();

private:
    HRESULT ConfigureOutput();
    HRESULT Drain(DecodedFrame& frame, bool& produced);

    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT resetToken_ = 0;
    bool outputConfigured_ = false;
    RECT visible_{};
};
