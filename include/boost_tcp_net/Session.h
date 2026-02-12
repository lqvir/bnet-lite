#pragma once

#include "boost_tcp_net/Common.h"

#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace bnet {

class IOService;
class Channel;

class Session {
public:
    Session() = default;
    Session(IOService& ioService, std::string host, std::uint16_t port, SessionMode mode, std::string name = "");

    void start(IOService& ioService, std::string host, std::uint16_t port, SessionMode mode, std::string name = "");

    Channel addChannel(std::string localName = "", std::string remoteName = "");

    void stop();

    bool stopped() const;
    const std::string& host() const;
    std::uint16_t port() const;
    SessionMode mode() const;
    const std::string& name() const;

private:
    IOService* ioService_ = nullptr;
    std::string host_;
    std::uint16_t port_ = 0;
    SessionMode mode_ = SessionMode::Client;
    std::string name_;
    bool stopped_ = true;

    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::mutex acceptMutex_;
    std::atomic<std::uint32_t> autoNameCounter_{0};
};

}  // namespace bnet
