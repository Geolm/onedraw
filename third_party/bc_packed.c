#include "bc_packed.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>


#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    #include <arm_neon.h>
    #define BCP_NEON
#endif

#if defined(_MSC_VER)
    #include <intrin.h>
    #pragma intrinsic(__popcnt)
    #pragma intrinsic(__popcnt64)
    #define popcount64(x) ((int)__popcnt64(x))
    #define popcount(x) ((int)__popcnt(x))
#else
    #define popcount64(x) __builtin_popcountll(x)
    #define popcount(x) __builtin_popcount(x)
#endif


//-----------------------------------------------------------------------------------------------------------------------------
// ceil(log2(n)) for n in [1, 256] (0 for n <= 1): the bit width of a fixed-width code for n symbols
static inline uint8_t le_ceil_log2(uint32_t n)
{
    uint8_t bits = 0;
    uint32_t value = 1;
    while (value < n)
    {
        value <<= 1;
        bits++;
    }
    return bits;
}


//-----------------------------------------------------------------------------------------------------------------------------
// Static Rice-Golomb entropy coder, inlined from the lite_encoding library.
//
// Every model is static: the rank table, the Rice parameter k, and the raw-byte escape
// q_escape are derived from a histogram before encoding and never adapt during the stream.
// Only the static encode/decode path is kept: there is no move-to-front, no k trend, and
// no literal/delta helpers, because the compressor only ever encodes pre-ranked symbols.
//
// Bitstream: a 64-bit reservoir buffers bits to keep I/O at byte granularity.
// Each symbol is written as q = rank >> k unary ones, a zero, then k remainder bits; a rank
// whose q reaches q_escape is written as q_escape unary bits, a zero, then the raw rank byte.
//
// A near-uniform histogram makes Rice uneconomical, so such a model falls back to a raw
// fixed-width code: bit 7 of k is set and the lower bits hold ceil(log2(num_symbols)), the
// number of bits written per symbol. The decoder mirrors this and reads that many bits back.
// A single-symbol model keeps the Rice path, as a 0-bit raw code would emit an empty payload.
//-----------------------------------------------------------------------------------------------------------------------------

#define LE_ALPHABET_SIZE (256U)
#define LE_MAX_K (8)
#define LE_K_RAW (1 << 7)

#ifdef _MSC_VER
    #pragma intrinsic(_BitScanForward64)
    static inline uint32_t le_ctz64(uint64_t mask)
    {
        unsigned long index;
        _BitScanForward64(&index, mask);
        return (uint32_t)index;
    }
#else
    #define le_ctz64(mask) (uint32_t)__builtin_ctzll(mask)
#endif

typedef enum le_status
{
    LE_OK = 0,
    LE_BUFFER_OVERRUN = -1
} le_status;

typedef struct le_stream
{
    uint8_t* buffer;
    size_t position;
    size_t size;

    uint64_t bit_reservoir;
    uint32_t bits_available;

    le_status status;
} le_stream;

typedef struct le_model
{
    uint8_t alphabet[LE_ALPHABET_SIZE];
    uint8_t index[LE_ALPHABET_SIZE];
    uint8_t k;          // rice k-value
    uint8_t q_escape;
    uint16_t num_symbols;
} le_model;

//-----------------------------------------------------------------------------------------------------------------------------
// pull bytes into the reservoir until it's full enough for any standard read
static inline void le_refill(le_stream* s)
{
    while (s->bits_available <= 56 && s->position < s->size)
    {
        s->bit_reservoir |= ((uint64_t)s->buffer[s->position]) << s->bits_available;
        s->bits_available += 8;
        s->position++;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void le_flush(le_stream* s)
{
    while (s->bits_available >= 8)
    {
        if (s->position >= s->size)
        {
            s->status = LE_BUFFER_OVERRUN;
            return;
        }
        s->buffer[s->position] = (uint8_t)(s->bit_reservoir & 0xFF);
        s->bit_reservoir >>= 8;
        s->bits_available -= 8;
        s->position++;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void le_init(le_stream *s, void* buffer, size_t size)
{
    s->buffer = (uint8_t*)buffer;
    s->size = size;
    s->position = 0;
    s->bit_reservoir = 0;
    s->bits_available = 0;
    s->status = LE_OK;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void le_begin_encode(le_stream* s)
{
    s->position = 0;
    s->bit_reservoir = 0;
    s->bits_available = 0;
    s->status = LE_OK;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline size_t le_end_encode(le_stream* s)
{
    if (s->status != LE_OK) 
        return 0;

    size_t bytes_to_write = (s->bits_available + 7) / 8;
    if (s->position + bytes_to_write > s->size)
    {
        s->status = LE_BUFFER_OVERRUN;
        return 0;
    }

    while (s->bits_available > 0)
    {
        s->buffer[s->position] = (uint8_t)(s->bit_reservoir & 0xFF);
        s->bit_reservoir >>= 8;
        s->position++;
        
        if (s->bits_available > 8)
            s->bits_available -= 8;
        else
            s->bits_available = 0;
    }

    return s->position;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void le_begin_decode(le_stream* s)
{
    s->position = 0;
    s->bit_reservoir = 0;
    s->bits_available = 0;
    s->status = LE_OK;
    le_refill(s);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void le_write_bits(le_stream* s, uint64_t data, uint8_t num_bits)
{
    s->bit_reservoir |= (data & ((1ULL << num_bits) - 1ULL)) << s->bits_available;
    s->bits_available += num_bits;
    if (s->bits_available >= 32)
        le_flush(s);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void le_write_byte(le_stream* s, uint8_t value)
{
    s->bit_reservoir |= ((uint64_t)value << s->bits_available);
    s->bits_available += 8;
    if (s->bits_available >= 32)
        le_flush(s);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint8_t le_read_byte(le_stream* s)
{
    if (s->bits_available < 8)
        le_refill(s);

    if (s->bits_available < 8) 
    {
        s->status = LE_BUFFER_OVERRUN;
        return 0; 
    }

    uint8_t value = (uint8_t)(s->bit_reservoir & 0xFF);
    s->bit_reservoir >>= 8;
    s->bits_available -= 8;
    return value;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint8_t le_read_bits(le_stream* s, uint8_t num_bits)
{
    if (s->bits_available < num_bits)
    {
        le_refill(s);
        if (s->bits_available < num_bits)
        {
            s->status = LE_BUFFER_OVERRUN;
            return 0;
        }
    }

    uint8_t value = (uint8_t)(s->bit_reservoir & ((1U << num_bits) - 1U));
    s->bit_reservoir >>= num_bits;
    s->bits_available -= num_bits;
    return value;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Builds a static model from a histogram: sorts symbols by frequency (most frequent first,
// ties broken by the lower symbol), then searches (k, q_escape) jointly to minimize the
// model's total bit count. With q_escape = Q, every rank with q = rank >> k >= Q is written
// as Q unary bits + a zero + a raw byte (Q + 9 bits); the others use normal rice (q + 1 + k bits).
static inline void le_static_model_init(le_model *model, const uint32_t* histogram, uint32_t num_symbols)
{
    assert(num_symbols && num_symbols <= LE_ALPHABET_SIZE);

    typedef struct 
    {
        uint8_t symbol;
        uint32_t count;
    } le_sym_freq;

    uint32_t total_count = 0;
    le_sym_freq freq_table[LE_ALPHABET_SIZE];
    for (uint32_t i = 0; i < LE_ALPHABET_SIZE; ++i)
    {
        freq_table[i].symbol = (uint8_t)i;
        freq_table[i].count = (i < num_symbols && histogram) ? histogram[i] : 0;
        total_count += freq_table[i].count;
    }

    for (uint32_t i = 1; i < LE_ALPHABET_SIZE; ++i)
    {
        le_sym_freq key = freq_table[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0 && (freq_table[j].count < key.count || 
              (freq_table[j].count == key.count && freq_table[j].symbol > key.symbol)))
        {
            freq_table[j + 1] = freq_table[j];
            j--;
        }
        freq_table[j + 1] = key;
    }

    for (uint32_t i = 0; i < LE_ALPHABET_SIZE; ++i)
    {
        uint8_t sym = freq_table[i].symbol;
        model->alphabet[i] = sym;
        model->index[sym] = (uint8_t)i;
    }

    uint8_t best_k = 2;
    uint8_t best_q_escape = 4;
    uint64_t min_total_bits = UINT64_MAX;

    uint64_t q_count[LE_ALPHABET_SIZE]; // rank counts bucketed by q = rank >> k

    for (uint8_t candidate_k = 0; candidate_k < 8; ++candidate_k)
    {
        uint32_t max_q = 0;
        uint64_t suffix = 0; // count of the ranks with q >= current Q

        memset(q_count, 0, sizeof(q_count));
        for (uint32_t index = 0; index < LE_ALPHABET_SIZE; ++index)
        {
            uint32_t count = freq_table[index].count;
            if (count == 0) continue;

            uint32_t q = index >> candidate_k;
            q_count[q] += count;
            suffix += count;
            if (q > max_q)
                max_q = q;
        }

        // beyond the largest q that occurs the escape never fires, so Q = max_q + 1 costs the same as any
        // larger Q. the decoder refills its 64-bit reservoir only below 32 bits, so every read must stay
        // under 32 bits: Q + 1 for the escape flag, q + 1 + k for the rice value
        uint32_t max_q_escape = max_q + 1;
        if (max_q_escape > 31)
            max_q_escape = 31;
        if (max_q_escape > 32u - candidate_k)
            max_q_escape = 32u - candidate_k;

        uint64_t prefix = 0; // bits of the ranks with q < current Q, written with normal rice
        for (uint32_t q = 1; q <= max_q_escape; ++q)
        {
            suffix -= q_count[q - 1];
            prefix += q_count[q - 1] * (q + candidate_k); // a rank with q' = q - 1 costs q' + 1 + k = q + k bits

            uint64_t total_bits = suffix * (q + 9) + prefix;
            if (total_bits < min_total_bits)
            {
                min_total_bits = total_bits;
                best_k = candidate_k;
                best_q_escape = (uint8_t)q;
            }
        }
    }

    // compute the actual number of symbols as it could be lower if some symbols are not in stream
    uint32_t i=0;
    while (i<num_symbols && freq_table[i].count > 0)
        i++;
    
    model->num_symbols = (uint16_t)i;

    // a near-uniform histogram is cheaper with a raw fixed-width code: every symbol costs
    // ceil(log2(num_symbols)) bits, which beats the best Rice code found above in that case.
    // flag it in bit 7 of k and store the bit width in the lower bits. a single-symbol model
    // is excluded: its raw cost is 0 bits, which would emit an empty payload and break the
    // stream's strict length invariant, so it keeps the (lossless) 1-bit Rice path.
    uint8_t raw_bits = le_ceil_log2(model->num_symbols);
    if ((model->num_symbols > 1) && (min_total_bits > (uint64_t)raw_bits * total_count))
    {
        model->k = (uint8_t)(LE_K_RAW | raw_bits);
        model->q_escape = 0;
    }
    else
    {
        model->k = best_k;
        model->q_escape = best_q_escape;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// Restores a model from the stream: rebuilds the rank table and the symbol-to-rank index.
static inline void le_static_model_load(le_model *model, const uint8_t* alphabet, uint32_t num_symbols, uint8_t k, uint8_t q_escape)
{
    *model = (le_model) {0};
    memcpy(model->alphabet, alphabet, num_symbols);
    model->k = k;
    model->q_escape = q_escape;
    model->num_symbols = (uint16_t)num_symbols;

    for (uint32_t i = 0; i < num_symbols; ++i)
        model->index[model->alphabet[i]] = (uint8_t)i;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void rice_encode(le_stream *s, uint32_t value, uint8_t k, uint8_t q_escape) 
{
    uint32_t q = value >> k;
    uint32_t q_limit = q_escape;
    uint32_t r = value & ((1U << k) - 1U);

    // checks if raw value is cheaper
    q = (q >= q_limit) ? q_limit : q;

    // unary prefix: q ones followed by a zero
    le_write_bits(s, (1ULL << q) - 1ULL, (uint8_t)(q + 1));

    // remainder or rawbyte
    if (q == q_limit)
        le_write_byte(s, value);
    else if (k > 0) 
        le_write_bits(s, (uint8_t)r, k);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint8_t rice_decode(le_stream *s, uint8_t k, uint8_t q_escape) 
{
    if (s->bits_available < 32) 
        le_refill(s);

    uint32_t q = le_ctz64(~s->bit_reservoir | (1ULL << 63));
    uint32_t q_limit = q_escape;

    if (q >= q_limit)
    {
        if (s->bits_available < (q_limit + 1)) 
        {
            s->status = LE_BUFFER_OVERRUN;
            return 0;
        }
        s->bit_reservoir >>= (q_limit + 1);
        s->bits_available -= (q_limit + 1);
        return le_read_byte(s);
    }

    uint32_t total_bits = q + 1 + k;
    if (s->bits_available < total_bits) 
    {
        s->status = LE_BUFFER_OVERRUN;
        return 0;
    }

    uint32_t val = s->bit_reservoir;
    
    s->bit_reservoir >>= total_bits;
    s->bits_available -= total_bits;

    uint32_t r = (val >> (q + 1)) & ((1U << k) - 1);
    return (uint8_t)((q << k) | r);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Encodes [value] by its rank in the model: a static model never changes, so the rank is the symbol.
// In raw mode (bit 7 of k) the rank is written as a fixed-width bit field, otherwise with Rice.
static inline void le_encode_symbol(le_stream *s, le_model *model, uint8_t value)
{
    uint32_t index = model->index[value];

    if (model->k & LE_K_RAW)
        le_write_bits(s, index, (uint8_t)(model->k & 0x7f));
    else
        rice_encode(s, index, model->k, model->q_escape);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Decodes one symbol: read the rank (fixed-width in raw mode, otherwise Rice), then map it back
// through the rank table.
static inline uint8_t le_decode_symbol(le_stream *restrict s, le_model *restrict model)
{
    uint8_t index;

    if (model->k & LE_K_RAW)
        index = le_read_bits(s, (uint8_t)(model->k & 0x7f));
    else
        index = rice_decode(s, model->k, model->q_escape);

    return model->alphabet[index];
}

//-----------------------------------------------------------------------------------------------------------------------------
// Bit cost that le_encode_symbol would write for [value] without writing it: a fixed field in raw
// mode, otherwise the Rice cost (a rank whose q reached q_escape costs the escape bits + a raw byte)
static inline uint32_t le_symbol_cost(const le_model* model, uint8_t value)
{
    uint32_t index = model->index[value];

    if (model->k & LE_K_RAW)
        return model->k & 0x7f;

    uint32_t q = index >> model->k;
    if (q >= model->q_escape)
        return model->q_escape + 1 + 8;

    return q + 1 + model->k;
}


//-----------------------------------------------------------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------------------------------------------------------

#define HASHMAP_SIZE            (1U << 20U)
#define TOP_TABLE_SIZE          (256U)
#define COLOR_DELTA_NUM_BITS    (7)
#define COLOR_DELTA_OFFSET      (1 << (COLOR_DELTA_NUM_BITS-1))

// contextual color models: number of context buckets on the (6-bit) green delta, see color_delta_context
#define COLOR_DELTA_CONTEXTS    (3)

// sliding dictionary of endpoint color pairs: a block whose two endpoint colors exactly match one of
// the previous COLOR_DICT_SIZE blocks of its zigzag chain is written as a 1-bit flag + a reference
// to that block, instead of the six color-delta symbols (see color_dict_match / color_ref_bits)
#define COLOR_DICT_SIZE         (256U)
#define COLOR_DICT_MASK         (COLOR_DICT_SIZE - 1U)

// bc4/bc5 contextual index models: an 8 x 8 [left][top] matrix keyed on the decoded 3-bit
// indices of the two neighbors of the pixel, and the channel count (BC4 = 1 channel, BC5 = red + green)
#define BC4_INDEX_DIM           (8)
#define BC4_INDEX_CONTEXTS      (BC4_INDEX_DIM * BC4_INDEX_DIM)
#define BCP_MAX_CHANNELS        (2)


//-----------------------------------------------------------------------------------------------------------------------------
// Structures
//-----------------------------------------------------------------------------------------------------------------------------
typedef struct hashmap_entry
{
    uint32_t key;
    uint32_t count;
} hashmap_entry;

struct bcp_context
{
    hashmap_entry* hashmap;
    bcp_mem_interface mem;

    // contextual color models: red/blue carry one model per green-delta context bucket (the chroma deltas
    // scale with the endpoint's own luminance delta). The first endpoint's green model is uncontexted,
    // the second is contextual on the sign of the first endpoint's green delta (negative / zero / positive)
    le_model red_model[2][COLOR_DELTA_CONTEXTS], green_model[2][COLOR_DELTA_CONTEXTS], blue_model[2][COLOR_DELTA_CONTEXTS];
    le_model mask_model; // 4 bits
    le_model table_reference_model;
    le_model table_difference_model;

    uint32_t top_table[TOP_TABLE_SIZE];
    uint32_t top_table_size;

    // seed "previous block" of every strip: average of the first block of each strip, the two 16-bit 565 colors packed in one uint32
    uint32_t strip_seed;

    // per-strip color predictor: bit s = 1 → strip s predicts endpoint colors from the block of the row above,
    // same zigzag position (visually: the horizontally mirrored column of the previous row), after its first
    // scanline; 0 → zigzag-previous block. the first scanline of every strip is always predicted from the
    // strip seed via the zigzag chain, as there is no previous row within the strip
    uint64_t strip_predictor;

    // per-strip color-pair dictionary: bit s = 1 → strip s writes the 1-bit flag + reference / delta
    // choice per block; 0 → the block colors are always the six deltas (no flag), i.e. the baseline.
    // a strip enables the dictionary only when it saves bits overall, so feature-rich strips (few
    // matching blocks) pay no flag overhead. see the per-strip dictionary enable in init_static_models
    uint64_t strip_dict_enable;

    // flat-index mode of the whole stream: 1 → a block whose two endpoints are identical stores no
    // index payload; the decoder expands the 2-bit value from the stream header to all 16 indices,
    // instead of the reference + mask + differences; 0 → every block stores its indices as usual.
    // decided once in the first pass: 1 only if at least one block has identical endpoints and every
    // such block holds the same uniform index word, else 0 (an encoder that leaves garbage or
    // differing indices on flat blocks forces mode 0, keeping the roundtrip byte-exact). the mode and
    // the value share one header byte (mode | value << 1) written in the stream header's alignment
    // padding, absorbed except in the worst parity case (a few bytes, covered by packed_bc1_maxsize)
    uint8_t bc1_index_mode;
    uint8_t bc1_index_value; // 2-bit value of the shared index word, expanded to all 16 indices, mode 1 only

    // bc4/bc5: per-channel static models — the 2 endpoint models (256 symbols) and the 64
    // index models (8 symbols each, 8 x 8 [left][top] matrix); each retains the full 516-byte
    // le_model in memory, the stream only stores num_symbols entries
    le_model bc4_endpoint_model[BCP_MAX_CHANNELS][2]; // [channel][endpoint]
    le_model bc4_index_model[BCP_MAX_CHANNELS][BC4_INDEX_CONTEXTS];
    uint8_t bc4_strip_seed[BCP_MAX_CHANNELS][2]; // [channel][endpoint]

    // per-channel flat-index mode: 1 → a block whose two endpoints are identical stores no index
    // payload; the decoder expands the 3-bit value from the stream header to all 16 indices, instead
    // of the 15 contextual symbols; 0 → the indices are stored as usual. decided per channel in the
    // first pass, same rule as the BC1 mode. the mode and the value share one header byte per channel
    // (mode | value << 1) after the seeds, in the 2-byte alignment padding (worst case a couple bytes,
    // covered by packed_bc4_maxsize / packed_bc5_maxsize)
    uint8_t bc4_index_mode[BCP_MAX_CHANNELS]; // [channel]
    uint8_t bc4_index_value[BCP_MAX_CHANNELS]; // [channel] 3-bit value, expanded to all 16 indices, mode 1 only
};

typedef struct bc1_block
{
    uint16_t color[2];
    uint32_t indices;
} bc1_block;

// bc4 block: the standard D3D layout, 8 bytes per 4x4 block. A BC5 block is the red bc4_block
// followed by the green one, 16 bytes per 4x4 block
typedef struct bc4_block
{
    uint8_t endpoint[2]; // [0] = max endpoint, [1] = min endpoint
    uint8_t indices[6];  // 16 x 3-bit indices, little-endian 48-bit field, raster order
} bc4_block;

typedef struct byte_stream
{
    uint8_t* buffer;
    size_t length;
    size_t pos;
} byte_stream;

typedef struct vq_cluster
{
    uint32_t bit_diff_count[32];
    uint32_t count;
} vq_cluster;


//-----------------------------------------------------------------------------------------------------------------------------
// Private functions
//-----------------------------------------------------------------------------------------------------------------------------

static inline void* malloc_wrapper(size_t size, void* user) {(void)user; return malloc(size);}
static inline void* realloc_wrapper(void* old_ptr, size_t old_size, size_t new_size, void* user){(void)user;(void)old_size;return realloc(old_ptr, new_size);}
static inline void free_wrapper(void* ptr, void* user) {(void)user; free(ptr);}

//----------------------------------------------------------------------------------------------------------------------------
static inline bcp_mem_interface default_allocator(void) 
{
    return (bcp_mem_interface) 
    {
        .malloc_fn  = malloc_wrapper,
        .realloc_fn = realloc_wrapper,
        .free_fn    = free_wrapper,
        .user       = NULL
    };
}

//----------------------------------------------------------------------------------------------------------------------------
static inline void bc1_extract_565(uint16_t color, uint8_t *r5, uint8_t *g6, uint8_t *b5)
{
    *r5 = (uint8_t)((color >> 11) & 0x1F);
    *g6 = (uint8_t)((color >> 5)  & 0x3F);
    *b5 = (uint8_t)(color & 0x1F);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline uint16_t bc1_pack_565(uint8_t r5, uint8_t g6, uint8_t b5)
{
    return (uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
}

//----------------------------------------------------------------------------------------------------------------------------
// Unpacks the strip seed into a "previous" block. The seed packs the two endpoint colors in one uint32, color[0] in the low word.
static inline void unpack_strip_seed(uint32_t seed, bc1_block* previous)
{
    previous->color[0] = (uint16_t)seed;
    previous->color[1] = (uint16_t)(seed >> 16);
    previous->indices = 0;
}

//----------------------------------------------------------------------------------------------------------------------------
// packs a block's two endpoint colors into one uint32: color[0] in the low word, color[1] in the high
// word — the same layout as the strip seed, so a color pair is a single comparable/storable value
static inline uint32_t color_pair(const bc1_block* b)
{
    return ((uint32_t)b->color[1] << 16) | (uint32_t)b->color[0];
}

//----------------------------------------------------------------------------------------------------------------------------
// unpacks a packed color pair (see color_pair) into a block's two endpoint colors
static inline void unpack_color_pair(uint32_t pair, uint16_t* c0, uint16_t* c1)
{
    *c0 = (uint16_t)pair;
    *c1 = (uint16_t)(pair >> 16);
}

//----------------------------------------------------------------------------------------------------------------------------
// sliding dictionary of the last COLOR_DICT_SIZE endpoint color pairs of a strip's zigzag chain.
// a fixed-size ring buffer keyed by chain position: block [p] owns slot [p & COLOR_DICT_MASK], so the
// most recent COLOR_DICT_SIZE blocks are always the live window. the caller clears it once per strip.
typedef struct color_dict
{
    uint32_t entry[COLOR_DICT_SIZE];
} color_dict;

//----------------------------------------------------------------------------------------------------------------------------
// searches the dictionary for an exact match of [pair] among the [position] previous blocks of the
// chain (at most COLOR_DICT_SIZE of them): returns the distance in blocks to the most recent match
// (1 = the immediately previous block, up to min(position, COLOR_DICT_SIZE)), or 0 if there is none.
// [dict] must hold the color pairs of chain positions [max(0, position - COLOR_DICT_SIZE) .. position - 1].
static inline uint32_t color_dict_match(const color_dict* dict, uint32_t position, uint32_t pair)
{
    uint32_t limit = (position < COLOR_DICT_SIZE) ? position : COLOR_DICT_SIZE;

    for (uint32_t back = 1; back <= limit; ++back)
    {
        if (dict->entry[(position - back) & COLOR_DICT_MASK] == pair)
            return back;
    }

    return 0;
}

//----------------------------------------------------------------------------------------------------------------------------
// number of bits to store a dictionary reference of the block at chain position [position]: the
// reference is the distance minus one, in [0, limit - 1] with limit = min(position, COLOR_DICT_SIZE),
// so it needs ceil(log2(limit)) bits (0 for the first block, 8 once the window is full)
static inline uint8_t color_ref_bits(uint32_t position)
{
    uint32_t limit = (position < COLOR_DICT_SIZE) ? position : COLOR_DICT_SIZE;
    return le_ceil_log2(limit);
}

//----------------------------------------------------------------------------------------------------------------------------
// Histogram of the six color-delta symbols: one bucket per (endpoint, context) model, mirroring
// the layout of the models themselves (16 models of 2^COLOR_DELTA_NUM_BITS symbols)
typedef struct color_hists
{
    uint32_t red[2][COLOR_DELTA_CONTEXTS][1<<COLOR_DELTA_NUM_BITS];
    uint32_t green[2][COLOR_DELTA_CONTEXTS][1<<COLOR_DELTA_NUM_BITS];
    uint32_t blue[2][COLOR_DELTA_CONTEXTS][1<<COLOR_DELTA_NUM_BITS];
} color_hists;

//----------------------------------------------------------------------------------------------------------------------------
// branchless absolute value (arithmetic shift: mask is -1 for a negative, 0 otherwise)
static inline uint32_t abs32(int v)
{
    int32_t mask = v >> 31;
    return (uint32_t)((v + mask) ^ mask);
}

//----------------------------------------------------------------------------------------------------------------------------
// total absolute magnitude of the color deltas [current] would emit against [reference]: both endpoints,
// the decorrelated green / red / blue magnitudes, i.e. the six symbols the encoder writes per block.
// used to pick the cheaper per-strip predictor (top block vs zigzag-previous)
static inline uint32_t color_delta_energy(const bc1_block* current, const bc1_block* reference)
{
    uint32_t energy = 0;

    for(uint32_t j=0; j<2; ++j)
    {
        uint8_t current_red, current_green, current_blue;
        uint8_t reference_red, reference_green, reference_blue;

        bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
        bc1_extract_565(reference->color[j], &reference_red, &reference_green, &reference_blue);

        int dred = current_red - reference_red;
        int dgreen = current_green - reference_green;
        int dblue = current_blue - reference_blue;

        energy += abs32(dgreen);

        dgreen /= 2;
        dred -= dgreen;
        dblue -= dgreen;

        energy += abs32(dred) + abs32(dblue);
    }

    return energy;
}

//----------------------------------------------------------------------------------------------------------------------------
// Context bucket of a green delta (6-bit green, so dgreen is in [-63, 63]): 3 buckets,
// negative / zero / positive. The encoder and decoder derive the same bucket from the emitted
// delta, so the context selection costs zero stream bits.
static inline uint8_t color_delta_context(int dgreen)
{
    if (dgreen < 0)
        return 0;
    if (dgreen > 0)
        return 1;
    return 2;
}

//----------------------------------------------------------------------------------------------------------------------------
// Bit cost of the six color-delta symbols [current] would emit against [reference] with the
// current color models: the same symbols the encoder writes per block, priced without writing
// them. used by the per-strip color-pair dictionary decision, which weighs the deltas of a
// matched block against the flag + reference they are replaced by
static inline uint32_t color_delta_cost(const bcp_context* ctx, const bc1_block* current, const bc1_block* reference)
{
    uint32_t cost = 0;
    int first_green_delta = 0;

    for(uint32_t j=0; j<2; ++j)
    {
        uint8_t current_red, current_green, current_blue;
        uint8_t reference_red, reference_green, reference_blue;

        bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
        bc1_extract_565(reference->color[j], &reference_red, &reference_green, &reference_blue);

        int dred = current_red - reference_red;
        int dgreen = current_green - reference_green;
        int dblue = current_blue - reference_blue;

        uint8_t green_context = (j == 0) ? 0 : color_delta_context(first_green_delta);
        cost += le_symbol_cost(&ctx->green_model[j][green_context], (uint8_t)(dgreen + COLOR_DELTA_OFFSET));

        if (j == 0)
            first_green_delta = dgreen;

        // the red/blue deltas scale with the endpoint's own green delta, bucket it before halving
        uint8_t chroma_context = color_delta_context(dgreen);

        dgreen /= 2;
        dred -= dgreen;
        dblue -= dgreen;

        cost += le_symbol_cost(&ctx->red_model[j][chroma_context], (uint8_t)(dred + COLOR_DELTA_OFFSET));
        cost += le_symbol_cost(&ctx->blue_model[j][chroma_context], (uint8_t)(dblue + COLOR_DELTA_OFFSET));
    }

    return cost;
}

//----------------------------------------------------------------------------------------------------------------------------
// adds the six color-delta symbols of [current] against [reference] to [h]: the same symbols
// the encoder writes per block, mirroring its delta computation
static inline void color_hists_add(color_hists* h, const bc1_block* current, const bc1_block* reference)
{
    int first_green_delta = 0;

    for(uint32_t j=0; j<2; ++j)
    {
        uint8_t current_red, current_green, current_blue;
        uint8_t reference_red, reference_green, reference_blue;

        bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
        bc1_extract_565(reference->color[j], &reference_red, &reference_green, &reference_blue);

        int dred = current_red - reference_red;
        int dgreen = current_green - reference_green;
        int dblue = current_blue - reference_blue;

        uint8_t green_context = (j == 0) ? 0 : color_delta_context(first_green_delta);
        h->green[j][green_context][dgreen + COLOR_DELTA_OFFSET]++;

        if (j == 0)
            first_green_delta = dgreen;

        // the red/blue deltas scale with the endpoint's own green delta, bucket it before halving
        uint8_t chroma_context = color_delta_context(dgreen);

        dgreen /= 2;
        dred -= dgreen;
        dblue -= dgreen;

        h->red[j][chroma_context][dred + COLOR_DELTA_OFFSET]++;
        h->blue[j][chroma_context][dblue + COLOR_DELTA_OFFSET]++;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// subtracts [m] from [h]: every symbol of [m] was added to [h] as well, so no count underflows
static inline void color_hists_sub(color_hists* h, const color_hists* m)
{
    for(uint32_t j=0; j<2; ++j)
    {
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            for(uint32_t i=0; i<(1<<COLOR_DELTA_NUM_BITS); ++i)
            {
                h->red[j][c][i] -= m->red[j][c][i];
                h->blue[j][c][i] -= m->blue[j][c][i];
            }

        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            for(uint32_t i=0; i<(1<<COLOR_DELTA_NUM_BITS); ++i)
                h->green[j][c][i] -= m->green[j][c][i];
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

//----------------------------------------------------------------------------------------------------------------------------
// Returns (popcount << 16) | index of the table entry with the smallest XOR popcount against [bitfield].
// Ties are broken by the fewest differing bytes, then by the largest entry value.
uint32_t nearest32(const uint32_t* table, uint32_t table_size, uint32_t bitfield)
{
    uint32_t scores[TOP_TABLE_SIZE];
    uint32_t masks[TOP_TABLE_SIZE];
    uint32_t i = 0;

#ifdef BCP_NEON
    static const uint8_t byte_weight[16] = { 1, 2, 4, 8, 1, 2, 4, 8, 1, 2, 4, 8, 1, 2, 4, 8 };
    uint32x4_t bf_vec = vdupq_n_u32(bitfield);
    uint8x16_t one_bytes = vdupq_n_u8(1);
    uint8x16_t weight_bytes = vld1q_u8(byte_weight);

    for (; i + 3 < table_size; i += 4)
    {
        uint32x4_t dict_vec = vld1q_u32(&table[i]);
        uint32x4_t delta = veorq_u32(dict_vec, bf_vec);
        uint8x16_t delta_bytes = vreinterpretq_u8_u32(delta);
        uint8x16_t counts = vcntq_u8(delta_bytes);
        uint16x8_t sum16 = vpaddlq_u8(counts);
        uint32x4_t sum32 = vpaddlq_u16(sum16);

        vst1q_u32(&scores[i], sum32);

        // differing-byte mask per entry: 1 per differing byte, weighted {1, 2, 4, 8} inside the entry then summed
        uint8x16_t nz_bytes = vandq_u8(vcgeq_u8(counts, one_bytes), one_bytes);
        uint16x8_t half = vpaddlq_u8(vmulq_u8(nz_bytes, weight_bytes));
        vst1q_u32(&masks[i], vpaddlq_u16(half));
    }

    // tail after the vector loop
    for (; i < table_size; ++i)
    {
        uint32_t difference = table[i] ^ bitfield;
        uint32_t score = popcount(difference);
        if (score == 0)
            return (0 << 16) | (i & 0xffff);
        scores[i] = score;

        uint32_t mask = 0;
        if ((difference & 0x000000FF) != 0) mask |= 1;
        if ((difference & 0x0000FF00) != 0) mask |= 2;
        if ((difference & 0x00FF0000) != 0) mask |= 4;
        if ((difference & 0xFF000000) != 0) mask |= 8;
        masks[i] = mask;
    }
#else
    // No vector ISA assumed: plain scalar loop over the whole table
    for (; i < table_size; ++i)
    {
        uint32_t difference = table[i] ^ bitfield;
        uint32_t score = popcount(difference);
        if (score == 0)
            return (0 << 16) | (i & 0xffff);
        scores[i] = score;

        uint32_t mask = 0;
        if ((difference & 0x000000FF) != 0) mask |= 1;
        if ((difference & 0x0000FF00) != 0) mask |= 2;
        if ((difference & 0x00FF0000) != 0) mask |= 4;
        if ((difference & 0xFF000000) != 0) mask |= 8;
        masks[i] = mask;
    }
#endif

    // find best: lowest popcount, then fewest differing bytes, then the largest table value
    uint32_t best_index = 0;
    uint32_t best_score = UINT32_MAX;
    uint32_t best_mask  = 0;
    for (uint32_t j = 0; j < table_size; ++j)
    {
        uint32_t score = scores[j];
        int mask_bits = popcount(masks[j]);
        if (score < best_score ||
            (score == best_score && (mask_bits < popcount(best_mask) || (mask_bits == popcount(best_mask) && table[j] > table[best_index]))))
        {
            best_score = score;
            best_index = j;
            best_mask  = masks[j];
        }
    }
    return ((best_score&0xffff)<<16) | (best_index&0xffff);
}

//----------------------------------------------------------------------------------------------------------------------------
// One Lloyd refinement round of the top table: assigns every sampled block to its nearest centroid,
// accumulates the XOR difference bit by bit per centroid, and moves each centroid to the bitwise
// majority of its assigned blocks.
//
// [sample_step] of 1 assigns every block (a full round), a larger step samples one block in
// [sample_step] to keep the round cheap, and [start_offset] de-aliases consecutive rounds.
// The bucket feedback drops the skip while the accumulated popcount error is high, so a round
// adapts between the two: with [sample_step] 1 both branches advance by one block.
static void vq_refine_round(const void* input, uint32_t num_blocks, uint32_t block_stride, uint32_t* centroids, uint32_t num_centroids,
                            vq_cluster* clusters, uint32_t sample_step, uint32_t start_offset)
{
    for(uint32_t i=0; i<num_centroids; ++i)
    {
        for(uint32_t j=0; j<32; ++j)
            clusters[i].bit_diff_count[j] = 0;

        clusters[i].count = 0;
    }

    uint32_t bucket = 0;
    const uint32_t threshold = 16;

    // find the best cluster for each sampled block, jittering the first block
    for(uint32_t block_index=start_offset; block_index<num_blocks;)
    {
        const bc1_block* b = (const bc1_block*) input + block_index * block_stride;

        uint32_t result = nearest32(centroids, num_centroids, b->indices);
        uint32_t score = (result >> 16);
        uint32_t best_entry = result & 0xffff;
        uint32_t diff = b->indices ^ centroids[best_entry];

        for(uint32_t i=0; i<32; ++i)
            clusters[best_entry].bit_diff_count[i] += (diff >> i) & 1;

        clusters[best_entry].count++;
        bucket += score;

        if (bucket >= threshold) 
        {
            // if error is high, we only move 1 block
            block_index++;
            bucket -= threshold; 
        } else 
        {
            // normal skip
            block_index += sample_step;
        }
    }

    // move centroid to the bitwise majority of its assigned blocks
    for(uint32_t i=0; i<num_centroids; ++i)
    {
        if (clusters[i].count>0)
        {
            for(uint32_t bit=0; bit<32; ++bit)
                if (clusters[i].bit_diff_count[bit] > (clusters[i].count/2))
                    centroids[i] ^= (1u << bit);

        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void vq_top_table(const void* input, uint32_t num_blocks, uint32_t block_stride, uint32_t* output, uint32_t* num_entries)
{
    // static array of odd steps to avoid aliasing and branches
    // first iteration is always 1 to ensure 100% initial coverage, and the two final full-assignment
    // rounds (step 1) converge the centroids on every block: the jittered sampling of the middle
    // rounds explores a better local optimum, the full rounds settle it (exp 15 in failed_experiments.md)
    static const uint32_t steps[6] = { 1, 5, 11, 17, 1, 1 };

    uint32_t centroids[TOP_TABLE_SIZE];
    vq_cluster clusters[TOP_TABLE_SIZE];

    // use the top table entries as candidate for the cluster
    for(uint32_t i=0; i<*num_entries; ++i)
        centroids[i] = output[i];

    // multiple iterations to stabilize the clusters
    for(uint32_t iteration=0; iteration<6; ++iteration)
        vq_refine_round(input, num_blocks, block_stride, centroids, *num_entries, clusters, steps[iteration], iteration % steps[iteration]);

    uint32_t num_clusters = 0;

    // fill the table with centroid
    for(uint32_t i=0; i<*num_entries; ++i)
        if (clusters[i].count > 1)
            output[num_clusters++] = centroids[i];

    // reduce if needed the size of the table to the number of cluster with at least one block
    *num_entries = num_clusters;
}

//----------------------------------------------------------------------------------------------------------------------------
void build_top_table(hashmap_entry* hashmap, const void* input, uint32_t num_blocks, uint32_t block_stride, uint32_t* output, uint32_t* num_entries)
{
    // clear the hashmap
    for(uint32_t i=0; i<HASHMAP_SIZE; ++i)
        hashmap[i].count = 0;

    // insert all blocks indices in the hashmap
    for(uint32_t i=0; i<num_blocks; ++i)
    {
        const bc1_block* b = (const bc1_block*) input + i * block_stride;

        uint32_t h = hash32(b->indices);
        uint32_t index = h & (HASHMAP_SIZE - 1);
        uint32_t first_index = index;

        bool inserted = false;
        while (!inserted)
        {
            if ((hashmap[index].count == 0) || (hashmap[index].key == b->indices))
            {
                hashmap[index].key = b->indices;
                hashmap[index].count++;
                inserted = true;
            }
            else
            {
                index = (index + 1) & (HASHMAP_SIZE - 1);
                assert(index != first_index);
            }
        }
    }

    // clear the table
    hashmap_entry table[TOP_TABLE_SIZE];
    for(uint32_t i=0; i<TOP_TABLE_SIZE; ++i)
        table[i].count = 0;

    // fill the table with top most used indices
    for (uint32_t i = 0; i<HASHMAP_SIZE; ++i)
    {
        uint32_t c = hashmap[i].count;
        if (c == 0) 
            continue;

        // if smaller than current min, skip
        if (c <= table[0].count)
            continue;

        // replace min
        table[0] = hashmap[i];

        // bubble new smallest to front
        for (uint32_t j = 1; j < TOP_TABLE_SIZE; j++)
        {
            if (table[j-1].count > table[j].count)
            {
                hashmap_entry tmp = table[j - 1];
                table[j-1] = table[j];
                table[j] = tmp;
            }
            else break;
        }
    }
    
    // reverse the table for output and count
    *num_entries = 0;
    for(uint32_t i=0; i<TOP_TABLE_SIZE; ++i)
    {
        output[i] = table[TOP_TABLE_SIZE-i-1].key;
        if (table[TOP_TABLE_SIZE-i-1].count>0)
            (*num_entries)++;
    }

    vq_top_table(input, num_blocks, block_stride, output, num_entries);

    // an empty top table corrupts the stream (the size is stored as size-1 in a byte, 0 reads back as 256 entries),
    // fall back to the single most frequent exact pattern, the hashmap still holds the exact pattern counts
    if (*num_entries == 0)
    {
        uint32_t best_key = 0;
        uint32_t best_count = 0;

        for (uint32_t i = 0; i < HASHMAP_SIZE; ++i)
        {
            if (hashmap[i].count == 0)
                continue;

            if ((hashmap[i].count > best_count) || ((hashmap[i].count == best_count) && (hashmap[i].key > best_key)))
            {
                best_count = hashmap[i].count;
                best_key = hashmap[i].key;
            }
        }

        output[0] = best_key;
        *num_entries = 1;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// fits the 16 color models (3 contextual red / blue per endpoint + the green models: endpoint 0
// uncontexted, endpoint 1 contextual on the sign of endpoint 0's green delta) from [hist]
static inline void init_color_models(bcp_context* ctx, const color_hists* hist)
{
    for(uint32_t j=0; j<2; ++j)
    {
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
        {
            le_static_model_init(&ctx->green_model[j][c], &hist->green[j][c][0], 1<<COLOR_DELTA_NUM_BITS);
            le_static_model_init(&ctx->red_model[j][c], &hist->red[j][c][0], 1<<COLOR_DELTA_NUM_BITS);
            le_static_model_init(&ctx->blue_model[j][c], &hist->blue[j][c][0], 1<<COLOR_DELTA_NUM_BITS);
        }
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// tile columns of the 64-tile grid: 8 for wide images, shrinking so the tiles stay at least
// 4 blocks wide, 1 for the narrowest supported image (the 16 px minimum)
static inline uint32_t tile_grid_x(uint32_t width_blocks)
{
    if (width_blocks >= 32) return 8;
    if (width_blocks >= 16) return 4;
    if (width_blocks >= 8)  return 2;
    return 1;
}

//-----------------------------------------------------------------------------------------------------------------------------
// block bounds of tile [strip_index] of the 64-tile grid, x1 / y1 one past. the even split of
// the block grid covers the image exactly, so images whose block count is not a multiple of
// the tile size lose no rows or columns (the old strip truncation dropped the trailing rows)
typedef struct tile_bounds
{
    uint32_t x0, x1;
    uint32_t y0, y1;
} tile_bounds;

//-----------------------------------------------------------------------------------------------------------------------------
static inline tile_bounds tile_bounds_get(uint32_t strip_index, uint32_t width_blocks, uint32_t height_blocks, uint32_t num_strips)
{
    uint32_t gx = tile_grid_x(width_blocks);
    uint32_t gy = num_strips / gx;
    uint32_t tx = strip_index % gx;
    uint32_t ty = strip_index / gx;

    return (tile_bounds) { .x0 = (tx * width_blocks) / gx, .x1 = ((tx + 1) * width_blocks) / gx,
                           .y0 = (ty * height_blocks) / gy, .y1 = ((ty + 1) * height_blocks) / gy };
}

//-----------------------------------------------------------------------------------------------------------------------------
void init_static_models(bcp_context* ctx, const void* input, uint32_t width_blocks, uint32_t height_blocks, uint32_t num_strips, uint32_t block_stride)
{
    color_hists hist;
    uint32_t hist_reference[TOP_TABLE_SIZE];
    uint32_t hist_mask[16];
    uint32_t hist_difference[LE_ALPHABET_SIZE];

    memset(&hist, 0, sizeof(hist));
    memset(hist_reference, 0, sizeof(hist_reference));
    memset(hist_mask, 0, sizeof(hist_mask));
    memset(hist_difference, 0, sizeof(hist_difference));

    // pre-pass: the average of the first block of each tile becomes the seed "previous" block used to start every tile
    uint32_t sums[6] = { 0, 0, 0, 0, 0, 0 }; // r, g, b of color[0] and color[1]

    for(uint32_t strip_index=0; strip_index<num_strips; ++strip_index)
    {
        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
        uint32_t zigzag_x = (tb.y0 & 1) ? tb.x0 : tb.x1 - 1;

        const bc1_block* first = (const bc1_block*) input + ((tb.y0 * width_blocks) + zigzag_x) * block_stride;

        for(uint32_t j=0; j<2; ++j)
        {
            uint8_t r, g, b;
            bc1_extract_565(first->color[j], &r, &g, &b);

            sums[j*3 + 0] += r;
            sums[j*3 + 1] += g;
            sums[j*3 + 2] += b;
        }
    }

    uint16_t average0 = bc1_pack_565((uint8_t)(sums[0] / num_strips), (uint8_t)(sums[1] / num_strips), (uint8_t)(sums[2] / num_strips));
    uint16_t average1 = bc1_pack_565((uint8_t)(sums[3] / num_strips), (uint8_t)(sums[4] / num_strips), (uint8_t)(sums[5] / num_strips));
    ctx->strip_seed = ((uint32_t)average1 << 16) | (uint32_t)average0;

    // pre-pass: per tile, pick the cheaper color predictor, zigzag-previous vs the row above (same zigzag
    // position) after the first scanline, by the total absolute magnitude of the color deltas each would emit
    ctx->strip_predictor = 0;
    for(uint32_t strip_index=0; strip_index<num_strips; ++strip_index)
    {
        uint64_t energy_previous = 0;
        uint64_t energy_top = 0;

        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);

        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
        uint32_t tile_width = tb.x1 - tb.x0;

        for(uint32_t y = tb.y0; y < tb.y1; ++y)
        {
            for(uint32_t x = 0; x < tile_width; ++x)
            {
                uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
                const bc1_block* current = (const bc1_block*) input + ((y * width_blocks) + zigzag_x) * block_stride;

                const bc1_block* top = &previous;
                if (y > tb.y0)
                    top = (const bc1_block*) input + ((y - 1) * width_blocks + zigzag_x) * block_stride; // same zigzag position, row above

                energy_previous += color_delta_energy(current, &previous);
                energy_top += color_delta_energy(current, top);

                previous = *current;
            }
        }

        if (energy_top < energy_previous)
            ctx->strip_predictor |= (uint64_t)1 << strip_index;
    }

    // provisional histogram: every block's color deltas, the dictionary not involved, plus the index
    // symbols (the dictionary does not touch the indices). the color models fit on this pass price
    // the matched blocks in the per-strip dictionary decision below, the final color models are
    // refit on the deltas that are actually emitted after the decision
    // the flat-index mode is decided here too: mode 1 (a block with identical endpoints stores no
    // index payload, the decoder expands the header value) only if at least one block has identical
    // endpoints and every such block holds the same uniform index word, else mode 0 (garbage or
    // differing indices on a flat block force the full encoding)
    bool flat_block = false;
    bool non_uniform_flat = false;
    bool divergent_flat = false;
    uint8_t flat_value = 0;
    for(uint32_t strip_index=0; strip_index<num_strips; ++strip_index)
    {
        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);
        const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
        uint32_t tile_width = tb.x1 - tb.x0;

        for(uint32_t y = tb.y0; y < tb.y1; ++y)
        {
            for(uint32_t x = 0; x < tile_width; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);

                const bc1_block* current = (const bc1_block*) input + ((y * width_blocks) + zigzag_x) * block_stride;

                // first scanline of a tile is always predicted from the seed via the zigzag-previous
                // chain, after that a top-flagged tile is predicted from the row above, same zigzag position
                const bc1_block* color_reference = &previous;
                if (use_top && y > tb.y0)
                    color_reference = (const bc1_block*) input + ((y - 1) * width_blocks + zigzag_x) * block_stride;

                color_hists_add(&hist, current, color_reference);

                if (current->color[0] == current->color[1])
                {
                    const uint8_t value = (uint8_t)(current->indices & 3);
                    if (current->indices != (uint32_t)value * 0x11111111u)
                        non_uniform_flat = true;
                    if (!flat_block)
                        flat_value = value;
                    else if (value != flat_value)
                        divergent_flat = true;
                    flat_block = true;
                }

                uint8_t reference = nearest32(ctx->top_table, ctx->top_table_size, current->indices) & 0xff;
                hist_reference[reference]++;

                uint32_t difference = current->indices ^ ctx->top_table[reference];
                uint32_t mask = 0;
                if ((difference & 0x000000FF) != 0) mask |= 1;
                if ((difference & 0x0000FF00) != 0) mask |= 2;
                if ((difference & 0x00FF0000) != 0) mask |= 4;
                if ((difference & 0xFF000000) != 0) mask |= 8;

                hist_mask[mask]++;

                for(uint32_t j=0; j<4; ++j)
                    if (mask & (1u << j))
                        hist_difference[(difference >> (j*8)) & 0xff]++;

                previous = *current;
            }
        }
    }

    if (flat_block && !non_uniform_flat && !divergent_flat)
    {
        ctx->bc1_index_mode = 1;
        ctx->bc1_index_value = flat_value;
    }
    else
    {
        ctx->bc1_index_mode = 0;
        ctx->bc1_index_value = 0;
    }

    init_color_models(ctx, &hist);

    // per-strip color-pair dictionary enable: an enabled strip pays one flag bit per block, so it
    // wins only when the savings of its matched blocks, the six deltas they would emit minus the
    // reference they pay, exceed that. the matched blocks of an enabled strip emit no deltas, so
    // their deltas are subtracted from the histogram before the final color models are refit
    ctx->strip_dict_enable = 0;
    color_dict dict;
    color_hists matched;
    for(uint32_t strip_index=0; strip_index<num_strips; ++strip_index)
    {
        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);
        memset(&dict, 0, sizeof(dict));
        memset(&matched, 0, sizeof(matched));
        const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
        uint32_t tile_width = tb.x1 - tb.x0;

        int64_t savings = 0;
        uint32_t num_blocks = 0;

        for(uint32_t y = tb.y0; y < tb.y1; ++y)
        {
            for(uint32_t x = 0; x < tile_width; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);

                const bc1_block* current = (const bc1_block*) input + ((y * width_blocks) + zigzag_x) * block_stride;
                uint32_t position = (y - tb.y0) * tile_width + x;

                const bc1_block* color_reference = &previous;
                if (use_top && y > tb.y0)
                    color_reference = (const bc1_block*) input + ((y - 1) * width_blocks + zigzag_x) * block_stride;

                if (color_dict_match(&dict, position, color_pair(current)) > 0)
                {
                    // a match: the flag + reference replaces the six deltas, weigh the two
                    savings += (int64_t)color_delta_cost(ctx, current, color_reference) - color_ref_bits(position);
                    color_hists_add(&matched, current, color_reference);
                }
                else
                    color_hists_add(&hist, current, color_reference);

                dict.entry[position & COLOR_DICT_MASK] = color_pair(current);
                previous = *current;
                num_blocks++;
            }
        }

        if (savings > num_blocks)
        {
            ctx->strip_dict_enable |= (uint64_t)1 << strip_index;
            color_hists_sub(&hist, &matched);
        }
    }

    // final color models: fit on the deltas that are actually emitted, every block of the disabled
    // strips and the non-matched blocks of the enabled ones
    init_color_models(ctx, &hist);

    le_static_model_init(&ctx->table_reference_model, hist_reference, TOP_TABLE_SIZE);
    le_static_model_init(&ctx->mask_model, hist_mask, 16);
    le_static_model_init(&ctx->table_difference_model, hist_difference, LE_ALPHABET_SIZE);
}



//-----------------------------------------------------------------------------------------------------------------------------
static inline void stream_align(byte_stream* stream, size_t alignment)
{
    // align with 0 value padding
    while ((stream->pos % alignment) != 0 && stream->pos < stream->length)
        stream->buffer[stream->pos++] = 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void read_stream_align(byte_stream* stream, size_t power_two_alignment)
{
    size_t mask = power_two_alignment - 1;
    stream->pos = (stream->pos + mask) & ~(mask);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool save_static_model(const le_model* model, byte_stream* stream)
{
    // a model whose histogram is all zero has no symbols but the stream format can only store a count of 1 to 256,
    // so we save a single dummy entry. alphabet[0] is always initialized by le_static_model_init and the decoder
    // never reads it because the encoder never encoded with it.
    uint32_t num_symbols = (model->num_symbols > 0) ? model->num_symbols : 1;

    size_t model_size = 3 + num_symbols;

    if (stream->pos + model_size > stream->length)
        return false;

    stream->buffer[stream->pos++] = (uint8_t)(num_symbols - 1); // minus 1 to store 256 symbols count on uint8_t
    stream->buffer[stream->pos++] = model->k;
    stream->buffer[stream->pos++] = model->q_escape;

    for(uint32_t i=0; i<num_symbols; ++i)
        stream->buffer[stream->pos++] = model->alphabet[i];

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool load_static_model(le_model* model, byte_stream* stream)
{
    // mirror of save_static_model
    if (stream->pos + 3 > stream->length)
        return false;

    uint32_t num_symbols = stream->buffer[stream->pos++] + 1;
    uint8_t k = stream->buffer[stream->pos++];
    uint8_t q_escape = stream->buffer[stream->pos++];

    // k stores the raw-mode flag in bit 7 and a width / Rice parameter (<= LE_MAX_K) in the lower bits
    if ((k & 0x7f) > LE_MAX_K)
        k = (uint8_t)((k & 0x80) | LE_MAX_K);

    if (stream->pos + num_symbols > stream->length)
        return false;

    le_static_model_load(model, &stream->buffer[stream->pos], num_symbols, k, q_escape);
    stream->pos += num_symbols;

    return true;
}


//-----------------------------------------------------------------------------------------------------------------------------
// BC4 / BC5
//-----------------------------------------------------------------------------------------------------------------------------

//-----------------------------------------------------------------------------------------------------------------------------
// Wrapped 8-bit delta of [curr] against [prev]: uint8 arithmetic wraps modulo 256
static inline uint8_t delta_encode_wrap(uint8_t prev, uint8_t curr)
{
    return (uint8_t)(curr - prev);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint8_t delta_decode_wrap(uint8_t prev, uint8_t delta)
{
    return (uint8_t)(prev + delta);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Index context: the 8 x 8 [left][top] matrix of the decoded 3-bit indices — the left neighbor
// of the pixel (a row start has none) and the pixel directly above (the first row has none),
// the missing neighbor defaulting to 0
static inline uint8_t bc4_index_context(uint8_t left, uint8_t top)
{
    return (uint8_t)(left * BC4_INDEX_DIM + top);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Endpoint prediction of the current block from the strip chain state: the first row of a strip
// is predicted from the chain-previous block (the seed for the strip's first block), a row start
// from the top block only, and every other block from the 2D predictor previous + top - top_previous
static inline uint8_t bc4_endpoint_predict(bool first_row, bool row_start, uint8_t prev, uint8_t top, uint8_t top_prev)
{
    if (first_row)
        return prev;
    if (row_start)
        return top;
    return (uint8_t)(prev + top - top_prev);
}

//-----------------------------------------------------------------------------------------------------------------------------
// The 16 3-bit indices of the block in raster order (index 0 = top-left pixel), read from /
// written to the 48-bit little-endian field of the 6 index bytes
static inline void bc4_unpack_indices(const bc4_block* b, uint8_t idx[16])
{
    uint32_t lo = (uint32_t)b->indices[0] | ((uint32_t)b->indices[1] << 8) | ((uint32_t)b->indices[2] << 16);
    uint32_t hi = (uint32_t)b->indices[3] | ((uint32_t)b->indices[4] << 8) | ((uint32_t)b->indices[5] << 16);

    for(uint32_t i=0; i<8; ++i)
        idx[i] = (uint8_t)((lo >> (3*i)) & 7);
    for(uint32_t i=8; i<16; ++i)
        idx[i] = (uint8_t)((hi >> (3*(i-8))) & 7);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void bc4_pack_indices(const uint8_t idx[16], bc4_block* b)
{
    uint64_t v = 0;

    for(uint32_t i=0; i<16; ++i)
        v |= ((uint64_t)idx[i] & 7) << (3*i);

    for(uint32_t j=0; j<6; ++j)
        b->indices[j] = (uint8_t)(v >> (8*j));
}

//-----------------------------------------------------------------------------------------------------------------------------
// Per-channel histogram of the bc4/bc5 payload symbols: 2 endpoint models (256 symbols) +
// 64 index models (8 symbols each)
typedef struct bc4_hists
{
    uint32_t endpoint[2][256];
    uint32_t index[BC4_INDEX_CONTEXTS][8];
} bc4_hists;

//-----------------------------------------------------------------------------------------------------------------------------
// Fits the bc4/bc5 static models from [input]: the per-channel tile seed is the average of the
// first block of each tile in chain order (as BC1), then one histogram pass with the exact
// tile predictors of the encoder. there is no per-tile mode decision, so a single fit is final.
// the per-channel flat-index mode is also decided in the histogram pass: mode 1 (a block whose
// two endpoints are identical stores no index payload, the decoder expands the header value) only if
// at least one such block exists and every such block holds the same uniform 3-bit value, else mode
// 0 (garbage or differing indices on a flat block force the full 15-symbol encoding)
static void bc4_init_models(bcp_context* ctx, const void* image, uint32_t width_blocks, uint32_t height_blocks, uint32_t num_strips, uint32_t num_channels, uint32_t block_stride)
{
    bc4_hists hist[BCP_MAX_CHANNELS] = { 0 };
    uint32_t sums[BCP_MAX_CHANNELS][2] = { 0 };
    uint8_t prev_e[BCP_MAX_CHANNELS][2], top_e[BCP_MAX_CHANNELS][2], top_prev_e[BCP_MAX_CHANNELS][2];
    bool flat_block[BCP_MAX_CHANNELS] = { false, false };
    bool non_uniform_flat[BCP_MAX_CHANNELS] = { false, false };
    bool divergent_flat[BCP_MAX_CHANNELS] = { false, false };
    uint8_t flat_value[BCP_MAX_CHANNELS] = { 0, 0 };

    // pass 1: tile seeds, the average of the first block of each tile in chain order
    for(uint32_t strip_index=0; strip_index<num_strips; ++strip_index)
    {
        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
        uint32_t x0 = (tb.y0 & 1) ? tb.x0 : tb.x1 - 1;

        for(uint32_t c=0; c<num_channels; ++c)
        {
            const bc4_block* b = (const bc4_block*)image + ((tb.y0 * width_blocks) + x0) * block_stride + c;

            sums[c][0] += b->endpoint[0];
            sums[c][1] += b->endpoint[1];
        }
    }

    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
            ctx->bc4_strip_seed[c][e] = (uint8_t)(sums[c][e] / num_strips);
    }

    // pass 2: histograms with the exact tile predictors of the encoder, plus the per-channel
    // flat-index mode decision (see the function header)
    for(uint32_t strip_index=0; strip_index<num_strips; ++strip_index)
    {
        for(uint32_t c=0; c<num_channels; ++c)
        {
            for(uint32_t e=0; e<2; ++e)
            {
                prev_e[c][e] = ctx->bc4_strip_seed[c][e];
                top_e[c][e] = 0;
                top_prev_e[c][e] = 0;
            }
        }

        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
        uint32_t tile_width = tb.x1 - tb.x0;

        for(uint32_t y = tb.y0; y < tb.y1; ++y)
        {
            for(uint32_t x = 0; x < tile_width; ++x)
            {
                // zig-zag pattern, same chain as BC1
                uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
                const bool first_row = (y == tb.y0);
                const bool row_start = (x == 0);

                for(uint32_t c=0; c<num_channels; ++c)
                {
                    const bc4_block* b = (const bc4_block*)image + (y * width_blocks + zigzag_x) * block_stride + c;
                    uint8_t idx[16];

                    if (!first_row)
                    {
                        // the block of the row above, same physical column (within the strip)
                        const bc4_block* t = (const bc4_block*)image + ((y - 1) * width_blocks + zigzag_x) * block_stride + c;
                        top_e[c][0] = t->endpoint[0];
                        top_e[c][1] = t->endpoint[1];
                    }

                    for(uint32_t e=0; e<2; ++e)
                    {
                        uint8_t pred = bc4_endpoint_predict(first_row, row_start, prev_e[c][e], top_e[c][e], top_prev_e[c][e]);

                        hist[c].endpoint[e][delta_encode_wrap(pred, b->endpoint[e])]++;
                    }

                    bc4_unpack_indices(b, idx);

                    if (b->endpoint[0] == b->endpoint[1])
                    {
                        const uint8_t first = idx[0];
                        if (!flat_block[c])
                            flat_value[c] = first;
                        else if (first != flat_value[c])
                            divergent_flat[c] = true;

                        for(uint32_t i=0; i<16; ++i)
                        {
                            if (idx[i] != first)
                            {
                                non_uniform_flat[c] = true;
                                break;
                            }
                        }

                        flat_block[c] = true;
                    }

                    for(uint32_t i=1; i<16; ++i)
                    {
                        // the missing neighbor of the pixel (first row, row start) defaults to 0
                        uint8_t left = ((i & 3) == 0) ? 0 : idx[i-1];
                        uint8_t top = (i < 4) ? 0 : idx[i-4];
                        uint8_t p = ((i & 3) == 0) ? top : left;

                        hist[c].index[bc4_index_context(left, top)][p ^ idx[i]]++;
                    }

                    if (!first_row)
                    {
                        top_prev_e[c][0] = top_e[c][0];
                        top_prev_e[c][1] = top_e[c][1];
                    }
                    prev_e[c][0] = b->endpoint[0];
                    prev_e[c][1] = b->endpoint[1];
                }
            }
        }
    }

    for(uint32_t c=0; c<num_channels; ++c)
    {
        if (flat_block[c] && !non_uniform_flat[c] && !divergent_flat[c])
        {
            ctx->bc4_index_mode[c] = 1;
            ctx->bc4_index_value[c] = flat_value[c];
        }
        else
        {
            ctx->bc4_index_mode[c] = 0;
            ctx->bc4_index_value[c] = 0;
        }
    }

    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
            le_static_model_init(&ctx->bc4_endpoint_model[c][e], &hist[c].endpoint[e][0], 256);
        for(uint32_t i=0; i<BC4_INDEX_CONTEXTS; ++i)
            le_static_model_init(&ctx->bc4_index_model[c][i], &hist[c].index[i][0], 8);
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC4 / BC5 texture: [image] holds (width/4) * (height/4) blocks, each
// [num_channels] bc4_block (1 for BC4, red + green for BC5). shared by the two formats
static size_t bcp_compress_bc45(bcp_context* ctx, const void* image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips, uint32_t num_channels, uint32_t block_stride)
{
    if ((width < 16) || (height < 256) || !image || !ctx || !output || !num_strips || (num_channels < 1) || (num_channels > BCP_MAX_CHANNELS))
        return 0;

    *num_strips = 64;   // for now, could depend on the resolution in the future

    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;

    bc4_init_models(ctx, image, width_blocks, height_blocks, *num_strips, num_channels, block_stride);

    byte_stream stream = {.buffer = output, .length = output_length, .pos = 0};

    // store models: per channel, the 2 endpoint models (256 symbols) then the 64 index models (8 symbols)
    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
            if (!save_static_model(&ctx->bc4_endpoint_model[c][e], &stream))
                return 0;
        for(uint32_t i=0; i<BC4_INDEX_CONTEXTS; ++i)
            if (!save_static_model(&ctx->bc4_index_model[c][i], &stream))
                return 0;
    }

    // strip seed: 2 bytes per channel, endpoint 0 then endpoint 1
    if (stream.pos + 2 * num_channels > stream.length)
        return 0;

    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
            stream.buffer[stream.pos++] = ctx->bc4_strip_seed[c][e];
    }

    // flat-index mode + value, one byte per channel (mode | value << 1), written right after the
    // seeds. the model region is always even-sized, so these bytes sit on an odd stream position and
    // the 2-align below can never land on them and zero them, keeping the encoder and the decoder in
    // agreement
    if (stream.pos + num_channels > stream.length)
        return 0;
    for(uint32_t c=0; c<num_channels; ++c)
        stream.buffer[stream.pos++] = (uint8_t)(ctx->bc4_index_mode[c] | (ctx->bc4_index_value[c] << 1));

    stream_align(&stream, sizeof(uint16_t));
    size_t strips_offset_array_size = sizeof(uint16_t) * (*num_strips - 1);

    // check if we have enough space
    if (stream.pos + strips_offset_array_size >= stream.length)
        return 0;

    // keep a pointer to strips offset
    uint16_t* strips_offset = (uint16_t*)&stream.buffer[stream.pos];
    stream.pos += strips_offset_array_size;

    // compressed data is aligned on 4 bytes, like BC1
    stream_align(&stream, sizeof(uint32_t));
    size_t previous_offset = stream.pos;

    for(uint32_t strip_index=0; strip_index<*num_strips; ++strip_index)
    {
        le_stream compressed_stream;
        le_init(&compressed_stream, &stream.buffer[stream.pos], stream.length - stream.pos);
        le_begin_encode(&compressed_stream);

        // strip offset is counted in dword, based on the previous one (delta compression);
        // strip 0 always starts the strip data so its offset is zero and not stored
        if (strip_index > 0)
            strips_offset[strip_index - 1] = (uint16_t)((stream.pos - previous_offset) / sizeof(uint32_t));
        previous_offset = stream.pos;

        uint8_t prev_e[BCP_MAX_CHANNELS][2], top_e[BCP_MAX_CHANNELS][2], top_prev_e[BCP_MAX_CHANNELS][2];
        for(uint32_t c=0; c<num_channels; ++c)
        {
            for(uint32_t e=0; e<2; ++e)
            {
                prev_e[c][e] = ctx->bc4_strip_seed[c][e];
                top_e[c][e] = 0;
                top_prev_e[c][e] = 0;
            }
        }

        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, *num_strips);
        uint32_t tile_width = tb.x1 - tb.x0;

        for(uint32_t y = tb.y0; y < tb.y1; ++y)
        {
            for(uint32_t x = 0; x < tile_width; ++x)
            {
                // zig-zag pattern, same chain as BC1
                uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
                const bool first_row = (y == tb.y0);
                const bool row_start = (x == 0);

                for(uint32_t c=0; c<num_channels; ++c)
                {
                    const bc4_block* b = (const bc4_block*)image + (y * width_blocks + zigzag_x) * block_stride + c;
                    uint8_t idx[16];

                    if (!first_row)
                    {
                        const bc4_block* t = (const bc4_block*)image + ((y - 1) * width_blocks + zigzag_x) * block_stride + c;
                        top_e[c][0] = t->endpoint[0];
                        top_e[c][1] = t->endpoint[1];
                    }

                    // endpoint payload: the two wrapped deltas against the strip-chain predictor
                    for(uint32_t e=0; e<2; ++e)
                    {
                        uint8_t pred = bc4_endpoint_predict(first_row, row_start, prev_e[c][e], top_e[c][e], top_prev_e[c][e]);

                        le_encode_symbol(&compressed_stream, &ctx->bc4_endpoint_model[c][e], delta_encode_wrap(pred, b->endpoint[e]));
                    }

                    // index payload: the first index in raw 3 bits, then each index xored with its
                    // intra-block prediction, contextual on the 8 x 8 [left][top] matrix
                    bc4_unpack_indices(b, idx);

                    // a flat block (identical endpoints) in mode 1 stores no index payload: the
                    // decoder expands the header value to all 16 indices; otherwise the full
                    // 15 contextual symbols
                    const bool flat = (b->endpoint[0] == b->endpoint[1]);
                    if (!(flat && ctx->bc4_index_mode[c]))
                    {
                        le_write_bits(&compressed_stream, idx[0], 3);

                        for(uint32_t i=1; i<16; ++i)
                        {
                            // the missing neighbor of the pixel (first row, row start) defaults to 0
                            uint8_t left = ((i & 3) == 0) ? 0 : idx[i-1];
                            uint8_t top = (i < 4) ? 0 : idx[i-4];
                            uint8_t p = ((i & 3) == 0) ? top : left;

                            le_encode_symbol(&compressed_stream, &ctx->bc4_index_model[c][bc4_index_context(left, top)], p ^ idx[i]);
                        }
                    }

                    if (!first_row)
                    {
                        top_prev_e[c][0] = top_e[c][0];
                        top_prev_e[c][1] = top_e[c][1];
                    }
                    prev_e[c][0] = b->endpoint[0];
                    prev_e[c][1] = b->endpoint[1];
                }
            }
        }

        size_t strip_size = le_end_encode(&compressed_stream);
        assert(strip_size < 262144); // if it happens, there is a problem somewhere else

        stream.pos += strip_size;

        stream_align(&stream, sizeof(uint32_t));

        if (compressed_stream.status == LE_BUFFER_OVERRUN)
            return 0;
    }

    return stream.pos;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Decompresses one strip of a BC4 / BC5 stream: the mirror of bcp_compress_bc45, the strip-chain
// references (top / top_previous) are read back from [output], which holds the decoded rows
static bool bcp_decompress_bc45(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index, uint32_t num_channels, uint32_t block_stride)
{
    if ((width < 16) || (height < 256) || !input || !ctx || !output || (num_strips == 0) || (strip_index >= num_strips) || (num_channels < 1) || (num_channels > BCP_MAX_CHANNELS))
        return false;

    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;

    byte_stream stream = {.buffer = (uint8_t*)input, .length = input_length, .pos = 0};

    // load models: per channel, the 2 endpoint models then the 64 index models
    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
            if (!load_static_model(&ctx->bc4_endpoint_model[c][e], &stream))
                return false;
        for(uint32_t i=0; i<BC4_INDEX_CONTEXTS; ++i)
            if (!load_static_model(&ctx->bc4_index_model[c][i], &stream))
                return false;
    }

    if (stream.pos + 2 * num_channels > stream.length)
        return false;

    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
            ctx->bc4_strip_seed[c][e] = stream.buffer[stream.pos++];
    }

    if (stream.pos + num_channels > stream.length)
        return false;
    for(uint32_t c=0; c<num_channels; ++c)
    {
        const uint8_t flat_byte = stream.buffer[stream.pos++]; // flat-index mode + value, after the seeds (see bc4_index_mode)
        ctx->bc4_index_mode[c] = flat_byte & 1;
        ctx->bc4_index_value[c] = (flat_byte >> 1) & 7;
    }

    read_stream_align(&stream, sizeof(uint16_t));
    size_t strips_offset_array_size = sizeof(uint16_t) * (num_strips - 1);

    if (stream.pos + strips_offset_array_size >= stream.length)
        return false;

    uint16_t* strips_offset_array = (uint16_t*)&stream.buffer[stream.pos];
    stream.pos += strips_offset_array_size;

    read_stream_align(&stream, sizeof(uint32_t));

    // compute the current strip offset and setup compressed stream; strip 0's offset is always zero
    size_t strip_offset = stream.pos;
    for(uint32_t i=0; i<strip_index; ++i)
        strip_offset += (size_t)strips_offset_array[i] * sizeof(uint32_t);

    assert(stream.length > strip_offset);

    le_stream compressed_stream;
    le_init(&compressed_stream, &stream.buffer[strip_offset], stream.length - strip_offset);
    le_begin_decode(&compressed_stream);

    uint8_t prev_e[BCP_MAX_CHANNELS][2], top_e[BCP_MAX_CHANNELS][2], top_prev_e[BCP_MAX_CHANNELS][2];
    for(uint32_t c=0; c<num_channels; ++c)
    {
        for(uint32_t e=0; e<2; ++e)
        {
            prev_e[c][e] = ctx->bc4_strip_seed[c][e];
            top_e[c][e] = 0;
            top_prev_e[c][e] = 0;
        }
    }

    tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
    uint32_t tile_width = tb.x1 - tb.x0;

    for(uint32_t y = tb.y0; y < tb.y1; ++y)
    {
        for(uint32_t x = 0; x < tile_width; ++x)
        {
            // zig-zag pattern, same chain as BC1
            uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
            const bool first_row = (y == tb.y0);
            const bool row_start = (x == 0);

            for(uint32_t c=0; c<num_channels; ++c)
            {
                bc4_block* b = (bc4_block*)output + (y * width_blocks + zigzag_x) * block_stride + c;
                uint8_t idx[16];

                if (!first_row)
                {
                    const bc4_block* t = (const bc4_block*)output + ((y - 1) * width_blocks + zigzag_x) * block_stride + c;
                    top_e[c][0] = t->endpoint[0];
                    top_e[c][1] = t->endpoint[1];
                }

                // endpoint payload: the two wrapped deltas against the strip-chain predictor
                for(uint32_t e=0; e<2; ++e)
                {
                    uint8_t pred = bc4_endpoint_predict(first_row, row_start, prev_e[c][e], top_e[c][e], top_prev_e[c][e]);

                    b->endpoint[e] = delta_decode_wrap(pred, le_decode_symbol(&compressed_stream, &ctx->bc4_endpoint_model[c][e]));
                }

                // index payload: the first index in raw 3 bits, then each index xored with its
                // intra-block prediction, contextual on the 8 x 8 [left][top] matrix
                // a flat block (identical endpoints, reconstructed exactly like the encoder sees the
                // source) in mode 1 stores no index payload: the header value is expanded to all 16
                const bool flat = (b->endpoint[0] == b->endpoint[1]);
                if (flat && ctx->bc4_index_mode[c])
                {
                    for(uint32_t i=0; i<16; ++i)
                        idx[i] = ctx->bc4_index_value[c];
                }
                else
                {
                    idx[0] = le_read_bits(&compressed_stream, 3);

                    for(uint32_t i=1; i<16; ++i)
                    {
                        uint8_t left = ((i & 3) == 0) ? 0 : idx[i-1];
                        uint8_t top = (i < 4) ? 0 : idx[i-4];
                        uint8_t p = ((i & 3) == 0) ? top : left;

                        idx[i] = (uint8_t)(p ^ le_decode_symbol(&compressed_stream, &ctx->bc4_index_model[c][bc4_index_context(left, top)]));
                    }
                }

                bc4_pack_indices(idx, b);

                if (!first_row)
                {
                    top_prev_e[c][0] = top_e[c][0];
                    top_prev_e[c][1] = top_e[c][1];
                }
                prev_e[c][0] = b->endpoint[0];
                prev_e[c][1] = b->endpoint[1];
            }
        }
    }

    return compressed_stream.status == LE_OK;
}


//-----------------------------------------------------------------------------------------------------------------------------
// Public functions
//-----------------------------------------------------------------------------------------------------------------------------


//-----------------------------------------------------------------------------------------------------------------------------
bcp_context* bcp_init(bcp_mem_interface* user_mem)
{
    bcp_mem_interface mem = (user_mem) ? *user_mem : default_allocator();

    bcp_context* ctx = mem.malloc_fn(sizeof(bcp_context), mem.user);
    if (ctx == NULL)
        return NULL;

    *ctx = (bcp_context) {.mem = mem };

    return ctx;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t packed_bc1_maxsize(uint32_t width, uint32_t height)
{
    // very rough. the +4 covers the worst-case header growth of the flat-index mode byte: it is written
    // right after the top-table size, before the 4-byte align, so the header can grow by up to the
    // 4-byte alignment quantum
    return (width/4) * (height/4) * 2 * sizeof(bc1_block) + 4;
}

//-----------------------------------------------------------------------------------------------------------------------------
// [block_stride] is the distance between blocks in units of bc1_block: 1 for a plain BC1 grid,
// 2 for the color half of a BC3 grid (see bcp_compress_bc3)
static size_t bcp_compress_bc1_impl(bcp_context* ctx, const void* bc1_image, uint32_t block_stride, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips)
{
    if (width < 16 || height < 256 || !bc1_image || !ctx || !output || !num_strips)
        return 0;

    *num_strips = 64;   // for now, could depend on the resolution in the future

    const uint32_t num_blocks = (width*height) / 16;
    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;

    if (ctx->hashmap == NULL)
        ctx->hashmap = ctx->mem.malloc_fn(sizeof(hashmap_entry) * HASHMAP_SIZE, ctx->mem.user);

    build_top_table(ctx->hashmap, bc1_image, num_blocks, block_stride, ctx->top_table, &ctx->top_table_size);
    init_static_models(ctx, bc1_image, width_blocks, height_blocks, *num_strips, block_stride);

    byte_stream stream = {.buffer = output, .length = output_length, .pos = 0};

    // store models: per endpoint, the contextual red, green, and blue models (3 contexts each) —
    // 16 color models — then the index models
    for(uint32_t j=0; j<2; ++j)
    {
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            if (!save_static_model(&ctx->red_model[j][c], &stream)) return 0;
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            if (!save_static_model(&ctx->green_model[j][c], &stream)) return 0;
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            if (!save_static_model(&ctx->blue_model[j][c], &stream)) return 0;
    }
    if (!save_static_model(&ctx->table_reference_model, &stream)) return 0;
    if (!save_static_model(&ctx->mask_model, &stream)) return 0;
    if (!save_static_model(&ctx->table_difference_model, &stream)) return 0;

    // top-table + flat-index mode + strip seed + predictor flags + dictionary enable flags
    if (stream.pos + 1 + 1 + 3 + sizeof(uint32_t) + 8 + 8 + ctx->top_table_size * sizeof(uint32_t) > stream.length)
        return 0;

    stream.buffer[stream.pos++] = (uint8_t) (ctx->top_table_size - 1); // there is no zero toptable, so minus 1 to fit in a uint8_t
    stream.buffer[stream.pos++] = (uint8_t)(ctx->bc1_index_mode | (ctx->bc1_index_value << 1)); // flat-index mode + value, written in the alignment padding (see bc1_index_mode)
    stream_align(&stream, sizeof(uint32_t));

    for(uint32_t j=0; j<4; ++j)
        stream.buffer[stream.pos++] = (uint8_t) ((ctx->strip_seed >> (j*8)) & 0xff);

    for(uint32_t j=0; j<8; ++j)
        stream.buffer[stream.pos++] = (uint8_t) ((ctx->strip_predictor >> (j*8)) & 0xff);

    for(uint32_t j=0; j<8; ++j)
        stream.buffer[stream.pos++] = (uint8_t) ((ctx->strip_dict_enable >> (j*8)) & 0xff);

    for(uint32_t i=0; i<ctx->top_table_size; ++i)
        for(uint32_t j=0; j<4; ++j)
            stream.buffer[stream.pos++] = (ctx->top_table[i] >> (j*8)) & 0xff;

    stream_align(&stream, sizeof(uint16_t));
    size_t strips_offset_array_size = sizeof(uint16_t) * (*num_strips - 1);

    // check if we have enough space
    if (stream.pos + strips_offset_array_size >= stream.length)
        return 0;
    
    // keep a pointer to strips offset
    uint16_t* strips_offset = (uint16_t*) &stream.buffer[stream.pos];
    stream.pos += strips_offset_array_size;

    // compressed data is aligned on 4 bytes to allow fast gpu loading
    stream_align(&stream, sizeof(uint32_t));
    size_t previous_offset = stream.pos;

    for(uint32_t strip_index=0; strip_index<*num_strips; ++strip_index)
    {
        le_stream compressed_stream;
        le_init(&compressed_stream, &stream.buffer[stream.pos], stream.length - stream.pos);
        le_begin_encode(&compressed_stream);

        // strip offset is counted in dword, based on the previous one (delta compression);
        // strip 0 always starts the strip data so its offset is zero and not stored
        if (strip_index > 0)
            strips_offset[strip_index - 1] = (uint16_t)((stream.pos - previous_offset) / sizeof(uint32_t));
        previous_offset = stream.pos;

        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);
        color_dict dict;
        memset(&dict, 0, sizeof(dict));
        const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
        const bool dict_enabled = ((ctx->strip_dict_enable >> strip_index) & 1) != 0;
        tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, *num_strips);
        uint32_t tile_width = tb.x1 - tb.x0;

        for(uint32_t y = tb.y0; y < tb.y1; ++y)
        {
            for(uint32_t x = 0; x < tile_width; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
                const bc1_block* current = (const bc1_block*) bc1_image + ((y * width_blocks) + zigzag_x) * block_stride;

                uint32_t position = (y - tb.y0) * tile_width + x;
                uint32_t pair = color_pair(current);

                // a disabled strip never matches: no flag bit, the six color deltas go straight into the stream
                uint32_t match = dict_enabled ? color_dict_match(&dict, position, pair) : 0;

                // color payload: an enabled strip writes a 1-bit flag, then either a reference to the matching
                // block (the six color-delta symbols are skipped) or the six deltas predicted from the color
                // reference; a disabled strip writes the six deltas directly, no flag
                if (dict_enabled)
                    le_write_bits(&compressed_stream, match ? 1 : 0, 1);
                if (match)
                    le_write_bits(&compressed_stream, match - 1, color_ref_bits(position));
                else
                {
                    // first scanline of a tile is always predicted from the seed via the zigzag-previous
                    // chain, after that a top-flagged tile is predicted from the row above, same zigzag position
                    const bc1_block* color_reference = &previous;
                    if (use_top && y > tb.y0)
                        color_reference = (const bc1_block*) bc1_image + ((y - 1) * width_blocks + zigzag_x) * block_stride;

                    int first_green_delta = 0;
                    for(uint32_t j=0; j<2; ++j)
                    {
                        uint8_t current_red, current_green, current_blue;
                        uint8_t reference_red, reference_green, reference_blue;

                        bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
                        bc1_extract_565(color_reference->color[j], &reference_red, &reference_green, &reference_blue);

                        int dred = current_red - reference_red;
                        int dgreen = current_green - reference_green;
                        int dblue = current_blue - reference_blue;

                        // endpoint 0's green is uncontexted, endpoint 1's is contextual on the sign of endpoint 0's
                        uint8_t green_context = (j == 0) ? 0 : color_delta_context(first_green_delta);
                        le_encode_symbol(&compressed_stream, &ctx->green_model[j][green_context], dgreen + COLOR_DELTA_OFFSET);

                        if (j == 0)
                            first_green_delta = dgreen;

                        // the red/blue deltas scale with the endpoint's own green delta, bucket it before halving
                        uint8_t chroma_context = color_delta_context(dgreen);

                        dgreen /= 2;
                        dred -= dgreen;
                        dblue -= dgreen;

                        le_encode_symbol(&compressed_stream, &ctx->red_model[j][chroma_context], dred + COLOR_DELTA_OFFSET);
                        le_encode_symbol(&compressed_stream, &ctx->blue_model[j][chroma_context], dblue + COLOR_DELTA_OFFSET);
                    }
                }

                // a flat block (identical endpoints) in mode 1 stores no index payload: the decoder
                // expands the header value to all 16 indices; otherwise the full reference + mask +
                // differences
                const bool flat = (current->color[0] == current->color[1]);
                if (!(flat && ctx->bc1_index_mode))
                {
                    uint8_t reference = nearest32(ctx->top_table, ctx->top_table_size, current->indices) & 0xff;

                    le_encode_symbol(&compressed_stream, &ctx->table_reference_model, reference);

                    uint32_t difference = current->indices ^ ctx->top_table[reference];
                    uint32_t mask = 0;
                    if ((difference & 0x000000FF) != 0) mask |= 1;
                    if ((difference & 0x0000FF00) != 0) mask |= 2;
                    if ((difference & 0x00FF0000) != 0) mask |= 4;
                    if ((difference & 0xFF000000) != 0) mask |= 8;

                    le_encode_symbol(&compressed_stream, &ctx->mask_model, mask);

                    for(uint32_t j=0; j<4; ++j)
                        if (mask & (1u << j))
                            le_encode_symbol(&compressed_stream, &ctx->table_difference_model, (difference >> (j*8)) & 0xff);
                }

                dict.entry[position & COLOR_DICT_MASK] = pair;
                previous = *current;
            }
        }

        size_t strip_size = le_end_encode(&compressed_stream);
        assert(strip_size < 262144); // if it happens, there is a problem somewhere else

        stream.pos += strip_size;

        stream_align(&stream, sizeof(uint32_t));

        if (compressed_stream.status == LE_BUFFER_OVERRUN)
            return 0;
    }

    return stream.pos;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bcp_compress_bc1(bcp_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips)
{
    return bcp_compress_bc1_impl(ctx, bc1_image, 1, width, height, output, output_length, num_strips);
}

//-----------------------------------------------------------------------------------------------------------------------------
// [block_stride] is the distance between blocks in units of bc1_block: 1 for a plain BC1 grid,
// 2 for the color half of a BC3 grid (see bcp_decompress_bc3)
static bool bcp_decompress_bc1_impl(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index, uint32_t block_stride)
{
    if (width < 16 || height < 256 || !input || !ctx || !output || strip_index >= num_strips)
        return false;

    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;

    byte_stream stream = {.buffer = (uint8_t *) input, .length = input_length, .pos = 0};

    // load models: per endpoint, the contextual red, green, and blue models (3 contexts each) —
    // 16 color models — then the index models
    for(uint32_t j=0; j<2; ++j)
    {
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            if (!load_static_model(&ctx->red_model[j][c], &stream)) return false;
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            if (!load_static_model(&ctx->green_model[j][c], &stream)) return false;
        for(uint32_t c=0; c<COLOR_DELTA_CONTEXTS; ++c)
            if (!load_static_model(&ctx->blue_model[j][c], &stream)) return false;
    }
    if (!load_static_model(&ctx->table_reference_model, &stream)) return false;
    if (!load_static_model(&ctx->mask_model, &stream)) return false;
    if (!load_static_model(&ctx->table_difference_model, &stream)) return false;

    // top-table
    if (stream.pos >= stream.length)
        return false;

    ctx->top_table_size = (uint32_t)stream.buffer[stream.pos++] + 1;
    if (stream.pos + 1 > stream.length)
        return false;
    {
        const uint8_t flat_byte = stream.buffer[stream.pos++]; // flat-index mode + value, written right after the top-table size (see bc1_index_mode)
        ctx->bc1_index_mode = flat_byte & 1;
        ctx->bc1_index_value = (flat_byte >> 1) & 3;
    }
    read_stream_align(&stream, sizeof(uint32_t));

    if (stream.pos + sizeof(uint32_t) + 8 + 8 > stream.length)
        return false;

    ctx->strip_seed = 0;
    for(uint32_t j=0; j<4; ++j)
        ctx->strip_seed |= (uint32_t)stream.buffer[stream.pos++] << (j*8);

    ctx->strip_predictor = 0;
    for(uint32_t j=0; j<8; ++j)
        ctx->strip_predictor |= (uint64_t)stream.buffer[stream.pos++] << (j*8);

    ctx->strip_dict_enable = 0;
    for(uint32_t j=0; j<8; ++j)
        ctx->strip_dict_enable |= (uint64_t)stream.buffer[stream.pos++] << (j*8);

    for(uint32_t i=0; i<ctx->top_table_size; ++i)
    {
        ctx->top_table[i] = 0;
    
        for(uint32_t j=0; j<4; ++j)
            ctx->top_table[i] |= (uint32_t)stream.buffer[stream.pos++] << (j*8);
    }

    // strip offsets
    read_stream_align(&stream, sizeof(uint16_t));
    size_t strips_offset_array_size = sizeof(uint16_t) * (num_strips - 1);

    if (stream.pos + strips_offset_array_size >= stream.length)
        return false;
    
    uint16_t* strips_offset_array = (uint16_t*) &stream.buffer[stream.pos];
    stream.pos += strips_offset_array_size;

    read_stream_align(&stream, sizeof(uint32_t));

    // compute the current strip offset and setup compressed stream; strip 0's offset is always zero
    // (it starts the strip data) so it is not stored, the stored deltas cover strips 1..N-1
    size_t strip_offset = stream.pos;
    for(uint32_t i=0; i<strip_index; ++i)
        strip_offset += strips_offset_array[i] * sizeof(uint32_t);

    assert(stream.length > strip_offset);

    le_stream compressed_stream;
    le_init(&compressed_stream, &stream.buffer[strip_offset], stream.length - strip_offset);
    le_begin_decode(&compressed_stream);
    
    bc1_block previous;
    unpack_strip_seed(ctx->strip_seed, &previous);
    color_dict dict;
    memset(&dict, 0, sizeof(dict));
    const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
    const bool dict_enabled = ((ctx->strip_dict_enable >> strip_index) & 1) != 0;
    tile_bounds tb = tile_bounds_get(strip_index, width_blocks, height_blocks, num_strips);
    uint32_t tile_width = tb.x1 - tb.x0;

    for(uint32_t y = tb.y0; y < tb.y1; ++y)
    {
        for(uint32_t x = 0; x < tile_width; ++x)
        {
            uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
            bc1_block* current = (bc1_block*) output + ((y * width_blocks) + zigzag_x) * block_stride;

            uint32_t position = (y - tb.y0) * tile_width + x;

            // color payload: an enabled strip writes a 1-bit flag, then either a dictionary reference
            // (copy the two endpoint colors of the referenced block, skipping the six delta symbols)
            // or the six deltas; a disabled strip writes the six deltas directly, no flag
            uint8_t match = dict_enabled ? le_read_bits(&compressed_stream, 1) : 0;
            if (match)
            {
                uint8_t ref = le_read_bits(&compressed_stream, color_ref_bits(position));
                uint32_t pair = dict.entry[(position - ref - 1) & COLOR_DICT_MASK];
                unpack_color_pair(pair, &current->color[0], &current->color[1]);
            }
            else
            {
                // first scanline of a tile is always predicted from the seed via the zigzag-previous chain,
                // after that a top-flagged tile is predicted from the row above, same zigzag position
                const bc1_block* color_reference = &previous;
                if (use_top && y > tb.y0)
                    color_reference = (const bc1_block*) output + ((y - 1) * width_blocks + zigzag_x) * block_stride;

                int first_green_delta = 0;
                for(uint32_t j=0; j<2; ++j)
                {
                    uint8_t reference_red, reference_green, reference_blue;
                    bc1_extract_565(color_reference->color[j], &reference_red, &reference_green, &reference_blue);

                    // endpoint 0's green is uncontexted, endpoint 1's is contextual on the sign of endpoint 0's
                    uint8_t green_context = (j == 0) ? 0 : color_delta_context(first_green_delta);
                    uint8_t green_symbol = le_decode_symbol(&compressed_stream, &ctx->green_model[j][green_context]);

                    // red and blue delta are based on green delta
                    int dgreen_orig = (int)green_symbol - COLOR_DELTA_OFFSET;
                    if (j == 0)
                        first_green_delta = dgreen_orig;
                    uint8_t chroma_context = color_delta_context(dgreen_orig);

                    uint8_t delta_red = le_decode_symbol(&compressed_stream, &ctx->red_model[j][chroma_context]);
                    uint8_t delta_blue = le_decode_symbol(&compressed_stream, &ctx->blue_model[j][chroma_context]);

                    int current_green_value = reference_green + dgreen_orig;
                    int dgreen_halved = dgreen_orig / 2;

                    int dred_orig = ((int)delta_red - COLOR_DELTA_OFFSET) + dgreen_halved;
                    int dblue_orig = ((int)delta_blue - COLOR_DELTA_OFFSET) + dgreen_halved;

                    int current_red_value = reference_red + dred_orig;
                    int current_blue_value = reference_blue + dblue_orig;

                    current->color[j] = bc1_pack_565((uint8_t)current_red_value, (uint8_t)current_green_value, (uint8_t)current_blue_value);
                }
            }
            
            // a flat block (identical endpoints, reconstructed exactly like the encoder sees the source) in
            // mode 1 stores no index payload: the header value is expanded to all 16 indices; otherwise
            // the full reference + mask + differences
            const bool flat = (current->color[0] == current->color[1]);
            if (flat && ctx->bc1_index_mode)
                current->indices = (uint32_t)ctx->bc1_index_value * 0x11111111u;
            else
            {
                uint32_t reference = le_decode_symbol(&compressed_stream, &ctx->table_reference_model);
                uint32_t mask = le_decode_symbol(&compressed_stream, &ctx->mask_model);

                uint32_t difference=0;
                for(uint32_t j=0; j<4; ++j)
                    if (mask & (1 << j))
                        difference = difference | ((uint32_t)le_decode_symbol(&compressed_stream, &ctx->table_difference_model) << (j*8));

                current->indices =  difference ^ ctx->top_table[reference];
            }

            dict.entry[position & COLOR_DICT_MASK] = color_pair(current);
            previous = *current;
        }
    }

    return compressed_stream.status == LE_OK;
}

//-----------------------------------------------------------------------------------------------------------------------------
bool bcp_decompress_bc1(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index)
{
    return bcp_decompress_bc1_impl(ctx, input, input_length, width, height, num_strips, output, strip_index, 1);
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t packed_bc4_maxsize(uint32_t width, uint32_t height)
{
    // very rough: 2x the standard BC4 block size, the BC1 convention. the +2 covers the worst-case
    // header growth of the flat-index mode byte (one byte, before the 2-byte align = the align quantum)
    return (width/4) * (height/4) * 2 * sizeof(bc4_block) + 2;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t packed_bc5_maxsize(uint32_t width, uint32_t height)
{
    // very rough: 2x the standard BC5 block size (a red + a green bc4_block). the +3 covers the
    // worst-case header growth of the flat-index mode bytes (2 bytes, before the 2-byte align, plus
    // the alignment itself in the worst parity case)
    return (width/4) * (height/4) * 2 * (2 * sizeof(bc4_block)) + 3;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bcp_compress_bc4(bcp_context* ctx, const void* bc4_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips)
{
    return bcp_compress_bc45(ctx, bc4_image, width, height, output, output_length, num_strips, 1, 1);
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bcp_compress_bc5(bcp_context* ctx, const void* bc5_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips)
{
    return bcp_compress_bc45(ctx, bc5_image, width, height, output, output_length, num_strips, 2, 2);
}

//-----------------------------------------------------------------------------------------------------------------------------
bool bcp_decompress_bc4(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index)
{
    return bcp_decompress_bc45(ctx, input, input_length, width, height, num_strips, output, strip_index, 1, 1);
}

//-----------------------------------------------------------------------------------------------------------------------------
bool bcp_decompress_bc5(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index)
{
    return bcp_decompress_bc45(ctx, input, input_length, width, height, num_strips, output, strip_index, 2, 2);
}


//-----------------------------------------------------------------------------------------------------------------------------
// BC3 = a BC1 block + a BC4 block per 4x4 cell (the standard D3D layout, 16 bytes per block).
// The compressed stream is the composition of the two already validated pipelines: a
// little-endian u32 prefix with the size of the BC1 substream, then the BC1 stream of the
// color half, then the BC4 (1 channel) stream of the alpha half. Each substream is
// self-describing (models, strip seed, strip offsets, strips, as its standalone format) and
// ends 4-byte aligned, so no new payload logic, model, or context state is needed.
// The color half is the block at byte offset 0 of a 16-byte superblock and the alpha half the
// one at byte offset 8, so both views use a block stride of 2.

//-----------------------------------------------------------------------------------------------------------------------------
size_t packed_bc3_maxsize(uint32_t width, uint32_t height)
{
    // 2x the standard BC3 block size, the BC1 / BC4 convention, plus the 4-byte prefix
    return packed_bc1_maxsize(width, height) + packed_bc4_maxsize(width, height) + sizeof(uint32_t);
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bcp_compress_bc3(bcp_context* ctx, const void* bc3_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips)
{
    if ((width < 16) || (height < 256) || !bc3_image || !ctx || !output || !num_strips || (output_length < sizeof(uint32_t)))
        return 0;

    *num_strips = 64;   // for now, could depend on the resolution in the future

    // prefix: the size of the BC1 substream, patched once it is known. The BC1 stream ends 4-byte
    // aligned (each strip does), so the BC4 substream that follows is 4-byte aligned too.
    uint32_t bc1_size = 0;
    output[0] = (uint8_t)(bc1_size & 0xff);
    output[1] = (uint8_t)((bc1_size >> 8) & 0xff);
    output[2] = (uint8_t)((bc1_size >> 16) & 0xff);
    output[3] = (uint8_t)((bc1_size >> 24) & 0xff);

    // color half: the BC1 pipeline on the blocks at byte offset 0, stride 2
    bc1_size = (uint32_t)bcp_compress_bc1_impl(ctx, bc3_image, 2, width, height, output + sizeof(uint32_t), output_length - sizeof(uint32_t), num_strips);
    if (bc1_size == 0)
        return 0;

    output[0] = (uint8_t)(bc1_size & 0xff);
    output[1] = (uint8_t)((bc1_size >> 8) & 0xff);
    output[2] = (uint8_t)((bc1_size >> 16) & 0xff);
    output[3] = (uint8_t)((bc1_size >> 24) & 0xff);

    // alpha half: the single-channel BC4 pipeline on the blocks at byte offset 8, stride 2
    size_t bc4_size = bcp_compress_bc45(ctx, (const uint8_t*)bc3_image + 8, width, height, output + sizeof(uint32_t) + bc1_size,
                                        output_length - sizeof(uint32_t) - bc1_size, num_strips, 1, 2);
    if (bc4_size == 0)
        return 0;

    return sizeof(uint32_t) + bc1_size + bc4_size;
}

//-----------------------------------------------------------------------------------------------------------------------------
bool bcp_decompress_bc3(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index)
{
    const uint8_t* in = (const uint8_t*)input;

    if ((width < 16) || (height < 256) || !in || !ctx || !output || (num_strips == 0) || (strip_index >= num_strips) || (input_length < sizeof(uint32_t)))
        return false;

    uint32_t bc1_size = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if ((size_t)bc1_size + sizeof(uint32_t) > input_length)
        return false;

    // color half, then alpha half, each on its own substream of the same strip decomposition
    bool ok = bcp_decompress_bc1_impl(ctx, in + sizeof(uint32_t), bc1_size, width, height, num_strips, output, strip_index, 2);

    return ok && bcp_decompress_bc45(ctx, in + sizeof(uint32_t) + bc1_size, input_length - sizeof(uint32_t) - bc1_size, width, height, num_strips,
                                     (uint8_t*)output + 8, strip_index, 1, 2);
}


//-----------------------------------------------------------------------------------------------------------------------------
void bcp_terminate(bcp_context* ctx)
{
    bcp_mem_interface mem = ctx->mem;

    mem.free_fn(ctx->hashmap, mem.user);
    mem.free_fn(ctx, mem.user);
}
