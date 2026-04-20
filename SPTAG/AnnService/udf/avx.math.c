#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <immintrin.h> // For AVX-512
#include <stdint.h>    // For uint32_t
#include <string.h>    // For memcpy
#include <stdlib.h>    // For malloc/free

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
    size_t posting_len, query_len, bitset_len = 0;
    const char *posting      = luaL_checklstring(L, 1, &posting_len);
    const char *query        = luaL_checklstring(L, 2, &query_len);
    int dim                  = luaL_checkinteger(L, 3);
    int vec_info_size        = luaL_checkinteger(L, 4);
    int meta_data_size       = luaL_checkinteger(L, 5);
    int top_n                = luaL_checkinteger(L, 6);
    int dist_mode            = luaL_checkinteger(L, 7);
    const char *bitset       = NULL;
    if (lua_gettop(L) >= 8 && !lua_isnil(L, 8)) {
        bitset = luaL_checklstring(L, 8, &bitset_len);
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
    size_t posting_len, query_len, bitset_len = 0;
    const char *posting      = luaL_checklstring(L, 1, &posting_len);
    const char *query        = luaL_checklstring(L, 2, &query_len);
    int dim                  = luaL_checkinteger(L, 3);
    int vec_info_size        = luaL_checkinteger(L, 4);
    int meta_data_size       = luaL_checkinteger(L, 5);
    int top_n                = luaL_checkinteger(L, 6);
    int dist_mode            = luaL_checkinteger(L, 7);
    const char *bitset       = NULL;
    if (lua_gettop(L) >= 8 && !lua_isnil(L, 8)) {
        bitset = luaL_checklstring(L, 8, &bitset_len);
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
  {"f32_l2",                  f32_l2},
  {"f32_dot",                 f32_dot},
  {"f32_to_bits",             f32_to_bits},
  {"batch_candidates_pairs",  batch_candidates_pairs},
  {"batch_candidates_packed", batch_candidates_packed},
  {NULL, NULL}
};

int luaopen_avx_math(lua_State * L) {
    luaL_newlib(L, avx_lib);
    return 1;
}
