#include "boost_tcp_net/Channel.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
    try {
        auto pair = bnet::Channel::makeLocalPair("local_client", "local_server");
        auto& client = pair.first;
        auto& server = pair.second;

        auto recvFuture = server.asyncRecvString();
        client.sendString("hello from local client");
        std::cout << "server recv: " << recvFuture.get() << '\n';

        std::vector<std::uint8_t> reply{7, 8, 9};
        server.sendVector(reply);

        std::vector<std::uint8_t> recvBytes;
        client.recvVector(recvBytes);

        std::cout << "client recv bytes:";
        for (std::uint8_t v : recvBytes) {
            std::cout << ' ' << static_cast<int>(v);
        }
        std::cout << '\n';
        std::cout << "client traffic tx/rx: " << client.sentBytes() << '/' << client.receivedBytes() << '\n';
        std::cout << "server traffic tx/rx: " << server.sentBytes() << '/' << server.receivedBytes() << '\n';

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "local demo failed: " << e.what() << '\n';
        return 1;
    }
}
