#include "Usbmux.h"

#include <ws2tcpip.h>

#include "../common/Log.h"

namespace {

constexpr uint16_t kUsbmuxPort = 27015;

bool SendAll(SOCKET socket, const char* data, int size) {
    while (size > 0) {
        const int sent = send(socket, data, size, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= sent;
    }
    return true;
}

bool ReceiveAll(SOCKET socket, char* data, int size) {
    while (size > 0) {
        const int received = recv(socket, data, size, 0);
        if (received <= 0) return false;
        data += received;
        size -= received;
    }
    return true;
}

SOCKET ConnectToService() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kUsbmuxPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string Plist(const std::string& body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
           "<plist version=\"1.0\"><dict>" +
           body +
           "<key>ClientVersionString</key><string>qbtcam-host</string>"
           "<key>ProgName</key><string>QBTCamera</string>"
           "<key>kLibUSBMuxVersion</key><integer>3</integer>"
           "</dict></plist>";
}

bool Request(SOCKET socket, const std::string& xml, uint32_t tag, std::string& reply) {
    const uint32_t header[4] = {static_cast<uint32_t>(16 + xml.size()), 1 /* plist */, 8 /* plist message */, tag};
    if (!SendAll(socket, reinterpret_cast<const char*>(header), sizeof(header))) return false;
    if (!SendAll(socket, xml.data(), static_cast<int>(xml.size()))) return false;
    uint32_t response[4];
    if (!ReceiveAll(socket, reinterpret_cast<char*>(response), sizeof(response))) return false;
    if (response[0] < 16 || response[0] > 1024 * 1024) return false;
    reply.assign(response[0] - 16, '\0');
    return ReceiveAll(socket, reply.data(), static_cast<int>(reply.size()));
}

// Value of <key>name</key><type>VALUE</type> within [from, to).
std::string Value(const std::string& xml, size_t from, size_t to, const char* name, const char* type) {
    const std::string key = std::string("<key>") + name + "</key>";
    const size_t keyAt = xml.find(key, from);
    if (keyAt == std::string::npos || keyAt >= to) return {};
    const std::string open = std::string("<") + type + ">";
    const size_t start = xml.find(open, keyAt + key.size());
    if (start == std::string::npos || start >= to) return {};
    const size_t end = xml.find(std::string("</") + type + ">", start);
    if (end == std::string::npos) return {};
    return xml.substr(start + open.size(), end - start - open.size());
}

} // namespace

std::vector<UsbDevice> UsbmuxListDevices() {
    std::vector<UsbDevice> devices;
    SOCKET s = ConnectToService();
    if (s == INVALID_SOCKET) return devices;
    std::string reply;
    const bool ok = Request(s, Plist("<key>MessageType</key><string>ListDevices</string>"), 1, reply);
    closesocket(s);
    if (!ok) return devices;

    // Each device has a Properties dict with ConnectionType, DeviceID and SerialNumber.
    const std::string marker = "<key>Properties</key>";
    for (size_t at = reply.find(marker); at != std::string::npos;) {
        const size_t next = reply.find(marker, at + marker.size());
        const size_t end = next == std::string::npos ? reply.size() : next;
        if (Value(reply, at, end, "ConnectionType", "string") == "USB") {
            const std::string id = Value(reply, at, end, "DeviceID", "integer");
            if (!id.empty()) devices.push_back({std::stoi(id), Value(reply, at, end, "SerialNumber", "string")});
        }
        at = next;
    }
    return devices;
}

SOCKET UsbmuxConnect(int deviceId, uint16_t port) {
    SOCKET s = ConnectToService();
    if (s == INVALID_SOCKET) return s;
    std::string reply;
    const std::string body = "<key>MessageType</key><string>Connect</string>"
                             "<key>DeviceID</key><integer>" + std::to_string(deviceId) + "</integer>"
                             "<key>PortNumber</key><integer>" + std::to_string(htons(port)) + "</integer>";
    if (!Request(s, Plist(body), 2, reply) || Value(reply, 0, reply.size(), "Number", "integer") != "0") {
        qbt::Log("usbmux: connect to device %d refused", deviceId);
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s; // from here on a raw tunnel to the app
}
