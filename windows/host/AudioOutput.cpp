#include "AudioOutput.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
// Instantiates PKEY_Device_FriendlyName in this file.
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cstdlib>

#include "Common.h"

namespace {

constexpr DWORD kSampleRate = 48'000;
constexpr size_t kTargetSamples = kSampleRate * 40 / 1000;  // start playing once this much is buffered
constexpr size_t kMaxSamples = kSampleRate * 150 / 1000;    // beyond this, catch up by dropping
constexpr REFERENCE_TIME kDeviceBuffer = 200'000;            // 20 ms WASAPI buffer

} // namespace

std::vector<AudioOutput::Device> AudioOutput::ListDevices() {
    std::vector<Device> devices;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return devices;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return devices;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        LPWSTR id = nullptr;
        if (FAILED(collection->Item(index, &device)) || FAILED(device->GetId(&id))) continue;
        std::wstring name = id;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) name = value.pwszVal;
            PropVariantClear(&value);
        }
        devices.emplace_back(id, name);
        CoTaskMemFree(id);
    }
    return devices;
}

void AudioOutput::Start(const std::wstring& deviceId) {
    Stop();
    if (deviceId.empty()) return;
    running_ = true;
    thread_ = std::thread(&AudioOutput::Run, this, deviceId);
}

void AudioOutput::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    Flush();
}

void AudioOutput::Push(const int16_t* samples, size_t count) {
    int peak = 0;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
    std::lock_guard lock(mutex_);
    peak_ = std::max(peak_, peak / 32768.0f);
    if (!running_) return; // nowhere to play: just keep the level meter alive
    queue_.insert(queue_.end(), samples, samples + count);
    if (queue_.size() > kMaxSamples * 4) { // device stalled or failed: don't grow without bound
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(queue_.size() - kTargetSamples));
    }
}

void AudioOutput::Flush() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    primed_ = false;
}

float AudioOutput::TakePeak() {
    std::lock_guard lock(mutex_);
    const float peak = peak_;
    peak_ = 0;
    return peak;
}

void AudioOutput::Fill(int16_t* destination, size_t count) {
    std::lock_guard lock(mutex_);
    if (queue_.size() > kMaxSamples) { // a network hiccup or clock drift built up a backlog
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(queue_.size() - kTargetSamples));
    }
    if (!primed_ && queue_.size() >= kTargetSamples) primed_ = true;
    size_t written = 0;
    if (primed_) {
        written = std::min(count, queue_.size());
        std::copy_n(queue_.begin(), written, destination);
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(written));
    }
    if (written < count) {
        std::fill(destination + written, destination + count, int16_t{0});
        if (queue_.empty()) primed_ = false; // underrun: rebuffer before playing again
    }
}

void AudioOutput::Run(std::wstring deviceId) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const HRESULT hr = [&]() -> HRESULT {
        ComPtr<IMMDeviceEnumerator> enumerator;
        RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
        ComPtr<IMMDevice> device;
        RETURN_IF_FAILED(enumerator->GetDevice(deviceId.c_str(), &device));
        ComPtr<IAudioClient> client;
        RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = kSampleRate * 2;
        // Windows converts our mono 48 kHz to whatever the device mixes at.
        RETURN_IF_FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                            kDeviceBuffer, 0, &format, nullptr));
        RETURN_IF_FAILED(client->SetEventHandle(ready));
        UINT32 bufferFrames = 0;
        RETURN_IF_FAILED(client->GetBufferSize(&bufferFrames));
        ComPtr<IAudioRenderClient> render;
        RETURN_IF_FAILED(client->GetService(IID_PPV_ARGS(&render)));
        RETURN_IF_FAILED(client->Start());
        qbt::Log("audio: playing (device buffer %u frames)", bufferFrames);

        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        HRESULT result = S_OK;
        while (running_) {
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(result = client->GetCurrentPadding(&padding))) break;
            const UINT32 available = bufferFrames - padding;
            if (available == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(result = render->GetBuffer(available, &data))) break;
            Fill(reinterpret_cast<int16_t*>(data), available);
            render->ReleaseBuffer(available, 0);
        }
        client->Stop();
        if (task) AvRevertMmThreadCharacteristics(task);
        return result;
    }();
    if (FAILED(hr)) qbt::Log("audio: output failed 0x%08lx", hr);
    CloseHandle(ready);
    CoUninitialize();
}
