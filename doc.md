# boost_tcp_net 详细使用文档
## 1. 项目定位
`boost_tcp_net` 是一个轻量级 C++ 通信库，接口风格参考 `cryptoTools/Network`，提供统一的三层抽象：
- `IOService`：管理 `boost::asio::io_context` 与线程池
- `Session`：管理会话生命周期（Client / Server）
- `Channel`：提供具体收发能力（同步 + 异步）

在现有 TCP 通信能力基础上，新增了本地直连通信能力：

- 不经过 TCP socket

## 2. 功能总览

支持能力：

- 异步接口（future）：同名无回调重载，返回 `std::future`
- 统一帧协议：8 字节大端长度头 + payload
- 本地通道：`Channel::makeLocalPair(nameA, nameB)`
## 3. 构建与运行
```bash
cmake -S . -B build
cmake --build build -j
./build/tcp_demo
说明：
- tcp_demo 演示 TCP Client/Server + 本地通道
- local_demo 仅演示本地直连通信（不依赖 socket）



1. 创建 IOService
2. 创建 server/client Session
4. 使用 send/recv 或异步接口收发
关键点：

- client 端 SessionMode::Client 会主动连接

## 5. 快速开始：本地直连通信

auto& client = pair.first;


server.recvString(msg);
行为特性：
- 双向互联，client -> server 与 server -> client 均可用
- 无网络开销，适合同进程模块间高频通信


### 6.1 IOService
构造：
- IOService(std::size_t threadCount = 0)
语义：

- 析构自动 stop()，停止 io_context 并回收线程
常用方法：

- boost::asio::io_context& context()


构造与启动：

- Session(IOService&, host, port, mode, name = "")


- void stop()
- bool stopped() const

语义说明：
- server 模式下，addChannel 会阻塞等待 accept
- localName 为空时自动生成 _auto_xxx
- 若指定 remoteName，localName 不能留空

### 6.3 Channel
连接与状态：
- bool isConnected() const
- bool waitForConnection(timeout)
- void waitForConnection()
- void close()
同步收发：
- send(const void*, size_t) / recv(void*, size_t)
- send(const T&) / recv(T&)（T 必须 trivial）
- sendVector<T>(...) / recvVector<T>(...)
- sendString(...) / recvString(...)

异步收发（回调）：
- asyncSendBytes(payload, callback)
- asyncSendString(text, callback)

- asyncSendBytes(payload)
- asyncSendString(text)
- asyncSendVector<T>(...)

本地通道工厂：
- Channel::makeLocalPair(nameA, nameB)

- setCompressionMode(mode)
- compressionMode()

通信量统计：
- sentBytes()
- receivedBytes()
- resetTrafficStats()

- 对变长消息进行 pack/unpack
- 按通道配置决定压缩模式
压缩模式：

- zlib：压缩后传输体积更小，CPU 开销更高
## 7. 协议说明
所有 sendString/sendVector/asyncSendBytes 都是帧协议：

- 帧头：8 字节大端长度（uint64_t）
- 帧体：序列化后的 payload（可能已压缩）
- 统计口径：帧协议接口的通信量统计包含帧头 + 帧体（压缩时按压缩后字节计）
限制：

- 超限会抛出异常或异步返回错误码（message_size）
## 8. 并发与线程模型


- 避免同一通道上的并发读写竞争
本地通道：

- 内部使用互斥锁 + 条件变量实现阻塞读写


- 同一 Channel 可跨线程使用，但业务层仍应避免无序并发调用导致语义混乱
- 高并发时优先使用异步接口减少阻塞线程
## 9. 错误处理与关闭语义
错误来源：

- 网络错误（连接断开、解析失败、socket 错误）
- 使用错误（未初始化通道、模式不支持）
关闭语义：

- Channel::close() 幂等
- 关闭后同步接口抛 NetworkError
- 异步接口通过 error_code 返回 operation_canceled 等错误
建议：
- 回调接口中统一检查 ec
- future.get() 外围统一 try/catch

## 10. 最佳实践
- 小消息、低延迟：优先 none 压缩
- 大消息、带宽敏感：启用 zlib
- 跨进程/跨机通信：使用 Session + TCP Channel
- 保持 API 一致：业务层尽量只依赖 Channel 抽象，不感知底层是 TCP 还是本地通道

## 11. 常见问题

Q: 本地通道和 TCP 通道能否混用同一套业务代码？
A: 可以。收发 API 和帧语义保持一致，通常只需替换通道创建方式。

Q: 为什么 waitForConnection() 在本地通道上几乎立即成功？
A: 本地通道在创建时即完成配对，不需要网络握手。

Q: sendVector<T> 对 T 有什么要求？
A: 必须是 trivial 类型；否则字节拷贝语义不安全。

Q: 单次消息大小受什么限制？
A: 当前实现上限约 64 MiB，超过会报错。
