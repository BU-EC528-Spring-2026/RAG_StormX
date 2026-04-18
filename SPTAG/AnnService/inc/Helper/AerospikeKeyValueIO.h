#pragma once

#include "inc/Helper/KeyValueIO.h"

#include <string>
#include <vector>

#ifdef AEROSPIKE
#include <aerospike/aerospike.h>
#include <aerospike/as_config.h>
#endif

namespace SPTAG::Helper
{

class AerospikeKeyValueIO final : public KeyValueIO
{
  public:
    AerospikeKeyValueIO(const std::string &host, uint16_t port, const std::string &nameSpace,
                        const std::string &setName, const std::string &valueBin,
                        const std::string &user = "", const std::string &password = "");
    ~AerospikeKeyValueIO() override;

    void ShutDown() override;
    bool Available() override;

    ErrorCode Checkpoint(std::string prefix) override;

    ErrorCode Get(const SizeType key, std::string *value, const std::chrono::microseconds &timeout,
                  std::vector<Helper::AsyncReadRequest> *reqs) override;

    ErrorCode Get(const SizeType key, Helper::PageBuffer<std::uint8_t> &value, const std::chrono::microseconds &timeout,
                  std::vector<Helper::AsyncReadRequest> *reqs, bool useCache = true) override;

    ErrorCode MultiGet(const std::vector<SizeType> &keys, std::vector<std::string> *values,
                       const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest> *reqs) override;

    ErrorCode MultiGet(const std::vector<SizeType> &keys, std::vector<SPTAG::Helper::PageBuffer<std::uint8_t>> &values,
                       const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest> *reqs) override;

    // Packed UDF mode: single aerospike_batch_apply call applies nearest_candidates_read on all
    // posting keys; each per-key result is a concatenated blob of top-n vectorInfo records.
    ErrorCode MultiGetNearest(
        const std::vector<SizeType> &keys,
        const void *query_blob, uint32_t query_size,
        uint32_t vector_info_size, uint32_t meta_data_size,
        uint32_t dimension, uint8_t value_type,
        uint32_t top_n, uint8_t dist_mode,
        const void *deleted_bitset, uint32_t deleted_bitset_size,
        std::vector<PageBuffer<std::uint8_t>> &values,
        const std::chrono::microseconds &timeout) override;

    // Pairs UDF mode: single aerospike_batch_apply call applies nearest_candidates_pairs on all
    // posting keys; each per-key result is a blob of 8-byte (VID:int32_le, dist:float32) pairs.
    ErrorCode MultiGetNearestPairs(
        const std::vector<SizeType> &keys,
        const void *query_blob, uint32_t query_size,
        uint32_t vector_info_size, uint32_t meta_data_size,
        uint32_t dimension, uint8_t value_type,
        uint32_t top_n, uint8_t dist_mode,
        const void *deleted_bitset, uint32_t deleted_bitset_size,
        std::vector<std::vector<NearestPair>> &pairs,
        const std::chrono::microseconds &timeout) override;

    ErrorCode Put(const SizeType key, const std::string &value, const std::chrono::microseconds &timeout,
                  std::vector<Helper::AsyncReadRequest> *reqs) override;

    ErrorCode Merge(const SizeType key, const std::string &value, const std::chrono::microseconds &timeout,
                    std::vector<Helper::AsyncReadRequest> *reqs,
                    std::function<bool(const void *val, const int size)> checksum) override;

    ErrorCode Delete(SizeType key) override;

  private:
    std::chrono::milliseconds ToMilliseconds(const std::chrono::microseconds &timeout) const;

#ifdef AEROSPIKE
    ErrorCode PutRaw(const SizeType key, const uint8_t *value, uint32_t valueSize,
                     const std::chrono::microseconds &timeout);

    std::string m_host;
    uint16_t m_port;
    std::string m_namespace;
    std::string m_setName;
    std::string m_valueBin;
    std::string m_user;
    std::string m_password;

    bool m_connected;
    as_config m_config;
    aerospike m_as;
#else
    bool m_connected;
#endif
};

} // namespace SPTAG::Helper
