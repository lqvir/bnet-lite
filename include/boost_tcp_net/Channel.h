#pragma once

#include "boost_tcp_net/Common.h"
#include "boost_tcp_net/Serialization.h"

#include <boost/system/error_code.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(__has_include)
#if __has_include(<span>)
#include <span>
#endif
#endif

namespace bnet {

class Session;

class Channel {
public:
    Channel() = default;
    Channel(const Channel&) = default;
    Channel(Channel&&) noexcept = default;
    Channel& operator=(const Channel&) = default;
    Channel& operator=(Channel&&) noexcept = default;
    ~Channel() = default;

    bool isConnected() const;
    bool waitForConnection(std::chrono::milliseconds timeout);
    void waitForConnection();

    void close();

    const std::string& localName() const;
    const std::string& remoteName() const;
    void setCompressionMode(serialization::CompressionMode mode);
    serialization::CompressionMode compressionMode() const;
    std::uint64_t sentBytes() const;
    std::uint64_t receivedBytes() const;
    void resetTrafficStats();

    void send(const void* data, std::size_t bytes);
    void recv(void* data, std::size_t bytes);

    template <typename T>
    typename std::enable_if<std::is_trivial<T>::value, void>::type send(const T& value) {
        send(&value, sizeof(T));
    }

    template <typename T>
    typename std::enable_if<std::is_trivial<T>::value, void>::type recv(T& value) {
        recv(&value, sizeof(T));
    }

    template <typename T>
    typename std::enable_if<std::is_trivial<T>::value, void>::type sendVector(const std::vector<T>& values) {
        if (!impl_) {
            throw NetworkError("channel is not initialized");
        }

        const auto* ptr = reinterpret_cast<const std::uint8_t*>(values.data());
        sendFrame(impl_, ptr, values.size() * sizeof(T));
    }

    template <typename T>
    typename std::enable_if<std::is_trivial<T>::value, void>::type recvVector(std::vector<T>& values) {
        if (!impl_) {
            throw NetworkError("channel is not initialized");
        }

        auto data = recvFrame(impl_);
        if (data.size() % sizeof(T) != 0) {
            throw NetworkError("received payload size does not match vector element size");
        }

        values.resize(data.size() / sizeof(T));
        if (!data.empty()) {
            std::memcpy(values.data(), data.data(), data.size());
        }
    }

    void sendString(const std::string& text);
    void recvString(std::string& text);
    void sendU32Payload(const std::uint32_t* data, std::size_t count);
#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
    void sendU32Payload(std::span<const std::uint32_t> values) {
        sendU32Payload(values.data(), values.size());
    }
#endif

    void asyncSendBytes(
        std::vector<std::uint8_t> payload,
        std::function<void(const boost::system::error_code&)> callback);
    void asyncSendU32Payload(
        std::vector<std::uint32_t> payload,
        std::function<void(const boost::system::error_code&)> callback);
    void asyncRecvBytes(
        std::function<void(const boost::system::error_code&, std::vector<std::uint8_t>)> callback);
    void asyncSendString(
        std::string text,
        std::function<void(const boost::system::error_code&)> callback);
    void asyncRecvString(
        std::function<void(const boost::system::error_code&, std::string)> callback);

    std::future<void> asyncSendBytes(std::vector<std::uint8_t> payload);
    std::future<void> asyncSendU32Payload(std::vector<std::uint32_t> payload);
    std::future<std::vector<std::uint8_t>> asyncRecvBytes();
    std::future<void> asyncSendString(std::string text);
    std::future<std::string> asyncRecvString();

    static std::pair<Channel, Channel> makeLocalPair(
        std::string firstName = "local_a",
        std::string secondName = "local_b");

    template <typename T>
    typename std::enable_if<std::is_trivial<T>::value, std::future<void>>::type
    asyncSendVector(std::vector<T> values) {
        std::vector<std::uint8_t> payload(values.size() * sizeof(T));
        if (!payload.empty()) {
            std::memcpy(payload.data(), values.data(), payload.size());
        }
        return asyncSendBytes(std::move(payload));
    }

    template <typename T>
    typename std::enable_if<std::is_trivial<T>::value, std::future<std::vector<T>>>::type
    asyncRecvVector() {
        auto promise = std::make_shared<std::promise<std::vector<T>>>();
        auto future = promise->get_future();

        asyncRecvBytes(
            [promise](
                const boost::system::error_code& ec,
                std::vector<std::uint8_t> payload) {
                if (ec) {
                    promise->set_exception(
                        std::make_exception_ptr(NetworkError("asyncRecvVector failed: " + ec.message())));
                    return;
                }

                if (payload.size() % sizeof(T) != 0) {
                    promise->set_exception(
                        std::make_exception_ptr(NetworkError(
                            "received payload size does not match vector element size")));
                    return;
                }

                std::vector<T> values(payload.size() / sizeof(T));
                if (!payload.empty()) {
                    std::memcpy(values.data(), payload.data(), payload.size());
                }
                promise->set_value(std::move(values));
            });

        return future;
    }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    explicit Channel(std::shared_ptr<Impl> impl);
    Channel(boost::asio::ip::tcp::socket&& socket, std::string localName, std::string remoteName);

    static void sendFrame(const std::shared_ptr<Impl>& impl, const std::uint8_t* data, std::size_t size);
    static std::vector<std::uint8_t> recvFrame(const std::shared_ptr<Impl>& impl);

    friend class Session;
};

}  // namespace bnet
