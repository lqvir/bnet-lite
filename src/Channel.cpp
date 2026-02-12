#include "boost_tcp_net/Channel.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/endian/conversion.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace bnet {

namespace {
constexpr std::uint64_t kMaxFrameBytes = 64ULL * 1024ULL * 1024ULL;

boost::system::error_code makeClosedError() {
    return boost::system::errc::make_error_code(boost::system::errc::operation_canceled);
}

boost::system::error_code makeFrameTooLargeError() {
    return boost::system::errc::make_error_code(boost::system::errc::message_size);
}

boost::system::error_code makeSerializationError() {
    return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
}

boost::system::error_code makeIoError() {
    return boost::system::errc::make_error_code(boost::system::errc::io_error);
}

struct LocalPipe {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::uint8_t> bytes;
    bool closed = false;
};

struct LocalEndpoint {
    std::shared_ptr<LocalPipe> incoming;
    std::shared_ptr<LocalPipe> outgoing;
    std::mutex sendMutex;
    std::mutex recvMutex;
};

void closeLocalPipe(const std::shared_ptr<LocalPipe>& pipe) {
    if (!pipe) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pipe->mutex);
        pipe->closed = true;
    }
    pipe->cv.notify_all();
}

bool isLocalPipeOpen(const std::shared_ptr<LocalPipe>& pipe) {
    if (!pipe) {
        return false;
    }

    std::lock_guard<std::mutex> lock(pipe->mutex);
    return !pipe->closed;
}

void localWriteExact(const std::shared_ptr<LocalEndpoint>& endpoint, const std::uint8_t* data, std::size_t bytes) {
    if (!endpoint) {
        throw NetworkError("channel is not initialized");
    }

    std::lock_guard<std::mutex> sendGuard(endpoint->sendMutex);

    {
        std::lock_guard<std::mutex> pipeGuard(endpoint->outgoing->mutex);
        if (endpoint->outgoing->closed) {
            throw NetworkError("channel is closed");
        }

        for (std::size_t i = 0; i < bytes; ++i) {
            endpoint->outgoing->bytes.push_back(data[i]);
        }
    }

    endpoint->outgoing->cv.notify_all();
}

void localReadExact(const std::shared_ptr<LocalEndpoint>& endpoint, std::uint8_t* data, std::size_t bytes) {
    if (!endpoint) {
        throw NetworkError("channel is not initialized");
    }

    std::lock_guard<std::mutex> recvGuard(endpoint->recvMutex);

    std::size_t copied = 0;
    while (copied < bytes) {
        std::unique_lock<std::mutex> pipeGuard(endpoint->incoming->mutex);
        endpoint->incoming->cv.wait(pipeGuard, [&]() {
            return endpoint->incoming->closed || !endpoint->incoming->bytes.empty();
        });

        while (copied < bytes && !endpoint->incoming->bytes.empty()) {
            data[copied++] = endpoint->incoming->bytes.front();
            endpoint->incoming->bytes.pop_front();
        }

        if (copied == bytes) {
            return;
        }

        if (endpoint->incoming->closed) {
            throw NetworkError("channel is closed");
        }
    }
}

}  // namespace

struct Channel::Impl {
    using executor_type = boost::asio::ip::tcp::socket::executor_type;

    Impl(boost::asio::ip::tcp::socket&& socketIn, std::string local, std::string remote)
        : socket(std::make_unique<boost::asio::ip::tcp::socket>(std::move(socketIn))),
          strand(std::make_unique<boost::asio::strand<executor_type>>(boost::asio::make_strand(socket->get_executor()))),
          localName(std::move(local)),
          remoteName(std::move(remote)) {}

    Impl(std::shared_ptr<LocalEndpoint> localEndpointIn, std::string local, std::string remote)
        : localEndpoint(std::move(localEndpointIn)),
          localName(std::move(local)),
          remoteName(std::move(remote)) {}

    bool isLocal() const {
        return static_cast<bool>(localEndpoint);
    }

    std::unique_ptr<boost::asio::ip::tcp::socket> socket;
    std::unique_ptr<boost::asio::strand<executor_type>> strand;
    std::shared_ptr<LocalEndpoint> localEndpoint;
    std::string localName;
    std::string remoteName;
    std::atomic<bool> closed{false};
    std::atomic<std::uint8_t> compressionMode{
        static_cast<std::uint8_t>(serialization::defaultCompressionMode())};
};

Channel::Channel(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

Channel::Channel(boost::asio::ip::tcp::socket&& socket, std::string localName, std::string remoteName)
    : Channel(std::make_shared<Impl>(std::move(socket), std::move(localName), std::move(remoteName))) {}

std::pair<Channel, Channel> Channel::makeLocalPair(std::string firstName, std::string secondName) {
    auto firstToSecond = std::make_shared<LocalPipe>();
    auto secondToFirst = std::make_shared<LocalPipe>();

    auto firstEndpoint = std::make_shared<LocalEndpoint>();
    firstEndpoint->incoming = secondToFirst;
    firstEndpoint->outgoing = firstToSecond;

    auto secondEndpoint = std::make_shared<LocalEndpoint>();
    secondEndpoint->incoming = firstToSecond;
    secondEndpoint->outgoing = secondToFirst;

    auto first = Channel(std::make_shared<Impl>(firstEndpoint, std::move(firstName), secondName));
    auto second = Channel(std::make_shared<Impl>(secondEndpoint, std::move(secondName), first.localName()));

    return {std::move(first), std::move(second)};
}

bool Channel::isConnected() const {
    if (!impl_ || impl_->closed.load(std::memory_order_acquire)) {
        return false;
    }

    if (impl_->isLocal()) {
        return isLocalPipeOpen(impl_->localEndpoint->incoming) && isLocalPipeOpen(impl_->localEndpoint->outgoing);
    }

    return impl_->socket && impl_->socket->is_open();
}

bool Channel::waitForConnection(std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (isConnected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return isConnected();
}

void Channel::waitForConnection() {
    if (!isConnected()) {
        throw NetworkError("channel is not connected");
    }
}

void Channel::close() {
    if (!impl_) {
        return;
    }

    if (impl_->isLocal()) {
        if (!impl_->closed.exchange(true, std::memory_order_acq_rel)) {
            closeLocalPipe(impl_->localEndpoint->incoming);
            closeLocalPipe(impl_->localEndpoint->outgoing);
        }
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    auto impl = impl_;

    boost::asio::dispatch(*impl->strand, [impl, done]() {
        if (!impl->closed.exchange(true, std::memory_order_acq_rel)) {
            boost::system::error_code ec;
            impl->socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            impl->socket->close(ec);
        }
        done->set_value();
    });

    future.get();
}

const std::string& Channel::localName() const {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }
    return impl_->localName;
}

const std::string& Channel::remoteName() const {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }
    return impl_->remoteName;
}

void Channel::setCompressionMode(serialization::CompressionMode mode) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }
    if (!serialization::isSupportedCompressionMode(mode)) {
        throw NetworkError("unsupported compression mode");
    }
    impl_->compressionMode.store(static_cast<std::uint8_t>(mode), std::memory_order_release);
}

serialization::CompressionMode Channel::compressionMode() const {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }
    return static_cast<serialization::CompressionMode>(
        impl_->compressionMode.load(std::memory_order_acquire));
}

void Channel::send(const void* data, std::size_t bytes) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }

    if (impl_->isLocal()) {
        if (impl_->closed.load(std::memory_order_acquire)) {
            throw NetworkError("channel is closed");
        }
        localWriteExact(impl_->localEndpoint, reinterpret_cast<const std::uint8_t*>(data), bytes);
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    auto impl = impl_;

    boost::asio::dispatch(*impl->strand, [impl, data, bytes, done]() {
        try {
            if (impl->closed.load(std::memory_order_acquire) || !impl->socket->is_open()) {
                throw NetworkError("channel is closed");
            }
            boost::asio::write(*impl->socket, boost::asio::buffer(data, bytes));
            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });

    future.get();
}

void Channel::recv(void* data, std::size_t bytes) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }

    if (impl_->isLocal()) {
        if (impl_->closed.load(std::memory_order_acquire)) {
            throw NetworkError("channel is closed");
        }
        localReadExact(impl_->localEndpoint, reinterpret_cast<std::uint8_t*>(data), bytes);
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    auto impl = impl_;

    boost::asio::dispatch(*impl->strand, [impl, data, bytes, done]() {
        try {
            if (impl->closed.load(std::memory_order_acquire) || !impl->socket->is_open()) {
                throw NetworkError("channel is closed");
            }
            boost::asio::read(*impl->socket, boost::asio::buffer(data, bytes));
            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });

    future.get();
}

void Channel::sendFrame(const std::shared_ptr<Impl>& impl, const std::uint8_t* data, std::size_t size) {
    if (impl->isLocal()) {
        if (impl->closed.load(std::memory_order_acquire)) {
            throw NetworkError("channel is closed");
        }

        const auto mode = static_cast<serialization::CompressionMode>(
            impl->compressionMode.load(std::memory_order_acquire));
        auto wire = serialization::pack(data, size, mode);

        if (wire.size() > kMaxFrameBytes) {
            throw NetworkError("payload exceeds max frame size");
        }

        std::uint64_t len = boost::endian::native_to_big(static_cast<std::uint64_t>(wire.size()));
        localWriteExact(impl->localEndpoint, reinterpret_cast<const std::uint8_t*>(&len), sizeof(len));
        if (!wire.empty()) {
            localWriteExact(impl->localEndpoint, wire.data(), wire.size());
        }
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();

    boost::asio::dispatch(*impl->strand, [impl, data, size, done]() {
        try {
            if (impl->closed.load(std::memory_order_acquire) || !impl->socket->is_open()) {
                throw NetworkError("channel is closed");
            }

            const auto mode = static_cast<serialization::CompressionMode>(
                impl->compressionMode.load(std::memory_order_acquire));
            auto wire = serialization::pack(data, size, mode);

            if (wire.size() > kMaxFrameBytes) {
                throw NetworkError("payload exceeds max frame size");
            }

            std::uint64_t len = boost::endian::native_to_big(static_cast<std::uint64_t>(wire.size()));
            boost::asio::write(*impl->socket, boost::asio::buffer(&len, sizeof(len)));
            if (!wire.empty()) {
                boost::asio::write(*impl->socket, boost::asio::buffer(wire.data(), wire.size()));
            }

            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });

    future.get();
}

std::vector<std::uint8_t> Channel::recvFrame(const std::shared_ptr<Impl>& impl) {
    if (impl->isLocal()) {
        if (impl->closed.load(std::memory_order_acquire)) {
            throw NetworkError("channel is closed");
        }

        std::uint64_t len = 0;
        localReadExact(impl->localEndpoint, reinterpret_cast<std::uint8_t*>(&len), sizeof(len));

        len = boost::endian::big_to_native(len);
        if (len > kMaxFrameBytes) {
            throw NetworkError("received payload exceeds max frame size");
        }

        std::vector<std::uint8_t> payload(static_cast<std::size_t>(len));
        if (!payload.empty()) {
            localReadExact(impl->localEndpoint, payload.data(), payload.size());
        }

        return serialization::unpack(payload);
    }

    auto done = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
    auto future = done->get_future();

    boost::asio::dispatch(*impl->strand, [impl, done]() {
        try {
            if (impl->closed.load(std::memory_order_acquire) || !impl->socket->is_open()) {
                throw NetworkError("channel is closed");
            }

            std::uint64_t len = 0;
            boost::asio::read(*impl->socket, boost::asio::buffer(&len, sizeof(len)));

            len = boost::endian::big_to_native(len);
            if (len > kMaxFrameBytes) {
                throw NetworkError("received payload exceeds max frame size");
            }

            std::vector<std::uint8_t> payload(static_cast<std::size_t>(len));
            if (!payload.empty()) {
                boost::asio::read(*impl->socket, boost::asio::buffer(payload.data(), payload.size()));
            }
            done->set_value(serialization::unpack(payload));
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });

    return future.get();
}

void Channel::sendString(const std::string& text) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }

    sendFrame(impl_, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Channel::recvString(std::string& text) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }

    auto payload = recvFrame(impl_);
    text.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
}

void Channel::sendU32Payload(const std::uint32_t* data, std::size_t count) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }
    if (count != 0 && data == nullptr) {
        throw NetworkError("sendU32Payload got null data with non-zero count");
    }
    if (count > (kMaxFrameBytes / sizeof(std::uint32_t))) {
        throw NetworkError("payload exceeds max frame size");
    }

    sendFrame(
        impl_,
        reinterpret_cast<const std::uint8_t*>(data),
        count * sizeof(std::uint32_t));
}

void Channel::asyncSendBytes(
    std::vector<std::uint8_t> payload,
    std::function<void(const boost::system::error_code&)> callback) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }

    if (!callback) {
        callback = [](const boost::system::error_code&) {};
    }

    auto impl = impl_;

    if (impl->isLocal()) {
        std::thread([impl, payload = std::move(payload), callback = std::move(callback)]() mutable {
            if (impl->closed.load(std::memory_order_acquire)) {
                callback(makeClosedError());
                return;
            }

            std::vector<std::uint8_t> wire;
            try {
                const auto mode = static_cast<serialization::CompressionMode>(
                    impl->compressionMode.load(std::memory_order_acquire));
                wire = serialization::pack(payload, mode);
            } catch (...) {
                callback(makeSerializationError());
                return;
            }

            if (wire.size() > kMaxFrameBytes) {
                callback(makeFrameTooLargeError());
                return;
            }

            std::uint64_t len = boost::endian::native_to_big(static_cast<std::uint64_t>(wire.size()));
            try {
                localWriteExact(impl->localEndpoint, reinterpret_cast<const std::uint8_t*>(&len), sizeof(len));
                if (!wire.empty()) {
                    localWriteExact(impl->localEndpoint, wire.data(), wire.size());
                }
                callback({});
            } catch (...) {
                callback(makeClosedError());
            }
        }).detach();
        return;
    }

    auto payloadPtr = std::make_shared<std::vector<std::uint8_t>>(std::move(payload));

    boost::asio::post(*impl->strand, [impl, payloadPtr, callback = std::move(callback)]() mutable {
        if (impl->closed.load(std::memory_order_acquire) || !impl->socket->is_open()) {
            callback(makeClosedError());
            return;
        }

        std::shared_ptr<std::vector<std::uint8_t>> wire;
        try {
            const auto mode = static_cast<serialization::CompressionMode>(
                impl->compressionMode.load(std::memory_order_acquire));
            wire = std::make_shared<std::vector<std::uint8_t>>(serialization::pack(*payloadPtr, mode));
        } catch (...) {
            callback(makeSerializationError());
            return;
        }

        if (wire->size() > kMaxFrameBytes) {
            callback(makeFrameTooLargeError());
            return;
        }

        auto header = std::make_shared<std::array<std::uint8_t, sizeof(std::uint64_t)>>();
        std::uint64_t len = boost::endian::native_to_big(static_cast<std::uint64_t>(wire->size()));
        std::memcpy(header->data(), &len, sizeof(len));

        std::array<boost::asio::const_buffer, 2> buffers{
            boost::asio::buffer(*header),
            boost::asio::buffer(wire->data(), wire->size())};

        boost::asio::async_write(
            *impl->socket,
            buffers,
            boost::asio::bind_executor(
                *impl->strand,
                [impl, header, wire, callback = std::move(callback)](
                    const boost::system::error_code& ec,
                    std::size_t) mutable {
                    callback(ec);
                }));
    });
}

void Channel::asyncSendU32Payload(
    std::vector<std::uint32_t> payload,
    std::function<void(const boost::system::error_code&)> callback) {
    if (!callback) {
        callback = [](const boost::system::error_code&) {};
    }

    if (payload.size() > (kMaxFrameBytes / sizeof(std::uint32_t))) {
        callback(makeFrameTooLargeError());
        return;
    }

    std::vector<std::uint8_t> bytes(payload.size() * sizeof(std::uint32_t));
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), payload.data(), bytes.size());
    }
    asyncSendBytes(std::move(bytes), std::move(callback));
}

void Channel::asyncRecvBytes(
    std::function<void(const boost::system::error_code&, std::vector<std::uint8_t>)> callback) {
    if (!impl_) {
        throw NetworkError("channel is not initialized");
    }

    if (!callback) {
        callback = [](const boost::system::error_code&, std::vector<std::uint8_t>) {};
    }

    auto impl = impl_;

    if (impl->isLocal()) {
        std::thread([impl, callback = std::move(callback)]() mutable {
            if (impl->closed.load(std::memory_order_acquire)) {
                callback(makeClosedError(), {});
                return;
            }

            std::uint64_t len = 0;
            try {
                localReadExact(impl->localEndpoint, reinterpret_cast<std::uint8_t*>(&len), sizeof(len));
            } catch (...) {
                callback(makeClosedError(), {});
                return;
            }

            len = boost::endian::big_to_native(len);
            if (len > kMaxFrameBytes) {
                callback(makeFrameTooLargeError(), {});
                return;
            }

            std::vector<std::uint8_t> payload(static_cast<std::size_t>(len));
            if (!payload.empty()) {
                try {
                    localReadExact(impl->localEndpoint, payload.data(), payload.size());
                } catch (...) {
                    callback(makeClosedError(), {});
                    return;
                }
            }

            try {
                auto unpacked = serialization::unpack(payload);
                callback({}, std::move(unpacked));
            } catch (...) {
                callback(makeSerializationError(), {});
            }
        }).detach();
        return;
    }

    boost::asio::post(*impl->strand, [impl, callback = std::move(callback)]() mutable {
        if (impl->closed.load(std::memory_order_acquire) || !impl->socket->is_open()) {
            callback(makeClosedError(), {});
            return;
        }

        auto header = std::make_shared<std::array<std::uint8_t, sizeof(std::uint64_t)>>();

        boost::asio::async_read(
            *impl->socket,
            boost::asio::buffer(*header),
            boost::asio::bind_executor(
                *impl->strand,
                [impl, header, callback = std::move(callback)](
                    const boost::system::error_code& ec,
                    std::size_t) mutable {
                    if (ec) {
                        callback(ec, {});
                        return;
                    }

                    std::uint64_t len = 0;
                    std::memcpy(&len, header->data(), sizeof(len));
                    len = boost::endian::big_to_native(len);

                    if (len > kMaxFrameBytes) {
                        callback(makeFrameTooLargeError(), {});
                        return;
                    }

                    auto payload = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(len));
                    if (payload->empty()) {
                        try {
                            auto unpacked = serialization::unpack(*payload);
                            callback({}, std::move(unpacked));
                        } catch (...) {
                            callback(makeSerializationError(), {});
                        }
                        return;
                    }

                    boost::asio::async_read(
                        *impl->socket,
                        boost::asio::buffer(payload->data(), payload->size()),
                        boost::asio::bind_executor(
                            *impl->strand,
                            [payload, callback = std::move(callback)](
                                const boost::system::error_code& readEc,
                                std::size_t) mutable {
                                if (readEc) {
                                    callback(readEc, {});
                                    return;
                                }

                                try {
                                    auto unpacked = serialization::unpack(*payload);
                                    callback({}, std::move(unpacked));
                                } catch (...) {
                                    callback(makeSerializationError(), {});
                                }
                            }));
                }));
    });
}

void Channel::asyncSendString(
    std::string text,
    std::function<void(const boost::system::error_code&)> callback) {
    std::vector<std::uint8_t> payload(text.size());
    if (!text.empty()) {
        std::memcpy(payload.data(), text.data(), text.size());
    }
    asyncSendBytes(std::move(payload), std::move(callback));
}

void Channel::asyncRecvString(
    std::function<void(const boost::system::error_code&, std::string)> callback) {
    if (!callback) {
        callback = [](const boost::system::error_code&, std::string) {};
    }

    asyncRecvBytes([callback = std::move(callback)](
                       const boost::system::error_code& ec,
                       std::vector<std::uint8_t> payload) mutable {
        if (ec) {
            callback(ec, {});
            return;
        }

        std::string text(payload.size(), '\0');
        if (!payload.empty()) {
            std::memcpy(text.data(), payload.data(), payload.size());
        }
        callback({}, std::move(text));
    });
}

std::future<void> Channel::asyncSendBytes(std::vector<std::uint8_t> payload) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    asyncSendBytes(
        std::move(payload),
        [promise](const boost::system::error_code& ec) {
            if (ec) {
                promise->set_exception(
                    std::make_exception_ptr(NetworkError("asyncSendBytes failed: " + ec.message())));
                return;
            }
            promise->set_value();
        });

    return future;
}

std::future<void> Channel::asyncSendU32Payload(std::vector<std::uint32_t> payload) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    asyncSendU32Payload(
        std::move(payload),
        [promise](const boost::system::error_code& ec) {
            if (ec) {
                promise->set_exception(
                    std::make_exception_ptr(NetworkError("asyncSendU32Payload failed: " + ec.message())));
                return;
            }
            promise->set_value();
        });

    return future;
}

std::future<std::vector<std::uint8_t>> Channel::asyncRecvBytes() {
    auto promise = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
    auto future = promise->get_future();

    asyncRecvBytes(
        [promise](const boost::system::error_code& ec, std::vector<std::uint8_t> payload) {
            if (ec) {
                promise->set_exception(
                    std::make_exception_ptr(NetworkError("asyncRecvBytes failed: " + ec.message())));
                return;
            }
            promise->set_value(std::move(payload));
        });

    return future;
}

std::future<void> Channel::asyncSendString(std::string text) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    asyncSendString(
        std::move(text),
        [promise](const boost::system::error_code& ec) {
            if (ec) {
                promise->set_exception(
                    std::make_exception_ptr(NetworkError("asyncSendString failed: " + ec.message())));
                return;
            }
            promise->set_value();
        });

    return future;
}

std::future<std::string> Channel::asyncRecvString() {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    asyncRecvString(
        [promise](const boost::system::error_code& ec, std::string text) {
            if (ec) {
                promise->set_exception(
                    std::make_exception_ptr(NetworkError("asyncRecvString failed: " + ec.message())));
                return;
            }
            promise->set_value(std::move(text));
        });

    return future;
}

}  // namespace bnet
