#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SPTAG
{
namespace Helper
{

class TiKVUdsClient
{
  public:
    explicit TiKVUdsClient(const std::string &udsPath);
    ~TiKVUdsClient();

    TiKVUdsClient(const TiKVUdsClient &) = delete;
    TiKVUdsClient &operator=(const TiKVUdsClient &) = delete;

    bool Available() const;
    void ShutDown();

    // status: 0 OK, 1 NotFound, 2 Timeout, 3 Error
    int32_t Health(const std::chrono::microseconds &timeout);

    int32_t GetU64(uint64_t key, std::string &value, const std::chrono::microseconds &timeout);
    int32_t MultiGetU64(const std::vector<uint64_t> &keys, std::vector<std::string> &values,
                        const std::chrono::microseconds &timeout);
    int32_t PutU64(uint64_t key, const std::string &value, const std::chrono::microseconds &timeout);
    int32_t DeleteU64(uint64_t key, const std::chrono::microseconds &timeout);

  private:
    bool EnsureConnected();
    void Disconnect();
    void SendThreadMain();
    void RecvThreadMain();

    struct ResponsePayload
    {
        int32_t status;
        std::vector<uint8_t> data;
    };
    int32_t SendRequestAndWait(uint64_t rid, const std::vector<uint8_t> &reqData, ResponsePayload &out,
                               const std::chrono::microseconds &timeout);
    bool SendAllBlocking(int fd, const void *buf, size_t len);
    bool RecvAllBlocking(int fd, void *buf, size_t len);

  private:
    std::string m_udsPath;
    std::atomic<int> m_fd{-1};
    std::atomic<uint64_t> m_reqId{1};

    struct QueuedRequest
    {
        std::vector<uint8_t> data;
    };
    std::queue<QueuedRequest> m_sendQueue;
    std::mutex m_sendMu;
    std::condition_variable m_sendCv;
    std::atomic<bool> m_stopping{false};

    std::unordered_map<uint64_t, std::shared_ptr<std::promise<ResponsePayload>>> m_pendingRequests;
    std::mutex m_pendingMu;

    std::thread m_sendThread;
    std::thread m_recvThread;
    std::mutex m_connMu;
};

} // namespace Helper
} // namespace SPTAG
