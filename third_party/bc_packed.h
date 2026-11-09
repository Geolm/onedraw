#ifndef BC_PACKED
#define BC_PACKED


#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//----------------------------------------------------------------------------------------------------------------------------
// version
#define BC_PACKED_MAJOR 1
#define BC_PACKED_MINOR 0
#define BC_PACKED_PATCH 1


//----------------------------------------------------------------------------------------------------------------------------
// Memory interface, used for the large working buffers of the compressor.
// If NULL is passed to bcp_init, a standard malloc/realloc/free backend is used.

typedef struct bcp_mem_interface
{
    void*  (*malloc_fn)(size_t size, void* user);
    void*  (*realloc_fn)(void* old_ptr, size_t old_size, size_t new_size, void* user);
    void   (*free_fn)(void* ptr, void* user);
    void*   user;
} bcp_mem_interface;


typedef struct bcp_context bcp_context;


//----------------------------------------------------------------------------------------------------------------------------
// Creates a context. Returns NULL if the allocation fails.
bcp_context* bcp_init(bcp_mem_interface* mem);

//----------------------------------------------------------------------------------------------------------------------------
// Frees the context and all its working buffers. NULL is accepted.
void bcp_terminate(bcp_context* ctx);

//----------------------------------------------------------------------------------------------------------------------------
// Maximum size of the compressed stream for a given texture, in bytes.
// The [output] buffer of bcp_compress_bc1 must be at least this large.
size_t packed_bc1_maxsize(uint32_t width, uint32_t height);

//----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC1 texture (width x height pixels, must be a multiple of 4) into [output].
// [num_strips] receives the number of strips used for the compressed stream.
// Returns the size of the compressed stream in bytes, or 0 on failure.
size_t bcp_compress_bc1(bcp_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
//
// Decompress one strip of the image, for a complete image this function should be called [num_strips] times
//      [input]         compressed data (output of bcp_compress_bc1)
//      [input_length]  size in byte of the compressed data
//      [width, height] dimensions of the output BC1 image
//      [num_strips]    number of strips, as returned by bcp_compress_bc1
//      [output]        must be big enough to contain a BC1 image of width x height
//      [strip_index]   index of the strip, must be in [0; num_strips[
// Returns false on error otherwise true
bool bcp_decompress_bc1(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);

//----------------------------------------------------------------------------------------------------------------------------
// Maximum size of the compressed stream for a given BC4 / BC5 texture, in bytes:
// 2x the standard block size, 16 bytes per 4x4 block for BC4 and 32 for BC5.
// The [output] buffer of bcp_compress_bc4 / bcp_compress_bc5 must be at least this large.
size_t packed_bc4_maxsize(uint32_t width, uint32_t height);
size_t packed_bc5_maxsize(uint32_t width, uint32_t height);

//----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC4 / BC5 texture (width x height pixels, must be a multiple of 4) into [output].
// [bc4_image] holds (width/4) * (height/4) standard BC4 blocks of 8 bytes each: two 8-bit
// endpoints followed by the little-endian 48-bit field of the 16 3-bit indices (raster order).
// [bc5_image] holds the standard 16-byte BC5 blocks: the red BC4 block followed by the green one.
// [num_strips] receives the number of strips used for the compressed stream.
// Returns the size of the compressed stream in bytes, or 0 on failure.
size_t bcp_compress_bc4(bcp_context* ctx, const void* bc4_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);
size_t bcp_compress_bc5(bcp_context* ctx, const void* bc5_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
//
// Decompress one strip of the image, for a complete image this function should be called [num_strips] times
//      [input]         compressed data (output of bcp_compress_bc4 / bcp_compress_bc5)
//      [input_length]  size in byte of the compressed data
//      [width, height] dimensions of the output BC4 / BC5 image
//      [num_strips]    number of strips, as returned by bcp_compress_bc4 / bcp_compress_bc5
//      [output]        must be big enough to contain a BC4 / BC5 image of width x height
//      [strip_index]   index of the strip, must be in [0; num_strips[
// Returns false on error otherwise true
bool bcp_decompress_bc4(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);
bool bcp_decompress_bc5(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);

//----------------------------------------------------------------------------------------------------------------------------
// Maximum size of the compressed stream for a given BC3 texture, in bytes:
// packed_bc1_maxsize + packed_bc4_maxsize + 4 (the prefix, see the stream description below).
// The [output] buffer of bcp_compress_bc3 must be at least this large.
size_t packed_bc3_maxsize(uint32_t width, uint32_t height);

//----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC3 texture (width x height pixels, must be a multiple of 4) into [output].
// [bc3_image] holds (width/4) * (height/4) standard 16-byte BC3 blocks: the 8-byte BC1 color
// block followed by the 8-byte BC4 alpha block (the standard D3D layout). The compressed
// stream is [u32 size of the BC1 stream][BC1 stream][BC4 stream], the two substreams being
// exactly the bcp_compress_bc1 / bcp_compress_bc4 streams of the color and alpha halves, so
// both the BC1 and the BC4 model sets, strip offsets and strips are present in the stream.
// [num_strips] receives the number of strips used for the compressed stream.
// Returns the size of the compressed stream in bytes, or 0 on failure.
size_t bcp_compress_bc3(bcp_context* ctx, const void* bc3_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
//
// Decompress one strip of the image, for a complete image this function should be called [num_strips] times
//      [input]         compressed data (output of bcp_compress_bc3)
//      [input_length]  size in byte of the compressed data
//      [width, height] dimensions of the output BC3 image
//      [num_strips]    number of strips, as returned by bcp_compress_bc3
//      [output]        must be big enough to contain a BC3 image of width x height
//      [strip_index]   index of the strip, must be in [0; num_strips[)
// Returns false on error otherwise true
bool bcp_decompress_bc3(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);

#ifdef __cplusplus
}
#endif

#endif

