#include "inc/Helper/AerospikeKeyValueIO.h"
#include "inc/Helper/SptagPostingLuaEmbed.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#ifdef AEROSPIKE
#include <aerospike/aerospike_batch.h>
#include <aerospike/aerospike_key.h>
#include <aerospike/aerospike_udf.h>
#include <aerospike/as_arraylist.h>
#include <aerospike/as_batch.h>
#include <aerospike/as_bytes.h>
#include <aerospike/as_error.h>
#include <aerospike/as_key.h>
#include <aerospike/as_list.h>
#include <aerospike/as_nil.h>
#include <aerospike/as_operations.h>
#include <aerospike/as_policy.h>
#include <aerospike/as_record.h>
#include <aerospike/as_status.h>
#include <aerospike/as_udf.h>
#include <aerospike/as_val.h>
#endif

namespace SPTAG::Helper
{

#ifdef AEROSPIKE
namespace
{
bool IsAuthConfigSuccess(bool status)
{
    return status;
}

bool IsAuthConfigSuccess(as_status status)
{
    return status == AEROSPIKE_OK;
}

bool IsAuthConfigSuccess(int status)
{
    return status == static_cast<int>(AEROSPIKE_OK);
}

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

// ---------------------------------------------------------------------------
// Batch-apply UDF helpers
// ---------------------------------------------------------------------------

// Extract the UDF return bytes from a batch-apply result record.
// aerospike_batch_apply stores the Lua return value under the "SUCCESS" bin.
// If not found there (older client convention), fall back to the first bin.
static const as_bytes *ExtractUDFResultBytes(const as_record *rec)
{
    if (rec == nullptr)
        return nullptr;

    const as_bytes *b = as_record_get_bytes(rec, "SUCCESS");
    if (b != nullptr)
        return b;

    // Fallback: iterate bins for the first as_bytes value.
    for (uint16_t i = 0; i < rec->bins.size; ++i)
    {
        const as_bin *bin = &rec->bins.entries[i];
        if (bin->valuep != nullptr && as_val_type((const as_val *)bin->valuep) == AS_BYTES)
            return reinterpret_cast<const as_bytes *>(bin->valuep);
    }
    return nullptr;
}

// Context for the packed-mode batch-apply callback.
struct BatchApplyPackedContext
{
    std::vector<SPTAG::Helper::PageBuffer<std::uint8_t>> *values;
    ErrorCode status;
};

// Called once per batch with all per-key UDF results.
// Fills values[i] from the blob returned by nearest_candidates_read.
bool BatchApplyPackedCallback(const as_batch_read *results, uint32_t n, void *udata)
{
    auto *ctx = reinterpret_cast<BatchApplyPackedContext *>(udata);
    if (ctx == nullptr || ctx->values == nullptr)
        return false;

    if (ctx->values->size() < n)
        ctx->values->resize(n);

    bool any_fail = false;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (results[i].result != AEROSPIKE_OK)
        {
            // Partial failure: leave the slot empty and continue; caller decides.
            SPTAGLIB_LOG(Helper::LogLevel::LL_Warning,
                         "BatchApplyPackedCallback: UDF failed for slot %u: code=%d\n",
                         i, static_cast<int>(results[i].result));
            (*ctx->values)[i].SetAvailableSize(0);
            any_fail = true;
            continue;
        }

        const as_bytes *result_bytes = ExtractUDFResultBytes(&results[i].record);
        if (result_bytes == nullptr)
        {
            // UDF returned nil/empty — treat as empty posting (not a hard error).
            (*ctx->values)[i].SetAvailableSize(0);
            continue;
        }

        uint32_t sz = result_bytes->size;
        const uint8_t *raw = as_bytes_get(result_bytes);
        (*ctx->values)[i].ReservePageBuffer(sz);
        if (sz > 0 && raw != nullptr)
            std::memcpy((*ctx->values)[i].GetBuffer(), raw, sz);
        (*ctx->values)[i].SetAvailableSize(sz);
    }

    ctx->status = any_fail ? ErrorCode::Fail : ErrorCode::Success;
    return true;
}

// Context for the pairs-mode batch-apply callback.
struct BatchApplyPairsContext
{
    std::vector<std::vector<SPTAG::Helper::KeyValueIO::NearestPair>> *pairs;
    ErrorCode status;
};

// Called once per batch with all per-key UDF results.
// Parses the 8-byte (VID:int32_le, dist:float32_le) records from nearest_candidates_pairs.
bool BatchApplyPairsCallback(const as_batch_read *results, uint32_t n, void *udata)
{
    static_assert(sizeof(SPTAG::Helper::KeyValueIO::NearestPair) == 8,
                  "NearestPair layout must be exactly 8 bytes");

    auto *ctx = reinterpret_cast<BatchApplyPairsContext *>(udata);
    if (ctx == nullptr || ctx->pairs == nullptr)
        return false;

    if (ctx->pairs->size() < n)
        ctx->pairs->resize(n);

    bool any_fail = false;
    for (uint32_t i = 0; i < n; ++i)
    {
        (*ctx->pairs)[i].clear();

        if (results[i].result != AEROSPIKE_OK)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Warning,
                         "BatchApplyPairsCallback: UDF failed for slot %u: code=%d\n",
                         i, static_cast<int>(results[i].result));
            any_fail = true;
            continue;
        }

        const as_bytes *result_bytes = ExtractUDFResultBytes(&results[i].record);
        if (result_bytes == nullptr || result_bytes->size == 0)
            continue;

        const uint8_t *raw = as_bytes_get(result_bytes);
        uint32_t sz = result_bytes->size;
        if (raw == nullptr || sz % 8 != 0)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Warning,
                         "BatchApplyPairsCallback: unexpected blob size %u for slot %u (expected multiple of 8)\n",
                         sz, i);
            any_fail = true;
            continue;
        }

        uint32_t pair_count = sz / 8;
        (*ctx->pairs)[i].resize(pair_count);
        // Each pair is laid out as [VID:int32_le | dist:float32_le].
        // memcpy is safe because NearestPair is a trivial struct with matching layout.
        std::memcpy((*ctx->pairs)[i].data(), raw, sz);
    }

    ctx->status = any_fail ? ErrorCode::Fail : ErrorCode::Success;
    return true;
}

// Build and populate an as_arraylist with the UDF arguments shared by both UDF functions:
//   bin_name, query_blob, vector_info_size, meta_data_size, dimension,
//   value_type, top_n, dist_mode, deleted_bitset
//
// Ownership of the allocated as_bytes objects is transferred to the list; call
// as_arraylist_destroy() to release everything.
static void BuildUDFArglist(as_arraylist *args,
                            const std::string &bin_name,
                            const void *query_blob, uint32_t query_size,
                            uint32_t vector_info_size, uint32_t meta_data_size,
                            uint32_t dimension, uint8_t value_type,
                            uint32_t top_n, uint8_t dist_mode,
                            const void *deleted_bitset, uint32_t deleted_bitset_size)
{
    as_arraylist_append_str(args, bin_name.c_str());

    // query_blob as as_bytes (heap copy — arraylist takes ownership via as_val_destroy)
    as_bytes *qb = as_bytes_new(query_size);
    if (query_size > 0 && query_blob != nullptr)
        as_bytes_set(qb, 0, reinterpret_cast<const uint8_t *>(query_blob), query_size);
    as_arraylist_append_bytes(args, qb);

    as_arraylist_append_int64(args, static_cast<int64_t>(vector_info_size));
    as_arraylist_append_int64(args, static_cast<int64_t>(meta_data_size));
    as_arraylist_append_int64(args, static_cast<int64_t>(dimension));
    as_arraylist_append_int64(args, static_cast<int64_t>(value_type));
    as_arraylist_append_int64(args, static_cast<int64_t>(top_n));
    as_arraylist_append_int64(args, static_cast<int64_t>(dist_mode));

    // deleted_bitset: always pass as bytes (never nil) so Lua can safely call bytes.size().
    as_bytes *db = as_bytes_new(deleted_bitset_size);
    if (deleted_bitset_size > 0 && deleted_bitset != nullptr)
        as_bytes_set(db, 0, reinterpret_cast<const uint8_t *>(deleted_bitset), deleted_bitset_size);
    as_arraylist_append_bytes(args, db);
}

} // namespace
#endif

AerospikeKeyValueIO::AerospikeKeyValueIO(const std::string &host, uint16_t port, const std::string &nameSpace,
                                         const std::string &setName, const std::string &valueBin,
                                         const std::string &user, const std::string &password)
    :
#ifdef AEROSPIKE
      m_host(host), m_port(port), m_namespace(nameSpace), m_setName(setName), m_valueBin(valueBin), m_user(user),
      m_password(password), m_connected(false)
#else
      m_connected(false)
#endif
{
#ifdef AEROSPIKE
    as_config_init(&m_config);
    as_config_add_host(&m_config, m_host.c_str(), m_port);

    if (!m_user.empty())
    {
        auto authStatus = as_config_set_user(&m_config, m_user.c_str(), m_password.c_str());
        if (!IsAuthConfigSuccess(authStatus))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Aerospike auth config failed: user=%s\n",
                         m_user.c_str());
            fprintf(stderr, "Aerospike auth config failed: host=%s port=%u user=%s\n",
                    m_host.c_str(), m_port, m_user.c_str());
        }
    }

    aerospike_init(&m_as, &m_config);

    as_error err{};
    if (aerospike_connect(&m_as, &err) == AEROSPIKE_OK)
    {
        m_connected = true;

        // Idempotently register the sptag_posting Lua UDF module so that
        // nearest_candidates_read / nearest_candidates_pairs are available
        // without a separate out-of-band deployment step.
        as_bytes lua_content;
        as_bytes_init_wrap(&lua_content,
                           const_cast<uint8_t *>(
                               reinterpret_cast<const uint8_t *>(kSptagPostingLua)),
                           static_cast<uint32_t>(kSptagPostingLuaSize),
                           false /* don't free static storage */);
        as_error udf_err{};
        as_status udf_status = aerospike_udf_put(&m_as, &udf_err, nullptr,
                                                  "sptag_posting.lua",
                                                  AS_UDF_TYPE_LUA, &lua_content);
        if (udf_status != AEROSPIKE_OK)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Warning,
                         "Aerospike UDF register failed (sptag_posting.lua): host=%s port=%u code=%d message=%s\n",
                         m_host.c_str(), m_port, udf_err.code, udf_err.message);
            fprintf(stderr,
                    "Aerospike UDF register failed (sptag_posting.lua): host=%s port=%u code=%d message=%s\n",
                    m_host.c_str(), m_port, udf_err.code, udf_err.message);
        }
        else
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "Aerospike UDF sptag_posting.lua registered: host=%s port=%u\n",
                         m_host.c_str(), m_port);
        }
    }
    else
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike connect failed: host=%s port=%u namespace=%s set=%s code=%d message=%s\n",
                     m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(), err.code, err.message);
        fprintf(stderr, "Aerospike connect failed: host=%s port=%u namespace=%s set=%s user=%s code=%d message=%s\n",
                m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                m_user.empty() ? "(none)" : m_user.c_str(), err.code, err.message);
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
    as_error err{};
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
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike Get failed: key=%lld host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                     static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                     m_valueBin.c_str(), status, err.code, err.message);
        fprintf(stderr,
                "Aerospike Get failed: key=%lld host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                m_valueBin.c_str(), status, err.code, err.message);
        return ErrorCode::Fail;
    }

    as_bytes *bytes = as_record_get_bytes(record, m_valueBin.c_str());
    if (bytes == nullptr)
    {
        as_record_destroy(record);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike Get failed: key=%lld missing bin '%s' in namespace=%s set=%s\n",
                     static_cast<long long>(key), m_valueBin.c_str(), m_namespace.c_str(), m_setName.c_str());
        fprintf(stderr, "Aerospike Get failed: key=%lld missing bin '%s' in namespace=%s set=%s\n",
                static_cast<long long>(key), m_valueBin.c_str(), m_namespace.c_str(), m_setName.c_str());
        return ErrorCode::Fail;
    }

    const uint8_t *raw = as_bytes_get(bytes);
    if (raw == nullptr && bytes->size > 0)
    {
        as_record_destroy(record);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike Get failed: key=%lld invalid bytes for bin '%s' size=%u\n",
                     static_cast<long long>(key), m_valueBin.c_str(), bytes->size);
        fprintf(stderr, "Aerospike Get failed: key=%lld invalid bytes for bin '%s' size=%u\n",
                static_cast<long long>(key), m_valueBin.c_str(), bytes->size);
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

    as_error err{};
    as_policy_batch policy;
    as_policy_batch_init(&policy);
    policy.base.total_timeout = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    policy.base.socket_timeout = policy.base.total_timeout;

    BatchReadContext ctx{values, &m_valueBin, ErrorCode::Success};
    as_status status = aerospike_batch_get(&m_as, &err, &policy, &batch, BatchReadCallback, &ctx);
    as_batch_destroy(&batch);
    if (status != AEROSPIKE_OK || ctx.status != ErrorCode::Success)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike MultiGet failed: keys=%u host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                     static_cast<uint32_t>(keys.size()), m_host.c_str(), m_port, m_namespace.c_str(),
                     m_setName.c_str(), m_valueBin.c_str(), status, err.code, err.message);
        fprintf(stderr,
                "Aerospike MultiGet failed: keys=%u host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                static_cast<uint32_t>(keys.size()), m_host.c_str(), m_port, m_namespace.c_str(),
                m_setName.c_str(), m_valueBin.c_str(), status, err.code, err.message);
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
    as_error err{};
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
        auto putRet = Put(key, value, timeout, nullptr);
        if (putRet != ErrorCode::Success)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Aerospike Merge fallback Put failed: key=%lld host=%s port=%u namespace=%s set=%s bin=%s\n",
                         static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                         m_valueBin.c_str());
            fprintf(stderr,
                    "Aerospike Merge fallback Put failed: key=%lld host=%s port=%u namespace=%s set=%s bin=%s\n",
                    static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                    m_valueBin.c_str());
        }
        return putRet;
    }
    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                 "Aerospike Merge failed: key=%lld host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                 static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                 m_valueBin.c_str(), status, err.code, err.message);
    fprintf(stderr,
            "Aerospike Merge failed: key=%lld host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
            static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
            m_valueBin.c_str(), status, err.code, err.message);
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
    as_error err{};
    as_key akey;
    as_key_init_int64(&akey, m_namespace.c_str(), m_setName.c_str(), static_cast<int64_t>(key));

    as_status status = aerospike_key_remove(&m_as, &err, nullptr, &akey);
    as_key_destroy(&akey);
    if (status == AEROSPIKE_OK || err.code == AEROSPIKE_ERR_RECORD_NOT_FOUND)
    {
        return ErrorCode::Success;
    }
    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                 "Aerospike Delete failed: key=%lld host=%s port=%u namespace=%s set=%s status=%d code=%d message=%s\n",
                 static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(),
                 status, err.code, err.message);
    fprintf(stderr,
            "Aerospike Delete failed: key=%lld host=%s port=%u namespace=%s set=%s status=%d code=%d message=%s\n",
            static_cast<long long>(key), m_host.c_str(), m_port, m_namespace.c_str(), m_setName.c_str(), status,
            err.code, err.message);
    return ErrorCode::Fail;
#else
    (void)key;
    return ErrorCode::Fail;
#endif
}

// ---------------------------------------------------------------------------
// MultiGetNearest  —  packed UDF mode
// ---------------------------------------------------------------------------
ErrorCode AerospikeKeyValueIO::MultiGetNearest(
    const std::vector<SizeType> &keys,
    const void *query_blob, uint32_t query_size,
    uint32_t vector_info_size, uint32_t meta_data_size,
    uint32_t dimension, uint8_t value_type,
    uint32_t top_n, uint8_t dist_mode,
    const void *deleted_bitset, uint32_t deleted_bitset_size,
    std::vector<PageBuffer<std::uint8_t>> &values,
    const std::chrono::microseconds &timeout)
{
    if (!m_connected)
        return ErrorCode::Fail;

    if (keys.empty())
    {
        values.clear();
        return ErrorCode::Success;
    }

#ifdef AEROSPIKE
    // Pre-size output so the callback can index by slot.
    if (values.size() < keys.size())
        values.resize(keys.size());

    // Build batch: one key per posting.
    as_batch batch;
    as_batch_inita(&batch, static_cast<uint32_t>(keys.size()));
    for (uint32_t i = 0; i < static_cast<uint32_t>(keys.size()); ++i)
        as_key_init_int64(as_batch_keyat(&batch, i), m_namespace.c_str(), m_setName.c_str(),
                          static_cast<int64_t>(keys[i]));

    // UDF argument list — same for every key in the batch.
    as_arraylist args;
    as_arraylist_inita(&args, 9);
    BuildUDFArglist(&args, m_valueBin,
                    query_blob, query_size,
                    vector_info_size, meta_data_size,
                    dimension, value_type,
                    top_n, dist_mode,
                    deleted_bitset, deleted_bitset_size);

    as_error err{};
    as_policy_batch batch_policy;
    as_policy_batch_init(&batch_policy);
    uint32_t timeout_ms = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    batch_policy.base.total_timeout = timeout_ms;
    batch_policy.base.socket_timeout = timeout_ms;

    as_policy_batch_apply apply_policy;
    as_policy_batch_apply_init(&apply_policy);

    BatchApplyPackedContext ctx{&values, ErrorCode::Success};
    as_status status = aerospike_batch_apply(
        &m_as, &err, &batch_policy, &apply_policy, &batch,
        "sptag_posting", "nearest_candidates_read",
        reinterpret_cast<as_list *>(&args),
        BatchApplyPackedCallback, &ctx);

    as_arraylist_destroy(&args);
    as_batch_destroy(&batch);

    if (status != AEROSPIKE_OK)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike MultiGetNearest (batch_apply) failed: keys=%u host=%s port=%u "
                     "namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                     static_cast<uint32_t>(keys.size()), m_host.c_str(), m_port,
                     m_namespace.c_str(), m_setName.c_str(), m_valueBin.c_str(),
                     status, err.code, err.message);
        fprintf(stderr,
                "Aerospike MultiGetNearest (batch_apply) failed: keys=%u host=%s port=%u "
                "namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                static_cast<uint32_t>(keys.size()), m_host.c_str(), m_port,
                m_namespace.c_str(), m_setName.c_str(), m_valueBin.c_str(),
                status, err.code, err.message);
        return ErrorCode::Fail;
    }
    return ctx.status;
#else
    (void)keys; (void)query_blob; (void)query_size;
    (void)vector_info_size; (void)meta_data_size;
    (void)dimension; (void)value_type;
    (void)top_n; (void)dist_mode;
    (void)deleted_bitset; (void)deleted_bitset_size;
    (void)values; (void)timeout;
    return ErrorCode::Fail;
#endif
}

// ---------------------------------------------------------------------------
// MultiGetNearestPairs  —  pairs UDF mode
// ---------------------------------------------------------------------------
ErrorCode AerospikeKeyValueIO::MultiGetNearestPairs(
    const std::vector<SizeType> &keys,
    const void *query_blob, uint32_t query_size,
    uint32_t vector_info_size, uint32_t meta_data_size,
    uint32_t dimension, uint8_t value_type,
    uint32_t top_n, uint8_t dist_mode,
    const void *deleted_bitset, uint32_t deleted_bitset_size,
    std::vector<std::vector<NearestPair>> &pairs,
    const std::chrono::microseconds &timeout)
{
    if (!m_connected)
        return ErrorCode::Fail;

    if (keys.empty())
    {
        pairs.clear();
        return ErrorCode::Success;
    }

#ifdef AEROSPIKE
    if (pairs.size() < keys.size())
        pairs.resize(keys.size());

    as_batch batch;
    as_batch_inita(&batch, static_cast<uint32_t>(keys.size()));
    for (uint32_t i = 0; i < static_cast<uint32_t>(keys.size()); ++i)
        as_key_init_int64(as_batch_keyat(&batch, i), m_namespace.c_str(), m_setName.c_str(),
                          static_cast<int64_t>(keys[i]));

    as_arraylist args;
    as_arraylist_inita(&args, 9);
    BuildUDFArglist(&args, m_valueBin,
                    query_blob, query_size,
                    vector_info_size, meta_data_size,
                    dimension, value_type,
                    top_n, dist_mode,
                    deleted_bitset, deleted_bitset_size);

    as_error err{};
    as_policy_batch batch_policy;
    as_policy_batch_init(&batch_policy);
    uint32_t timeout_ms = static_cast<uint32_t>(ToMilliseconds(timeout).count());
    batch_policy.base.total_timeout = timeout_ms;
    batch_policy.base.socket_timeout = timeout_ms;

    as_policy_batch_apply apply_policy;
    as_policy_batch_apply_init(&apply_policy);

    BatchApplyPairsContext ctx{&pairs, ErrorCode::Success};
    as_status status = aerospike_batch_apply(
        &m_as, &err, &batch_policy, &apply_policy, &batch,
        "sptag_posting", "nearest_candidates_pairs",
        reinterpret_cast<as_list *>(&args),
        BatchApplyPairsCallback, &ctx);

    as_arraylist_destroy(&args);
    as_batch_destroy(&batch);

    if (status != AEROSPIKE_OK)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike MultiGetNearestPairs (batch_apply) failed: keys=%u host=%s port=%u "
                     "namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                     static_cast<uint32_t>(keys.size()), m_host.c_str(), m_port,
                     m_namespace.c_str(), m_setName.c_str(), m_valueBin.c_str(),
                     status, err.code, err.message);
        fprintf(stderr,
                "Aerospike MultiGetNearestPairs (batch_apply) failed: keys=%u host=%s port=%u "
                "namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                static_cast<uint32_t>(keys.size()), m_host.c_str(), m_port,
                m_namespace.c_str(), m_setName.c_str(), m_valueBin.c_str(),
                status, err.code, err.message);
        return ErrorCode::Fail;
    }
    return ctx.status;
#else
    (void)keys; (void)query_blob; (void)query_size;
    (void)vector_info_size; (void)meta_data_size;
    (void)dimension; (void)value_type;
    (void)top_n; (void)dist_mode;
    (void)deleted_bitset; (void)deleted_bitset_size;
    (void)pairs; (void)timeout;
    return ErrorCode::Fail;
#endif
}

#ifdef AEROSPIKE
ErrorCode AerospikeKeyValueIO::PutRaw(const SizeType key, const uint8_t *value, uint32_t valueSize,
                                      const std::chrono::microseconds &timeout)
{
    as_error err{};
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
    if (status != AEROSPIKE_OK)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                     "Aerospike Put failed: key=%lld size=%u host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                     static_cast<long long>(key), valueSize, m_host.c_str(), m_port, m_namespace.c_str(),
                     m_setName.c_str(), m_valueBin.c_str(), status, err.code, err.message);
        fprintf(stderr,
                "Aerospike Put failed: key=%lld size=%u host=%s port=%u namespace=%s set=%s bin=%s status=%d code=%d message=%s\n",
                static_cast<long long>(key), valueSize, m_host.c_str(), m_port, m_namespace.c_str(),
                m_setName.c_str(), m_valueBin.c_str(), status, err.code, err.message);
    }
    return status == AEROSPIKE_OK ? ErrorCode::Success : ErrorCode::Fail;
}
#endif

} // namespace SPTAG::Helper
