# boost_tcp_net

这是一个按 `cryptoTools/Network` 风格实现的精简版 C++ 网络模块，核心目标是：

- `IOService`：统一管理 `boost::asio::io_context` 和线程池。
- `Session`：管理 Client/Server 会话，负责连接或监听。
- `Channel`：负责具体的 TCP 收发，提供同步与异步（`future` + `callback`）接口。
- `Serialization`：独立序列化模块（参考 SEAL 的 header + compression 思路），可选压缩以降低带宽。

## 设计对照（相对 cryptoTools）

- 保留三层结构：`IOService -> Session -> Channel`。
- 保留 RAII 生命周期：对象析构会自动清理线程和 socket。
- 保留 `addChannel()` 风格：由会话创建通信通道。
- 简化内容：没有实现 cryptoTools 里的多路命名配对、TLS、复杂队列与自定义操作调度。
- 无需依赖 SEAL：序列化模块为独立实现。

## 协议

`sendString/sendVector/asyncSendBytes/asyncRecvBytes` 使用“长度前缀帧”协议：

- 8 字节大端长度头（`uint64_t`）
- 后接 payload

这样能支持变长消息，避免 TCP 粘包拆包问题。
可通过以下接口查看当前通道累计通信量统计（字节）：

- `Channel::sentBytes()`
- `Channel::receivedBytes()`
- `Channel::resetTrafficStats()`

说明：对帧协议接口（如 `sendString/recvString/asyncSendBytes/asyncRecvBytes`），统计的是链路上传输的字节数，即 `8` 字节长度头 + 序列化后 payload（启用压缩时为压缩后的大小）。

另外提供了 `Channel::sendU32Payload(...)`，用于按 `uint32_t` 元素发送 payload。
在 C++20 且标准库支持 `std::span` 时，可直接传 `std::span<const uint32_t>`。

## 独立序列化模块

`boost_tcp_net/Serialization` 提供：

- 16 字节头部（magic/version/mode/size）
- 压缩模式：`none`、`zlib`（若构建环境存在 zlib）
- `pack/unpack` 字节序列化
- `save/load` 回调式序列化接口

`Channel` 会在发送变长消息时自动调用该模块做打包/解包，并按通道压缩模式处理 payload。
可通过 `Channel::setCompressionMode(...)` 设置当前通道发送压缩策略。

## 异步接口

- `asyncSendBytes(payload, callback)`
- `asyncSendU32Payload(vector<uint32_t>, callback)`
- `asyncRecvBytes(callback)`
- `asyncSendString(text, callback)`
- `asyncRecvString(callback)`
- `asyncSendBytes(payload)` / `asyncRecvBytes()`
- `asyncSendU32Payload(vector<uint32_t>)`
- `asyncSendString(text)` / `asyncRecvString()`
- `asyncSendVector<T>(vector<T>)` / `asyncRecvVector<T>()`
- `Channel::makeLocalPair(nameA, nameB)`：创建一对本地直连通道，不经过 TCP。

## 本地直连通信

除了 TCP 通信之外，可以直接在同一进程内创建一对互联 `Channel`：

```cpp
auto pair = bnet::Channel::makeLocalPair("local_client", "local_server");
auto& localClient = pair.first;
auto& localServer = pair.second;

localClient.sendString("hello local");

std::string msg;
localServer.recvString(msg);
```

本地通道会复用和 TCP 相同的帧协议与压缩逻辑，因此
`sendString/sendVector/asyncSendBytes/asyncRecvBytes` 的行为保持一致。

## 第三方库准备

根据 `CMakeLists.txt`，依赖分为必需和可选两类：

- 必需：
  - CMake >= `3.16`
  - C++17 编译器（`g++`/`clang++`）
  - Boost `system` 组件（`find_package(Boost REQUIRED COMPONENTS system)`）
  - 线程库（`find_package(Threads REQUIRED)`，通常随系统工具链提供）
- 可选：
  - Zlib（`find_package(ZLIB QUIET)`），用于启用压缩模式

Linux (Ubuntu/Debian) 参考安装：

```bash
sudo apt update
sudo apt install -y cmake build-essential libboost-system-dev zlib1g-dev
```

macOS (Homebrew) 参考安装：

```bash
brew install cmake boost zlib
```

如果不需要压缩功能，可以不安装 zlib；库仍可正常构建和通信。
如果安装了 zlib，构建时会自动启用 `BNET_USE_ZLIB=1`。

## 快速构建

```bash
cd boost_tcp_net
cmake -S . -B build
cmake --build build -j
./build/tcp_demo
./build/local_demo
```
