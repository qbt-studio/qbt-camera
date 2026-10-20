#include "Decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

HRESULT H264Decoder::Initialize(ID3D11Device* device) {
    RETURN_IF_FAILED(MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_));
    RETURN_IF_FAILED(deviceManager_->ResetDevice(device, resetToken_));
    RETURN_IF_FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_)));

    ComPtr<IMFAttributes> attributes;
    RETURN_IF_FAILED(decoder_->GetAttributes(&attributes));
    UINT32 d3d11Aware = FALSE;
    attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
    if (!d3d11Aware) {
        qbt::Log("decoder: H.264 decoder is not D3D11 aware");
        return MF_E_UNSUPPORTED_D3D_TYPE;
    }
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE); // emit each frame immediately, no reorder buffering
    RETURN_IF_FAILED(decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager_.Get())));

    ComPtr<IMFMediaType> input;
    RETURN_IF_FAILED(MFCreateMediaType(&input));
    RETURN_IF_FAILED(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    RETURN_IF_FAILED(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(decoder_->SetInputType(0, input.Get(), 0));
    if (SUCCEEDED(ConfigureOutput())) outputConfigured_ = true; // otherwise after the first SPS

    RETURN_IF_FAILED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    RETURN_IF_FAILED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    qbt::Log("decoder: hardware H.264 decoder ready");
    return S_OK;
}

HRESULT H264Decoder::ConfigureOutput() {
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = decoder_->GetOutputAvailableType(0, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) return MF_E_INVALIDMEDIATYPE;
        RETURN_IF_FAILED(hr);
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
        RETURN_IF_FAILED(decoder_->SetOutputType(0, type.Get(), 0));

        UINT32 width = 0, height = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        visible_ = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            visible_ = {area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + static_cast<LONG>(area.Area.cx),
                        area.OffsetY.value + static_cast<LONG>(area.Area.cy)};
        }
        qbt::Log("decoder: output NV12 %ux%u, visible %ldx%ld", width, height, visible_.right - visible_.left,
                 visible_.bottom - visible_.top);
        return S_OK;
    }
}

HRESULT H264Decoder::Decode(const uint8_t* data, size_t size, LONGLONG timeHns, DecodedFrame& frame) {
    ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer));
    BYTE* destination = nullptr;
    RETURN_IF_FAILED(buffer->Lock(&destination, nullptr, nullptr));
    memcpy(destination, data, size);
    buffer->Unlock();
    RETURN_IF_FAILED(buffer->SetCurrentLength(static_cast<DWORD>(size)));

    ComPtr<IMFSample> sample;
    RETURN_IF_FAILED(MFCreateSample(&sample));
    RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
    RETURN_IF_FAILED(sample->SetSampleTime(timeHns));

    bool produced = false;
    HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) { // output pending: take it, then feed again
        RETURN_IF_FAILED(Drain(frame, produced));
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    RETURN_IF_FAILED(hr);
    RETURN_IF_FAILED(Drain(frame, produced));
    return produced ? S_OK : S_FALSE;
}

HRESULT H264Decoder::Drain(DecodedFrame& frame, bool& produced) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output{};
        DWORD status = 0;
        const HRESULT hr = decoder_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        ComPtr<IMFSample> result;
        result.Attach(output.pSample); // with D3D the decoder allocates the sample for us

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { // new SPS: size or format changed (e.g. phone rotated)
            RETURN_IF_FAILED(ConfigureOutput());
            outputConfigured_ = true;
            continue;
        }
        RETURN_IF_FAILED(hr);
        if (!result) continue;

        ComPtr<IMFMediaBuffer> buffer;
        RETURN_IF_FAILED(result->GetBufferByIndex(0, &buffer));
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        RETURN_IF_FAILED(buffer.As(&dxgiBuffer));
        frame.texture.Reset();
        RETURN_IF_FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&frame.texture)));
        RETURN_IF_FAILED(dxgiBuffer->GetSubresourceIndex(&frame.subresource));
        frame.visible = visible_;
        frame.sample = result; // newest wins if several came out
        produced = true;
    }
}

void H264Decoder::Flush() {
    if (decoder_) decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}
