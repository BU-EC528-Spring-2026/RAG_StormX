#include "inc/Helper/AerospikeKeyValueIO.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#ifdef AEROSPIKE
#include <aerospike/aerospike_batch.h>
#include <aerospike/aerospike_key.h>
#include <aerospike/as_batch.h>
#include <aerospike/as_bytes.h>
#include <aerospike/as_error.h>
#include <aerospike/as_key.h>
#include <aerospike/as_operations.h>
#include <aerospike/as_policy.h>
#include <aerospike/as_record.h>
#include <aerospike/as_status.h>
#endif

namespace SPTAG::Helper
{

#ifdef AEROSPIKE
namespace
{
struct BatchReadContext
{
    std::vector<std::string> *values;
    const std::string *valueBin;
    ErrorCode status;
};

bool BatchReadCallback(const as_batch_read *results, uint32_t n, void *udata)
{
    auto *ctx = reinterpret_cast<BatchReadContext *>(udata);
    if (ctx == nullptr || ctx->values == nullptr || ctx->valueBin == nullptr)
    {
        return false;
    }

    if (ctx->values->size() < n)
    {
        ctx->values->resize(n);
    }

    for (uint32_t i = 0; i < n; ++i)
    {
        if (results[i].result != AEROSPIKE_OK)
        {
            ctx->status = ErrorCode::Fail;
            return false;
        }

        as_bytes *bytes = as_record_get_bytes(&results[i].record, ctx->valueBin->c_str());
        if (bytes == nullptr)
        {
            ctx->status = ErrorCode::Fail;
            return false;
        }

        const uint8_t *raw = as_bytes_get(bytes);
        if (raw == nullptr && bytes->size > 0)
        {
            ctx->status = ErrorCode::Fail;
            return false;
        }
        (*ctx->values)[i].assign(reinterpret_cast<const char *>(raw), bytes->size);
    }

    ctx->status = ErrorCode::Success;
    return true;
}
} // namespace
#endif

AerospikeKeyValueIO::AerospikeKeyValueIO(const std::string &host, uint16_t port, const std::string &nameSpace,
                                         const std::string &setName, const std::string &valueBin)
    :
#ifdef AEROSPIKE
      m_host(host), m_port(port), m_namespace(nameSpace), m_setName(setName), m_valueBin(valueBin), m_connected(false)
#else
      m_connected(false)
#endif
{
#ifdef AEROSPIKE
    as_config_init(&m_config);
    as_config_add_host(&m_config, m_host.c_str(), m_port);
    aerospike_init(&m_as, &m_config);

    as_error err;
    if (aerospike_connect(&m_as, &err) == AEROSPIKE_OK)
    {
        m_connected = true;
    }
    else
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Aerospike connect failed: code=%d message=%s\n", err.code,
                     err.message);
    }
#endif
}

AerospikeKeyValueIO::~AerospikeKeyValueIO()
{
    ShutDown();
}

void AerospikeKeyValueIO::ShutDown()
{
#ifdef AEROSPIKE
    if (!m_connected)
        return;

    as_error err;
    aerospike_close(&m_as, &err);
    aerospike_destroy(&m_as);
    m_connected = false;
#else
    m_connected = false;
#endif
}

bool AerospikeKeyValueIO::Available()
{
    return m_connected;
}

ErrorCode AerospikeKeyValueIO::Checkpoint(std::string /*prefix*/)
{
    // Aerospike persists server-side; no explicit client checkpoint required.
    return ErrorCode::Success;
}

std::chrono::milliseconds AerospikeKeyValueIO::ToMilliseconds(const std::chrono::microseconds &timeout) const
{
    if (timeout == MaxTimeout)
    {
        return std::chrono::milliseconds(0);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() <= 0)
    {
        return std::chrono::milliseconds(1);
    }
    if (ms.count() > std::numeric_limits<uint32_t>::max())
    {
        return std::chrono::milliseconds(std::numeric_limits<uint32_t>::max());
    }
    return ms;
}

ErrorCode AerospikeKeyValueIO::Get(const SizeType key, std::string *value, const std::chrono::microseconds &timeout,
                                   std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    if (value == nullptr || !m_connected)
    {
        return ErrorCode::Fail;
    }

#ifdef AEROSPIKE
    as_error err;
    as_policy_read policy;
    as_policy_read_init(&policy);
    policy.base.total_timeout = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    policy.base.socket_timeout = policy.base.total_timeout;

    as_key akey;
    as_key_init_int64(&akey, m_namespace.c_str(), m_setName.c_str(), static_cast<int64_t>(key));

    as_record *record = nullptr;
    as_status status = aerospike_key_get(&m_as, &err, &policy, &akey, &record);
    as_key_destroy(&akey);

    if (status != AEROSPIKE_OK || record == nullptr)
    {
        if (record != nullptr)
            as_record_destroy(record);
        return ErrorCode::Fail;
    }

    as_bytes *bytes = as_record_get_bytes(record, m_valueBin.c_str());
    if (bytes == nullptr)
    {
        as_record_destroy(record);
        return ErrorCode::Fail;
    }

    const uint8_t *raw = as_bytes_get(bytes);
    if (raw == nullptr && bytes->size > 0)
    {
        as_record_destroy(record);
        return ErrorCode::Fail;
    }

    value->assign(reinterpret_cast<const char *>(raw), bytes->size);
    as_record_destroy(record);
    return ErrorCode::Success;
#else
    (void)key;
    (void)timeout;
    return ErrorCode::Fail;
#endif
}

ErrorCode AerospikeKeyValueIO::MultiGet(const std::vector<SizeType> &keys, std::vector<std::string> *values,
                                        const std::chrono::microseconds &timeout,
                                        std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    if (values == nullptr || !m_connected)
    {
        return ErrorCode::Fail;
    }

    values->clear();
    values->resize(keys.size());
    if (keys.empty())
    {
        return ErrorCode::Success;
    }

#ifdef AEROSPIKE
    as_batch batch;
    as_batch_inita(&batch, static_cast<uint32_t>(keys.size()));
    for (uint32_t i = 0; i < keys.size(); ++i)
    {
        as_key_init_int64(as_batch_keyat(&batch, i), m_namespace.c_str(), m_setName.c_str(), static_cast<int64_t>(keys[i]));
    }

    as_error err;
    as_policy_batch policy;
    as_policy_batch_init(&policy);
    policy.base.total_timeout = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    policy.base.socket_timeout = policy.base.total_timeout;

    BatchReadContext ctx{values, &m_valueBin, ErrorCode::Success};
    as_status status = aerospike_batch_get(&m_as, &err, &policy, &batch, BatchReadCallback, &ctx);
    as_batch_destroy(&batch);
    if (status != AEROSPIKE_OK || ctx.status != ErrorCode::Success)
    {
        return ErrorCode::Fail;
    }
    return ErrorCode::Success;
#else
    (void)keys;
    (void)timeout;
    return ErrorCode::Fail;
#endif
}

ErrorCode AerospikeKeyValueIO::Get(const SizeType key, Helper::PageBuffer<std::uint8_t> &value,
                                   const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest> *reqs,
                                   bool /*useCache*/)
{
    std::string posting;
    auto ret = Get(key, &posting, timeout, reqs);
    if (ret != ErrorCode::Success)
    {
        return ret;
    }

    value.ReservePageBuffer(posting.size());
    if (!posting.empty())
    {
        std::memcpy(value.GetBuffer(), posting.data(), posting.size());
    }
    value.SetAvailableSize(posting.size());
    return ErrorCode::Success;
}

ErrorCode AerospikeKeyValueIO::MultiGet(const std::vector<SizeType> &keys,
                                        std::vector<SPTAG::Helper::PageBuffer<std::uint8_t>> &values,
                                        const std::chrono::microseconds &timeout,
                                        std::vector<Helper::AsyncReadRequest> *reqs)
{
    std::vector<std::string> results;
    auto ret = MultiGet(keys, &results, timeout, reqs);
    if (ret != ErrorCode::Success)
    {
        return ret;
    }

    if (values.size() < results.size())
    {
        values.resize(results.size());
    }
    for (size_t i = 0; i < results.size(); ++i)
    {
        values[i].ReservePageBuffer(results[i].size());
        if (!results[i].empty())
        {
            std::memcpy(values[i].GetBuffer(), results[i].data(), results[i].size());
        }
        values[i].SetAvailableSize(results[i].size());
    }
    return ErrorCode::Success;
}

ErrorCode AerospikeKeyValueIO::Put(const SizeType key, const std::string &value,
                                   const std::chrono::microseconds &timeout,
                                   std::vector<Helper::AsyncReadRequest> * /*reqs*/)
{
    if (!m_connected)
    {
        return ErrorCode::Fail;
    }
#ifdef AEROSPIKE
    return PutRaw(key, reinterpret_cast<const uint8_t *>(value.data()), static_cast<uint32_t>(value.size()), timeout);
#else
    (void)key;
    (void)value;
    (void)timeout;
    return ErrorCode::Fail;
#endif
}

ErrorCode AerospikeKeyValueIO::Merge(const SizeType key, const std::string &value,
                                     const std::chrono::microseconds &timeout,
                                     std::vector<Helper::AsyncReadRequest> * /*reqs*/,
                                     std::function<bool(const void *val, const int size)> /*checksum*/)
{
    if (!m_connected)
    {
        return ErrorCode::Fail;
    }

    if (value.empty())
    {
        return ErrorCode::Success;
    }

#ifdef AEROSPIKE
    as_error err;
    as_policy_operate policy;
    as_policy_operate_init(&policy);
    policy.base.total_timeout = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    policy.base.socket_timeout = policy.base.total_timeout;

    as_key akey;
    as_key_init_int64(&akey, m_namespace.c_str(), m_setName.c_str(), static_cast<int64_t>(key));

    as_operations ops;
    as_operations_inita(&ops, 1);
    as_operations_add_append_raw(&ops, m_valueBin.c_str(), reinterpret_cast<const uint8_t *>(value.data()),
                                 static_cast<uint32_t>(value.size()));

    as_record *rec = nullptr;
    as_status status = aerospike_key_operate(&m_as, &err, &policy, &akey, &ops, &rec);
    as_operations_destroy(&ops);
    as_key_destroy(&akey);
    if (rec != nullptr)
        as_record_destroy(rec);

    if (status == AEROSPIKE_OK)
    {
        return ErrorCode::Success;
    }

    // Append can fail on missing records. Fallback to put creates the record with appended bytes.
    if (err.code == AEROSPIKE_ERR_RECORD_NOT_FOUND)
    {
        return Put(key, value, timeout, nullptr);
    }
    return ErrorCode::Fail;
#else
    (void)key;
    (void)value;
    (void)timeout;
    return ErrorCode::Fail;
#endif
}

ErrorCode AerospikeKeyValueIO::Delete(SizeType key)
{
    if (!m_connected)
    {
        return ErrorCode::Fail;
    }

#ifdef AEROSPIKE
    as_error err;
    as_key akey;
    as_key_init_int64(&akey, m_namespace.c_str(), m_setName.c_str(), static_cast<int64_t>(key));

    as_status status = aerospike_key_remove(&m_as, &err, nullptr, &akey);
    as_key_destroy(&akey);
    if (status == AEROSPIKE_OK || err.code == AEROSPIKE_ERR_RECORD_NOT_FOUND)
    {
        return ErrorCode::Success;
    }
    return ErrorCode::Fail;
#else
    (void)key;
    return ErrorCode::Fail;
#endif
}

#ifdef AEROSPIKE
ErrorCode AerospikeKeyValueIO::PutRaw(const SizeType key, const uint8_t *value, uint32_t valueSize,
                                      const std::chrono::microseconds &timeout)
{
    as_error err;
    as_policy_write policy;
    as_policy_write_init(&policy);
    policy.base.total_timeout = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    policy.base.socket_timeout = policy.base.total_timeout;

    as_key akey;
    as_key_init_int64(&akey, m_namespace.c_str(), m_setName.c_str(), static_cast<int64_t>(key));

    std::unique_ptr<uint8_t[]> blob(new uint8_t[valueSize]);
    if (valueSize > 0)
    {
        std::memcpy(blob.get(), value, valueSize);
    }

    as_record rec;
    as_record_inita(&rec, 1);
    as_record_set_rawp(&rec, m_valueBin.c_str(), blob.release(), valueSize, true);

    as_status status = aerospike_key_put(&m_as, &err, &policy, &akey, &rec);
    as_record_destroy(&rec);
    as_key_destroy(&akey);
    return status == AEROSPIKE_OK ? ErrorCode::Success : ErrorCode::Fail;
}
#endif

} // namespace SPTAG::Helper
