#include "mjpegw.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>


//-----------------------------------------------------------------------------------------------------------------------------
// AVI chunks structures
//-----------------------------------------------------------------------------------------------------------------------------

#pragma pack(push, 1)

typedef struct
{
    char     riff[4];     // "RIFF"
    uint32_t size;        // file_size - 8  (PATCH LATER)
    char     avi[4];      // "AVI "
} riff_header;


typedef struct
{
    char     list[4];     // "LIST"
    uint32_t size;        // size of this LIST - 8  (PATCH LATER)
    char     type[4];     // "hdrl"
} list_header;

typedef struct
{
    char     id[4];       // "avih"
    uint32_t size;        // 56
    uint32_t microsec_per_frame;
    uint32_t max_bytes_per_sec;
    uint32_t padding_granularity;
    uint32_t flags;
    uint32_t total_frames;
    uint32_t initial_frames;
    uint32_t streams;
    uint32_t suggested_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[4];
} avih_chunk;

typedef struct
{
    char     list[4];     // "LIST"
    uint32_t size;        // size of this LIST - 8 (PATCH LATER)
    char     type[4];     // "strl"
} avi_stream_list;

typedef struct
{
    char     id[4];       // "strh"
    uint32_t size;        // 56
    char     type[4];     // "vids"
    char     handler[4];  // "MJPG"
    uint32_t flags;
    uint32_t priority;
    uint16_t language;
    uint16_t initial_frames;
    uint32_t scale;
    uint32_t rate;
    uint32_t start;
    uint32_t length;
    uint32_t suggested_buffer_size;
    uint32_t quality;
    uint32_t sample_size;
    struct
    {
        int16_t left;
        int16_t top;
        int16_t right;
        int16_t bottom;
    } frame;
} strh_chunk;

typedef struct
{
    char     id[4];       // "strf"
    uint32_t size;        // 40
    uint32_t bi_size;     // 40
    int32_t  bi_width;
    int32_t  bi_height;
    uint16_t bi_planes;
    uint16_t bi_bit_count;
    uint32_t bi_compression; // "MJPG" == 0x47504A4D
    uint32_t bi_image_size;  // 0 usually fine
    int32_t  bi_x_ppm;
    int32_t  bi_y_ppm;
    uint32_t bi_clr_used;
    uint32_t bi_clr_important;
} strf_chunk;

typedef struct
{
    char     list[4];     // "LIST"
    uint32_t size;        // PATCH LATER
    char     type[4];     // "movi"
} movi_header;

typedef struct
{
    char     id[4];       // "00dc"
    uint32_t size;        // jpeg_data_size
    // followed by jpeg_data[size]
} frame_chunk;

typedef struct
{
    char     id[4];       // "00dc"
    uint32_t flags;       // 0x10 = keyframe
    uint32_t offset;      // offset relative to start of 'movi' LIST DATA
    uint32_t size;        // jpeg_data_size
} idx1_entry;

typedef struct
{
    char     id[4];       // "idx1"
    uint32_t size;        // num_entries * 16
    // followed by avi_idx1_entry_t entries[num_frames]
} idx1_header;

#pragma pack(pop)


//-----------------------------------------------------------------------------------------------------------------------------
// Tiny_jpeg function prototype (see implementation at the end of this file)
//-----------------------------------------------------------------------------------------------------------------------------

typedef void tje_write_func(void* context, void* data, int size);

int tje_encode_with_func(tje_write_func* func,
                         void* context,
                         const int quality,
                         const int width,
                         const int height,
                         const int num_components,
                         const unsigned char* src_data);


//-----------------------------------------------------------------------------------------------------------------------------
// mjpegw_context
//-----------------------------------------------------------------------------------------------------------------------------
typedef struct mjpegw_context
{
    FILE* f;

    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t frame_count;

    mjpegw_mem_interface mem;

    long riff_pos;
    long movi_pos;
    long frame_count_pos;
    long length_pos;

    // main avi structures
    riff_header riff;
    list_header hdrl;
    avih_chunk avih;
    avi_stream_list strl;
    strh_chunk strh;
    strf_chunk strf;
    movi_header movi;

    idx1_entry* idx;
    uint32_t idx_count;
    uint32_t idx_capacity;

    uint8_t* jpeg_data;
    uint32_t jpeg_size;
    uint32_t jpeg_capacity;
} mjpegw_context;


//-----------------------------------------------------------------------------------------------------------------------------
static inline void* malloc_wrapper(size_t size, void* user)
{
    (void)user; // unused
    return malloc(size);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void* realloc_wrapper(void* old_ptr, size_t old_size, size_t new_size, void* user)
{
    (void)user;
    (void)old_size;
    return realloc(old_ptr, new_size);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void free_wrapper(void* ptr, void* user)
{
    (void)user;
    free(ptr);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline mjpegw_mem_interface default_allocator(void) 
{
    return (mjpegw_mem_interface) 
    {
        .malloc_fn  = malloc_wrapper,
        .realloc_fn = realloc_wrapper,
        .free_fn    = free_wrapper,
        .user       = NULL
    };
}

//-----------------------------------------------------------------------------------------------------------------------------
mjpegw_context* mjpegw_open(const char *filename, uint32_t width, uint32_t height, uint32_t fps, mjpegw_mem_interface* mem)
{
    mjpegw_context* ctx = NULL;

    // use user allocator or default
    mjpegw_mem_interface allocator = mem ? *mem : default_allocator();

    ctx = allocator.malloc_fn(sizeof(mjpegw_context), allocator.user);
    if(!ctx)
        return NULL;

    *ctx = (mjpegw_context) {0};

    ctx->width  = width;
    ctx->height = height;
    ctx->fps = fps;
    ctx->frame_count = 0;
    ctx->mem = allocator;

    ctx->f = fopen(filename, "wb+");
    if(!ctx->f)
    {
        ctx->mem.free_fn(ctx, ctx->mem.user);
        return NULL;
    }

    memcpy(ctx->riff.riff, "RIFF", 4);
    ctx->riff.size = 0; // patch later
    memcpy(ctx->riff.avi, "AVI ", 4);
    ctx->riff_pos = ftell(ctx->f);
    fwrite(&ctx->riff, sizeof(ctx->riff), 1, ctx->f);

    memcpy(ctx->hdrl.list, "LIST", 4);
    ctx->hdrl.size = 188;
    memcpy(ctx->hdrl.type, "hdrl", 4);
    fwrite(&ctx->hdrl, sizeof(ctx->hdrl), 1, ctx->f);

    ctx->frame_count_pos = ftell(ctx->f) + offsetof(avih_chunk, total_frames);
    memcpy(ctx->avih.id, "avih", 4);
    ctx->avih.size = 56;
    ctx->avih.microsec_per_frame = 1000000 / fps;
    ctx->avih.max_bytes_per_sec = 0;
    ctx->avih.padding_granularity = 0;
    ctx->avih.flags = 0x10; // HAS_INDEX
    ctx->avih.total_frames = 0; // patch later
    ctx->avih.initial_frames = 0;
    ctx->avih.streams = 1;
    ctx->avih.suggested_buffer_size = ctx->width*ctx->height*3;
    ctx->avih.width = width;
    ctx->avih.height = height;
    fwrite(&ctx->avih, sizeof(ctx->avih), 1, ctx->f);

    memcpy(ctx->strl.list, "LIST", 4);
    ctx->strl.size = sizeof(strh_chunk) + sizeof(strf_chunk);
    assert(ctx->strl.size == 112);
    memcpy(ctx->strl.type, "strl", 4);
    fwrite(&ctx->strl, sizeof(ctx->strl), 1, ctx->f);

    ctx->length_pos = ftell(ctx->f) + offsetof(strh_chunk, length);
    memcpy(ctx->strh.id, "strh", 4);
    ctx->strh.size = 56;
    memcpy(ctx->strh.type, "vids", 4);
    memcpy(ctx->strh.handler, "MJPG", 4);
    ctx->strh.flags = 0x10; // HAS_INDEX
    ctx->strh.priority = 0;
    ctx->strh.language = 0;
    ctx->strh.initial_frames = 0;
    ctx->strh.scale = 1;
    ctx->strh.rate = fps;
    ctx->strh.start = 0;
    ctx->strh.length = 0; // patch later
    ctx->strh.suggested_buffer_size = ctx->width*ctx->height*3;
    ctx->strh.quality = -1;
    ctx->strh.sample_size = 0;
    ctx->strh.frame.left = 0;
    ctx->strh.frame.top = 0;
    ctx->strh.frame.right = width;
    ctx->strh.frame.bottom = height;
    fwrite(&ctx->strh, sizeof(ctx->strh), 1, ctx->f);

    memcpy(ctx->strf.id, "strf", 4);
    ctx->strf.size = sizeof(ctx->strf);
    ctx->strf.bi_size = 40;
    ctx->strf.bi_width = width;
    ctx->strf.bi_height = height;
    ctx->strf.bi_planes = 1;
    ctx->strf.bi_bit_count = 24; // RGB24
    ctx->strf.bi_compression = 0x47504A4D; // 'MJPG'
    ctx->strf.bi_image_size = 0;
    ctx->strf.bi_x_ppm = 0;
    ctx->strf.bi_y_ppm = 0;
    ctx->strf.bi_clr_used = 0;
    ctx->strf.bi_clr_important = 0;
    fwrite(&ctx->strf, sizeof(ctx->strf), 1, ctx->f);

    memcpy(ctx->movi.list, "LIST", 4);
    ctx->movi.size = 0; // patch later
    memcpy(ctx->movi.type, "movi", 4);
    ctx->movi_pos = ftell(ctx->f);
    fwrite(&ctx->movi, sizeof(ctx->movi), 1, ctx->f);

    ctx->idx_capacity = 256;
    ctx->idx_count = 0;
    ctx->idx = ctx->mem.malloc_fn(sizeof(idx1_entry) * ctx->idx_capacity, ctx->mem.user);
    if (!ctx->idx)
    {
        fclose(ctx->f);
        ctx->mem.free_fn(ctx, ctx->mem.user);
        return NULL;
    }

    ctx->jpeg_capacity = width*height;
    ctx->jpeg_data = ctx->mem.malloc_fn(ctx->jpeg_capacity, ctx->mem.user);
    ctx->jpeg_size = 0;

    return ctx;
}

//-----------------------------------------------------------------------------------------------------------------------------
void jpeg_write_func(void* context, void* data, int size)
{
    mjpegw_context *ctx = (mjpegw_context*) context;

    if ((ctx->jpeg_size + size) >= ctx->jpeg_capacity)
    {
        ctx->jpeg_data = ctx->mem.realloc_fn(ctx->jpeg_data, ctx->jpeg_capacity, ctx->jpeg_size * 2, ctx->mem.user);
        assert(ctx->jpeg_data);
        ctx->jpeg_capacity *= 2;
    }

    uint8_t* ptr = (uint8_t*) ctx->jpeg_data;
    ptr += ctx->jpeg_size;
    memcpy(ptr, data, size);
    ctx->jpeg_size += size;
}


//-----------------------------------------------------------------------------------------------------------------------------
void mjpegw_add_frame(mjpegw_context *ctx, const void* pixels, const int quality)
{
    ctx->jpeg_size = 0;

    tje_encode_with_func(jpeg_write_func, ctx, quality, ctx->width, ctx->height, 4, (const unsigned char*)pixels);
    
    if(ctx->idx_count >= ctx->idx_capacity)
    {
        uint32_t new_capacity = ctx->idx_capacity * 2;
        idx1_entry* new_idx = ctx->mem.realloc_fn(ctx->idx, sizeof(idx1_entry) * ctx->idx_capacity,
                                                  sizeof(idx1_entry) * new_capacity, ctx->mem.user);

        assert(new_idx != NULL);
        ctx->idx = new_idx;
        ctx->idx_capacity = new_capacity;
    }

    long frame_pos = ftell(ctx->f);
    assert(frame_pos != -1L);

    uint32_t chunk_size = ctx->jpeg_size;
    if (ctx->jpeg_size & 1)
        chunk_size++;

    frame_chunk hdr = { .size = chunk_size };
    memcpy(hdr.id, "00dc", 4);
    fwrite(&hdr, sizeof(frame_chunk), 1, ctx->f);
    fwrite(ctx->jpeg_data, 1, ctx->jpeg_size, ctx->f);

    if (ctx->jpeg_size & 1)
    {
        uint8_t pad = 0;
        fwrite(&pad, 1, 1, ctx->f);
    }

    ctx->movi.size += sizeof(frame_chunk) + chunk_size;

    idx1_entry* entry = &ctx->idx[ctx->idx_count++];
    memcpy(entry->id, "00dc", 4);
    entry->flags = 0x10;
    entry->offset = (uint32_t)(frame_pos - ctx->movi_pos - 8); // relative to 'movi' data start
    entry->size = chunk_size;

    ctx->frame_count++;
}

//-----------------------------------------------------------------------------------------------------------------------------
void mjpegw_close(mjpegw_context *ctx)
{
    assert(ctx);

    // patch frame count
    fseek(ctx->f, ctx->frame_count_pos, SEEK_SET);
    fwrite(&ctx->frame_count, sizeof(uint32_t), 1, ctx->f);
    fseek(ctx->f, ctx->length_pos, SEEK_SET);
    fwrite(&ctx->frame_count, sizeof(uint32_t), 1, ctx->f);

    // write idx1 chunk
    fseek(ctx->f, 0, SEEK_END); 
    idx1_header idxh = {0};
    memcpy(idxh.id, "idx1", 4);
    idxh.size = ctx->idx_count * sizeof(idx1_entry);

    fwrite(&idxh, sizeof(idx1_header), 1, ctx->f);
    if (ctx->idx_count)
        fwrite(ctx->idx, sizeof(idx1_entry), ctx->idx_count, ctx->f);

    // patch movi LIST size
    long cur_pos = ftell(ctx->f);
    fseek(ctx->f, ctx->movi_pos + 4, SEEK_SET);
    uint32_t movi_size = (uint32_t)(cur_pos - (ctx->movi_pos + 8));
    fwrite(&movi_size, sizeof(uint32_t), 1, ctx->f);

    // patch RIFF size
    fseek(ctx->f, ctx->riff_pos + 4, SEEK_SET);
    uint32_t riff_size = (uint32_t)(cur_pos - (ctx->riff_pos + 8));
    fwrite(&riff_size, sizeof(uint32_t), 1, ctx->f);

    if (ctx->idx)
    {
        ctx->mem.free_fn(ctx->idx, ctx->mem.user);
        ctx->idx = NULL;
        ctx->idx_capacity = 0;
    }

    if (ctx->jpeg_data)
    {
        ctx->mem.free_fn(ctx->jpeg_data, ctx->mem.user);
        ctx->jpeg_data = NULL;
        ctx->jpeg_capacity = 0;
    }

    fclose(ctx->f);
    ctx->f = NULL;
    ctx->mem.free_fn(ctx, ctx->mem.user);
}

//-----------------------------------------------------------------------------------------------------------------------------
// tiny_jpeg, code copied and stripped from https://github.com/serge-rgb/TinyJPEG
//-----------------------------------------------------------------------------------------------------------------------------

#define tjei_min(a, b) ((a) < b) ? (a) : (b);
#define tjei_max(a, b) ((a) < b) ? (b) : (a);


#if defined(_MSC_VER)
#define TJEI_FORCE_INLINE __forceinline
// #define TJEI_FORCE_INLINE __declspec(noinline)  // For profiling
#else
#define TJEI_FORCE_INLINE static // TODO: equivalent for gcc & clang
#endif


#define TJEI_BUFFER_SIZE 1024
#define tje_log(msg)


typedef struct
{
    void*           context;
    tje_write_func* func;
} TJEWriteContext;

typedef struct
{
    // Huffman data.
    uint8_t         ehuffsize[4][257];
    uint16_t        ehuffcode[4][256];
    uint8_t const * ht_bits[4];
    uint8_t const * ht_vals[4];

    // Cuantization tables.
    uint8_t         qt_luma[64];
    uint8_t         qt_chroma[64];

    // fwrite by default. User-defined when using tje_encode_with_func.
    TJEWriteContext write_context;

    // Buffered output. Big performance win when using the usual stdlib implementations.
    size_t          output_buffer_count;
    uint8_t         output_buffer[TJEI_BUFFER_SIZE];
} TJEState;

// ============================================================
// Table definitions.
//
// The spec defines tjei_default reasonably good quantization matrices and huffman
// specification tables.
//
//
// Instead of hard-coding the final huffman table, we only hard-code the table
// spec suggested by the specification, and then derive the full table from
// there.  This is only for didactic purposes but it might be useful if there
// ever is the case that we need to swap huffman tables from various sources.
// ============================================================


// K.1 - suggested luminance QT
static const uint8_t tjei_default_qt_luma_from_spec[] =
{
   16,11,10,16, 24, 40, 51, 61,
   12,12,14,19, 26, 58, 60, 55,
   14,13,16,24, 40, 57, 69, 56,
   14,17,22,29, 51, 87, 80, 62,
   18,22,37,56, 68,109,103, 77,
   24,35,55,64, 81,104,113, 92,
   49,64,78,87,103,121,120,101,
   72,92,95,98,112,100,103, 99,
};

static const uint8_t tjei_default_qt_chroma_from_paper[] =
{
    // Example QT from JPEG paper
    16,  12, 14,  14, 18, 24,  49,  72,
    11,  10, 16,  24, 40, 51,  61,  12,
    13,  17, 22,  35, 64, 92,  14,  16,
    22,  37, 55,  78, 95, 19,  24,  29,
    56,  64, 87,  98, 26, 40,  51,  68,
    81, 103, 112, 58, 57, 87,  109, 104,
    121,100, 60,  69, 80, 103, 113, 120,
    103, 55, 56,  62, 77, 92,  101, 99,
};

// == Procedure to 'deflate' the huffman tree: JPEG spec, C.2

// Number of 16 bit values for every code length. (K.3.3.1)
static const uint8_t tjei_default_ht_luma_dc_len[16] =
{
    0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0
};
// values
static const uint8_t tjei_default_ht_luma_dc[12] =
{
    0,1,2,3,4,5,6,7,8,9,10,11
};

// Number of 16 bit values for every code length. (K.3.3.1)
static const uint8_t tjei_default_ht_chroma_dc_len[16] =
{
    0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0
};
// values
static const uint8_t tjei_default_ht_chroma_dc[12] =
{
    0,1,2,3,4,5,6,7,8,9,10,11
};

// Same as above, but AC coefficients.
static const uint8_t tjei_default_ht_luma_ac_len[16] =
{
    0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d
};
static const uint8_t tjei_default_ht_luma_ac[] =
{
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

static const uint8_t tjei_default_ht_chroma_ac_len[16] =
{
    0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77
};
static const uint8_t tjei_default_ht_chroma_ac[] =
{
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};


// ============================================================
// Code
// ============================================================

// Zig-zag order:
static const uint8_t tjei_zig_zag[64] =
{
    0,   1,  5,  6, 14, 15, 27, 28,
    2,   4,  7, 13, 16, 26, 29, 42,
    3,   8, 12, 17, 25, 30, 41, 43,
    9,  11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};

// Memory order as big endian.
// On little-endian machines: 0xhilo -> 0xlohi which looks as 0xhi 0xlo in memory
// On big-endian machines: leave 0xhilo unchanged
static uint16_t tjei_be_word(const uint16_t native_word)
{
    uint8_t bytes[2];
    uint16_t result;
    bytes[1] = (native_word & 0x00ff);
    bytes[0] = ((native_word & 0xff00) >> 8);
    memcpy(&result, bytes, sizeof(bytes));
    return result;
}

// ============================================================
// The following structs exist only for code clarity, debugability, and
// readability. They are used when writing to disk, but it is useful to have
// 1-packed-structs to document how the format works, and to inspect memory
// while developing.
// ============================================================

static const uint8_t tjeik_jfif_id[] = "JFIF";
static const uint8_t tjeik_com_str[] = "Created by Tiny JPEG Encoder";

// TODO: Get rid of packed structs!
#pragma pack(push)
#pragma pack(1)
typedef struct
{
    uint16_t SOI;
    // JFIF header.
    uint16_t APP0;
    uint16_t jfif_len;
    uint8_t  jfif_id[5];
    uint16_t version;
    uint8_t  units;
    uint16_t x_density;
    uint16_t y_density;
    uint8_t  x_thumb;
    uint8_t  y_thumb;
} TJEJPEGHeader;

typedef struct
{
    uint16_t com;
    uint16_t com_len;
    char     com_str[sizeof(tjeik_com_str) - 1];
} TJEJPEGComment;

// Helper struct for TJEFrameHeader (below).
typedef struct
{
    uint8_t  component_id;
    uint8_t  sampling_factors;    // most significant 4 bits: horizontal. 4 LSB: vertical (A.1.1)
    uint8_t  qt;                  // Quantization table selector.
} TJEComponentSpec;

typedef struct
{
    uint16_t         SOF;
    uint16_t         len;                   // 8 + 3 * frame.num_components
    uint8_t          precision;             // Sample precision (bits per sample).
    uint16_t         height;
    uint16_t         width;
    uint8_t          num_components;        // For this implementation, will be equal to 3.
    TJEComponentSpec component_spec[3];
} TJEFrameHeader;

typedef struct
{
    uint8_t component_id;                 // Just as with TJEComponentSpec
    uint8_t dc_ac;                        // (dc|ac)
} TJEFrameComponentSpec;

typedef struct
{
    uint16_t              SOS;
    uint16_t              len;
    uint8_t               num_components;  // 3.
    TJEFrameComponentSpec component_spec[3];
    uint8_t               first;  // 0
    uint8_t               last;  // 63
    uint8_t               ah_al;  // o
} TJEScanHeader;
#pragma pack(pop)


static void tjei_write(TJEState* state, const void* data, size_t num_bytes, size_t num_elements)
{
    size_t to_write = num_bytes * num_elements;

    // Cap to the buffer available size and copy memory.
    size_t capped_count = tjei_min(to_write, TJEI_BUFFER_SIZE - 1 - state->output_buffer_count);

    memcpy(state->output_buffer + state->output_buffer_count, data, capped_count);
    state->output_buffer_count += capped_count;

    assert (state->output_buffer_count <= TJEI_BUFFER_SIZE - 1);

    // Flush the buffer.
    if ( state->output_buffer_count == TJEI_BUFFER_SIZE - 1 ) {
        state->write_context.func(state->write_context.context, state->output_buffer, (int)state->output_buffer_count);
        state->output_buffer_count = 0;
    }

    // Recursively calling ourselves with the rest of the buffer.
    if (capped_count < to_write) {
        tjei_write(state, (uint8_t*)data+capped_count, to_write - capped_count, 1);
    }
}

static void tjei_write_DQT(TJEState* state, const uint8_t* matrix, uint8_t id)
{
    uint16_t DQT = tjei_be_word(0xffdb);
    tjei_write(state, &DQT, sizeof(uint16_t), 1);
    uint16_t len = tjei_be_word(0x0043); // 2(len) + 1(id) + 64(matrix) = 67 = 0x43
    tjei_write(state, &len, sizeof(uint16_t), 1);
    assert(id < 4);
    uint8_t precision_and_id = id;  // 0x0000 8 bits | 0x00id
    tjei_write(state, &precision_and_id, sizeof(uint8_t), 1);
    // Write matrix
    tjei_write(state, matrix, 64*sizeof(uint8_t), 1);
}

typedef enum
{
    TJEI_DC = 0,
    TJEI_AC = 1
} TJEHuffmanTableClass;

static void tjei_write_DHT(TJEState* state,
                           uint8_t const * matrix_len,
                           uint8_t const * matrix_val,
                           TJEHuffmanTableClass ht_class,
                           uint8_t id)
{
    int num_values = 0;
    for ( int i = 0; i < 16; ++i ) {
        num_values += matrix_len[i];
    }
    assert(num_values <= 0xffff);

    uint16_t DHT = tjei_be_word(0xffc4);
    // 2(len) + 1(Tc|th) + 16 (num lengths) + ?? (num values)
    uint16_t len = tjei_be_word(2 + 1 + 16 + (uint16_t)num_values);
    assert(id < 4);
    uint8_t tc_th = (uint8_t)((((uint8_t)ht_class) << 4) | id);

    tjei_write(state, &DHT, sizeof(uint16_t), 1);
    tjei_write(state, &len, sizeof(uint16_t), 1);
    tjei_write(state, &tc_th, sizeof(uint8_t), 1);
    tjei_write(state, matrix_len, sizeof(uint8_t), 16);
    tjei_write(state, matrix_val, sizeof(uint8_t), (size_t)num_values);
}
// ============================================================
//  Huffman deflation code.
// ============================================================

// Returns all code sizes from the BITS specification (JPEG C.3)
static uint8_t* tjei_huff_get_code_lengths(uint8_t huffsize[/*256*/], uint8_t const * bits)
{
    int k = 0;
    for ( int i = 0; i < 16; ++i ) {
        for ( int j = 0; j < bits[i]; ++j ) {
            huffsize[k++] = (uint8_t)(i + 1);
        }
        huffsize[k] = 0;
    }
    return huffsize;
}

// Fills out the prefixes for each code.
static uint16_t* tjei_huff_get_codes(uint16_t codes[], uint8_t* huffsize, int64_t count)
{
    uint16_t code = 0;
    int k = 0;
    uint8_t sz = huffsize[0];
    for(;;) {
        do {
            assert(k < count);
            codes[k++] = code++;
        } while (huffsize[k] == sz);
        if (huffsize[k] == 0) {
            return codes;
        }
        do {
            code = (uint16_t)(code << 1);
            ++sz;
        } while( huffsize[k] != sz );
    }
}

static void tjei_huff_get_extended(uint8_t* out_ehuffsize,
                                   uint16_t* out_ehuffcode,
                                   uint8_t const * huffval,
                                   uint8_t* huffsize,
                                   uint16_t* huffcode, int64_t count)
{
    int k = 0;
    do {
        uint8_t val = huffval[k];
        out_ehuffcode[val] = huffcode[k];
        out_ehuffsize[val] = huffsize[k];
        k++;
    } while ( k < count );
}
// ============================================================

// Returns:
//  out[1] : number of bits
//  out[0] : bits
TJEI_FORCE_INLINE void tjei_calculate_variable_length_int(int value, uint16_t out[2])
{
    int abs_val = value;
    if ( value < 0 ) {
        abs_val = -abs_val;
        --value;
    }
    out[1] = 1;
    while( abs_val >>= 1 ) {
        ++out[1];
    }
    out[0] = (uint16_t)(value & ((1 << out[1]) - 1));
}

// Write bits to file.
TJEI_FORCE_INLINE void tjei_write_bits(TJEState* state,
                                       uint32_t* bitbuffer, uint32_t* location,
                                       uint16_t num_bits, uint16_t bits)
{
    //   v-- location
    //  [                     ]   <-- bit buffer
    // 32                     0
    //
    // This call pushes to the bitbuffer and saves the location. Data is pushed
    // from most significant to less significant.
    // When we can write a full byte, we write a byte and shift.

    // Push the stack.
    uint32_t nloc = *location + num_bits;
    *bitbuffer |= (uint32_t)(bits << (32 - nloc));
    *location = nloc;
    while ( *location >= 8 ) {
        // Grab the most significant byte.
        uint8_t c = (uint8_t)((*bitbuffer) >> 24);
        // Write it to file.
        tjei_write(state, &c, 1, 1);
        if ( c == 0xff )  {
            // Special case: tell JPEG this is not a marker.
            char z = 0;
            tjei_write(state, &z, 1, 1);
        }
        // Pop the stack.
        *bitbuffer <<= 8;
        *location -= 8;
    }
}

// DCT implementation by Thomas G. Lane.
// Obtained through NVIDIA
//  http://developer.download.nvidia.com/SDK/9.5/Samples/vidimaging_samples.html#gpgpu_dct
//
// QUOTE:
//  This implementation is based on Arai, Agui, and Nakajima's algorithm for
//  scaled DCT.  Their original paper (Trans. IEICE E-71(11):1095) is in
//  Japanese, but the algorithm is described in the Pennebaker & Mitchell
//  JPEG textbook (see REFERENCES section in file README).  The following code
//  is based directly on figure 4-8 in P&M.
//
static void tjei_fdct (float * data)
{
    float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    float tmp10, tmp11, tmp12, tmp13;
    float z1, z2, z3, z4, z5, z11, z13;
    float *dataptr;
    int ctr;

    /* Pass 1: process rows. */

    dataptr = data;
    for ( ctr = 7; ctr >= 0; ctr-- ) {
        tmp0 = dataptr[0] + dataptr[7];
        tmp7 = dataptr[0] - dataptr[7];
        tmp1 = dataptr[1] + dataptr[6];
        tmp6 = dataptr[1] - dataptr[6];
        tmp2 = dataptr[2] + dataptr[5];
        tmp5 = dataptr[2] - dataptr[5];
        tmp3 = dataptr[3] + dataptr[4];
        tmp4 = dataptr[3] - dataptr[4];

        /* Even part */

        tmp10 = tmp0 + tmp3;    /* phase 2 */
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0] = tmp10 + tmp11; /* phase 3 */
        dataptr[4] = tmp10 - tmp11;

        z1 = (tmp12 + tmp13) * ((float) 0.707106781); /* c4 */
        dataptr[2] = tmp13 + z1;    /* phase 5 */
        dataptr[6] = tmp13 - z1;

        /* Odd part */

        tmp10 = tmp4 + tmp5;    /* phase 2 */
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        /* The rotator is modified from fig 4-8 to avoid extra negations. */
        z5 = (tmp10 - tmp12) * ((float) 0.382683433); /* c6 */
        z2 = ((float) 0.541196100) * tmp10 + z5; /* c2-c6 */
        z4 = ((float) 1.306562965) * tmp12 + z5; /* c2+c6 */
        z3 = tmp11 * ((float) 0.707106781); /* c4 */

        z11 = tmp7 + z3;        /* phase 5 */
        z13 = tmp7 - z3;

        dataptr[5] = z13 + z2;  /* phase 6 */
        dataptr[3] = z13 - z2;
        dataptr[1] = z11 + z4;
        dataptr[7] = z11 - z4;

        dataptr += 8;     /* advance pointer to next row */
    }

    /* Pass 2: process columns. */

    dataptr = data;
    for ( ctr = 8-1; ctr >= 0; ctr-- ) {
        tmp0 = dataptr[8*0] + dataptr[8*7];
        tmp7 = dataptr[8*0] - dataptr[8*7];
        tmp1 = dataptr[8*1] + dataptr[8*6];
        tmp6 = dataptr[8*1] - dataptr[8*6];
        tmp2 = dataptr[8*2] + dataptr[8*5];
        tmp5 = dataptr[8*2] - dataptr[8*5];
        tmp3 = dataptr[8*3] + dataptr[8*4];
        tmp4 = dataptr[8*3] - dataptr[8*4];

        /* Even part */

        tmp10 = tmp0 + tmp3;    /* phase 2 */
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[8*0] = tmp10 + tmp11; /* phase 3 */
        dataptr[8*4] = tmp10 - tmp11;

        z1 = (tmp12 + tmp13) * ((float) 0.707106781); /* c4 */
        dataptr[8*2] = tmp13 + z1; /* phase 5 */
        dataptr[8*6] = tmp13 - z1;

        /* Odd part */

        tmp10 = tmp4 + tmp5;    /* phase 2 */
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        /* The rotator is modified from fig 4-8 to avoid extra negations. */
        z5 = (tmp10 - tmp12) * ((float) 0.382683433); /* c6 */
        z2 = ((float) 0.541196100) * tmp10 + z5; /* c2-c6 */
        z4 = ((float) 1.306562965) * tmp12 + z5; /* c2+c6 */
        z3 = tmp11 * ((float) 0.707106781); /* c4 */

        z11 = tmp7 + z3;        /* phase 5 */
        z13 = tmp7 - z3;

        dataptr[8*5] = z13 + z2; /* phase 6 */
        dataptr[8*3] = z13 - z2;
        dataptr[8*1] = z11 + z4;
        dataptr[8*7] = z11 - z4;

        dataptr++;          /* advance pointer to next column */
    }
}

#define ABS(x) ((x) < 0 ? -(x) : (x))

static void tjei_encode_and_write_MCU(TJEState* state,
                                      float* mcu,
                                      float* qt,  // Pre-processed quantization matrix.
                                      uint8_t* huff_dc_len, uint16_t* huff_dc_code, // Huffman tables
                                      uint8_t* huff_ac_len, uint16_t* huff_ac_code,
                                      int* pred,  // Previous DC coefficient
                                      uint32_t* bitbuffer,  // Bitstack.
                                      uint32_t* location)
{
    int du[64];  // Data unit in zig-zag order

    float dct_mcu[64];
    memcpy(dct_mcu, mcu, 64 * sizeof(float));

    tjei_fdct(dct_mcu);
    for ( int i = 0; i < 64; ++i ) {
        float fval = dct_mcu[i];
        fval *= qt[i];
        fval = floorf(fval + 1024 + 0.5f);
        fval -= 1024;
        int val = (int)fval;
        du[tjei_zig_zag[i]] = val;
    }

    uint16_t vli[2];

    // Encode DC coefficient.
    int diff = du[0] - *pred;
    *pred = du[0];
    if ( diff != 0 ) {
        tjei_calculate_variable_length_int(diff, vli);
        // Write number of bits with Huffman coding
        tjei_write_bits(state, bitbuffer, location, huff_dc_len[vli[1]], huff_dc_code[vli[1]]);
        // Write the bits.
        tjei_write_bits(state, bitbuffer, location, vli[1], vli[0]);
    } else {
        tjei_write_bits(state, bitbuffer, location, huff_dc_len[0], huff_dc_code[0]);
    }

    // ==== Encode AC coefficients ====

    int last_non_zero_i = 0;
    // Find the last non-zero element.
    for ( int i = 63; i > 0; --i ) {
        if (du[i] != 0) {
            last_non_zero_i = i;
            break;
        }
    }

    for ( int i = 1; i <= last_non_zero_i; ++i ) {
        // If zero, increase count. If >=15, encode (FF,00)
        int zero_count = 0;
        while ( du[i] == 0 ) {
            ++zero_count;
            ++i;
            if (zero_count == 16) {
                // encode (ff,00) == 0xf0
                tjei_write_bits(state, bitbuffer, location, huff_ac_len[0xf0], huff_ac_code[0xf0]);
                zero_count = 0;
            }
        }
        tjei_calculate_variable_length_int(du[i], vli);

        assert(zero_count < 0x10);
        assert(vli[1] <= 10);

        uint16_t sym1 = (uint16_t)((uint16_t)zero_count << 4) | vli[1];

        assert(huff_ac_len[sym1] != 0);

        // Write symbol 1  --- (RUNLENGTH, SIZE)
        tjei_write_bits(state, bitbuffer, location, huff_ac_len[sym1], huff_ac_code[sym1]);
        // Write symbol 2  --- (AMPLITUDE)
        tjei_write_bits(state, bitbuffer, location, vli[1], vli[0]);
    }

    if (last_non_zero_i != 63) {
        // write EOB HUFF(00,00)
        tjei_write_bits(state, bitbuffer, location, huff_ac_len[0], huff_ac_code[0]);
    }
    return;
}

enum {
    TJEI_LUMA_DC,
    TJEI_LUMA_AC,
    TJEI_CHROMA_DC,
    TJEI_CHROMA_AC,
};

struct TJEProcessedQT
{
    float chroma[64];
    float luma[64];
};

// Set up huffman tables in state.
static void tjei_huff_expand(TJEState* state)
{
    assert(state);

    state->ht_bits[TJEI_LUMA_DC]   = tjei_default_ht_luma_dc_len;
    state->ht_bits[TJEI_LUMA_AC]   = tjei_default_ht_luma_ac_len;
    state->ht_bits[TJEI_CHROMA_DC] = tjei_default_ht_chroma_dc_len;
    state->ht_bits[TJEI_CHROMA_AC] = tjei_default_ht_chroma_ac_len;

    state->ht_vals[TJEI_LUMA_DC]   = tjei_default_ht_luma_dc;
    state->ht_vals[TJEI_LUMA_AC]   = tjei_default_ht_luma_ac;
    state->ht_vals[TJEI_CHROMA_DC] = tjei_default_ht_chroma_dc;
    state->ht_vals[TJEI_CHROMA_AC] = tjei_default_ht_chroma_ac;

    // How many codes in total for each of LUMA_(DC|AC) and CHROMA_(DC|AC)
    int32_t spec_tables_len[4] = { 0 };

    for ( int i = 0; i < 4; ++i ) {
        for ( int k = 0; k < 16; ++k ) {
            spec_tables_len[i] += state->ht_bits[i][k];
        }
    }

    // Fill out the extended tables..
    uint8_t huffsize[4][257];
    uint16_t huffcode[4][256];
    for ( int i = 0; i < 4; ++i ) {
        assert (256 >= spec_tables_len[i]);
        tjei_huff_get_code_lengths(huffsize[i], state->ht_bits[i]);
        tjei_huff_get_codes(huffcode[i], huffsize[i], spec_tables_len[i]);
    }
    for ( int i = 0; i < 4; ++i ) {
        int64_t count = spec_tables_len[i];
        tjei_huff_get_extended(state->ehuffsize[i],
                               state->ehuffcode[i],
                               state->ht_vals[i],
                               &huffsize[i][0],
                               &huffcode[i][0], count);
    }
}

static int tjei_encode_main(TJEState* state,
                            const unsigned char* src_data,
                            const int width,
                            const int height,
                            const int src_num_components)
{
    if (src_num_components != 3 && src_num_components != 4) {
        return 0;
    }

    if (width > 0xffff || height > 0xffff) {
        return 0;
    }

    struct TJEProcessedQT pqt;
    // Again, taken from classic japanese implementation.
    //
    /* For float AA&N IDCT method, divisors are equal to quantization
     * coefficients scaled by scalefactor[row]*scalefactor[col], where
     *   scalefactor[0] = 1
     *   scalefactor[k] = cos(k*PI/16) * sqrt(2)    for k=1..7
     * We apply a further scale factor of 8.
     * What's actually stored is 1/divisor so that the inner loop can
     * use a multiplication rather than a division.
     */
    static const float aan_scales[] = {
        1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
        1.0f, 0.785694958f, 0.541196100f, 0.275899379f
    };

    // build (de)quantization tables
    for(int y=0; y<8; y++) {
        for(int x=0; x<8; x++) {
            int i = y*8 + x;
            pqt.luma[y*8+x] = 1.0f / (8 * aan_scales[x] * aan_scales[y] * state->qt_luma[tjei_zig_zag[i]]);
            pqt.chroma[y*8+x] = 1.0f / (8 * aan_scales[x] * aan_scales[y] * state->qt_chroma[tjei_zig_zag[i]]);
        }
    }

    { // Write header
        TJEJPEGHeader header;
        // JFIF header.
        header.SOI = tjei_be_word(0xffd8);  // Sequential DCT
        header.APP0 = tjei_be_word(0xffe0);

        uint16_t jfif_len = sizeof(TJEJPEGHeader) - 4 /*SOI & APP0 markers*/;
        header.jfif_len = tjei_be_word(jfif_len);
        memcpy(header.jfif_id, (void*)tjeik_jfif_id, 5);
        header.version = tjei_be_word(0x0102);
        header.units = 0x01;  // Dots-per-inch
        header.x_density = tjei_be_word(0x0060);  // 96 DPI
        header.y_density = tjei_be_word(0x0060);  // 96 DPI
        header.x_thumb = 0;
        header.y_thumb = 0;
        tjei_write(state, &header, sizeof(TJEJPEGHeader), 1);
    }
    {  // Write comment
        TJEJPEGComment com;
        uint16_t com_len = 2 + sizeof(tjeik_com_str) - 1;
        // Comment
        com.com = tjei_be_word(0xfffe);
        com.com_len = tjei_be_word(com_len);
        memcpy(com.com_str, (void*)tjeik_com_str, sizeof(tjeik_com_str)-1);
        tjei_write(state, &com, sizeof(TJEJPEGComment), 1);
    }

    // Write quantization tables.
    tjei_write_DQT(state, state->qt_luma, 0x00);
    tjei_write_DQT(state, state->qt_chroma, 0x01);

    {  // Write the frame marker.
        TJEFrameHeader header;
        header.SOF = tjei_be_word(0xffc0);
        header.len = tjei_be_word(8 + 3 * 3);
        header.precision = 8;
        assert(width <= 0xffff);
        assert(height <= 0xffff);
        header.width = tjei_be_word((uint16_t)width);
        header.height = tjei_be_word((uint16_t)height);
        header.num_components = 3;
        uint8_t tables[3] = {
            0,  // Luma component gets luma table (see tjei_write_DQT call above.)
            1,  // Chroma component gets chroma table
            1,  // Chroma component gets chroma table
        };
        for (int i = 0; i < 3; ++i) {
            TJEComponentSpec spec;
            spec.component_id = (uint8_t)(i + 1);  // No particular reason. Just 1, 2, 3.
            spec.sampling_factors = (uint8_t)0x11;
            spec.qt = tables[i];

            header.component_spec[i] = spec;
        }
        // Write to file.
        tjei_write(state, &header, sizeof(TJEFrameHeader), 1);
    }

    tjei_write_DHT(state, state->ht_bits[TJEI_LUMA_DC],   state->ht_vals[TJEI_LUMA_DC], TJEI_DC, 0);
    tjei_write_DHT(state, state->ht_bits[TJEI_LUMA_AC],   state->ht_vals[TJEI_LUMA_AC], TJEI_AC, 0);
    tjei_write_DHT(state, state->ht_bits[TJEI_CHROMA_DC], state->ht_vals[TJEI_CHROMA_DC], TJEI_DC, 1);
    tjei_write_DHT(state, state->ht_bits[TJEI_CHROMA_AC], state->ht_vals[TJEI_CHROMA_AC], TJEI_AC, 1);

    // Write start of scan
    {
        TJEScanHeader header;
        header.SOS = tjei_be_word(0xffda);
        header.len = tjei_be_word((uint16_t)(6 + (sizeof(TJEFrameComponentSpec) * 3)));
        header.num_components = 3;

        uint8_t tables[3] = {
            0x00,
            0x11,
            0x11,
        };
        for (int i = 0; i < 3; ++i) {
            TJEFrameComponentSpec cs;
            // Must be equal to component_id from frame header above.
            cs.component_id = (uint8_t)(i + 1);
            cs.dc_ac = (uint8_t)tables[i];

            header.component_spec[i] = cs;
        }
        header.first = 0;
        header.last  = 63;
        header.ah_al = 0;
        tjei_write(state, &header, sizeof(TJEScanHeader), 1);

    }
    // Write compressed data.

    float du_y[64];
    float du_b[64];
    float du_r[64];

    // Set diff to 0.
    int pred_y = 0;
    int pred_b = 0;
    int pred_r = 0;

    // Bit stack
    uint32_t bitbuffer = 0;
    uint32_t location = 0;


    for ( int y = 0; y < height; y += 8 ) {
        for ( int x = 0; x < width; x += 8 ) {
            // Block loop: ====
            for ( int off_y = 0; off_y < 8; ++off_y ) {
                for ( int off_x = 0; off_x < 8; ++off_x ) {
                    int block_index = (off_y * 8 + off_x);

                    int src_index = (((y + off_y) * width) + (x + off_x)) * src_num_components;

                    int col = x + off_x;
                    int row = y + off_y;

                    if(row >= height) {
                        src_index -= (width * (row - height + 1)) * src_num_components;
                    }
                    if(col >= width) {
                        src_index -= (col - width + 1) * src_num_components;
                    }
                    assert(src_index < width * height * src_num_components);

                    uint8_t r = src_data[src_index + 0];
                    uint8_t g = src_data[src_index + 1];
                    uint8_t b = src_data[src_index + 2];

                    float luma = 0.299f   * r + 0.587f    * g + 0.114f    * b - 128;
                    float cb   = -0.1687f * r - 0.3313f   * g + 0.5f      * b;
                    float cr   = 0.5f     * r - 0.4187f   * g - 0.0813f   * b;

                    du_y[block_index] = luma;
                    du_b[block_index] = cb;
                    du_r[block_index] = cr;
                }
            }

            tjei_encode_and_write_MCU(state, du_y,
                                     pqt.luma,
                                     state->ehuffsize[TJEI_LUMA_DC], state->ehuffcode[TJEI_LUMA_DC],
                                     state->ehuffsize[TJEI_LUMA_AC], state->ehuffcode[TJEI_LUMA_AC],
                                     &pred_y, &bitbuffer, &location);
            tjei_encode_and_write_MCU(state, du_b,
                                     pqt.chroma,
                                     state->ehuffsize[TJEI_CHROMA_DC], state->ehuffcode[TJEI_CHROMA_DC],
                                     state->ehuffsize[TJEI_CHROMA_AC], state->ehuffcode[TJEI_CHROMA_AC],
                                     &pred_b, &bitbuffer, &location);
            tjei_encode_and_write_MCU(state, du_r,
                                     pqt.chroma,
                                     state->ehuffsize[TJEI_CHROMA_DC], state->ehuffcode[TJEI_CHROMA_DC],
                                     state->ehuffsize[TJEI_CHROMA_AC], state->ehuffcode[TJEI_CHROMA_AC],
                                     &pred_r, &bitbuffer, &location);


        }
    }

    // Finish the image.
    { // Flush
        if (location > 0 && location < 8) {
            tjei_write_bits(state, &bitbuffer, &location, (uint16_t)(8 - location), 0);
        }
    }
    uint16_t EOI = tjei_be_word(0xffd9);
    tjei_write(state, &EOI, sizeof(uint16_t), 1);

    if (state->output_buffer_count) {
        state->write_context.func(state->write_context.context, state->output_buffer, (int)state->output_buffer_count);
        state->output_buffer_count = 0;
    }

    return 1;
}

int tje_encode_with_func(tje_write_func* func,
                         void* context,
                         const int quality,
                         const int width,
                         const int height,
                         const int num_components,
                         const unsigned char* src_data)
{
    if (quality < 1 || quality > 3) {
        tje_log("[ERROR] -- Valid 'quality' values are 1 (lowest), 2, or 3 (highest)\n");
        return 0;
    }

    TJEState state = { 0 };

    uint8_t qt_factor = 1;
    switch(quality) {
    case 3:
        for ( int i = 0; i < 64; ++i ) {
            state.qt_luma[i]   = 1;
            state.qt_chroma[i] = 1;
        }
        break;
    case 2:
        qt_factor = 10;
        // don't break. fall through.
    case 1:
        for ( int i = 0; i < 64; ++i ) {
            state.qt_luma[i]   = tjei_default_qt_luma_from_spec[i] / qt_factor;
            if (state.qt_luma[i] == 0) {
                state.qt_luma[i] = 1;
            }
            state.qt_chroma[i] = tjei_default_qt_chroma_from_paper[i] / qt_factor;
            if (state.qt_chroma[i] == 0) {
                state.qt_chroma[i] = 1;
            }
        }
        break;
    default:
        assert(!"invalid code path");
        break;
    }

    TJEWriteContext wc = { 0 };

    wc.context = context;
    wc.func = func;

    state.write_context = wc;


    tjei_huff_expand(&state);

    int result = tjei_encode_main(&state, src_data, width, height, num_components);

    return result;
}

