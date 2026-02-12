#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace bnet {

class IOService {
public:
    explicit IOService(std::size_t threadCount = 0);
    ~IOService();

    IOService(const IOService&) = delete;
    IOService& operator=(const IOService&) = delete;

    boost::asio::io_context& context();
    const boost::asio::io_context& context() const;

    void stop();

private:
    boost::asio::io_context ioContext_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopped_{false};
};

}  // namespace bnet
