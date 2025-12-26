#pragma once

// udp_receiver.cpp (updated)
// Cross-platform UDP receiver + sender class (IPv4).
// Build:
//  Linux: g++ -std=c++17 udp_receiver.cpp -pthread -o udp_receiver
//  Windows (MSVC): cl /EHsc udp_receiver.cpp ws2_32.lib

#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include "Logger.h"

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using sock_t = SOCKET;
static const sock_t INVALID_SOCK = INVALID_SOCKET;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
using sock_t = int;
static const sock_t INVALID_SOCK = -1;
#endif

//extern bool load_UdpConfig(const std::string path, UdpConfig& cfg);
//extern bool hexCharToVal(char c, uint8_t& out);
//extern bool hexStringToBytes(const std::string& hexStr, std::vector<uint8_t>& outBytes);

class UdpReceiver {
public:
    // callback: (dataBytes, remoteIp, remotePort)
    using Callback = std::function<void(const std::vector<uint8_t>&, const std::string&, uint16_t)>;

    UdpReceiver(uint16_t port = 3680, const std::string& bindIp = "0.0.0.0")
        : port_(port), bindIp_(bindIp), sock_(INVALID_SOCK), running_(false)
    {
        /*port_ = 3677;
        bindIp_ = "0.0.0.0";
        sock_ = INVALID_SOCK;
        running_ = false;*/
    }

    ~UdpReceiver() {
        stop();
    }

    // Start background receive thread. Returns false on setup failure.
    bool start() {
        std::lock_guard<std::mutex> lg(startStopMutex_);
        if (running_)
        {
            Logger::getInstance().Log("----[UdpReceiver] start() ip = [" + bindIp_ + "] port = [" + std::to_string(port_) + "]");
            return true;
        }

        // create socket
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCK) {
            printLastError("socket()");
            cleanupWinsockIfNeeded();
            return false;
        }

        // allow reuse (helps with quick restarts)
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#else
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

        // allow broadcast by default (so sendBroadcast works without extra sockopt later)
#ifdef _WIN32
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
#else
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#endif

        // bind
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (bindIp_.empty() || bindIp_ == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        else {
            if (inet_pton(AF_INET, bindIp_.c_str(), &addr.sin_addr) != 1) {
                Logger::getInstance().Log("Invalid bind IP: " + bindIp_);
                closeSocket();
                cleanupWinsockIfNeeded();
                return false;
            }
        }

        if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            printLastError("bind()");
            closeSocket();
            cleanupWinsockIfNeeded();
            return false;
        }

        running_ = true;
        workerThread_ = std::thread(&UdpReceiver::runLoop, this);
        return true;
    }

    // Stop background thread and close socket
    void stop() {
        std::lock_guard<std::mutex> lg(startStopMutex_);
        if (!running_) return;
        running_ = false;

        // closing socket will cause select/recvfrom to error/return
        closeSocket();

        if (workerThread_.joinable()) workerThread_.join();
        cleanupWinsockIfNeeded();
    }

    void setCallback(Callback cb) {
        std::lock_guard<std::mutex> lg(cbMutex_);
        callback_ = std::move(cb);
    }

    // Join IPv4 multicast group (simple helper). Return true on success.
    bool joinMulticastGroup(const std::string& mcastAddr) {
        if (sock_ == INVALID_SOCK) {
            Logger::getInstance().Log("Socket not created/bound yet");
            return false;
        }
        ip_mreq mreq{};
        if (inet_pton(AF_INET, mcastAddr.c_str(), &mreq.imr_multiaddr) != 1) {
            Logger::getInstance().Log("Invalid multicast address: " + mcastAddr);
            return false;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) != 0) {
            printLastError("setsockopt(IP_ADD_MEMBERSHIP)");
            return false;
        }
        return true;
    }

    // ---------------------- SENDING API ----------------------

    // Send to specific ip:port (unicast). Returns true on success.
    bool sendTo(const std::vector<uint8_t>& data, const std::string& ip, uint16_t port) {
        if (sock_ == INVALID_SOCK) {
            Logger::getInstance().Log("sendTo: socket not initialized");
            return false;
        }
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) {
            Logger::getInstance().Log("sendTo: invalid ip: " + ip);
            return false;
        }
        int sent = ::sendto(sock_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                            reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (sent < 0) {
            printLastError("sendto()");
            return false;
        }
        return true;
    }
    bool sendTo(const std::string& text, const std::string& ip, uint16_t port) {
        // convert string to vector<uint8_t> without extra null terminator
        std::vector<uint8_t> bytes(text.begin(), text.end());
        Logger::getInstance().Log("---- [udp Send] time = [" + text + "], ip = [" + ip + "], port = [" + std::to_string(port) + "]");
        return sendTo(bytes, ip, port);
    }
    bool sendInt32AsHex(int32_t value, const std::string& ip, uint16_t port);
    bool sendTo(int32_t value, const std::string& ip, uint16_t port, bool addNewline = false);

    // 发送 16-bit 整数（按 big-endian 的 2 字节发送）
    bool sendInt16AsHex(int16_t value, const std::string& ip, uint16_t port);

    // Send to last sender (reply). Returns false if no last sender recorded.
    bool sendToLastSender(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lg(lastSrcMutex_);
        if (!lastSrcValid_) {
            Logger::getInstance().Log("sendToLastSender: no last sender recorded");
            return false;
        }
        sockaddr_in dst = lastSrcAddr_; // copy
        int sent = ::sendto(sock_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                            reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (sent < 0) {
            printLastError("sendto(last)");
            return false;
        }
        return true;
    }

    // Broadcast send. broadcastAddr typically "255.255.255.255" or "192.168.1.255".
    // Returns true on success.
    bool sendBroadcast(const std::vector<uint8_t>& data, const std::string& broadcastAddr, uint16_t port) {
        if (sock_ == INVALID_SOCK) {
            Logger::getInstance().Log("sendBroadcast: socket not initialized");
            return false;
        }
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        if (inet_pton(AF_INET, broadcastAddr.c_str(), &dst.sin_addr) != 1) {
            Logger::getInstance().Log("sendBroadcast: invalid broadcast addr: " + broadcastAddr);
            return false;
        }
        // Ensure SO_BROADCAST is set (we set it in start(), but double-check)
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
#else
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#endif

        int sent = ::sendto(sock_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                            reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (sent < 0) {
            printLastError("sendto(broadcast)");
            return false;
        }
        return true;
    }

private:
    void runLoop() {
        const int BUF_SIZE = 65536;
        std::vector<uint8_t> buffer(BUF_SIZE);

        while (running_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock_, &readfds);

            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 500000; // 500ms timeout so we can check running_ periodically

            int nfds = 0;
#ifdef _WIN32
            nfds = 0; // ignored on Windows
#else
            nfds = sock_ + 1;
#endif

            int sel = select(nfds, &readfds, nullptr, nullptr, &tv);
            if (sel > 0) {
                if (FD_ISSET(sock_, &readfds)) {
                    sockaddr_in src{};
                    socklen_t srclen = sizeof(src);
                    int n = recvfrom(sock_, reinterpret_cast<char*>(buffer.data()), BUF_SIZE, 0,
                                     reinterpret_cast<sockaddr*>(&src), &srclen);
                    if (n > 0) {
                        // store last sender
                        {
                            std::lock_guard<std::mutex> lg(lastSrcMutex_);
                            lastSrcAddr_ = src;
                            lastSrcValid_ = true;
                            // update human-readable
                            char ipbuf[INET_ADDRSTRLEN] = { 0 };
                            inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
                            lastSrcIp_ = std::string(ipbuf);
                            lastSrcPort_ = ntohs(src.sin_port);
                        }

                        std::vector<uint8_t> data(buffer.begin(), buffer.begin() + n);
                        std::string ip = addrToString(src);
                        uint16_t port = ntohs(src.sin_port);
                        Callback cbCopy;
                        {
                            std::lock_guard<std::mutex> lg(cbMutex_);
                            cbCopy = callback_;
                        }
                        if (cbCopy) {
                            try {
                                cbCopy(data, ip, port);
                            }
                            catch (const std::exception& e) {
                                Logger::getInstance().Log(std::string("Callback threw: ") + e.what());
                            }
                            catch (...) {
                                Logger::getInstance().Log("Callback threw unknown exception");
                            }
                        }
                    }
                    else if (n == 0) {
                        // nothing
                    }
                    else {
                        // error
                        printLastError("recvfrom()");
                        // continue (socket maybe closed)
                    }
                }
            }
            else if (sel == 0) {
                // timeout, loop again
            }
            else {
                // select error
                printLastError("select()");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    std::string addrToString(const sockaddr_in& a) {
        char buf[INET_ADDRSTRLEN] = { 0 };
        const char* res = inet_ntop(AF_INET, (const void*)&a.sin_addr, buf, INET_ADDRSTRLEN);
        if (res) return std::string(buf);
        return std::string("0.0.0.0");
    }

    void closeSocket() {
        if (sock_ == INVALID_SOCK) return;
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = INVALID_SOCK;
    }

    void cleanupWinsockIfNeeded() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void printLastError(const char* where) {
#ifdef _WIN32
        int err = WSAGetLastError();
        Logger::getInstance().Log(std::string(where) + " failed, WSAGetLastError=" + std::to_string(err));
#else
        Logger::getInstance().Log(std::string(where) + " failed: " + std::strerror(errno));
#endif
    }

    // members
    uint16_t port_;
    std::string bindIp_;
    sock_t sock_;
    std::atomic<bool> running_;
    std::thread workerThread_;
    std::mutex cbMutex_;
    Callback callback_;
    std::mutex startStopMutex_;

    // last sender info
    std::mutex lastSrcMutex_;
    sockaddr_in lastSrcAddr_{};
    bool lastSrcValid_ = false;
    std::string lastSrcIp_;
    uint16_t lastSrcPort_ = 0;
};

