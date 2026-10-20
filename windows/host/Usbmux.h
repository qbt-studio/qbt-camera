#pragma once
#include <winsock2.h>

#include <cstdint>
#include <string>
#include <vector>

// Client for Apple Mobile Device Service (usbmuxd on 127.0.0.1:27015): turns a TCP port on a
// USB-connected iPhone into a local socket. Messages are XML plists behind a 16-byte header.
struct UsbDevice {
    int id = 0;
    std::string serial;
};

/// USB-connected devices; empty when none or when the Apple service isn't installed/running.
std::vector<UsbDevice> UsbmuxListDevices();

/// Socket tunnelled to `port` on the device, or INVALID_SOCKET (e.g. the app isn't running).
SOCKET UsbmuxConnect(int deviceId, uint16_t port);
