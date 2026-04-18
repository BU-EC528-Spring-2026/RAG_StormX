#ifndef _SPTAG_HELPER_KEYVALUEIO_H_
#define _SPTAG_HELPER_KEYVALUEIO_H_

#include "inc/Core/Common.h"
#include "inc/Helper/DiskIO.h"
#include <vector>
#include <chrono>
#include <cstdint>
#include <limits>

namespace SPTAG
{
    namespace Helper
    {
        class KeyValueIO {
        public:
            // One entry returned by MultiGetNearestPairs: a pre-scored (VID, distance) tuple
            // produced by the server-side nearest_candidates_pairs UDF. Only valid when no
            // PQ quantizer is active (caller must enforce this constraint).
            struct NearestPair {
                SizeType vid;
                float    dist;
            };

            // The pair-mode wire format is `[VID:int32_le | dist:float32_le]` per record,
            // written by sptag_posting.lua (`nearest_candidates_pairs`) using a bit-level
            // float->int32 packing that is bit-identical to IEEE-754 single precision.
            // The pairs callback in AerospikeKeyValueIO.cpp decodes the blob with a single
            // `memcpy` into `std::vector<NearestPair>`, which is safe only when:
            //   1. `NearestPair` is exactly 8 bytes (VID + float, no padding).
            //   2. `float` is IEEE-754 binary32 on this compiler/platform.
            //   3. The host is little-endian (same byte order Lua emits).
            // If any of these fail, we would silently hand garbage distances to AddPoint.
            static_assert(sizeof(NearestPair) == 8,
                          "NearestPair must be 8 bytes to match Lua pair wire format");
            static_assert(sizeof(SizeType) == 4,
                          "SizeType must be int32 to match Lua pair wire format");
            static_assert(sizeof(float) == 4,
                          "float must be 4 bytes to match Lua pair wire format");
            static_assert(std::numeric_limits<float>::is_iec559,
                          "float must be IEEE-754 binary32 (Lua writes IEEE bits via set_int32_le)");

            // The Lua UDF (sptag_posting.lua) dispatches on value_type with hard-coded
            // integer constants: Int8=0, UInt8=1, Float=3 (Int16=2 is defined but
            // not yet consumed server-side). These must match the C++ VectorValueType
            // enum order declared in Core/DefinitionList.h; otherwise Lua would
            // misread stored vector bytes (e.g. treat UInt8 bytes as Float) and
            // return corrupt distances.
            static_assert(static_cast<uint8_t>(VectorValueType::Int8)  == 0,
                          "VectorValueType::Int8 must be 0 to match Lua read_component");
            static_assert(static_cast<uint8_t>(VectorValueType::UInt8) == 1,
                          "VectorValueType::UInt8 must be 1 to match Lua read_component");
            static_assert(static_cast<uint8_t>(VectorValueType::Int16) == 2,
                          "VectorValueType::Int16 must be 2 to match Lua read_component");
            static_assert(static_cast<uint8_t>(VectorValueType::Float) == 3,
                          "VectorValueType::Float must be 3 to match Lua read_component");

            KeyValueIO() {}

            virtual ~KeyValueIO() {}

            virtual void ShutDown() = 0;

            virtual ErrorCode Get(const std::string& key, std::string* value, const std::chrono::microseconds& timeout, std::vector<Helper::AsyncReadRequest>* reqs) { return ErrorCode::Undefined; }

            virtual ErrorCode Get(const SizeType key, std::string* value, const std::chrono::microseconds& timeout, std::vector<Helper::AsyncReadRequest>* reqs) = 0;

            virtual ErrorCode Get(const SizeType key, Helper::PageBuffer<std::uint8_t> &value, const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest> *reqs, bool useCache = true) { return ErrorCode::Undefined; }

            virtual ErrorCode MultiGet(const std::vector<SizeType>& keys, std::vector<SPTAG::Helper::PageBuffer<std::uint8_t>>& values, const std::chrono::microseconds& timeout, std::vector<Helper::AsyncReadRequest>* reqs) { return ErrorCode::Undefined; }

            virtual ErrorCode MultiGet(const std::vector<SizeType>& keys, std::vector<std::string>* values, const std::chrono::microseconds& timeout, std::vector<Helper::AsyncReadRequest>* reqs) = 0;

            virtual ErrorCode MultiGet(const std::vector<std::string>& keys, std::vector<std::string>* values, const std::chrono::microseconds &timeout, std::vector<Helper::AsyncReadRequest>* reqs) { return ErrorCode::Undefined; }            
 
            // Packed mode: server-side UDF (nearest_candidates_read) filters each posting to
            // its top_n closest vectorInfo records. The returned PageBuffers contain concatenated
            // vectorInfo entries; the client re-runs ComputeDistance for final ranking.
            //
            // deleted_bitset / deleted_bitset_size: compact bitset where bit (VID%8) of byte
            // (VID/8) is set when that VID is deleted. Forwarded to Lua so dead vectors are
            // excluded *before* entering the top-n heap, preventing empty-slot recall collapse.
            // Pass nullptr / size 0 when there are no deletes.
            //
            // Non-Aerospike backends return ErrorCode::Fail by default.
            virtual ErrorCode MultiGetNearest(
                const std::vector<SizeType>& keys,
                const void* query_blob, uint32_t query_size,
                uint32_t vector_info_size, uint32_t meta_data_size,
                uint32_t dimension, uint8_t value_type,
                uint32_t top_n, uint8_t dist_mode,
                const void* deleted_bitset, uint32_t deleted_bitset_size,
                std::vector<PageBuffer<std::uint8_t>>& values,
                const std::chrono::microseconds& timeout)
            { return ErrorCode::Fail; }

            // Pairs mode: server-side UDF (nearest_candidates_pairs) returns top_n
            // (VID, float32 distance) pairs per posting. The client pushes them directly into
            // QueryResultSet::AddPoint, skipping ComputeDistance entirely.
            // MUST NOT be called when a PQ quantizer is active (caller enforces).
            //
            // deleted_bitset semantics identical to MultiGetNearest.
            //
            // Non-Aerospike backends return ErrorCode::Fail by default.
            virtual ErrorCode MultiGetNearestPairs(
                const std::vector<SizeType>& keys,
                const void* query_blob, uint32_t query_size,
                uint32_t vector_info_size, uint32_t meta_data_size,
                uint32_t dimension, uint8_t value_type,
                uint32_t top_n, uint8_t dist_mode,
                const void* deleted_bitset, uint32_t deleted_bitset_size,
                std::vector<std::vector<NearestPair>>& pairs,
                const std::chrono::microseconds& timeout)
            { return ErrorCode::Fail; }

            virtual ErrorCode Put(const std::string& key, const std::string& value, const std::chrono::microseconds& timeout, std::vector<Helper::AsyncReadRequest>* reqs) { return ErrorCode::Undefined; }

            virtual ErrorCode Put(const SizeType key, const std::string& value, const std::chrono::microseconds& timeout, std::vector<Helper::AsyncReadRequest>* reqs) = 0;

            virtual ErrorCode Merge(const SizeType key, const std::string &value,
                                    const std::chrono::microseconds &timeout,
                                    std::vector<Helper::AsyncReadRequest> *reqs,
                                    std::function<bool(const void *val, const int size)> checksum) = 0;

            virtual ErrorCode Delete(SizeType key) = 0;

            virtual ErrorCode DeleteRange(SizeType start, SizeType end) {return ErrorCode::Undefined;}

            virtual void ForceCompaction() {}

            virtual void GetStat() {}

            virtual int64_t GetNumBlocks() { return 0; }
            
            virtual bool Available() { return false; }

            virtual ErrorCode Check(const SizeType key, int size, std::vector<std::uint8_t> *visited)
            {
                return ErrorCode::Undefined;
            }

            virtual int64_t GetApproximateMemoryUsage() const
            {
                return 0;
            }

            virtual ErrorCode Checkpoint(std::string prefix) {return ErrorCode::Undefined;}

            virtual ErrorCode StartToScan(SizeType& key, std::string* value) {return ErrorCode::Undefined;}

            virtual ErrorCode NextToScan(SizeType& key, std::string* value) {return ErrorCode::Undefined;}
        };
    }
}

#endif
