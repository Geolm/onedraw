//-----------------------------------------------------------------------------------------------------------------------------
// bc_decompress.metal
//
// For more details about the algorithm : https://github.com/Geolm/bc_packed
//-----------------------------------------------------------------------------------------------------------------------------

#include <metal_stdlib>
using namespace metal;

constant uint NUM_STRIPS = 64; // 64 independent strips, one decoded thread per strip
constant uint LE_ALPHABET_SIZE = 256;
constant uint BC4_INDEX_ALPHABET_SIZE = 8; // bc4 index models: 8 prediction values
constant uint BC4_INDEX_DIM = 8;           // bc4 index context: the 8 x 8 [left][top] matrix
constant uint BC4_INDEX_CONTEXTS = BC4_INDEX_DIM * BC4_INDEX_DIM;
constant uint BC4_DICT_SIZE = 255;         // bc4 index-word dictionary: list length, the rank model holds + 1 escape
constant uint LE_MAX_K = 8;

// shared by every kernel: the stream carries no width / height / format, the caller provides them
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint64_t input_length;
} decode_args_t;

// mirror of le_model: the decoder only needs the rank table, k and q_escape (no symbol index)
typedef struct
{
    uint8_t alphabet[LE_ALPHABET_SIZE];
    uint8_t k;          // bit 7 = raw mode flag, lower bits = raw bit width or rice k
    uint8_t q_escape;
    uint16_t num_symbols;
} model_t;

// mirror of the bc4 8-symbol le_model: the rank table only needs the 8 rank entries (see model_t)
typedef struct
{
    uint8_t alphabet[BC4_INDEX_ALPHABET_SIZE];
    uint8_t k;          // bit 7 = raw mode flag, lower bits = raw bit width or rice k
    uint8_t q_escape;
    uint16_t num_symbols;
} model8_t;

// tile columns of the 64-tile grid: 8 for wide images, shrinking so the tiles stay at least
// 4 blocks wide, 1 for the narrowest supported image (the 16 px minimum)
uint tile_grid_x(uint width_blocks)
{
    if (width_blocks >= 32) return 8;
    if (width_blocks >= 16) return 4;
    if (width_blocks >= 8)  return 2;
    return 1;
}

// block bounds of tile [strip_index] of the 64-tile grid, x1 / y1 one past. the even split of
// the block grid covers the image exactly, so images whose block count is not a multiple of
// the tile size lose no rows or columns (the old strip truncation dropped the trailing rows)
typedef struct tile_bounds
{
    uint x0, x1;
    uint y0, y1;
} tile_bounds;

tile_bounds tile_bounds_get(uint strip_index, uint width_blocks, uint height_blocks)
{
    uint gx = tile_grid_x(width_blocks);
    uint gy = NUM_STRIPS / gx;
    uint tx = strip_index % gx;
    uint ty = strip_index / gx;

    return tile_bounds { .x0 = (tx * width_blocks) / gx, .x1 = ((tx + 1) * width_blocks) / gx,
                         .y0 = (ty * height_blocks) / gy, .y1 = ((ty + 1) * height_blocks) / gy };
}

// mirror of le_stream: a 64-bit reservoir buffers bits to keep I/O at byte granularity,
// least significant bit read first
typedef struct
{
    const device uint8_t* buffer;
    uint64_t size;
    uint64_t position;
    uint64_t bit_reservoir;
    uint32_t bits_available;
    bool overrun;
} bit_stream_t;


//-----------------------------------------------------------------------------------------------------------------------------
// pull bytes into the reservoir until it's full enough for any standard read
void bs_refill(thread bit_stream_t* s)
{
    while (s->bits_available <= 56 && s->position < s->size)
    {
        s->bit_reservoir |= (uint64_t)s->buffer[s->position] << s->bits_available;
        s->bits_available += 8;
        s->position++;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// reads [num_bits] (1 to 8) least significant bits of the reservoir
uint8_t bs_read_bits(thread bit_stream_t* s, uint32_t num_bits)
{
    if (s->bits_available < num_bits)
    {
        bs_refill(s);
        if (s->bits_available < num_bits)
        {
            s->overrun = true;
            return 0;
        }
    }

    uint32_t value = (uint32_t)(s->bit_reservoir & (((uint64_t)1 << num_bits) - 1));
    s->bit_reservoir >>= num_bits;
    s->bits_available -= num_bits;
    return (uint8_t)value;
}

//-----------------------------------------------------------------------------------------------------------------------------
uint8_t bs_read_byte(thread bit_stream_t* s)
{
    return bs_read_bits(s, 8);
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of rice_decode: q = run of ones from the LSB (terminated by a zero), then either a
// raw rank byte (q reached q_escape) or the k remainder bits
uint8_t rice_decode(thread bit_stream_t* s, uint32_t k, uint32_t q_escape)
{
    if (s->bits_available < 32)
        bs_refill(s);

    // count the consecutive ones from the LSB: the index of the first zero. bit 63 is force
    // set in the mask so the count can never run past the 64-bit word (ctz = count trailing zeros)
    uint64_t mask = ~s->bit_reservoir | (((uint64_t)1) << 63);
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);

    // MSL has no 64-bit ctz: count in the low half, or 32 + the high half if the low half is all ones
    uint32_t q = (lo != 0) ? (uint32_t)ctz((int)lo) : ((uint32_t)32u + (uint32_t)ctz((int)hi));

    if (q >= q_escape)
    {
        if (s->bits_available < (q_escape + 1))
        {
            s->overrun = true;
            return 0;
        }

        s->bit_reservoir >>= (q_escape + 1);
        s->bits_available -= (q_escape + 1);
        return bs_read_byte(s);
    }

    uint32_t total_bits = q + 1 + k;
    if ((s->bits_available < total_bits) || (total_bits >= 64))
    {
        s->overrun = true;
        return 0;
    }

    uint32_t val = (uint32_t)s->bit_reservoir;

    s->bit_reservoir >>= total_bits;
    s->bits_available -= total_bits;

    uint32_t r = (val >> (q + 1)) & (((uint32_t)1 << k) - 1);
    return (uint8_t)((q << k) | r);
}

//-----------------------------------------------------------------------------------------------------------------------------
// decodes one symbol: read the rank (fixed-width in raw mode, otherwise Rice), then map it
// back through the rank table
uint8_t decode_symbol(thread bit_stream_t* s, const thread model_t* m)
{
    uint8_t index = (m->k & 0x80) ? bs_read_bits(s, m->k & 0x7f) : rice_decode(s, m->k, m->q_escape);

    return m->alphabet[index];
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of le_decode_symbol for the compact 8-symbol bc4 index models: the CPU's 256-entry
// rank table is zero-filled past num_symbols, so an out-of-range rank (corrupted stream) reads 0
// there; the 8-entry table cannot, so the bound check emulates it (valid streams always have
// rank < num_symbols, the encoder only writes the ranks of the symbols it fitted)
uint8_t decode_symbol8(thread bit_stream_t* s, const thread model8_t* m)
{
    uint8_t index = (m->k & 0x80) ? bs_read_bits(s, m->k & 0x7f) : rice_decode(s, m->k, m->q_escape);

    return (index < m->num_symbols) ? m->alphabet[index] : 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of load_static_model / le_static_model_load
bool load_model(const device uint8_t* input, uint64_t length, thread uint64_t* pos, thread model_t* m)
{
    if (*pos + 3 > length)
        return false;

    uint64_t num_symbols = (uint64_t)input[*pos] + 1;
    uint8_t k = input[*pos + 1];
    uint8_t q_escape = input[*pos + 2];

    // k stores the raw-mode flag in bit 7 and a width / rice parameter (<= LE_MAX_K) in the lower bits
    if ((k & 0x7f) > LE_MAX_K)
        k = (uint8_t)((k & 0x80) | LE_MAX_K);

    *pos += 3;
    if (*pos + num_symbols > length)
        return false;

    for (uint32_t i = 0; i < LE_ALPHABET_SIZE; ++i)
        m->alphabet[i] = 0;

    for (uint64_t i = 0; i < num_symbols; ++i)
        m->alphabet[i] = input[*pos + i];

    *pos += num_symbols;
    m->k = k;
    m->q_escape = q_escape;
    m->num_symbols = (uint16_t)num_symbols;

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of load_static_model for the compact 8-symbol bc4 index models: a corrupted stream
// may claim up to 256 symbols, so only the first 8 are stored in the rank table and num_symbols
// is clamped with them; the CPU's zero-filled 256-entry table returns 0 for the same out-of-range
// ranks (decode_symbol8), so both still decode the same bytes
bool load_model8(const device uint8_t* input, uint64_t length, thread uint64_t* pos, thread model8_t* m)
{
    if (*pos + 3 > length)
        return false;

    uint64_t num_symbols = (uint64_t)input[*pos] + 1;
    uint8_t k = input[*pos + 1];
    uint8_t q_escape = input[*pos + 2];

    // k stores the raw-mode flag in bit 7 and a width / rice parameter (<= LE_MAX_K) in the lower bits
    if ((k & 0x7f) > LE_MAX_K)
        k = (uint8_t)((k & 0x80) | LE_MAX_K);

    *pos += 3;
    if (*pos + num_symbols > length)
        return false;

    for (uint32_t i = 0; i < BC4_INDEX_ALPHABET_SIZE; ++i)
        m->alphabet[i] = 0;

    uint64_t stored = (num_symbols > BC4_INDEX_ALPHABET_SIZE) ? BC4_INDEX_ALPHABET_SIZE : num_symbols;

    for (uint64_t i = 0; i < stored; ++i)
        m->alphabet[i] = input[*pos + i];

    *pos += num_symbols;
    m->k = k;
    m->q_escape = q_escape;
    m->num_symbols = (uint16_t)stored;

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// little endian u16 / u32 reads from the stream. The stream aligns strip offsets on u16 and
// the dictionary enable word on u32, so read the word directly (cheaper than per-byte gather)
uint16_t read_u16(const device uint8_t* input, uint64_t pos)
{
    return ((const device uint16_t*)input)[pos >> 1];
}

uint32_t read_u32(const device uint8_t* input, uint64_t pos)
{
    return ((const device uint32_t*)input)[pos >> 2];
}

//-----------------------------------------------------------------------------------------------------------------------------
// bcp_decompress_bc4: GPU mirror of bcp_decompress_bc4 (third_party/bc_packed.c): one thread
// decompresses one strip, the standard block layout is two 8-bit endpoints followed by the
// 48-bit little-endian field of the 16 3-bit indices, 8 bytes per block.
//
// mirror of bc4_endpoint_predict (third_party/bc_packed.c): the first row of a strip is predicted
// from the chain-previous block (the strip seed for the first block), a row start from the top
// block only, and every other block from the 2D predictor previous + top - top_previous
uint8_t bc4_endpoint_predict(bool first_row, bool row_start, uint8_t prev, uint8_t top, uint8_t top_prev)
{
    if (first_row)
        return prev;
    if (row_start)
        return top;

    return (uint8_t)(prev + top - top_prev);
}

// mirror of bc4_index_context (third_party/bc_packed.c): the 8 x 8 [left][top] matrix context, a
// missing neighbor (first row, row start) defaults to 0
uint8_t bc4_index_context(uint8_t left, uint8_t top)
{
    return (uint8_t)(left * BC4_INDEX_DIM + top);
}

// mirror of mtf_touch (third_party/bc_packed.c): move [pair] to the front of the 8-entry
// endpoint-pair MTF list, inserting it if new; the list holds distinct pairs, the oldest one is
// dropped once it is full. [list] must hold the 8 entries, [count] the number of valid ones
void mtf_touch(thread uint16_t* list, thread uint* count, uint16_t pair)
{
    uint at = 0;
    bool found = false;
    for (uint i = 0; i < *count; ++i)
    {
        if (list[i] == pair)
        {
            at = i;
            found = true;
            break;
        }
    }

    if (found)
    {
        for (uint j = at; j > 0; --j)
            list[j] = list[j - 1];
    }
    else if (*count < 8)
    {
        for (uint j = *count; j > 0; --j)
            list[j] = list[j - 1];
        (*count)++;
    }
    else
    {
        for (uint j = 7; j > 0; --j)
            list[j] = list[j - 1];
    }

    list[0] = pair;
}

// mirror of bc4_dict_touch (third_party/bc_packed.c) without the lookup map: [rank] < BC4_DICT_SIZE
// promotes the list position (the permutation shifts only), otherwise [new_word] takes a
// fresh slot (the tail slot is evicted once the list is full). [word] holds the fixed slots,
// [order] the recency permutation, order[i] = slot of rank i, [count] the valid entries
void bc4_dict_touch(thread uint8_t* word, thread uint8_t* order, thread uint* count,
                    const thread uint8_t* new_word, uint rank)
{
    if (rank < BC4_DICT_SIZE)
    {
        uint8_t slot = order[rank];
        for (uint j = rank; j > 0; --j)
            order[j] = order[j - 1];
        order[0] = slot;
        return;
    }

    uint c = *count;
    uint slot = (c < BC4_DICT_SIZE) ? c : order[BC4_DICT_SIZE - 1];
    for (uint j = 0; j < 6; ++j)
        word[slot * 6 + j] = new_word[j];
    for (uint j = (c < BC4_DICT_SIZE) ? c : BC4_DICT_SIZE - 1; j > 0; --j)
        order[j] = order[j - 1];
    order[0] = (uint8_t)slot;
    *count = (c < BC4_DICT_SIZE) ? c + 1 : BC4_DICT_SIZE;
}

// mirror of bc4_decode_index_payload (third_party/bc_packed.c): the first index with its 8-symbol
// model, then each index xored with its intra-block prediction
void bc4_decode_index_payload(thread bit_stream_t* s, const thread model8_t* first_index,
                              const thread model8_t* index, thread uint8_t* idx)
{
    idx[0] = decode_symbol8(s, first_index);

    for (uint i = 1; i < 16; ++i)
    {
        // the missing neighbor of the pixel (first row, row start) defaults to 0
        uint8_t left = ((i & 3) == 0) ? 0 : idx[i - 1];
        uint8_t top = (i < 4) ? 0 : idx[i - 4];
        uint8_t p = ((i & 3) == 0) ? top : left;

        idx[i] = (uint8_t)(p ^ decode_symbol8(s, &index[bc4_index_context(left, top)]));
    }
}

void bc4_decode_strip(const device uint8_t* input,
                      device uint8_t* output,
                      constant decode_args_t& args,
                      uint3 grid_pos)
{
    if ((args.width < 16) || (args.height < 256) || (grid_pos.x >= NUM_STRIPS))
        return;

    const uint strip_id = grid_pos.x;
    const uint height_blocks = args.height / 4;
    const uint width_blocks = args.width / 4;
    const uint64_t length = args.input_length;

    // offset both input and output by run_id so each run reads from and writes to
    // a distinct memory region, exercising DRAM bandwidth instead of staying in L2
    const uint64_t image_size = (uint64_t)width_blocks * (uint64_t)height_blocks * 8;
    input += (uint64_t)grid_pos.y * length;
    output += (uint64_t)grid_pos.y * image_size;

    //-------------------------------------------------------------------------------------------------------------------------
    // load the static models: the 2 endpoint models (256 symbols), the endpoint MTF-8 model
    // (9 symbols, a dummy 1-symbol model when the MTF is disabled), the first-index model
    // (8 symbols), the index-word dictionary rank model (256 symbols, a dummy 1-symbol model
    // when no tile is enabled) then the 64 compact index models (8 symbols)
    model_t endpoint[2];
    model_t mtf;
    model_t dict_rank;
    model8_t first_index;
    model8_t index[BC4_INDEX_CONTEXTS];

    thread uint64_t pos = 0;
    for (uint e = 0; e < 2; ++e)
        if (!load_model(input, length, &pos, &endpoint[e]))
            return;

    if (!load_model(input, length, &pos, &mtf))
        return;

    if (!load_model8(input, length, &pos, &first_index))
        return;

    if (!load_model(input, length, &pos, &dict_rank))
        return;

    for (uint i = 0; i < BC4_INDEX_CONTEXTS; ++i)
        if (!load_model8(input, length, &pos, &index[i]))
            return;

    // strip seed: 2 bytes, endpoint 0 then endpoint 1
    if (pos + 2 > length)
        return;

    uint8_t seed[2];
    for (uint e = 0; e < 2; ++e)
        seed[e] = input[pos++];

    // flat-index mode + value + endpoint MTF-8 enable: one byte (mode | value << 1 | mtf << 4),
    // after the seeds, before the 4-align
    if (pos + 1 > length)
        return;

    uint8_t flat_byte = input[pos++];
    bool flat_mode = (flat_byte & 1) != 0;
    uint8_t flat_value = (flat_byte >> 1) & 7;
    bool mtf_enable = (flat_byte >> 4) != 0;

    // index-word dictionary enable: one u64, bit i = tile i, u32-aligned in the stream,
    // so read the words directly
    if (pos + 8 > length)
        return;

    pos = (pos + 3) & ~(uint64_t)3;
    uint64_t enable = (uint64_t)read_u32(input, pos) | ((uint64_t)read_u32(input, pos + 4) << 32);
    pos += 8;
    bool dict_enable = ((enable >> strip_id) & 1) != 0;

    //-------------------------------------------------------------------------------------------------------------------------
    // strip offsets (deltas in dword units), then the strip bitstream of this thread
    pos = (pos + 1) & ~(uint64_t)1; // 2-byte align

    if (pos + (NUM_STRIPS - 1) * 2 >= length)
        return;

    // strip 0 always starts the strip data so its offset is zero and not stored, the stored
    // deltas cover strips 1..N-1
    uint64_t strip_offset = pos + (NUM_STRIPS - 1) * 2;
    strip_offset = (strip_offset + 3) & ~(uint64_t)3; // 4-byte align, as done on encode

    for (uint i = 0; i < strip_id; ++i)
        strip_offset += (uint64_t)read_u16(input, pos + i * 2) * 4;

    if (strip_offset >= length)
        return;

    bit_stream_t compressed_stream;
    compressed_stream.buffer = input + strip_offset;
    compressed_stream.size = length - strip_offset;
    compressed_stream.position = 0;
    compressed_stream.bit_reservoir = 0;
    compressed_stream.bits_available = 0;
    compressed_stream.overrun = false;
    bs_refill(&compressed_stream);

    //-------------------------------------------------------------------------------------------------------------------------
    // decode the strip block by block, zigzag order
    // seed "previous block" of the strip: one byte per endpoint
    uint8_t prev_e[2], top_e[2], top_prev_e[2];
    for (uint e = 0; e < 2; ++e)
    {
        prev_e[e] = seed[e];
        top_e[e] = 0;
        top_prev_e[e] = 0;
    }

    // the MTF list and the index-word dictionary are cleared per tile: one thread decodes one
    // tile, so this is the whole state; the decoder keeps no lookup map (the rank names the
    // list position directly)
    uint16_t mtf_pair[8];
    uint mtf_count = 0;

    thread uint8_t dict_word[BC4_DICT_SIZE][6];
    thread uint8_t dict_order[BC4_DICT_SIZE];
    thread uint dict_count = 0;

    const tile_bounds tb = tile_bounds_get(strip_id, width_blocks, height_blocks);
    const uint tile_width = tb.x1 - tb.x0;

    // the output is read back for the top prediction: the two endpoints of a block
    // sit in the low bytes of one u32 (the block is 8-aligned)

    for (uint y = tb.y0; y < tb.y1; ++y)
    {
        for (uint x = 0; x < tile_width; ++x)
        {
            // zig-zag pattern
            uint zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
            const bool first_row = (y == tb.y0);
            const bool row_start = (x == 0);
            const uint64_t block_index = (uint64_t)y * width_blocks + zigzag_x;

            device uint8_t* b = output + block_index * 8;
            uint8_t idx[16];

            if (!first_row)
            {
                const device uint32_t* t = (const device uint32_t*)(output + ((uint64_t)(y - 1) * width_blocks + zigzag_x) * 8);
                uint32_t top = t[0];
                top_e[0] = (uint8_t)(top & 0xFF);
                top_e[1] = (uint8_t)((top >> 8) & 0xFF);
            }

            // endpoint payload: with MTF, the 9-symbol rank of the block's endpoint pair
            // (0..7 = the pair at that list position, 8 = escape to the two wrapped deltas
            // against the strip-chain predictor, the list updated in either case); without
            // MTF, the two deltas only
            uint8_t e[2];
            if (mtf_enable)
            {
                uint8_t rank = decode_symbol(&compressed_stream, &mtf);

                if (rank < 8)
                {
                    uint16_t pair = mtf_pair[rank];
                    e[0] = (uint8_t)(pair & 0xFF);
                    e[1] = (uint8_t)(pair >> 8);
                }
                else
                {
                    for (uint j = 0; j < 2; ++j)
                    {
                        uint8_t pred = bc4_endpoint_predict(first_row, row_start, prev_e[j], top_e[j], top_prev_e[j]);

                        e[j] = (uint8_t)(pred + decode_symbol(&compressed_stream, &endpoint[j]));
                    }
                }

                mtf_touch(mtf_pair, &mtf_count, (uint16_t)(((uint16_t)e[1] << 8) | (uint16_t)e[0]));
            }
            else
            {
                for (uint j = 0; j < 2; ++j)
                {
                    uint8_t pred = bc4_endpoint_predict(first_row, row_start, prev_e[j], top_e[j], top_prev_e[j]);

                    e[j] = (uint8_t)(pred + decode_symbol(&compressed_stream, &endpoint[j]));
                }
            }

            // index payload: with the dictionary enabled for the tile, one 256-symbol rank of
            // the block's 48-bit index word (0..254 = the word at that move-to-front position,
            // expanded into the index field; 255 = the escape, the full payload below, the
            // word inserted at the front of the list); otherwise the full payload. a flat
            // block (identical endpoints, reconstructed exactly like the encoder sees the
            // source) in mode 1 stores no index payload at all: the header value is expanded
            // to all 16
            if ((e[0] == e[1]) && flat_mode)
            {
                for (uint i = 0; i < 16; ++i)
                    idx[i] = flat_value;
            }
            else if (dict_enable)
            {
                uint rank = decode_symbol(&compressed_stream, &dict_rank);

                if (rank < BC4_DICT_SIZE)
                {
                    const thread uint8_t* w = &dict_word[dict_order[rank]][0];
                    uint64_t wv = 0;
                    for (uint j = 0; j < 6; ++j)
                        wv |= ((uint64_t)w[j]) << (8 * j);
                    for (uint i = 0; i < 16; ++i)
                        idx[i] = (uint8_t)((wv >> (3 * i)) & 7);

                    bc4_dict_touch(&dict_word[0][0], &dict_order[0], &dict_count, w, rank);
                }
                else
                {
                    bc4_decode_index_payload(&compressed_stream, &first_index, &index[0], idx);

                    thread uint8_t w[6];
                    uint64_t wv = 0;
                    for (uint i = 0; i < 16; ++i)
                        wv |= ((uint64_t)idx[i] & 7) << (3 * i);
                    for (uint j = 0; j < 6; ++j)
                        w[j] = (uint8_t)(wv >> (8 * j));

                    bc4_dict_touch(&dict_word[0][0], &dict_order[0], &dict_count, w, BC4_DICT_SIZE);
                }
            }
            else
            {
                bc4_decode_index_payload(&compressed_stream, &first_index, &index[0], idx);
            }

            // the standard block: two endpoints + the 48-bit little-endian index field, as two aligned u32 words
            uint64_t v = 0;
            for (uint i = 0; i < 16; ++i)
                v |= ((uint64_t)idx[i] & 7) << (3 * i);

            device uint32_t* p32 = (device uint32_t*)b;
            p32[0] = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)v << 16);
            p32[1] = (uint32_t)(v >> 16);

            if (!first_row)
            {
                top_prev_e[0] = top_e[0];
                top_prev_e[1] = top_e[1];
            }
            prev_e[0] = e[0];
            prev_e[1] = e[1];
        }
    }
}

kernel void bcp_decompress_bc4(const device uint8_t* input [[buffer(0)]],
                               device uint8_t* output [[buffer(1)]],
                               constant decode_args_t& args [[buffer(2)]],
                               uint3 grid_pos [[thread_position_in_grid]])
{
    bc4_decode_strip(input, output, args, grid_pos);
}
