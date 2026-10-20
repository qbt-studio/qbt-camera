#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Plays the iPhone microphone (48 kHz mono 16-bit) on a chosen Windows output device, e.g.
// "CABLE Input" of VB-Audio Virtual Cable so call apps can pick "CABLE Output" as a microphone.
// A ~40 ms jitter buffer absorbs network timing; anything beyond ~150 ms is dropped so the
// sound never drifts behind the picture.
class AudioOutput {
public:
    using Device = std::pair<std::wstring, std::wstring>; // endpoint id, friendly name

    ~AudioOutput() { Stop(); }

    /// Active playback devices.
    static std::vector<Device> ListDevices();

    /// Starts playing on `deviceId` (replacing the current device); empty id = don't play.
    void Start(const std::wstring& deviceId);
    void Stop();

    /// Called from the stream thread with each audio packet.
    void Push(const int16_t* samples, size_t count);
    /// Drops buffered sound, e.g. when the phone disconnects.
    void Flush();
    /// Peak level 0..1 since the previous call (for the level meter).
    float TakePeak();

private:
    void Run(std::wstring deviceId);
    void Fill(int16_t* destination, size_t count);

    std::mutex mutex_;
    std::deque<int16_t> queue_;
    bool primed_ = false;
    float peak_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
};
