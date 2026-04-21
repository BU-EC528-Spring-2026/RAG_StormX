-- -----------------------------------------------------------------------
-- Local smoke test for avx_math.so — run this BEFORE ever registering
-- a freshly built .so against the Aerospike cluster.
--
-- Motivation: a bad avx_math.so can segfault the aerospiked process
-- (verified 2026-04-21: SIGSEGV on both nodes after registering an
-- untested rewrite). Running the same C entrypoints against a bare
-- lua5.4 interpreter on the client host reproduces the same crash
-- locally without taking the cluster down.
--
-- Usage:
--   cd SPTAG/AnnService/udf
--   lua5.4 test_avx_math_local.lua            # default: loads ./avx_math.so
--   AVX_MATH_SO=/path/to/avx_math.so lua5.4 test_avx_math_local.lua
--
-- The test only exercises the STRING-accepting entrypoints
-- (batch_candidates_pairs, batch_candidates_packed). The `_ud` variants
-- take Aerospike `as_bytes` userdata and can only be exercised via the
-- server; they share all rank/score/allocation code with the string
-- variants below, so bugs in the hot path still surface here.
-- -----------------------------------------------------------------------
local so_path = os.getenv("AVX_MATH_SO") or "./avx_math.so"

-- Prefer package.loadlib to avoid require's search-path interference.
local init, err = package.loadlib(so_path, "luaopen_avx_math")
if not init then error(("loadlib failed: %s"):format(tostring(err))) end
local avx = init()
assert(type(avx) == "table", "luaopen_avx_math did not return a table")
assert(type(avx.batch_candidates_pairs) == "function",  "missing batch_candidates_pairs")
assert(type(avx.batch_candidates_packed) == "function", "missing batch_candidates_packed")

-- --- helpers -------------------------------------------------------------
local pack, unpack = string.pack, string.unpack

-- Build a posting blob: N entries, each `meta_size + dim * sizeof(value_type)` bytes.
-- For the uint8/L2 + f32/L2 paths we only need:
--   bytes 0..3   : int32 little-endian vid
--   bytes 4..15  : 12-byte metadata filler (matches SPTAG m_metaDataSize=12)
--   bytes 16..   : vector payload
local META_SIZE = 12
local DIM       = 8   -- keeps tests tiny but exercises the AVX-512 remainder loop

local function make_u8_entry(vid, vec)
    local parts = { pack("<i4", vid), string.rep("\0", META_SIZE - 4) }
    for i = 1, DIM do parts[#parts + 1] = string.char(vec[i]) end
    return table.concat(parts)
end

local function make_f32_entry(vid, vec)
    local parts = { pack("<i4", vid), string.rep("\0", META_SIZE - 4) }
    for i = 1, DIM do parts[#parts + 1] = pack("<f", vec[i]) end
    return table.concat(parts)
end

local function bitset_with(vids, n)
    local bytes = math.floor((n + 7) / 8)
    local buf = {}
    for i = 1, bytes do buf[i] = 0 end
    for _, vid in ipairs(vids) do
        local byte_idx = math.floor(vid / 8) + 1
        local bit_idx  = vid % 8
        buf[byte_idx] = (buf[byte_idx] or 0) | (1 << bit_idx)
    end
    local out = {}
    for i = 1, bytes do out[i] = string.char(buf[i]) end
    return table.concat(out)
end

local passed, failed = 0, 0
local function check(name, cond, msg)
    if cond then
        passed = passed + 1
        io.write(("  [PASS] %s\n"):format(name))
    else
        failed = failed + 1
        io.write(("  [FAIL] %s  %s\n"):format(name, msg or ""))
    end
end

-- --- Test 1: uint8 L2, no deletions ------------------------------------
do
    io.write("Test 1: u8 L2, 4 vectors, no deletions, top_n=2\n")
    local vectors = {
        { 1, 2, 3, 4, 5, 6, 7, 8 },        -- vid=10
        { 0, 0, 0, 0, 0, 0, 0, 0 },        -- vid=11
        { 1, 2, 3, 4, 5, 6, 7, 9 },        -- vid=12  (closest)
        { 255,255,255,255,255,255,255,255},-- vid=13
    }
    local posting = make_u8_entry(10, vectors[1])
               .. make_u8_entry(11, vectors[2])
               .. make_u8_entry(12, vectors[3])
               .. make_u8_entry(13, vectors[4])
    local query = string.char(1,2,3,4,5,6,7,8)
    local vec_info_size = META_SIZE + DIM
    local top_n = 2
    local bitset = ""
    local ok, result = pcall(avx.batch_candidates_pairs,
        posting, query, DIM, vec_info_size, META_SIZE, top_n, 0, bitset, 1)
    check("pairs no-deletions returned without error", ok, tostring(result))
    if ok then
        check("pairs returned bytes", type(result) == "string",
              ("type=%s"):format(type(result)))
        check("pairs returned exactly top_n pairs", #result == top_n * 8,
              ("len=%d"):format(#result))
        if #result == top_n * 8 then
            local vid0 = unpack("<i4", result, 1)
            local score0 = unpack("<f", result, 5)
            check("pairs best vid is 10 (exact match)", vid0 == 10,
                  ("vid0=%d score0=%f"):format(vid0, score0))
            check("pairs best score == 0", score0 == 0,
                  ("score0=%f"):format(score0))
        end
    end
end

-- --- Test 2: all vectors deleted ---------------------------------------
do
    io.write("Test 2: u8 L2, 4 vectors all deleted -> kept=0 path\n")
    local posting = make_u8_entry(0, {1,1,1,1,1,1,1,1})
               .. make_u8_entry(1, {2,2,2,2,2,2,2,2})
               .. make_u8_entry(2, {3,3,3,3,3,3,3,3})
               .. make_u8_entry(3, {4,4,4,4,4,4,4,4})
    local query = string.char(1,1,1,1,1,1,1,1)
    local vec_info_size = META_SIZE + DIM
    local bitset = bitset_with({0, 1, 2, 3}, 4)
    local ok, result = pcall(avx.batch_candidates_pairs,
        posting, query, DIM, vec_info_size, META_SIZE, 2, 0, bitset, 1)
    check("pairs all-deleted returned without error", ok, tostring(result))
    if ok then
        check("pairs all-deleted result is empty string",
              type(result) == "string" and #result == 0,
              ("len=%d"):format(#result))
    end

    -- Same for packed
    local ok2, r2 = pcall(avx.batch_candidates_packed,
        posting, query, DIM, vec_info_size, META_SIZE, 2, 0, bitset, 1)
    check("packed all-deleted returned without error", ok2, tostring(r2))
    if ok2 then
        check("packed all-deleted result is empty string",
              type(r2) == "string" and #r2 == 0,
              ("len=%d"):format(#r2))
    end
end

-- --- Test 3: empty posting ---------------------------------------------
do
    io.write("Test 3: empty posting -> kept=0 path\n")
    local query = string.char(0,0,0,0,0,0,0,0)
    local ok, result = pcall(avx.batch_candidates_pairs,
        "", query, DIM, META_SIZE + DIM, META_SIZE, 2, 0, "", 1)
    check("pairs empty posting ok", ok, tostring(result))
    check("pairs empty posting => empty result",
          ok and type(result) == "string" and #result == 0,
          ("len=%s"):format(ok and #result or "?"))

    local ok2, r2 = pcall(avx.batch_candidates_packed,
        "", query, DIM, META_SIZE + DIM, META_SIZE, 2, 0, "", 1)
    check("packed empty posting ok", ok2, tostring(r2))
    check("packed empty posting => empty result",
          ok2 and type(r2) == "string" and #r2 == 0,
          ("len=%s"):format(ok2 and #r2 or "?"))
end

-- --- Test 4: top_n within stack cap (128) ------------------------------
do
    io.write("Test 4: top_n=128 exercises stack buffer\n")
    local N = 200
    local parts = {}
    for i = 0, N-1 do
        parts[#parts + 1] = make_u8_entry(i, {i%256, 0,0,0,0,0,0,0})
    end
    local posting = table.concat(parts)
    local query = string.char(0,0,0,0,0,0,0,0)
    local ok, r = pcall(avx.batch_candidates_pairs,
        posting, query, DIM, META_SIZE + DIM, META_SIZE, 128, 0, "", 1)
    check("pairs top_n=128 ok", ok, tostring(r))
    check("pairs top_n=128 returns 128 pairs",
          ok and #r == 128 * 8,
          ("len=%s"):format(ok and #r or "?"))
end

-- --- Test 5: top_n above stack cap (512 > SPTAG_UDF_MAX_TOPN=256) -------
do
    io.write("Test 5: top_n=512 exercises heap fallback\n")
    local N = 600
    local parts = {}
    for i = 0, N-1 do
        parts[#parts + 1] = make_u8_entry(i, {i%256, 0,0,0,0,0,0,0})
    end
    local posting = table.concat(parts)
    local query = string.char(0,0,0,0,0,0,0,0)
    local ok, r = pcall(avx.batch_candidates_pairs,
        posting, query, DIM, META_SIZE + DIM, META_SIZE, 512, 0, "", 1)
    check("pairs top_n=512 ok (heap path)", ok, tostring(r))
    check("pairs top_n=512 returns 512 pairs",
          ok and #r == 512 * 8,
          ("len=%s"):format(ok and #r or "?"))
end

-- --- Test 6: f32 L2 path ------------------------------------------------
do
    io.write("Test 6: float32 L2, 3 vectors, top_n=1\n")
    local q_vec = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0}
    local posting = make_f32_entry(7, {10, 20, 30, 40, 50, 60, 70, 80})
               .. make_f32_entry(8, q_vec)
               .. make_f32_entry(9, {0, 0, 0, 0, 0, 0, 0, 0})
    local q_parts = {}
    for i = 1, DIM do q_parts[i] = pack("<f", q_vec[i]) end
    local query = table.concat(q_parts)
    local vec_info_size = META_SIZE + DIM * 4
    local ok, r = pcall(avx.batch_candidates_pairs,
        posting, query, DIM, vec_info_size, META_SIZE, 1, 0, "", 3)
    check("f32 L2 ok", ok, tostring(r))
    if ok and #r == 8 then
        local vid0 = unpack("<i4", r, 1)
        local score0 = unpack("<f", r, 5)
        check("f32 L2 picks exact match (vid=8)", vid0 == 8,
              ("vid0=%d score0=%f"):format(vid0, score0))
    else
        check("f32 L2 returned one pair", false,
              ("len=%s"):format(ok and #r or "?"))
    end
end

io.write(("\nResult: %d passed / %d failed\n"):format(passed, failed))
os.exit(failed == 0 and 0 or 1)
