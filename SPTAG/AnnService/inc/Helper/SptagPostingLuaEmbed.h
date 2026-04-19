#pragma once

// Auto-generated from udf/sptag_posting.lua — do not edit by hand.
// Regenerate: python3 AnnService/scripts/embed_sptag_posting_lua.py

#include <cstddef>

namespace SPTAG::Helper {

static constexpr const char kSptagPostingLua[] = R"LUA(
-- sptag_posting.lua — Aerospike Record UDFs for SPTAG posting list operations.
--
-- Each vector entry in a posting list is a fixed-size blob of `vector_info_size`
-- bytes. The first 4 bytes are the VID (int32 LE), byte 5 is the version (uint8).
--
-- deleted_vids_blob: packed array of (vid:int32_le, version:uint8) pairs (5 bytes
-- each). A vector is considered dead if its VID appears in the blob AND its
-- in-record version does NOT match the blob's version, OR if the vid appears with
-- a special 0xFF version sentinel meaning "hard-deleted".

local function read_int32_le(b, offset)
    -- offset is 1-based
    return bytes.get_int32(b, offset)
end

local function is_dead(vid, version, del_blob, del_count)
    if del_blob == nil or del_count == 0 then
        return false
    end
    local entry_size = 5 -- 4 bytes VID + 1 byte version
    for i = 0, del_count - 1 do
        local off = i * entry_size + 1
        local d_vid = bytes.get_int32(del_blob, off)
        if d_vid == vid then
            local d_ver = bytes.get_byte(del_blob, off + 4)
            if d_ver == 0xFF then
                return true -- hard-deleted sentinel
            end
            if d_ver ~= version then
                return true -- stale version
            end
            return false -- version matches => still alive
        end
    end
    return false
end

local function uint32_from_int32(v)
    if v < 0 then
        return v + 4294967296
    end
    return v
end

local function int32_bits_to_float(v)
    local bits = uint32_from_int32(v)
    local sign = 1.0
    if bits >= 2147483648 then
        sign = -1.0
        bits = bits - 2147483648
    end

    local exponent = math.floor(bits / 8388608)
    local mantissa = bits - exponent * 8388608

    if exponent == 255 then
        if mantissa == 0 then
            return sign * (1 / 0)
        end
        return 0 / 0
    end

    if exponent == 0 then
        if mantissa == 0 then
            return sign * 0.0
        end
        return sign * math.ldexp(mantissa / 8388608, -126)
    end

    return sign * math.ldexp(1.0 + mantissa / 8388608, exponent - 127)
end

-- Inverse of int32_bits_to_float: pack a Lua float into its IEEE 754 bit pattern
-- returned as a signed int32 (same convention as bytes.get_int32 / bytes.set_int32).
local function float_to_int32_bits(f)
    -- NaN
    if f ~= f then
        return 2143289344  -- 0x7FC00000 quiet NaN
    end
    local sign_bit = 0
    if f < 0.0 then
        sign_bit = 2147483648  -- 0x80000000 (stored as unsigned, sign-extended on C side)
        f = -f
    end
    if f == 0.0 then
        return sign_bit
    end
    -- +infinity
    if f >= math.huge then
        return sign_bit + 2139095040  -- 0x7F800000
    end
    -- math.frexp: f = m * 2^e, 0.5 <= m < 1
    local m, e = math.frexp(f)
    -- IEEE 754 normal biased exponent = (e - 1) + 127 = e + 126
    local biased_exp = e + 126
    if biased_exp <= 0 then
        -- Subnormal: mantissa = m * 2^(e+149)
        local mantissa = math.floor(m * math.ldexp(1, e + 149))
        return sign_bit + mantissa
    end
    if biased_exp >= 255 then
        return sign_bit + 2139095040  -- clamp to infinity
    end
    -- Normal: fractional mantissa = (2m - 1) * 2^23
    local mantissa = math.floor((2.0 * m - 1.0) * 8388608 + 0.5)
    if mantissa >= 8388608 then mantissa = 8388607 end
    if mantissa < 0 then mantissa = 0 end
    return sign_bit + biased_exp * 8388608 + mantissa
end

local function read_float32_le(b, offset)
    return int32_bits_to_float(bytes.get_int32(b, offset))
end

local function bytes_per_component(value_type)
    if value_type == 0 or value_type == 1 then
        return 1
    end
    if value_type == 3 then
        return 4
    end
    return 0
end

local function read_component(b, offset, value_type)
    if value_type == 1 then
        return bytes.get_byte(b, offset)
    end
    if value_type == 0 then
        local raw = bytes.get_byte(b, offset)
        if raw >= 128 then
            return raw - 256
        end
        return raw
    end
    if value_type == 3 then
        return read_float32_le(b, offset)
    end
    return nil
end

local function is_deleted_by_bitset(vid, deleted_bitset, del_size)
    if deleted_bitset == nil or del_size == 0 or vid < 0 then
        return false
    end

    local byte_idx = math.floor(vid / 8) + 1
    if byte_idx > del_size then
        return false
    end

    local bit_idx = vid % 8
    local bval = bytes.get_byte(deleted_bitset, byte_idx)
    local mask = math.floor(2 ^ bit_idx)
    return math.floor(bval / mask) % 2 == 1
end

local function decode_query_blob(query_blob, dimension, value_type)
    local query = {}
    local offset = 1
    local width = bytes_per_component(value_type)
    if width == 0 then
        return nil
    end
    for i = 1, dimension do
        local val = read_component(query_blob, offset, value_type)
        if val == nil then
            return nil
        end
        query[i] = val
        offset = offset + width
    end
    return query
end

-- compact_posting(rec, bin_name, vector_info_size, deleted_vids_blob)
--
-- Iterates the posting list in-place, removes vectors whose VID is dead
-- (deleted or stale version). Writes back compacted blob.
-- Returns the new vector count, or 0 if posting was nil/empty.
function compact_posting(rec, bin_name, vector_info_size, deleted_vids_blob)
    local posting = rec[bin_name]
    if posting == nil then return 0 end

    local total = bytes.size(posting)
    if total == 0 then return 0 end

    local vec_count = math.floor(total / vector_info_size)
    local del_count = 0
    if deleted_vids_blob ~= nil and bytes.size(deleted_vids_blob) > 0 then
        del_count = math.floor(bytes.size(deleted_vids_blob) / 5)
    end

    local out = bytes(vec_count * vector_info_size)
    local write_pos = 1
    local kept = 0

    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid = bytes.get_int32(posting, offset)
        local version = bytes.get_byte(posting, offset + 4)

        if not is_dead(vid, version, deleted_vids_blob, del_count) then
            bytes.set_bytes(out, write_pos, posting, offset, vector_info_size)
            write_pos = write_pos + vector_info_size
            kept = kept + 1
        end
    end

    if kept == 0 then
        rec[bin_name] = bytes(0)
    else
        local final_out = bytes(kept * vector_info_size)
        bytes.set_bytes(final_out, 1, out, 1, kept * vector_info_size)
        rec[bin_name] = final_out
    end

    aerospike:update(rec)
    return kept
end

-- filtered_read(rec, bin_name, vector_info_size, deleted_bitset)
--
-- Returns only the live vectors from the posting list. Dead vectors
-- (whose VID bit is set in deleted_bitset) never cross the network.
-- deleted_bitset is a byte-array where bit at position VID indicates
-- deletion: byte[floor(vid/8)] & (1 << (vid%8)).
function filtered_read(rec, bin_name, vector_info_size, deleted_bitset)
    local posting = rec[bin_name]
    if posting == nil then return bytes(0) end

    local total = bytes.size(posting)
    if total == 0 then return bytes(0) end

    local vec_count = math.floor(total / vector_info_size)
    local del_size = 0
    if deleted_bitset ~= nil then
        del_size = bytes.size(deleted_bitset)
    end

    if del_size == 0 then
        return posting
    end

    local out = bytes(total)
    local write_pos = 1
    local kept = 0

    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid = bytes.get_int32(posting, offset)

        if not is_deleted_by_bitset(vid, deleted_bitset, del_size) then
            bytes.set_bytes(out, write_pos, posting, offset, vector_info_size)
            write_pos = write_pos + vector_info_size
            kept = kept + 1
        end
    end

    if kept == vec_count then
        return posting
    end

    if kept == 0 then
        return bytes(0)
    end

    local final_out = bytes(kept * vector_info_size)
    bytes.set_bytes(final_out, 1, out, 1, kept * vector_info_size)
    return final_out
end

-- nearest_candidates_read(rec, bin_name, query_blob, vector_info_size, meta_data_size,
--                         dimension, value_type, top_n, dist_mode, deleted_bitset)
--
-- Returns the top local vectorInfo records from this posting after scoring each
-- live vector against the query. The return payload remains packed vectorInfo
-- records so the client can reuse its existing final-ranking logic.
-- deleted_bitset filtering runs BEFORE distance scoring so deleted vectors never
-- consume top_n slots.
function nearest_candidates_read(rec, bin_name, query_blob, vector_info_size, meta_data_size,
                                 dimension, value_type, top_n, dist_mode, deleted_bitset)
    local posting = rec[bin_name]
    if posting == nil then return bytes(0) end

    local total = bytes.size(posting)
    if total == 0 or top_n == nil or top_n <= 0 or dimension == nil or dimension <= 0 then
        return bytes(0)
    end

    local width = bytes_per_component(value_type)
    if width == 0 then
        return bytes(0)
    end

    if query_blob == nil or bytes.size(query_blob) < dimension * width then
        return bytes(0)
    end

    local del_size = 0
    if deleted_bitset ~= nil then
        del_size = bytes.size(deleted_bitset)
    end

    local query = decode_query_blob(query_blob, dimension, value_type)
    if query == nil then
        return bytes(0)
    end
    local vec_count = math.floor(total / vector_info_size)
    local kept = 0
    local top_offsets = {}
    local top_scores = {}

    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid = bytes.get_int32(posting, offset)
        -- Filter deleted BEFORE scoring so no deleted vector consumes a top_n slot
        if not is_deleted_by_bitset(vid, deleted_bitset, del_size) then
            local score = 0.0
            local vector_offset = offset + meta_data_size

            if dist_mode == 1 then
                local dot = 0.0
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        return bytes(0)
                    end
                    dot = dot + query[d] * value
                    cursor = cursor + width
                end
                score = 1.0 - dot
            else
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        return bytes(0)
                    end
                    local diff = query[d] - value
                    score = score + diff * diff
                    cursor = cursor + width
                end
            end

            if kept < top_n or score < top_scores[kept] then
                local pos = kept + 1
                if kept < top_n then
                    kept = kept + 1
                    pos = kept
                end

                while pos > 1 and score < top_scores[pos - 1] do
                    top_scores[pos] = top_scores[pos - 1]
                    top_offsets[pos] = top_offsets[pos - 1]
                    pos = pos - 1
                end

                top_scores[pos] = score
                top_offsets[pos] = offset
            end
        end
    end

    if kept == 0 then
        return bytes(0)
    end

    local out = bytes(kept * vector_info_size)
    local write_pos = 1
    for i = 1, kept do
        bytes.set_bytes(out, write_pos, posting, top_offsets[i], vector_info_size)
        write_pos = write_pos + vector_info_size
    end
    return out
end

-- nearest_candidates_pairs(rec, bin_name, query_blob, vector_info_size, meta_data_size,
--                          dimension, value_type, top_n, dist_mode, deleted_bitset)
--
-- Returns the top_n (VID, distance) pairs from this posting after scoring each live
-- vector against the query. Output is 8 bytes per pair:
--   bytes [0..3]: VID as int32 little-endian
--   bytes [4..7]: distance as IEEE 754 float32 little-endian
--
-- The client skips ComputeDistance and pushes pairs straight into QueryResultSet.
-- Only valid for non-PQ indexes (plain L2 or 1-dot Cosine on stored vector bytes).
-- deleted_bitset filtering runs BEFORE scoring so deleted vectors never consume
-- a top_n slot.
function nearest_candidates_pairs(rec, bin_name, query_blob, vector_info_size, meta_data_size,
                                  dimension, value_type, top_n, dist_mode, deleted_bitset)
    local posting = rec[bin_name]
    if posting == nil then return bytes(0) end

    local total = bytes.size(posting)
    if total == 0 or top_n == nil or top_n <= 0 or dimension == nil or dimension <= 0 then
        return bytes(0)
    end

    local width = bytes_per_component(value_type)
    if width == 0 then
        return bytes(0)
    end

    if query_blob == nil or bytes.size(query_blob) < dimension * width then
        return bytes(0)
    end

    local del_size = 0
    if deleted_bitset ~= nil then
        del_size = bytes.size(deleted_bitset)
    end

    local query = decode_query_blob(query_blob, dimension, value_type)
    if query == nil then
        return bytes(0)
    end

    local vec_count = math.floor(total / vector_info_size)
    local kept = 0
    local top_vids = {}
    local top_scores = {}

    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid = bytes.get_int32(posting, offset)
        -- Filter deleted BEFORE scoring so no deleted vector consumes a top_n slot
        if not is_deleted_by_bitset(vid, deleted_bitset, del_size) then
            local score = 0.0
            local vector_offset = offset + meta_data_size

            if dist_mode == 1 then
                local dot = 0.0
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        return bytes(0)
                    end
                    dot = dot + query[d] * value
                    cursor = cursor + width
                end
                score = 1.0 - dot
            else
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        return bytes(0)
                    end
                    local diff = query[d] - value
                    score = score + diff * diff
                    cursor = cursor + width
                end
            end

            if kept < top_n or score < top_scores[kept] then
                local pos = kept + 1
                if kept < top_n then
                    kept = kept + 1
                    pos = kept
                end

                while pos > 1 and score < top_scores[pos - 1] do
                    top_scores[pos] = top_scores[pos - 1]
                    top_vids[pos] = top_vids[pos - 1]
                    pos = pos - 1
                end

                top_scores[pos] = score
                top_vids[pos] = vid
            end
        end
    end

    if kept == 0 then
        return bytes(0)
    end

    -- Pack output: 8 bytes per pair [VID int32_le | dist float32_le]
    local out = bytes(kept * 8)
    for i = 1, kept do
        local p = (i - 1) * 8 + 1
        bytes.set_int32(out, p, top_vids[i])
        bytes.set_int32(out, p + 4, float_to_int32_bits(top_scores[i]))
    end
    return out
end

-- atomic_merge_append(rec, bin_name, new_data, max_posting_bytes)
--
-- If the record exists and has the bin, append new_data to existing value.
-- If the record or bin is empty/missing, create with new_data.
-- Enforces max_posting_bytes: returns -1 when the combined size would exceed
-- the limit, signaling the client to trigger a posting split.
-- Returns the new total byte size on success, or -1 on overflow.
function atomic_merge_append(rec, bin_name, new_data, max_posting_bytes)
    if new_data == nil or bytes.size(new_data) == 0 then
        if rec[bin_name] ~= nil then
            return bytes.size(rec[bin_name])
        end
        return 0
    end

    local append_size = bytes.size(new_data)
    local existing = rec[bin_name]

    if existing ~= nil and bytes.size(existing) > 0 then
        local current_size = bytes.size(existing)
        if max_posting_bytes > 0 and (current_size + append_size) > max_posting_bytes then
            return -1
        end
        local combined = bytes(current_size + append_size)
        bytes.set_bytes(combined, 1, existing, 1, current_size)
        bytes.set_bytes(combined, current_size + 1, new_data, 1, append_size)
        rec[bin_name] = combined
    else
        rec[bin_name] = new_data
    end

    if aerospike:exists(rec) then
        aerospike:update(rec)
    else
        aerospike:create(rec)
    end
    return bytes.size(rec[bin_name])
end

-- merge_into(rec, bin_name, vector_info_size, other_posting_blob, skip_vids_blob)
--
-- Merges other_posting_blob into this record's posting, deduplicating by VID.
-- Vectors whose VID appears in skip_vids_blob (packed sorted int32 LE, 4 bytes
-- each) are stripped from this record's posting before merging.
-- other_posting_blob is assumed to already be filtered (only live vectors).
-- Returns the new vector count.
function merge_into(rec, bin_name, vector_info_size, other_posting_blob, skip_vids_blob)
    local posting = rec[bin_name]
    local seen = {}
    local kept = 0

    local skip_set = {}
    if skip_vids_blob ~= nil and bytes.size(skip_vids_blob) > 0 then
        local skip_count = math.floor(bytes.size(skip_vids_blob) / 4)
        for i = 0, skip_count - 1 do
            local vid = bytes.get_int32(skip_vids_blob, i * 4 + 1)
            skip_set[vid] = true
        end
    end

    -- Upper-bound allocation: own posting + other posting
    local own_size = 0
    if posting ~= nil then own_size = bytes.size(posting) end
    local other_size = 0
    if other_posting_blob ~= nil then other_size = bytes.size(other_posting_blob) end
    local out = bytes(own_size + other_size)
    local write_pos = 1

    -- First pass: keep live vectors from this record's posting
    if posting ~= nil and own_size > 0 then
        local count = math.floor(own_size / vector_info_size)
        for i = 0, count - 1 do
            local offset = i * vector_info_size + 1
            local vid = bytes.get_int32(posting, offset)
            if not skip_set[vid] and not seen[vid] then
                bytes.set_bytes(out, write_pos, posting, offset, vector_info_size)
                write_pos = write_pos + vector_info_size
                seen[vid] = true
                kept = kept + 1
            end
        end
    end

    -- Second pass: add non-duplicate vectors from other posting
    if other_posting_blob ~= nil and other_size > 0 then
        local count = math.floor(other_size / vector_info_size)
        for i = 0, count - 1 do
            local offset = i * vector_info_size + 1
            local vid = bytes.get_int32(other_posting_blob, offset)
            if not seen[vid] then
                bytes.set_bytes(out, write_pos, other_posting_blob, offset, vector_info_size)
                write_pos = write_pos + vector_info_size
                seen[vid] = true
                kept = kept + 1
            end
        end
    end

    if kept == 0 then
        rec[bin_name] = bytes(0)
    else
        local final_out = bytes(kept * vector_info_size)
        bytes.set_bytes(final_out, 1, out, 1, kept * vector_info_size)
        rec[bin_name] = final_out
    end

    if aerospike:exists(rec) then
        aerospike:update(rec)
    else
        aerospike:create(rec)
    end
    return kept
end
)LUA";

static constexpr size_t kSptagPostingLuaSize = sizeof(kSptagPostingLua) - 1;

} // namespace SPTAG::Helper
