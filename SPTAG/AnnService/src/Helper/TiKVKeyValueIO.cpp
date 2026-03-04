#include "inc/Helper/TiKVKeyValueIO.h"
#include <fstream>
#include <mutex>
#include <string.h>

namespace SPTAG::Helper
{

static std::ofstream g_tikv_ops("tikv_operations.csv", std::ios::app);
static std::mutex g_tikv_mu;

inline void LogOp(const std::string &op)
{
    std::lock_guard<std::mutex> lock(g_tikv_mu);
    if (g_tikv_ops.is_open())
    {
        g_tikv_ops << op << "\n";
        g_tikv_ops.flush();
    }
}

TiKVKeyValueIO::TiKVKeyValueIO(const std::string &udsPath, size_t cacheBytes)
    : m_client(udsPath), m_cacheCap(cacheBytes), m_cacheUsed(0)
{
}

void TiKVKeyValueIO::ShutDown()
{
    std::lock_guard<std::mutex> g(m_mu);
    m_map.clear();
    m_lru.clear();
    m_cacheUsed = 0;
    // We do not shutdown the client since we don't connect.
}

bool TiKVKeyValueIO::Available()
{
    return true; // Always available
}

ErrorCode TiKVKeyValueIO::Checkpoint(std::string prefix)
{
    LogOp("CHECKPOINT, " + prefix);
    return ErrorCode::Success;
}

bool TiKVKeyValueIO::CacheGet(SizeType key, std::string &value)
{
    if (m_cacheCap == 0)
        return false;
    auto it = m_map.find(key);
    if (it == m_map.end())
        return false;
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    value = it->second->value;
    return true;
}

void TiKVKeyValueIO::CachePut(SizeType key, const std::string &value)
{
    if (m_cacheCap == 0)
        return;
    size_t sz = value.size();

    auto it = m_map.find(key);
    if (it != m_map.end())
    {
        m_cacheUsed -= it->second->size;
        it->second->value = value;
        it->second->size = sz;
        m_cacheUsed += sz;
        m_lru.splice(m_lru.begin(), m_lru, it->second);
    }
    else
    {
        CacheEntry e;
        e.key = key;
        e.value = value;
        e.size = sz;
        m_lru.push_front(std::move(e));
        m_map[key] = m_lru.begin();
        m_cacheUsed += sz;
    }

    while (m_cacheUsed > m_cacheCap && !m_lru.empty())
    {
        auto &back = m_lru.back();
        m_cacheUsed -= back.size;
        m_map.erase(back.key);
        m_lru.pop_back();
    }
}

ErrorCode TiKVKeyValueIO::Get(const SizeType key, std::string *value, const std::chrono::microseconds &timeout,
                              std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    LogOp("GET, " + std::to_string(key));
    if (value != nullptr)
    {
        *value = "dummy";
    }
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Get(const SizeType key, Helper::PageBuffer<std::uint8_t> &value,
                              const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest> *reqs,
                              bool useCache)
{
    LogOp("GET, " + std::to_string(key));
    value.ReservePageBuffer(8);
    value.SetAvailableSize(8);
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::MultiGet(const std::vector<SizeType> &keys, std::vector<std::string> *values,
                                   const std::chrono::microseconds &timeout,
                                   std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    std::string op = "MULTIGET";
    for (auto k : keys)
    {
        op += ", " + std::to_string(k);
    }
    LogOp(op);

    if (values != nullptr)
    {
        values->assign(keys.size(), "dummy");
    }
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::MultiGet(const std::vector<SizeType> &keys,
                                   std::vector<SPTAG::Helper::PageBuffer<std::uint8_t>> &values,
                                   const std::chrono::microseconds &timeout,
                                   std::vector<Helper::AsyncReadRequest> *reqs)
{
    std::string op = "MULTIGET";
    for (auto k : keys)
    {
        op += ", " + std::to_string(k);
    }
    LogOp(op);

    if (values.size() < keys.size())
    {
        values.resize(keys.size());
    }
    for (size_t i = 0; i < keys.size(); i++)
    {
        values[i].ReservePageBuffer(8);
        values[i].SetAvailableSize(8);
    }
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Put(const SizeType key, const std::string &value, const std::chrono::microseconds &timeout,
                              std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    LogOp("PUT, " + std::to_string(key) + ", " + std::to_string(value.size()));
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Merge(const SizeType key, const std::string &value, const std::chrono::microseconds &timeout,
                                std::vector<Helper::AsyncReadRequest> *reqs,
                                std::function<bool(const void *val, const int size)> checksum)
{
    LogOp("MERGE, " + std::to_string(key) + ", " + std::to_string(value.size()));
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Delete(SizeType key)
{
    LogOp("DELETE, " + std::to_string(key));
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Check(const SizeType key, int size, std::vector<std::uint8_t> *visited)
{
    return ErrorCode::Success;
}

} // namespace SPTAG::Helper
