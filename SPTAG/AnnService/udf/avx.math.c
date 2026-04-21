#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <immintrin.h> // For AVX-512
#include <stdint.h>    // For uint32_t
#include <string.h>    // For memcpy, strcmp
#include <stdlib.h>    // For malloc/free
#include <dlfcn.h>     // For dlsym(RTLD_DEFAULT, ...) to resolve asd's as_bytes_* exports

// ---------------------------------------------------------------------------
// Zero-copy bytes support
//
// Aerospike's mod-lua wraps `as_bytes *` as a Lua userdata (struct mod_lua_box
// with an 8-byte payload pointer). The upstream metatable name is "Bytes".
// By resolving as_bytes_get / as_bytes_size from the asd process at runtime
// (via dlsym) we can read the raw posting buffer without the Lua wrapper
// having to call `bytes.get_string(posting, 1, total)` — which allocates a
// fresh Lua string on every UDF invocation (tens of KB per posting).
//
// Both entry points (string and userdata) go through `resolve_blob`, so the
// same compiled function handles either calling convention; the Lua wrapper
// feature-detects on the new `_ud` alias names.
// ---------------------------------------------------------------------------

typedef struct mod_lua_box_s {
    uint8_t   scope;
    void     *value;   // as_bytes *
} mod_lua_box;

// Function pointers resolved lazily from the host process (asd). We don't
// link against libaerospike-client.so — asd already exports these and the
// dlsym(RTLD_DEFAULT, ...) lookup returns the hosting process's copy.
static const uint8_t * (*p_as_bytes_get)(const void *)  = NULL;
static uint32_t        (*p_as_bytes_size)(const void *) = NULL;
static int             as_bytes_api_resolved           = 0;

static int resolve_as_bytes_api(void) {
    if (as_bytes_api_resolved) return p_as_bytes_get && p_as_bytes_size;
    as_bytes_api_resolved = 1;
    p_as_bytes_get  = (const uint8_t * (*)(const void *))dlsym(RTLD_DEFAULT, "as_bytes_get");
    p_as_bytes_size = (uint32_t (*)(const void *))       dlsym(RTLD_DEFAULT, "as_bytes_size");
    return p_as_bytes_get && p_as_bytes_size;
}

// Try to interpret stack slot `idx` as a mod-lua bytes userdata. Returns 1
// and fills (*out_ptr, *out_size) on success, 0 otherwise. We probe several
// plausible metatable names because mod-lua's registered name has varied
// across releases ("Bytes" in current mod-lua; some older builds use
// "as_bytes"). Falls through to 0 if nothing matches or dlsym failed.
static int as_bytes_from_ud(lua_State *L, int idx,
                            const uint8_t **out_ptr, size_t *out_size) {
    if (lua_type(L, idx) != LUA_TUSERDATA) return 0;
    if (!resolve_as_bytes_api()) return 0;

    static const char * const names[] = { "Bytes", "as_bytes", NULL };
    void *ud = NULL;
    for (int i = 0; names[i] != NULL; ++i) {
        ud = luaL_testudata(L, idx, names[i]);
        if (ud != NULL) break;
    }
    if (ud == NULL) return 0;

    const mod_lua_box *box = (const mod_lua_box *)ud;
    if (box->value == NULL) return 0;
    *out_ptr  = p_as_bytes_get(box->value);
    *out_size = (size_t)p_as_bytes_size(box->value);
    if (*out_ptr == NULL) return 0;
    return 1;
}

// Accepts the arg at `idx` as either a Lua string OR a mod-lua Bytes userdata.
// When `optional` is non-zero, nil / missing returns an empty view (ptr=NULL,
// size=0). Returns 0 on unrecognized type, leaving Lua to raise the error.
static int resolve_blob(lua_State *L, int idx,
                        const char **out_ptr, size_t *out_size,
                        int optional) {
    int t = (idx <= lua_gettop(L)) ? lua_type(L, idx) : LUA_TNONE;
    if (optional && (t == LUA_TNONE || t == LUA_TNIL)) {
        *out_ptr = NULL; *out_size = 0;
        return 1;
    }
    if (t == LUA_TSTRING) {
        *out_ptr = lua_tolstring(L, idx, out_size);
        return 1;
    }
    if (t == LUA_TUSERDATA) {
        const uint8_t *p = NULL; size_t sz = 0;
        if (as_bytes_from_ud(L, idx, &p, &sz)) {
            *out_ptr  = (const char *)p;
            *out_size = sz;
            return 1;
        }
    }
    return 0;
}

// Helper matching SPTAG's _mm512_sqdf_ps logic
static inline __m512 sptag_sqdf_ps(__m512 X, __m512 Y) {
    __m512 d = _mm512_sub_ps(X, Y);
    return _mm512_mul_ps(d, d);
}

// Calculates L2 distance strictly matching SPTAG logic
static int f32_l2(lua_State *L) {
    size_t posting_len, query_len;
    const char *posting = luaL_checklstring(L, 1, &posting_len);
    int offset = luaL_checkinteger(L, 2) - 1; 
    const char *query = luaL_checklstring(L, 3, &query_len);
    int dim = luaL_checkinteger(L, 4);

    const float *v1 = (const float *)(posting + offset);
    const float *v2 = (const float *)query;
    
    int i = 0;
    __m512 diff512 = _mm512_setzero_ps();
    
    // Core AVX-512 loop
    for (; i <= dim - 16; i += 16) {
        __m512 a = _mm512_loadu_ps(&v1[i]);
        __m512 b = _mm512_loadu_ps(&v2[i]);
        diff512 = _mm512_add_ps(diff512, sptag_sqdf_ps(a, b));
    }

    // Modern compiler intrinsic to sum all 16 lanes into a single scalar
    float distance = _mm512_reduce_add_ps(diff512);

    // Scalar Tail processing (Mimicking SPTAG's lowest level scalar fallback)
    for (; i < dim; ++i) {
        float diff = v1[i] - v2[i];
        distance += diff * diff;
    }

    lua_pushnumber(L, distance);
    return 1;
}

// Calculates Cosine (Dot Product) strictly matching SPTAG logic
static int f32_dot(lua_State *L) {
    size_t posting_len, query_len;
    const char *posting = luaL_checklstring(L, 1, &posting_len);
    int offset = luaL_checkinteger(L, 2) - 1; 
    const char *query = luaL_checklstring(L, 3, &query_len);
    int dim = luaL_checkinteger(L, 4);

    const float *v1 = (const float *)(posting + offset);
    const float *v2 = (const float *)query;

    int i = 0;
    __m512 diff512 = _mm512_setzero_ps();
    
    // Core AVX-512 loop
    for (; i <= dim - 16; i += 16) {
        __m512 a = _mm512_loadu_ps(&v1[i]);
        __m512 b = _mm512_loadu_ps(&v2[i]);
        // SPTAG explicitly uses mul_ps then add_ps, not FMA
        __m512 mult = _mm512_mul_ps(a, b);
        diff512 = _mm512_add_ps(diff512, mult);
    }

    // Modern compiler intrinsic to sum all 16 lanes into a single scalar
    float dot = _mm512_reduce_add_ps(diff512);

    // Scalar Tail
    for (; i < dim; ++i) {
        dot += v1[i] * v2[i];
    }

    // SPTAG Cosine implementation returns 1 - diff
    lua_pushnumber(L, 1.0f - dot);
    return 1;
}

// Fast float to IEEE 754 int32 bitcast.
// Pushes the bit pattern as a *signed* int32 so it round-trips cleanly
// through Aerospike's bytes.set_int32 on the Lua side. Matches the
// Lua-5.3+ fallback `string.unpack("<i4", string.pack("<f", f))`.
static int f32_to_bits(lua_State *L) {
    float f = (float)luaL_checknumber(L, 1);
    int32_t bits;
    // memcpy avoids strict-aliasing UB; reinterpret as signed.
    memcpy(&bits, &f, sizeof(float));
    lua_pushinteger(L, (lua_Integer)bits);
    return 1;
}

// AVX-512 L2 distance for UInt8 vectors (e.g. SIFT). Processes 32 dimensions
// per iteration: widens u8 -> i16, computes (a - b), then `_mm512_madd_epi16`
// horizontally squares-and-adds adjacent i16 lanes into i32 accumulators.
static inline float u8_l2(const unsigned char *v1, const unsigned char *v2, int dim) {
    int i = 0;
    __m512i acc = _mm512_setzero_si512();
    for (; i <= dim - 32; i += 32) {
        __m256i a8 = _mm256_loadu_si256((const __m256i *)(v1 + i));
        __m256i b8 = _mm256_loadu_si256((const __m256i *)(v2 + i));
        __m512i a  = _mm512_cvtepu8_epi16(a8);
        __m512i b  = _mm512_cvtepu8_epi16(b8);
        __m512i d  = _mm512_sub_epi16(a, b);
        acc = _mm512_add_epi32(acc, _mm512_madd_epi16(d, d));
    }
    int distance = _mm512_reduce_add_epi32(acc);
    for (; i < dim; ++i) {
        int diff = (int)v1[i] - (int)v2[i];
        distance += diff * diff;
    }
    return (float)distance;
}

// Test bit `vid` in a packed bitset (LSB-first within each byte).
// Matches the layout produced by Lua `is_deleted_by_bitset` and by
// the C++ `BuildDeletedBitset()` helper.
static inline int bitset_test(const unsigned char *bits, size_t bits_size, int vid) {
    if (bits == NULL || bits_size == 0 || vid < 0) return 0;
    size_t byte_idx = (size_t)vid >> 3;
    if (byte_idx >= bits_size) return 0;
    return (bits[byte_idx] >> (vid & 7)) & 1;
}

// Processes the ENTIRE posting list in one C call:
//   - Filters deleted vectors via the optional bitset (no Lua pre-pass needed)
//   - Scores every surviving vector with AVX-512 (L2 or Dot)
//   - Maintains a top-n insertion-sorted ranking entirely in C
//   - Returns the results packed as a raw byte string: N x (int32 vid | float32 score)
//
// Args (Lua stack):
//   1. posting (string)    — raw bytes of the posting list
//   2. query   (string)    — raw bytes of the query vector
//   3. dim     (int)       — vector dimensionality
//   4. vec_info_size (int) — total bytes per posting entry
//   5. meta_data_size (int)— bytes of metadata before the float vector in each entry
//   6. top_n   (int)       — max candidates to return
//   7. dist_mode (int)     — 1 = Cosine/Dot  (returns 1-dot),  else = L2
//   8. deleted_bitset (string, optional) — packed bit-per-VID; "" disables filtering
//   9. value_type (int, optional) — SPTAG VectorValueType: 1=UInt8, 3=Float (default)
//      UInt8 only supports L2; Cosine on UInt8 returns "" so Lua can fall back.
static int batch_candidates_pairs(lua_State *L) {
    size_t posting_len = 0, query_len = 0, bitset_len = 0;
    const char *posting = NULL;
    const char *query   = NULL;
    const char *bitset  = NULL;

    if (!resolve_blob(L, 1, &posting, &posting_len, 0)) {
        return luaL_error(L, "batch_candidates_pairs: arg 1 (posting) must be string or Bytes userdata");
    }
    if (!resolve_blob(L, 2, &query, &query_len, 0)) {
        return luaL_error(L, "batch_candidates_pairs: arg 2 (query) must be string or Bytes userdata");
    }
    int dim            = luaL_checkinteger(L, 3);
    int vec_info_size  = luaL_checkinteger(L, 4);
    int meta_data_size = luaL_checkinteger(L, 5);
    int top_n          = luaL_checkinteger(L, 6);
    int dist_mode      = luaL_checkinteger(L, 7);
    if (!resolve_blob(L, 8, &bitset, &bitset_len, 1)) {
        return luaL_error(L, "batch_candidates_pairs: arg 8 (bitset) must be string, Bytes userdata, or nil");
    }
    int value_type = 3;
    if (lua_gettop(L) >= 9 && !lua_isnil(L, 9)) {
        value_type = luaL_checkinteger(L, 9);
    }
    // UInt8 + Cosine isn't implemented in C: signal the Lua wrapper to fall back.
    if (value_type == 1 && dist_mode == 1) {
        lua_pushlstring(L, "", 0);
        return 1;
    }

    int vec_count = (int)(posting_len / vec_info_size);
    if (vec_count == 0 || top_n <= 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }

    // Allocate top-n ranking arrays
    int   *top_vids   = (int *)  malloc(top_n * sizeof(int));
    float *top_scores = (float *)malloc(top_n * sizeof(float));
    if (!top_vids || !top_scores) {
        free(top_vids);
        free(top_scores);
        return luaL_error(L, "batch_candidates_pairs: out of memory");
    }

    int kept = 0;
    const float *v2 = (const float *)query;

    for (int v = 0; v < vec_count; v++) {
        int byte_offset = v * vec_info_size;

        // Extract VID from the start of the entry (little-endian int32)
        int vid;
        memcpy(&vid, posting + byte_offset, sizeof(int));

        if (bitset_test((const unsigned char *)bitset, bitset_len, vid)) {
            continue;
        }

        // Vector data starts after the metadata header
        const char *vec_bytes = posting + byte_offset + meta_data_size;

        float score = 0.0f;
        if (value_type == 1) {
            // UInt8 L2 (e.g. SIFT): query is also raw u8 bytes
            score = u8_l2((const unsigned char *)vec_bytes,
                          (const unsigned char *)query, dim);
        } else {
            const float *v1 = (const float *)vec_bytes;
            int i = 0;
            __m512 acc = _mm512_setzero_ps();
            if (dist_mode == 1) {
                for (; i <= dim - 16; i += 16) {
                    __m512 a = _mm512_loadu_ps(&v1[i]);
                    __m512 b = _mm512_loadu_ps(&v2[i]);
                    acc = _mm512_add_ps(acc, _mm512_mul_ps(a, b));
                }
                float dot = _mm512_reduce_add_ps(acc);
                for (; i < dim; ++i) dot += v1[i] * v2[i];
                score = 1.0f - dot;
            } else {
                for (; i <= dim - 16; i += 16) {
                    __m512 a = _mm512_loadu_ps(&v1[i]);
                    __m512 b = _mm512_loadu_ps(&v2[i]);
                    acc = _mm512_add_ps(acc, sptag_sqdf_ps(a, b));
                }
                score = _mm512_reduce_add_ps(acc);
                for (; i < dim; ++i) {
                    float diff = v1[i] - v2[i];
                    score += diff * diff;
                }
            }
        }

        // C-level insertion sort (ascending: best score at index 0)
        if (kept < top_n || score < top_scores[kept - 1]) {
            int pos = kept;
            if (kept < top_n) kept++;
            while (pos > 0 && score < top_scores[pos - 1]) {
                if (pos < top_n) {
                    top_scores[pos] = top_scores[pos - 1];
                    top_vids[pos]   = top_vids[pos - 1];
                }
                pos--;
            }
            top_scores[pos] = score;
            top_vids[pos]   = vid;
        }
    }

    // Pack results: each pair is 4 bytes (int32 vid) + 4 bytes (float32 score bits)
    int   out_size   = kept * 8;
    char *out_buffer = (char *)malloc(out_size);
    if (!out_buffer) {
        free(top_vids);
        free(top_scores);
        return luaL_error(L, "batch_candidates_pairs: out of memory for output");
    }

    for (int i = 0; i < kept; i++) {
        int p = i * 8;
        memcpy(out_buffer + p,     &top_vids[i],   4);
        memcpy(out_buffer + p + 4, &top_scores[i], 4);
    }

    lua_pushlstring(L, out_buffer, out_size);

    free(top_vids);
    free(top_scores);
    free(out_buffer);
    return 1;
}

// Same scoring/filtering loop as batch_candidates_pairs, but instead of returning
// (vid|score) tuples, it returns the raw posting bytes for the top-N survivors —
// i.e. `top_n * vec_info_size` bytes laid out exactly like the input. The C++
// caller (Packed mode) re-runs ComputeDistance on these bytes for full precision.
//
// Args (Lua stack):
//   1..7 — same as batch_candidates_pairs
//   8    — deleted_bitset (string, optional)

static int batch_candidates_packed(lua_State *L) {
    size_t posting_len = 0, query_len = 0, bitset_len = 0;
    const char *posting = NULL;
    const char *query   = NULL;
    const char *bitset  = NULL;

    if (!resolve_blob(L, 1, &posting, &posting_len, 0)) {
        return luaL_error(L, "batch_candidates_packed: arg 1 (posting) must be string or Bytes userdata");
    }
    if (!resolve_blob(L, 2, &query, &query_len, 0)) {
        return luaL_error(L, "batch_candidates_packed: arg 2 (query) must be string or Bytes userdata");
    }
    int dim            = luaL_checkinteger(L, 3);
    int vec_info_size  = luaL_checkinteger(L, 4);
    int meta_data_size = luaL_checkinteger(L, 5);
    int top_n          = luaL_checkinteger(L, 6);
    int dist_mode      = luaL_checkinteger(L, 7);
    if (!resolve_blob(L, 8, &bitset, &bitset_len, 1)) {
        return luaL_error(L, "batch_candidates_packed: arg 8 (bitset) must be string, Bytes userdata, or nil");
    }
    int value_type = 3;
    if (lua_gettop(L) >= 9 && !lua_isnil(L, 9)) {
        value_type = luaL_checkinteger(L, 9);
    }
    if (value_type == 1 && dist_mode == 1) {
        lua_pushlstring(L, "", 0);
        return 1;
    }

    int vec_count = (int)(posting_len / vec_info_size);
    if (vec_count == 0 || top_n <= 0 || vec_info_size <= 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }

    int   *top_offsets = (int *)  malloc(top_n * sizeof(int));
    float *top_scores  = (float *)malloc(top_n * sizeof(float));
    if (!top_offsets || !top_scores) {
        free(top_offsets);
        free(top_scores);
        return luaL_error(L, "batch_candidates_packed: out of memory");
    }

    int kept = 0;
    const float *v2 = (const float *)query;

    for (int v = 0; v < vec_count; v++) {
        int byte_offset = v * vec_info_size;

        int vid;
        memcpy(&vid, posting + byte_offset, sizeof(int));

        if (bitset_test((const unsigned char *)bitset, bitset_len, vid)) {
            continue;
        }

        const char *vec_bytes = posting + byte_offset + meta_data_size;

        float score = 0.0f;
        if (value_type == 1) {
            score = u8_l2((const unsigned char *)vec_bytes,
                          (const unsigned char *)query, dim);
        } else {
            const float *v1 = (const float *)vec_bytes;
            int i = 0;
            __m512 acc = _mm512_setzero_ps();
            if (dist_mode == 1) {
                for (; i <= dim - 16; i += 16) {
                    __m512 a = _mm512_loadu_ps(&v1[i]);
                    __m512 b = _mm512_loadu_ps(&v2[i]);
                    acc = _mm512_add_ps(acc, _mm512_mul_ps(a, b));
                }
                float dot = _mm512_reduce_add_ps(acc);
                for (; i < dim; ++i) dot += v1[i] * v2[i];
                score = 1.0f - dot;
            } else {
                for (; i <= dim - 16; i += 16) {
                    __m512 a = _mm512_loadu_ps(&v1[i]);
                    __m512 b = _mm512_loadu_ps(&v2[i]);
                    acc = _mm512_add_ps(acc, sptag_sqdf_ps(a, b));
                }
                score = _mm512_reduce_add_ps(acc);
                for (; i < dim; ++i) {
                    float diff = v1[i] - v2[i];
                    score += diff * diff;
                }
            }
        }

        if (kept < top_n || score < top_scores[kept - 1]) {
            int pos = kept;
            if (kept < top_n) kept++;
            while (pos > 0 && score < top_scores[pos - 1]) {
                if (pos < top_n) {
                    top_scores[pos]  = top_scores[pos - 1];
                    top_offsets[pos] = top_offsets[pos - 1];
                }
                pos--;
            }
            top_scores[pos]  = score;
            top_offsets[pos] = byte_offset;
        }
    }

    int   out_size   = kept * vec_info_size;
    char *out_buffer = (char *)malloc(out_size > 0 ? out_size : 1);
    if (!out_buffer) {
        free(top_offsets);
        free(top_scores);
        return luaL_error(L, "batch_candidates_packed: out of memory for output");
    }

    for (int i = 0; i < kept; i++) {
        memcpy(out_buffer + i * vec_info_size,
               posting + top_offsets[i],
               vec_info_size);
    }

    lua_pushlstring(L, out_buffer, out_size);

    free(top_offsets);
    free(top_scores);
    free(out_buffer);
    return 1;
}

static const struct luaL_Reg avx_lib [] = {
  {"f32_l2",                     f32_l2},
  {"f32_dot",                    f32_dot},
  {"f32_to_bits",                f32_to_bits},
  {"batch_candidates_pairs",     batch_candidates_pairs},
  {"batch_candidates_packed",    batch_candidates_packed},
  // Zero-copy aliases: accept either a Lua string or an Aerospike `Bytes`
  // userdata for the posting/query/bitset blobs. Presence of these names in
  // the loaded module is the feature-flag the Lua wrapper uses to decide
  // whether it can skip the per-call `bytes.get_string(...)` copies.
  {"batch_candidates_pairs_ud",  batch_candidates_pairs},
  {"batch_candidates_packed_ud", batch_candidates_packed},
  {NULL, NULL}
};

int luaopen_avx_math(lua_State * L) {
    luaL_newlib(L, avx_lib);
    return 1;
}
