#pragma once

#include "boost_tcp_net/Common.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <vector>

namespace bnet::serialization {

enum class CompressionMode : std::uint8_t {
    none = 0,
    zlib = 1,
};

struct Header {
    std::uint16_t magic;
    std::uint8_t header_size;
    std::uint8_t version_major;
    std::uint8_t version_minor;
    CompressionMode compr_mode;
    std::uint16_t reserved;
    std::uint64_t size;
};

inline constexpr std::uint16_t kMagic = 0xB17E;
inline constexpr std::uint8_t kVersionMajor = 1;
inline constexpr std::uint8_t kVersionMinor = 0;
inline constexpr std::uint8_t kHeaderSize = 0x10;
static_assert(sizeof(Header) == kHeaderSize, "serialization header size must be 16 bytes");

CompressionMode defaultCompressionMode() noexcept;
bool isSupportedCompressionMode(CompressionMode mode) noexcept;
bool isValidHeader(const Header& header) noexcept;

std::vector<std::uint8_t> pack(
    const std::uint8_t* data,
    std::size_t size,
    CompressionMode mode = CompressionMode::none);
std::vector<std::uint8_t> pack(
    const std::vector<std::uint8_t>& payload,
    CompressionMode mode = CompressionMode::none);

std::vector<std::uint8_t> unpack(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> unpack(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> save(
    const std::function<void(std::ostream&)>& save_members,
    CompressionMode mode = CompressionMode::none);
void load(
    const std::uint8_t* data,
    std::size_t size,
    const std::function<void(std::istream&)>& load_members);
void load(
    const std::vector<std::uint8_t>& payload,
    const std::function<void(std::istream&)>& load_members);

}  // namespace bnet::serialization

