#include "boost_tcp_net/Serialization.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <string>

#ifdef BNET_USE_ZLIB
#include <zlib.h>
#endif

namespace bnet::serialization {
namespace {

std::vector<std::uint8_t> compressZlib(const std::uint8_t* data, std::size_t size) {
#ifdef BNET_USE_ZLIB
    if (size == 0) {
        return {};
    }

    uLong source_len = static_cast<uLong>(size);
    uLongf bound = compressBound(source_len);
    std::vector<std::uint8_t> out(bound);

    uLongf out_len = bound;
    int ret = compress2(
        reinterpret_cast<Bytef*>(out.data()),
        &out_len,
        reinterpret_cast<const Bytef*>(data),
        source_len,
        Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) {
        throw NetworkError("zlib compression failed");
    }

    out.resize(static_cast<std::size_t>(out_len));
    return out;
#else
    (void)data;
    (void)size;
    throw NetworkError("zlib compression is not enabled");
#endif
}

std::vector<std::uint8_t> decompressZlib(const std::uint8_t* data, std::size_t size) {
#ifdef BNET_USE_ZLIB
    std::vector<std::uint8_t> out;
    if (size == 0) {
        return out;
    }

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::uint8_t*>(data));
    stream.avail_in = static_cast<uInt>(size);

    if (inflateInit(&stream) != Z_OK) {
        throw NetworkError("zlib inflate init failed");
    }

    std::array<std::uint8_t, 4096> chunk{};
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        ret = inflate(&stream, Z_NO_FLUSH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            throw NetworkError("zlib decompression failed");
        }

        std::size_t produced = chunk.size() - stream.avail_out;
        out.insert(out.end(), chunk.data(), chunk.data() + produced);
    }

    inflateEnd(&stream);
    return out;
#else
    (void)data;
    (void)size;
    throw NetworkError("zlib decompression is not enabled");
#endif
}

Header makeHeader(CompressionMode mode, std::uint64_t total_size) {
    Header header{};
    header.magic = kMagic;
    header.header_size = kHeaderSize;
    header.version_major = kVersionMajor;
    header.version_minor = kVersionMinor;
    header.compr_mode = mode;
    header.reserved = 0;
    header.size = total_size;
    return header;
}

}  // namespace

CompressionMode defaultCompressionMode() noexcept {
#ifdef BNET_USE_ZLIB
    return CompressionMode::zlib;
#else
    return CompressionMode::none;
#endif
}

bool isSupportedCompressionMode(CompressionMode mode) noexcept {
    switch (mode) {
    case CompressionMode::none:
        return true;
    case CompressionMode::zlib:
#ifdef BNET_USE_ZLIB
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

bool isValidHeader(const Header& header) noexcept {
    if (header.magic != kMagic) {
        return false;
    }
    if (header.header_size != kHeaderSize) {
        return false;
    }
    if (header.version_major != kVersionMajor) {
        return false;
    }
    if (!isSupportedCompressionMode(header.compr_mode)) {
        return false;
    }
    return true;
}

std::vector<std::uint8_t> pack(const std::uint8_t* data, std::size_t size, CompressionMode mode) {
    if (size && data == nullptr) {
        throw NetworkError("pack input is null");
    }
    if (!isSupportedCompressionMode(mode)) {
        throw NetworkError("unsupported compression mode");
    }

    std::vector<std::uint8_t> body;
    CompressionMode effective_mode = mode;

    if (mode == CompressionMode::zlib && size > 0) {
        body = compressZlib(data, size);
        // Compression can hurt on tiny/incompressible payloads.
        if (body.size() >= size) {
            effective_mode = CompressionMode::none;
            body.assign(data, data + size);
        }
    } else {
        body.assign(data, data + size);
    }

    std::uint64_t total_size = static_cast<std::uint64_t>(kHeaderSize + body.size());
    Header header = makeHeader(effective_mode, total_size);

    std::vector<std::uint8_t> out(static_cast<std::size_t>(total_size));
    std::memcpy(out.data(), &header, kHeaderSize);
    if (!body.empty()) {
        std::memcpy(out.data() + kHeaderSize, body.data(), body.size());
    }

    return out;
}

std::vector<std::uint8_t> pack(const std::vector<std::uint8_t>& payload, CompressionMode mode) {
    return pack(payload.data(), payload.size(), mode);
}

std::vector<std::uint8_t> unpack(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return {};
    }
    if (data == nullptr) {
        throw NetworkError("unpack input is null");
    }

    // Backward compatibility: legacy frame payload without serialization header.
    if (size < kHeaderSize) {
        return std::vector<std::uint8_t>(data, data + size);
    }

    Header header{};
    std::memcpy(&header, data, kHeaderSize);

    if (header.magic != kMagic) {
        return std::vector<std::uint8_t>(data, data + size);
    }

    if (!isValidHeader(header)) {
        throw NetworkError("invalid serialization header");
    }
    if (header.size != size) {
        throw NetworkError("serialized payload size mismatch");
    }

    const auto* body = data + kHeaderSize;
    std::size_t body_size = size - kHeaderSize;

    switch (header.compr_mode) {
    case CompressionMode::none:
        return std::vector<std::uint8_t>(body, body + body_size);
    case CompressionMode::zlib:
        return decompressZlib(body, body_size);
    default:
        throw NetworkError("unsupported compression mode in payload");
    }
}

std::vector<std::uint8_t> unpack(const std::vector<std::uint8_t>& payload) {
    return unpack(payload.data(), payload.size());
}

std::vector<std::uint8_t> save(
    const std::function<void(std::ostream&)>& save_members,
    CompressionMode mode) {
    if (!save_members) {
        throw NetworkError("save callback is empty");
    }

    std::ostringstream stream(std::ios::out | std::ios::binary);
    save_members(stream);
    std::string raw = stream.str();

    const auto* ptr = reinterpret_cast<const std::uint8_t*>(raw.data());
    return pack(ptr, raw.size(), mode);
}

void load(
    const std::uint8_t* data,
    std::size_t size,
    const std::function<void(std::istream&)>& load_members) {
    if (!load_members) {
        throw NetworkError("load callback is empty");
    }

    auto raw = unpack(data, size);
    std::string buffer(raw.begin(), raw.end());
    std::istringstream stream(buffer, std::ios::in | std::ios::binary);
    load_members(stream);
}

void load(
    const std::vector<std::uint8_t>& payload,
    const std::function<void(std::istream&)>& load_members) {
    load(payload.data(), payload.size(), load_members);
}

}  // namespace bnet::serialization
