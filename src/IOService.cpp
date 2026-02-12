#include "boost_tcp_net/IOService.h"

#include <algorithm>

namespace bnet {

IOService::IOService(std::size_t threadCount)
    : workGuard_(boost::asio::make_work_guard(ioContext_)) {
    if (threadCount == 0) {
        threadCount = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    }

    workers_.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this]() {
            ioContext_.run();
        });
    }
}

IOService::~IOService() {
    stop();
}

boost::asio::io_context& IOService::context() {
    return ioContext_;
}

const boost::asio::io_context& IOService::context() const {
    return ioContext_;
}

void IOService::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }

    workGuard_.reset();
    ioContext_.stop();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

}  // namespace bnet
