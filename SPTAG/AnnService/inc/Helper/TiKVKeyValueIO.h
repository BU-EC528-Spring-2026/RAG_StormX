#pragma once

#include "inc/Helper/KeyValueIO.h"
#include "inc/Helper/TiKVUdsClient.h"
#include <mutex>
#include <unordered_map>
#include <list>

namespace SPTAG::Helper {

class TiKVKeyValueIO final : public KeyValueIO {
public:
    explicit TiKVKeyValueIO(const std::string& udsPath, size_t cacheBytes);
    ~TiKVKeyValueIO() override { ShutDown(); }

    void ShutDown() override;

    ErrorCode Get(const SizeType key,
                  std::string* value,
                  const std::chrono::microseconds& timeout,
                  std::vector<Helper::AsyncReadRequest>* reqs) override;

    ErrorCode Get(const SizeType key,
                  Helper::PageBuffer<std::uint8_t>& value,
                  const std::chrono::microseconds& timeout,
                  std::vector<Helper::AsyncReadRequest>* reqs,
                  bool useCache = true) override;

    ErrorCode MultiGet(const std::vector<SizeType>& keys,
                       std::vector<std::string>* values,
                       const std::chrono::microseconds& timeout,
                       std::vector<Helper::AsyncReadRequest>* reqs) override;

    ErrorCode Put(const SizeType key,
                  const std::string& value,
                  const std::chrono::microseconds& timeout,
                  std::vector<Helper::AsyncReadRequest>* reqs) override;

    ErrorCode Merge(const SizeType key,
                    const std::string& value,
                    const std::chrono::microseconds& timeout,
                    std::vector<Helper::AsyncReadRequest>* reqs,
                    std::function<bool(const void *val, const int size)> checksum) override;

    ErrorCode Delete(SizeType key) override;

    bool Available() override;

private:
    struct CacheEntry {
        SizeType key;
        std::string value;
        size_t size;
    };

    void CachePut(SizeType key, const std::string& value);
    bool CacheGet(SizeType key, std::string& value);

private:
    TiKVUdsClient m_client;
    size_t m_cacheCap;
    size_t m_cacheUsed;

    std::mutex m_mu;
    std::list<CacheEntry> m_lru;
    std::unordered_map<SizeType, std::list<CacheEntry>::iterator> m_map;
};

} // namespace SPTAG::Helper
