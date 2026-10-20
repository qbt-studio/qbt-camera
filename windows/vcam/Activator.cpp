#include "Activator.h"

Activator::Activator() {
    if (FAILED(MFCreateAttributes(&attributes_, 4))) throw std::bad_alloc();
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** object) {
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    ComPtr<MediaSource> source;
    const HRESULT hr = Microsoft::WRL::MakeAndInitialize<MediaSource>(&source, attributes_.Get());
    if (FAILED(hr)) {
        qbt::Log("vcam: creating media source failed 0x%08lx", hr);
        return hr;
    }
    return source.CopyTo(riid, object);
}

STDMETHODIMP Activator::ShutdownObject() {
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    return S_OK;
}
