#pragma once

#include <stdexcept>

namespace bnet {

enum class SessionMode : bool {
    Client,
    Server,
};

class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace bnet
