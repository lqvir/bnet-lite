#include "boost_tcp_net/Session.h"

#include "boost_tcp_net/Channel.h"
#include "boost_tcp_net/IOService.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <utility>

namespace bnet {
namespace {

boost::asio::ip::tcp::endpoint makeServerEndpoint(
    boost::asio::io_context& ioContext,
    const std::string& host,
    std::uint16_t port) {
    namespace ip = boost::asio::ip;

    if (host.empty() || host == "*" || host == "0.0.0.0") {
        return ip::tcp::endpoint(ip::tcp::v4(), port);
    }

    ip::tcp::resolver resolver(ioContext);
    boost::system::error_code ec;
    auto result = resolver.resolve(host, std::to_string(port), ec);
    if (ec || result.empty()) {
        throw NetworkError("failed to resolve server bind endpoint: " + ec.message());
    }
    return *result.begin();
}

}  // namespace

Session::Session(IOService& ioService, std::string host, std::uint16_t port, SessionMode mode, std::string name) {
    start(ioService, std::move(host), port, mode, std::move(name));
}

void Session::start(IOService& ioService, std::string host, std::uint16_t port, SessionMode mode, std::string name) {
    if (!stopped_) {
        throw NetworkError("session is already started");
    }

    ioService_ = &ioService;
    host_ = std::move(host);
    port_ = port;
    mode_ = mode;
    name_ = std::move(name);
    stopped_ = false;

    if (mode_ == SessionMode::Server) {
        auto& ioContext = ioService_->context();
        auto endpoint = makeServerEndpoint(ioContext, host_, port_);

        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(ioContext);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen(boost::asio::socket_base::max_listen_connections);
    }
}

Channel Session::addChannel(std::string localName, std::string remoteName) {
    if (stopped_ || ioService_ == nullptr) {
        throw NetworkError("session is not started");
    }

    if (localName.empty()) {
        localName = "_auto_" + std::to_string(autoNameCounter_.fetch_add(1));
        if (!remoteName.empty()) {
            throw NetworkError("remoteName must be empty when localName is empty");
        }
    }

    if (remoteName.empty()) {
        remoteName = localName;
    }

    namespace ip = boost::asio::ip;
    ip::tcp::socket socket(ioService_->context());

    if (mode_ == SessionMode::Server) {
        if (!acceptor_) {
            throw NetworkError("server session has no acceptor");
        }

        std::lock_guard<std::mutex> lock(acceptMutex_);
        acceptor_->accept(socket);
    } else {
        ip::tcp::resolver resolver(ioService_->context());
        auto endpoints = resolver.resolve(host_, std::to_string(port_));
        boost::asio::connect(socket, endpoints);
    }

    socket.set_option(ip::tcp::no_delay(true));
    return Channel(std::move(socket), std::move(localName), std::move(remoteName));
}

void Session::stop() {
    if (stopped_) {
        return;
    }

    stopped_ = true;

    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->cancel(ec);
        acceptor_->close(ec);
        acceptor_.reset();
    }
}

bool Session::stopped() const {
    return stopped_;
}

const std::string& Session::host() const {
    return host_;
}

std::uint16_t Session::port() const {
    return port_;
}

SessionMode Session::mode() const {
    return mode_;
}

const std::string& Session::name() const {
    return name_;
}

}  // namespace bnet
