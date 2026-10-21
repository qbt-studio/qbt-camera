#pragma once
#include <windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <wrl/client.h>

#include "../common/Log.h"

using Microsoft::WRL::ComPtr;

#define RETURN_IF_FAILED(expression)              \
    do {                                          \
        const HRESULT hr_ = (expression);         \
        if (FAILED(hr_)) return hr_;              \
    } while (false)

// A decoded picture living in a GPU texture owned by the decoder.
struct DecodedFrame {
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    RECT visible{};             // displayable area (decoders pad 1080 to 1088)
    ComPtr<IMFSample> sample;   // keeps the decoder surface alive while we use it
    bool rgb = false;           // RGBA texture (background compositor output) instead of NV12
    bool fullRange = false;     // NV12 with 0–255 levels (the matte stream)
};
