#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace SPTAG::Helper {

class TiKVUdsClient {
public:
    explicit TiKVUdsClient(const std::string& udsPath);
    ~TiKVUdsClient();

    TiKVUdsClient(const TiKVUdsClient&) = delete;
    TiKVUdsClient& operator=(const TiKVUdsClient&) = delete;

    bool Available() const;
    void ShutDown();

    // status: 0 OK, 1 NotFound, 2 Timeout, 3 Error
    int32_t Health(const std::chrono::microseconds& timeout);

    int32_t GetU64(uint64_t key, std::string& value, const std::chrono::microseconds& timeout);
    int32_t MultiGetU64(const std::vector<uint64_t>& keys, std::vector<std::string>& values, const std::chrono::microseconds& timeout);
    int32_t PutU64(uint64_t key, const std::string& value, const std::chrono::microseconds& timeout);
    int32_t DeleteU64(uint64_t key, const std::chrono::microseconds& timeout);

private:
    bool EnsureConnected();
    bool SendAll(const void* buf, size_t len, const std::chrono::microseconds& timeout);
    bool RecvAll(void* buf, size_t len, const std::chrono::microseconds& timeout);

    int32_t ReadResponse(uint64_t expectReqId, std::vector<uint8_t>& payload, const std::chrono::microseconds& timeout);

private:
    std::string m_udsPath;
    int m_fd;
    uint64_t m_reqId;
};

} // namespace SPTAG::Helper
