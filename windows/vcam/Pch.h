#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <devicetopology.h> // IKsControl (+ ks.h, ksmedia.h)
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/module.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../common/Log.h"

using Microsoft::WRL::ComPtr;

#define RETURN_IF_FAILED(expression)              \
    do {                                          \
        const HRESULT hr_ = (expression);         \
        if (FAILED(hr_)) return hr_;              \
    } while (false)

#define RETURN_HR_IF_NULL(hr, pointer)            \
    do {                                          \
        if ((pointer) == nullptr) return (hr);    \
    } while (false)
