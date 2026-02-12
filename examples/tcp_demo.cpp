#include "boost_tcp_net/Channel.h"
#include "boost_tcp_net/IOService.h"
#include "boost_tcp_net/Session.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    try {
        bnet::IOService ioService(2);

        bnet::Session server(ioService, "127.0.0.1", 18080, bnet::SessionMode::Server, "demo_session");
        bnet::Session client(ioService, "127.0.0.1", 18080, bnet::SessionMode::Client, "demo_session");

        std::thread serverThread([&]() {
            bnet::Channel serverChannel = server.addChannel("data");
            serverChannel.setCompressionMode(bnet::serialization::defaultCompressionMode());

            auto recvFuture = serverChannel.asyncRecvString();
            std::string message = recvFuture.get();
            std::cout << "server recv: " << message << '\n';

            std::vector<std::uint8_t> bytes{1, 2, 3, 4, 5};
            std::promise<void> sendDone;
            auto sendDoneFuture = sendDone.get_future();
            serverChannel.asyncSendBytes(
                bytes,
                [&sendDone](const boost::system::error_code& ec) {
                    if (ec) {
                        sendDone.set_exception(
                            std::make_exception_ptr(std::runtime_error(ec.message())));
                        return;
                    }
                    sendDone.set_value();
                });
            sendDoneFuture.get();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        bnet::Channel clientChannel = client.addChannel("data");
        clientChannel.setCompressionMode(bnet::serialization::defaultCompressionMode());
        auto sendFuture = clientChannel.asyncSendString("hello from client");
        sendFuture.get();

        auto recvFuture = clientChannel.asyncRecvBytes();
        auto payload = recvFuture.get();

        std::cout << "client recv bytes:";
        for (std::uint8_t v : payload) {
            std::cout << ' ' << static_cast<int>(v);
        }
        std::cout << '\n';

        auto localPair = bnet::Channel::makeLocalPair("local_client", "local_server");
        auto& localClient = localPair.first;
        auto& localServer = localPair.second;

        auto localRecvFuture = localServer.asyncRecvString();
        localClient.sendString("hello from local client");
        std::cout << "local server recv: " << localRecvFuture.get() << '\n';

        localServer.sendString("hello from local server");
        std::string localReply;
        localClient.recvString(localReply);
        std::cout << "local client recv: " << localReply << '\n';

        serverThread.join();

        server.stop();
        client.stop();
        ioService.stop();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "demo failed: " << e.what() << '\n';
        return 1;
    }
}
