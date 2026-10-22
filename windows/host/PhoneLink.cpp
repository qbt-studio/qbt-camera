// The subnet scan waits on up to 254 sockets in one select().
#define FD_SETSIZE 512
#include "PhoneLink.h"

#include <ws2tcpip.h>

#include "../common/Log.h"
#include "Usbmux.h"

namespace {

bool ReceiveAll(SOCKET socket, void* buffer, size_t size) {
    auto* data = static_cast<char*>(buffer);
    while (size > 0) {
        const int chunk = static_cast<int>(size > (1 << 30) ? (1 << 30) : size);
        const int received = recv(socket, data, chunk, 0);
        if (received <= 0) return false;
        data += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

void SetBlocking(SOCKET socket, bool blocking) {
    u_long nonBlocking = blocking ? 0 : 1;
    ioctlsocket(socket, FIONBIO, &nonBlocking);
}

SOCKET ConnectWithTimeout(const std::string& ip, int timeoutMs) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(qbt::kVideoPort);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    SetBlocking(s, false);
    connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(s, &writable);
    FD_SET(s, &failed);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int error = 0;
    int length = sizeof(error);
    if (select(0, nullptr, &writable, &failed, &timeout) != 1 || !FD_ISSET(s, &writable) ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    SetBlocking(s, true);
    return s;
}

std::string ToString(uint32_t hostOrderAddress) {
    in_addr address{};
    address.s_addr = htonl(hostOrderAddress);
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

// The app sends capabilities and a keyframe as soon as a client connects; read and keep that first packet
// so we don't have to close the probe socket and reconnect a second time.
bool LooksLikeQbtCamera(SOCKET s, qbt::PacketHeader& header, std::vector<uint8_t>& payload) {
    DWORD timeout = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    if (!ReceiveAll(s, &header, sizeof(header)) || header.magic != qbt::kPacketMagic || header.length > qbt::kMaxPacketBytes)
        return false;
    payload.resize(header.length);
    return header.length == 0 || ReceiveAll(s, payload.data(), payload.size());
}

std::vector<uint32_t> LocalAddresses() {
    std::vector<uint32_t> addresses;
    const auto add = [&](uint32_t ip) {
        const uint8_t a = (ip >> 24) & 0xFF, b = (ip >> 16) & 0xFF;
        const bool isPrivate = a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
        if (!isPrivate) return;
        for (uint32_t existing : addresses) {
            if ((existing & 0xFFFFFF00u) == (ip & 0xFFFFFF00u)) return;
        }
        addresses.push_back(ip);
    };
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in outside{};
    outside.sin_family = AF_INET;
    outside.sin_port = htons(1);
    inet_pton(AF_INET, "10.255.255.255", &outside.sin_addr);
    sockaddr_in own{};
    int ownLength = sizeof(own);
    if (connect(probe, reinterpret_cast<sockaddr*>(&outside), sizeof(outside)) == 0 &&
        getsockname(probe, reinterpret_cast<sockaddr*>(&own), &ownLength) == 0) {
        add(ntohl(own.sin_addr.s_addr));
    }
    closesocket(probe);

    char hostName[256] = {};
    if (gethostname(hostName, sizeof(hostName)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* list = nullptr;
        if (getaddrinfo(hostName, nullptr, &hints, &list) == 0 && list) {
            for (addrinfo* it = list; it; it = it->ai_next) {
                add(ntohl(reinterpret_cast<sockaddr_in*>(it->ai_addr)->sin_addr.s_addr));
            }
            freeaddrinfo(list);
        }
    }
    return addresses;
}

/// Connects to port 50500 on every host of our /24 network(s) and returns the connected socket + first packet.
SOCKET ScanSubnet(const std::atomic<bool>& stop, std::string& found, qbt::PacketHeader& firstHeader,
                  std::vector<uint8_t>& firstPayload) {
    for (const uint32_t ownAddress : LocalAddresses()) {
        if (stop) break;
        const uint32_t network = ownAddress & 0xFFFFFF00u;
        std::vector<std::pair<SOCKET, uint32_t>> attempts;
        fd_set writable;
        FD_ZERO(&writable);
        for (uint32_t host = 1; host < 255; ++host) {
            const uint32_t address = network | host;
            if (address == ownAddress) continue;
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            SetBlocking(s, false);
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(qbt::kVideoPort);
            target.sin_addr.s_addr = htonl(address);
            connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target));
            FD_SET(s, &writable);
            attempts.emplace_back(s, address);
        }
        timeval timeout{0, 600'000};
        select(0, nullptr, &writable, nullptr, &timeout);

        SOCKET winner = INVALID_SOCKET;
        for (auto& [s, address] : attempts) {
            if (winner == INVALID_SOCKET && !stop && FD_ISSET(s, &writable)) {
                int error = 0;
                int length = sizeof(error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
                if (error == 0) {
                    SetBlocking(s, true);
                    if (LooksLikeQbtCamera(s, firstHeader, firstPayload)) {
                        found = ToString(address);
                        winner = s;
                        continue;
                    }
                }
            }
            closesocket(s);
        }
        if (winner != INVALID_SOCKET) return winner;
    }
    return INVALID_SOCKET;
}

std::wstring Widen(const std::string& text) {
    return std::wstring(text.begin(), text.end()); // ASCII only (IP addresses)
}

} // namespace

bool PhoneLink::Connect(const std::atomic<bool>& stop, std::wstring& route, bool& usb) {
    usb = false;
    for (const UsbDevice& device : UsbmuxListDevices()) {
        SOCKET s = UsbmuxConnect(device.id, qbt::kVideoPort);
        if (s != INVALID_SOCKET) {
            qbt::Log("link: USB device %d (%s)", device.id, device.serial.c_str());
            Adopt(s);
            route = L"USB";
            usb = true;
            return true;
        }
    }
    if (stop) return false;

    std::string remembered;
    {
        std::lock_guard lock(addressMutex_);
        remembered = lastWifiAddress_;
    }
    if (!remembered.empty()) {
        SOCKET s = ConnectWithTimeout(remembered, 800);
        if (s != INVALID_SOCKET) {
            qbt::Log("link: Wi-Fi %s (remembered)", remembered.c_str());
            Adopt(s);
            route = L"Wi-Fi " + Widen(remembered);
            return true;
        }
    }
    if (stop) return false;

    std::string found;
    qbt::PacketHeader firstHeader{};
    std::vector<uint8_t> firstPayload;
    SOCKET s = ScanSubnet(stop, found, firstHeader, firstPayload);
    if (s == INVALID_SOCKET) return false;
    if (stop) {
        closesocket(s);
        return false;
    }
    qbt::Log("link: Wi-Fi %s (found by scan)", found.c_str());
    RememberWifiAddress(found);
    Adopt(s);
    firstHeader_ = firstHeader;
    firstPayload_ = std::move(firstPayload);
    hasFirstPacket_ = true;
    route = L"Wi-Fi " + Widen(found);
    return true;
}

void PhoneLink::RememberWifiAddress(const std::string& address) {
    std::lock_guard lock(addressMutex_);
    lastWifiAddress_ = address;
}

void PhoneLink::Adopt(SOCKET s) {
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    int receiveBuffer = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
    DWORD timeout = 3000; // the phone sends 30–60 frames a second; silence means the link is gone
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    Close();
    hasFirstPacket_ = false;
    socket_ = s;
}

bool PhoneLink::ReadPacket(qbt::PacketHeader& header, std::vector<uint8_t>& payload) {
    if (hasFirstPacket_) {
        hasFirstPacket_ = false;
        header = firstHeader_;
        payload = std::move(firstPayload_);
        return true;
    }
    const SOCKET s = socket_;
    if (s == INVALID_SOCKET || !ReceiveAll(s, &header, sizeof(header))) return false;
    if (header.magic != qbt::kPacketMagic || header.length > qbt::kMaxPacketBytes) {
        qbt::Log("link: unexpected data (magic %08x, length %u)", header.magic, header.length);
        return false;
    }
    payload.resize(header.length);
    return header.length == 0 || ReceiveAll(s, payload.data(), payload.size());
}

bool PhoneLink::Send(const void* data, size_t size) {
    std::lock_guard lock(sendMutex_);
    const SOCKET s = socket_;
    if (s == INVALID_SOCKET) return false;
    auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const int sent = send(s, bytes, static_cast<int>(size), 0);
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool PhoneLink::SendAck(uint32_t framesReceived) {
    const qbt::Ack ack{qbt::kAckMagic, framesReceived};
    return Send(&ack, sizeof(ack));
}

bool PhoneLink::SendCommand(const std::string& json) {
    std::string message(sizeof(qbt::Ack), '\0');
    const qbt::Ack header{qbt::kCommandMagic, static_cast<uint32_t>(json.size())};
    memcpy(message.data(), &header, sizeof(header));
    message += json;
    return Send(message.data(), message.size());
}

void PhoneLink::Close() {
    const SOCKET s = socket_.exchange(INVALID_SOCKET);
    if (s == INVALID_SOCKET) return;
    shutdown(s, SD_BOTH);
    closesocket(s);
}
