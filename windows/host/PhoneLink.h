#pragma once
#include <winsock2.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../common/Protocol.h"

// Connection to the QBT Camera app on the iPhone: USB first (via usbmux), then Wi-Fi.
class PhoneLink {
public:
    /// Tries USB, then the last Wi-Fi address, then a scan of the local /24 network.
    /// `route` gets a human-readable description ("USB", "Wi-Fi 192.168.1.5"); `usb` tells which.
    bool Connect(const std::atomic<bool>& stop, std::wstring& route, bool& usb);

    /// The phone's Wi-Fi address as it reported it; used for instant fallback when USB drops.
    void RememberWifiAddress(const std::string& address);

    bool ReadPacket(qbt::PacketHeader& header, std::vector<uint8_t>& payload);
    bool SendAck(uint32_t framesReceived);
    /// JSON command to the phone; callable from any thread.
    bool SendCommand(const std::string& json);
    /// Safe to call from another thread to unblock a pending read.
    void Close();

private:
    void Adopt(SOCKET socket);
    bool Send(const void* data, size_t size);

    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::mutex sendMutex_; // acks (stream thread) and commands (UI thread) must not interleave
    std::mutex addressMutex_;
    std::string lastWifiAddress_;
};
