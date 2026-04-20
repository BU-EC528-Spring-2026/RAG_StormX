-- sptag_posting.lua — Aerospike Record UDFs for SPTAG posting list operations.
--
-- Targets the Lua 5.4 runtime embedded in Aerospike Server 7.0+ (mod-lua).
-- Uses native bitwise operators (&, |, ~, >>, <<), integer division (//)
-- and string.pack / string.unpack for IEEE-754 bit-casting. None of these
-- exist in 5.1, so this file will refuse to load on pre-7.0 servers.

-- ======================================================================
-- LOGGING (Aerospike UDF: info() -> aerospike.log / journal)
-- Prefix keeps grep easy: grep sptag_posting /var/log/aerospike/aerospike.log
-- ======================================================================
local function slog(msg)
    info("[sptag_posting] " .. msg)
end

-- ======================================================================
-- AVX-512 LAZY LOADER
--
-- Loaded on first use, NOT at module-registration time. Aerospike Server
-- 7.0+/8.0+ runs the module chunk at register time to harvest globals,
-- and that early phase happens before the `aerospike` system table is
-- bound and before mod-lua's overridden `require` is wired up. Calling
-- info()/require() at the top level reliably triggers
--     compile_error: aerospike.lua:8: attempt to index a nil value
--                    (global 'aerospike')
-- on `aql register module`, so the load *must* be deferred.
--
-- To probe the loader synchronously after registration, call init():
--   aql -h <seed> -c "execute sptag_posting.init() on <ns>.<set> where pk='diag'"
-- Returns "avx_math:ok" or "avx_math:missing" and emits the same log
-- line as a real UDF call would on first use.
-- ======================================================================
local avx_math    = nil
local avx_loaded  = false
local has_avx     = false
local avx_load_err = nil   -- last require() error, surfaced by init()

-- Append common Aerospike UDF deployment directories to the C-module
-- search path. Safe at top level: pure string concatenation, doesn't
-- touch the `aerospike` table or call `require`. We append several
-- plausible locations because the actual `lua-userpath` is configurable
-- per node and may differ from the default.
package.cpath = package.cpath
    .. ";/opt/aerospike/usr/udf/lua/?.so"
    .. ";/opt/aerospike/sys/udf/lua/?.so"
    .. ";/etc/aerospike/usr/udf/?.so"
    .. ";/var/lib/aerospike/usr/udf/?.so"

local function get_avx()
    if not avx_loaded then
        avx_loaded = true

        -- avx_math.so must be registered with the cluster via
        -- `aql register module 'avx_math.so'` (or aerospike_udf_put).
        -- The cluster ships the binary to every node and places it
        -- where the embedded Lua's `require` finds it natively.
        local ok, mod = pcall(require, "avx_math")
        if ok and type(mod) == "table" then
            has_avx, avx_math, avx_load_err = true, mod, nil
            slog("avx_math loaded ok via require (lazy, first_call)")
        else
            has_avx, avx_math = false, nil
            avx_load_err = tostring(mod)
            slog("FATAL: avx_math.so not loaded. require err=" ..
                avx_load_err ..
                " ; cpath=" .. tostring(package and package.cpath or "<nil>"))
            slog("Register the C module: `aql -c \"register module 'avx_math.so'\"` " ..
                 "(or call aerospike_udf_put with type LUA). " ..
                 "Falling back to slow Lua math (will likely time out under 50 ms).")
        end
    end
    return has_avx, avx_math
end

-- Explicit probe UDF. Forces the lazy loader to run inside a real UDF
-- context and returns a single diagnostic string so the operator does
-- not need to grep aerospike.log on every node.
--
--   aql -h <seed> -c "execute sptag_posting.init() on <ns>.<set> where pk='diag'"
--
-- On success returns:
--   "avx_math:ok"
-- On failure returns:
--   "avx_math:missing | err=<require error> | cpath=<package.cpath>"
function init(rec)
    local ok, _ = get_avx()
    if ok then
        slog("init() probe: avx_math present")
        return "avx_math:ok"
    end
    slog("init() probe: avx_math MISSING -- queries will fall back to slow Lua")
    return "avx_math:missing | err=" .. tostring(avx_load_err) ..
           " | cpath=" .. tostring(package and package.cpath or "<nil>")
end

local function dist_mode_label(dm)
    if dm == 1 then return "cosine" end
    return "l2"
end

local function value_type_label(vt)
    if vt == 0 then return "int8" end
    if vt == 1 then return "uint8" end
    if vt == 3 then return "float32" end
    return "unknown(" .. tostring(vt) .. ")"
end

-- ======================================================================
-- HELPER FUNCTIONS (Lua 5.4 idioms)
-- ======================================================================

local function is_dead(vid, version, del_blob, del_count)
    if del_blob == nil or del_count == 0 then return false end
    local entry_size = 5
    for i = 0, del_count - 1 do
        local off = i * entry_size + 1
        local d_vid = bytes.get_int32(del_blob, off)
        if d_vid == vid then
            local d_ver = bytes.get_byte(del_blob, off + 4)
            if d_ver == 0xFF then return true end
            if d_ver ~= version then return true end
            return false
        end
    end
    return false
end

-- Lua 5.3+: `string.pack("<f", n)` produces 4 bytes of little-endian IEEE-754
-- and `string.unpack("<i4", ...)` reinterprets them as a signed int32. This
-- replaces the pure-Lua frexp dance and is bit-exact.
local function float_to_int32_bits(f)
    -- Prefer the fast C bit-caster if avx_math is loaded.
    local h_avx, a_math = get_avx()
    if h_avx and a_math.f32_to_bits then
        return a_math.f32_to_bits(f)
    end
    local bits = string.unpack("<i4", string.pack("<f", f))
    return bits
end

local function read_float32_le(b, offset)
    -- bytes.get_string returns a Lua string; string.unpack reads the float
    -- without going through the int32 representation at all.
    local s = bytes.get_string(b, offset, 4)
    local f = string.unpack("<f", s)
    return f
end

local function bytes_per_component(value_type)
    if value_type == 0 or value_type == 1 then return 1 end
    if value_type == 3 then return 4 end
    return 0
end

local function read_component(b, offset, value_type)
    if value_type == 1 then
        return bytes.get_byte(b, offset)
    end
    if value_type == 0 then
        local raw = bytes.get_byte(b, offset)
        if raw >= 128 then return raw - 256 end
        return raw
    end
    if value_type == 3 then
        return read_float32_le(b, offset)
    end
    return nil
end

local function is_deleted_by_bitset(vid, deleted_bitset, del_size)
    if deleted_bitset == nil or del_size == 0 or vid < 0 then return false end
    -- vid is an integer in 5.4: shift / mask are exact and far cheaper
    -- than math.floor(vid / 8) / (vid % 8).
    local byte_idx = (vid >> 3) + 1
    if byte_idx > del_size then return false end
    local bit_idx = vid & 7
    local bval = bytes.get_byte(deleted_bitset, byte_idx)
    return ((bval >> bit_idx) & 1) == 1
end

local function decode_query_blob(query_blob, dimension, value_type)
    local query  = {}
    local offset = 1
    local width  = bytes_per_component(value_type)
    if width == 0 then return nil end
    for i = 1, dimension do
        local val = read_component(query_blob, offset, value_type)
        if val == nil then return nil end
        query[i] = val
        offset = offset + width
    end
    return query
end

-- ======================================================================
-- CORE API (AVX Optimized)
-- ======================================================================

function nearest_candidates_read(rec, bin_name, query_blob, vector_info_size, meta_data_size,
                                 dimension, value_type, top_n, dist_mode, deleted_bitset)
    local posting = rec[bin_name]
    if posting == nil then
        slog("nearest_candidates_read: exit missing_bin bin=" .. tostring(bin_name))
        return bytes(0)
    end

    local total = bytes.size(posting)
    if total == 0 or top_n == nil or top_n <= 0 or dimension == nil or dimension <= 0 then
        slog(string.format(
            "nearest_candidates_read: exit bad_args total=%s top_n=%s dim=%s",
            tostring(total), tostring(top_n), tostring(dimension)))
        return bytes(0)
    end

    local width = bytes_per_component(value_type)
    if width == 0 then
        slog("nearest_candidates_read: exit unsupported_value_type vt=" .. tostring(value_type))
        return bytes(0)
    end
    if query_blob == nil or bytes.size(query_blob) < dimension * width then
        slog(string.format(
            "nearest_candidates_read: exit query_blob_too_small need=%s have=%s",
            tostring(dimension * width), tostring(query_blob and bytes.size(query_blob) or "nil")))
        return bytes(0)
    end

    local del_size = 0
    if deleted_bitset ~= nil then del_size = bytes.size(deleted_bitset) end

    local query_fallback = decode_query_blob(query_blob, dimension, value_type)
    if query_fallback == nil then
        slog(string.format(
            "nearest_candidates_read: exit decode_query_failed dim=%s vt=%s",
            tostring(dimension), value_type_label(value_type)))
        return bytes(0)
    end

    local vec_count = total // vector_info_size

    -- ================================================================
    -- FAST C PATH: batch_candidates_packed handles filter + score + rank
    -- and returns the top-N posting entries verbatim.
    -- ================================================================
    -- C path supports float32 (any dist_mode) and uint8 L2. Skip C for
    -- uint8+Cosine because that's not implemented natively yet.
    local h_avx, a_math = get_avx()
    local c_supported = h_avx and (value_type == 3 or (value_type == 1 and dist_mode ~= 1))
    local c_reason = "c_path"
    if not h_avx then
        c_reason = "lua_fallback_no_avx_math"
    elseif value_type ~= 3 and not (value_type == 1 and dist_mode ~= 1) then
        c_reason = "lua_fallback_vt_or_metric (" .. value_type_label(value_type) .. "+" .. dist_mode_label(dist_mode) .. " not in C)"
    end
    slog(string.format(
        "nearest_candidates_read: bin=%s vec_count=%s vis=%s meta=%s dim=%s vt=%s dist=%s top_n=%s posting_bytes=%s del_bitset_bytes=%s path=%s lua_ops~=%s (timeout_risk_if_lua)",
        tostring(bin_name), tostring(vec_count), tostring(vector_info_size), tostring(meta_data_size),
        tostring(dimension), value_type_label(value_type), dist_mode_label(dist_mode), tostring(top_n),
        tostring(total), tostring(del_size), c_reason, tostring(vec_count * dimension)))

    if c_supported then
        local query_str   = bytes.get_string(query_blob, 1, bytes.size(query_blob))
        local posting_str = bytes.get_string(posting, 1, total)
        local bitset_str  = ""
        if del_size > 0 then
            bitset_str = bytes.get_string(deleted_bitset, 1, del_size)
        end

        local result_str = a_math.batch_candidates_packed(
            posting_str, query_str, dimension,
            vector_info_size, meta_data_size, top_n, dist_mode,
            bitset_str, value_type)

        local result_len = #result_str
        if result_len == 0 then
            slog("nearest_candidates_read: c_path batch_candidates_packed returned empty")
            return bytes(0)
        end

        local out = bytes(result_len)
        bytes.set_string(out, 1, result_str)
        slog(string.format("nearest_candidates_read: c_path ok out_bytes=%s", tostring(result_len)))
        return out
    end

    -- ================================================================
    -- SLOW LUA FALLBACK (non-float32 types or missing C module)
    -- ================================================================
    slog("nearest_candidates_read: lua_fallback scoring start (large vec_count may exceed udf timeout)")
    local kept        = 0
    local top_offsets = {}
    local top_scores  = {}

    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid    = bytes.get_int32(posting, offset)
        if not is_deleted_by_bitset(vid, deleted_bitset, del_size) then
            local score         = 0.0
            local vector_offset = offset + meta_data_size

            if dist_mode == 1 then
                local dot    = 0.0
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        slog(string.format(
                            "nearest_candidates_read: lua_fallback exit corrupt_posting vec_i=%s dim_i=%s off=%s",
                            tostring(i), tostring(d), tostring(cursor)))
                        return bytes(0)
                    end
                    dot    = dot + query_fallback[d] * value
                    cursor = cursor + width
                end
                score = 1.0 - dot
            else
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        slog(string.format(
                            "nearest_candidates_read: lua_fallback exit corrupt_posting vec_i=%s dim_i=%s off=%s",
                            tostring(i), tostring(d), tostring(cursor)))
                        return bytes(0)
                    end
                    local diff = query_fallback[d] - value
                    score      = score + diff * diff
                    cursor     = cursor + width
                end
            end

            if kept < top_n or score < top_scores[kept] then
                local pos = kept + 1
                if kept < top_n then
                    kept = kept + 1
                    pos  = kept
                end
                while pos > 1 and score < top_scores[pos - 1] do
                    top_scores[pos]  = top_scores[pos - 1]
                    top_offsets[pos] = top_offsets[pos - 1]
                    pos              = pos - 1
                end
                top_scores[pos]  = score
                top_offsets[pos] = offset
            end
        end
    end

    if kept == 0 then
        slog("nearest_candidates_read: lua_fallback exit no_candidates_after_filter")
        return bytes(0)
    end

    local out       = bytes(kept * vector_info_size)
    local write_pos = 1
    for i = 1, kept do
        bytes.set_bytes(out, write_pos, posting, top_offsets[i], vector_info_size)
        write_pos = write_pos + vector_info_size
    end
    slog(string.format("nearest_candidates_read: lua_fallback ok kept=%s out_bytes=%s", tostring(kept), tostring(bytes.size(out))))
    return out
end

function nearest_candidates_pairs(rec, bin_name, query_blob, vector_info_size, meta_data_size,
                                  dimension, value_type, top_n, dist_mode, deleted_bitset)
    local posting = rec[bin_name]
    if posting == nil then
        slog("nearest_candidates_pairs: exit missing_bin bin=" .. tostring(bin_name))
        return bytes(0)
    end

    local total = bytes.size(posting)
    if total == 0 or top_n == nil or top_n <= 0 or dimension == nil or dimension <= 0 then
        slog(string.format(
            "nearest_candidates_pairs: exit bad_args total=%s top_n=%s dim=%s",
            tostring(total), tostring(top_n), tostring(dimension)))
        return bytes(0)
    end

    local width = bytes_per_component(value_type)
    if width == 0 then
        slog("nearest_candidates_pairs: exit unsupported_value_type vt=" .. tostring(value_type))
        return bytes(0)
    end
    if query_blob == nil or bytes.size(query_blob) < dimension * width then
        slog(string.format(
            "nearest_candidates_pairs: exit query_blob_too_small need=%s have=%s",
            tostring(dimension * width), tostring(query_blob and bytes.size(query_blob) or "nil")))
        return bytes(0)
    end

    local del_size = 0
    if deleted_bitset ~= nil then del_size = bytes.size(deleted_bitset) end

    local vec_count = total // vector_info_size

    -- ================================================================
    -- FAST C PATH: batch_candidates_pairs handles filter + score + rank
    -- ================================================================
    -- C path supports float32 (any dist_mode) and uint8 L2.
    local h_avx, a_math = get_avx()
    local c_supported = h_avx and (value_type == 3 or (value_type == 1 and dist_mode ~= 1))
    local c_reason = "c_path"
    if not h_avx then
        c_reason = "lua_fallback_no_avx_math"
    elseif value_type ~= 3 and not (value_type == 1 and dist_mode ~= 1) then
        c_reason = "lua_fallback_vt_or_metric (" .. value_type_label(value_type) .. "+" .. dist_mode_label(dist_mode) .. " not in C)"
    end
    slog(string.format(
        "nearest_candidates_pairs: bin=%s vec_count=%s vis=%s meta=%s dim=%s vt=%s dist=%s top_n=%s posting_bytes=%s del_bitset_bytes=%s path=%s lua_ops~=%s (timeout_risk_if_lua)",
        tostring(bin_name), tostring(vec_count), tostring(vector_info_size), tostring(meta_data_size),
        tostring(dimension), value_type_label(value_type), dist_mode_label(dist_mode), tostring(top_n),
        tostring(total), tostring(del_size), c_reason, tostring(vec_count * dimension)))

    if c_supported then
        local query_str   = bytes.get_string(query_blob, 1, bytes.size(query_blob))
        local posting_str = bytes.get_string(posting, 1, total)
        local bitset_str  = ""
        if del_size > 0 then
            bitset_str = bytes.get_string(deleted_bitset, 1, del_size)
        end

        -- Delegate filter + scoring + ranking to C. Returns a packed byte string
        -- already in our wire layout: N x (int32 vid | float32 score bits).
        local result_str = a_math.batch_candidates_pairs(
            posting_str, query_str, dimension,
            vector_info_size, meta_data_size, top_n, dist_mode,
            bitset_str, value_type)

        local result_len = #result_str
        if result_len == 0 then
            slog("nearest_candidates_pairs: c_path batch_candidates_pairs returned empty")
            return bytes(0)
        end

        -- Single bulk copy: no per-pair Lua unpack.
        local out = bytes(result_len)
        bytes.set_string(out, 1, result_str)
        slog(string.format("nearest_candidates_pairs: c_path ok out_bytes=%s", tostring(result_len)))
        return out
    end

    -- ================================================================
    -- SLOW LUA FALLBACK (non-float32 types or missing C module)
    -- ================================================================
    local query_fallback = decode_query_blob(query_blob, dimension, value_type)
    if query_fallback == nil then
        slog(string.format(
            "nearest_candidates_pairs: lua_fallback exit decode_query_failed dim=%s vt=%s",
            tostring(dimension), value_type_label(value_type)))
        return bytes(0)
    end

    local kept       = 0
    local top_vids   = {}
    local top_scores = {}

    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid    = bytes.get_int32(posting, offset)
        if not is_deleted_by_bitset(vid, deleted_bitset, del_size) then
            local score         = 0.0
            local vector_offset = offset + meta_data_size

            if dist_mode == 1 then
                local dot    = 0.0
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        slog(string.format(
                            "nearest_candidates_pairs: lua_fallback exit corrupt_posting vec_i=%s dim_i=%s off=%s",
                            tostring(i), tostring(d), tostring(cursor)))
                        return bytes(0)
                    end
                    dot    = dot + query_fallback[d] * value
                    cursor = cursor + width
                end
                score = 1.0 - dot
            else
                local cursor = vector_offset
                for d = 1, dimension do
                    local value = read_component(posting, cursor, value_type)
                    if value == nil then
                        slog(string.format(
                            "nearest_candidates_pairs: lua_fallback exit corrupt_posting vec_i=%s dim_i=%s off=%s",
                            tostring(i), tostring(d), tostring(cursor)))
                        return bytes(0)
                    end
                    local diff = query_fallback[d] - value
                    score      = score + diff * diff
                    cursor     = cursor + width
                end
            end

            if kept < top_n or score < top_scores[kept] then
                local pos = kept + 1
                if kept < top_n then
                    kept = kept + 1
                    pos  = kept
                end
                while pos > 1 and score < top_scores[pos - 1] do
                    top_scores[pos] = top_scores[pos - 1]
                    top_vids[pos]   = top_vids[pos - 1]
                    pos             = pos - 1
                end
                top_scores[pos] = score
                top_vids[pos]   = vid
            end
        end
    end

    if kept == 0 then
        slog("nearest_candidates_pairs: lua_fallback exit no_candidates_after_filter")
        return bytes(0)
    end

    local out = bytes(kept * 8)
    for i = 1, kept do
        local p = (i - 1) * 8 + 1
        bytes.set_int32(out, p,     top_vids[i])
        bytes.set_int32(out, p + 4, float_to_int32_bits(top_scores[i]))
    end
    slog(string.format("nearest_candidates_pairs: lua_fallback ok kept=%s out_bytes=%s", tostring(kept), tostring(bytes.size(out))))
    return out
end

-- ======================================================================
-- WRITE OPERATIONS
-- ======================================================================

function compact_posting(rec, bin_name, vector_info_size, deleted_vids_blob)
    local posting = rec[bin_name]
    if posting == nil then return 0 end
    local total = bytes.size(posting)
    if total == 0 then return 0 end
    local vec_count = total // vector_info_size
    local del_count = 0
    if deleted_vids_blob ~= nil and bytes.size(deleted_vids_blob) > 0 then
        del_count = bytes.size(deleted_vids_blob) // 5
    end
    local out       = bytes(vec_count * vector_info_size)
    local write_pos = 1
    local kept      = 0
    for i = 0, vec_count - 1 do
        local offset  = i * vector_info_size + 1
        local vid     = bytes.get_int32(posting, offset)
        local version = bytes.get_byte(posting, offset + 4)
        if not is_dead(vid, version, deleted_vids_blob, del_count) then
            bytes.set_bytes(out, write_pos, posting, offset, vector_info_size)
            write_pos = write_pos + vector_info_size
            kept      = kept + 1
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

function filtered_read(rec, bin_name, vector_info_size, deleted_bitset)
    local posting = rec[bin_name]
    if posting == nil then return bytes(0) end
    local total = bytes.size(posting)
    if total == 0 then return bytes(0) end
    local vec_count = total // vector_info_size
    local del_size  = 0
    if deleted_bitset ~= nil then del_size = bytes.size(deleted_bitset) end
    if del_size == 0 then return posting end
    local out       = bytes(total)
    local write_pos = 1
    local kept      = 0
    for i = 0, vec_count - 1 do
        local offset = i * vector_info_size + 1
        local vid    = bytes.get_int32(posting, offset)
        if not is_deleted_by_bitset(vid, deleted_bitset, del_size) then
            bytes.set_bytes(out, write_pos, posting, offset, vector_info_size)
            write_pos = write_pos + vector_info_size
            kept      = kept + 1
        end
    end
    if kept == vec_count then return posting end
    if kept == 0 then return bytes(0) end
    local final_out = bytes(kept * vector_info_size)
    bytes.set_bytes(final_out, 1, out, 1, kept * vector_info_size)
    return final_out
end

function atomic_merge_append(rec, bin_name, new_data, max_posting_bytes)
    if new_data == nil or bytes.size(new_data) == 0 then
        if rec[bin_name] ~= nil then return bytes.size(rec[bin_name]) end
        return 0
    end
    local append_size = bytes.size(new_data)
    local existing    = rec[bin_name]
    if existing ~= nil and bytes.size(existing) > 0 then
        local current_size = bytes.size(existing)
        if max_posting_bytes > 0 and (current_size + append_size) > max_posting_bytes then return -1 end
        local combined = bytes(current_size + append_size)
        bytes.set_bytes(combined, 1,                existing, 1, current_size)
        bytes.set_bytes(combined, current_size + 1, new_data, 1, append_size)
        rec[bin_name] = combined
    else
        rec[bin_name] = new_data
    end
    if aerospike:exists(rec) then aerospike:update(rec) else aerospike:create(rec) end
    return bytes.size(rec[bin_name])
end

function merge_into(rec, bin_name, vector_info_size, other_posting_blob, skip_vids_blob)
    local posting  = rec[bin_name]
    local seen     = {}
    local kept     = 0
    local skip_set = {}
    if skip_vids_blob ~= nil and bytes.size(skip_vids_blob) > 0 then
        local skip_count = bytes.size(skip_vids_blob) // 4
        for i = 0, skip_count - 1 do
            local vid = bytes.get_int32(skip_vids_blob, i * 4 + 1)
            skip_set[vid] = true
        end
    end
    local own_size = 0
    if posting ~= nil then own_size = bytes.size(posting) end
    local other_size = 0
    if other_posting_blob ~= nil then other_size = bytes.size(other_posting_blob) end
    local out       = bytes(own_size + other_size)
    local write_pos = 1

    if posting ~= nil and own_size > 0 then
        local count = own_size // vector_info_size
        for i = 0, count - 1 do
            local offset = i * vector_info_size + 1
            local vid    = bytes.get_int32(posting, offset)
            if not skip_set[vid] and not seen[vid] then
                bytes.set_bytes(out, write_pos, posting, offset, vector_info_size)
                write_pos  = write_pos + vector_info_size
                seen[vid]  = true
                kept       = kept + 1
            end
        end
    end

    if other_posting_blob ~= nil and other_size > 0 then
        local count = other_size // vector_info_size
        for i = 0, count - 1 do
            local offset = i * vector_info_size + 1
            local vid    = bytes.get_int32(other_posting_blob, offset)
            if not seen[vid] then
                bytes.set_bytes(out, write_pos, other_posting_blob, offset, vector_info_size)
                write_pos  = write_pos + vector_info_size
                seen[vid]  = true
                kept       = kept + 1
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
    if aerospike:exists(rec) then aerospike:update(rec) else aerospike:create(rec) end
    return kept
end
