#include "inc/Helper/TiKVKeyValueIO.h"
#include <string.h>

namespace SPTAG::Helper
{

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
    m_client.ShutDown();
}

bool TiKVKeyValueIO::Available()
{
    // Health check keeps failure modes explicit
    if (!m_client.Available())
    {
        return m_client.Health(std::chrono::microseconds(2000000)) == 0;
    }
    return true;
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
    if (value == nullptr)
        return ErrorCode::Undefined;

    {
        std::lock_guard<std::mutex> g(m_mu);
        if (CacheGet(key, *value))
            return ErrorCode::Success;
    }

    std::string v;
    int32_t st = m_client.GetU64((uint64_t)key, v, timeout);
    if (st != 0)
        return ErrorCode::Undefined;

    {
        std::lock_guard<std::mutex> g(m_mu);
        CachePut(key, v);
    }
    *value = std::move(v);
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Get(const SizeType key, Helper::PageBuffer<std::uint8_t> &value,
                              const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest> *reqs,
                              bool useCache)
{
    std::string s;
    if (useCache)
    {
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (CacheGet(key, s))
            {
                value.ReservePageBuffer(s.size());
                memcpy(value.GetBuffer(), s.data(), s.size());
                value.SetAvailableSize(s.size());
                return ErrorCode::Success;
            }
        }
    }

    ErrorCode ec = Get(key, &s, timeout, reqs);
    if (ec != ErrorCode::Success)
        return ec;

    value.ReservePageBuffer(s.size());
    memcpy(value.GetBuffer(), s.data(), s.size());
    value.SetAvailableSize(s.size());
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::MultiGet(const std::vector<SizeType> &keys, std::vector<std::string> *values,
                                   const std::chrono::microseconds &timeout,
                                   std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    if (values == nullptr)
        return ErrorCode::Undefined;
    values->clear();
    values->resize(keys.size());

    std::vector<uint64_t> missKeys;
    missKeys.reserve(keys.size());
    std::vector<size_t> missIdx;
    missIdx.reserve(keys.size());

    // First pass: cache hits
    {
        std::lock_guard<std::mutex> g(m_mu);
        for (size_t i = 0; i < keys.size(); i++)
        {
            std::string v;
            if (CacheGet(keys[i], v))
            {
                (*values)[i] = std::move(v);
            }
            else
            {
                missKeys.push_back((uint64_t)keys[i]);
                missIdx.push_back(i);
            }
        }
    }

    if (missKeys.empty())
        return ErrorCode::Success;

    std::vector<std::string> missVals;
    int32_t st = m_client.MultiGetU64(missKeys, missVals, timeout);
    if (st != 0)
        return ErrorCode::Undefined;
    if (missVals.size() != missKeys.size())
        return ErrorCode::Undefined;

    {
        std::lock_guard<std::mutex> g(m_mu);
        for (size_t j = 0; j < missKeys.size(); j++)
        {
            size_t i = missIdx[j];
            (*values)[i] = missVals[j];
            CachePut(keys[i], missVals[j]);
        }
    }

    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::MultiGet(const std::vector<SizeType> &keys,
                                   std::vector<SPTAG::Helper::PageBuffer<std::uint8_t>> &values,
                                   const std::chrono::microseconds &timeout,
                                   std::vector<Helper::AsyncReadRequest> *reqs)
{
    std::vector<std::string> strValues;
    ErrorCode ec = MultiGet(keys, &strValues, timeout, reqs);
    if (ec != ErrorCode::Success)
        return ec;

    if (values.size() < keys.size())
    {
        values.resize(keys.size());
    }

    for (size_t i = 0; i < keys.size(); i++)
    {
        values[i].ReservePageBuffer(strValues[i].size());
        memcpy(values[i].GetBuffer(), strValues[i].data(), strValues[i].size());
        values[i].SetAvailableSize(strValues[i].size());
    }
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Put(const SizeType key, const std::string &value, const std::chrono::microseconds &timeout,
                              std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    int32_t st = m_client.PutU64((uint64_t)key, value, timeout);
    if (st != 0)
        return ErrorCode::Undefined;

    {
        std::lock_guard<std::mutex> g(m_mu);
        CachePut(key, value);
    }
    return ErrorCode::Success;
}

ErrorCode TiKVKeyValueIO::Merge(const SizeType key, const std::string &value, const std::chrono::microseconds &timeout,
                                std::vector<Helper::AsyncReadRequest> *reqs,
                                std::function<bool(const void *val, const int size)> checksum)
{
    // Implement "merge" as: read existing -> validate checksum -> overwrite with new value.
    // This keeps correctness in C++ (checksum callback cannot run in Go).
    std::string oldv;
    ErrorCode ec = Get(key, &oldv, timeout, reqs);
    if (ec != ErrorCode::Success)
        return ec;

    if (checksum)
    {
        if (!checksum(oldv.data(), (int)oldv.size()))
            return ErrorCode::Undefined;
    }
    return Put(key, value, timeout, reqs);
}

ErrorCode TiKVKeyValueIO::Delete(SizeType key)
{
    int32_t st = m_client.DeleteU64((uint64_t)key, std::chrono::microseconds(2000000));
    if (st != 0)
        return ErrorCode::Undefined;

    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_map.find(key);
        if (it != m_map.end())
        {
            m_cacheUsed -= it->second->size;
            m_lru.erase(it->second);
            m_map.erase(it);
        }
    }
    return ErrorCode::Success;
}

} // namespace SPTAG::Helper
