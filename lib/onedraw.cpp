#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"

#include "onedraw.h"
#include "common.h"
#include "default_font.h"
#include "default_font_atlas.h"
#include "default_font_highdpi.h"
#include "default_font_atlas_highdpi.h"
#include "binning.h"
#include "rasterization.h"
#include <stdatomic.h>

// ---------------------------------------------------------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------------------------------------------------------

#define SAFE_RELEASE(p) if (p!=nullptr) {p->release(); p = nullptr;}
#define UNUSED_VARIABLE(a) (void)(a)
#define LAST_CLIP_INDEX ((uint8_t) r->commands.clipshapes_buffer.GetNumElements()-1)
#define assert_msg(expr, msg) assert((expr) && (msg))

// ---------------------------------------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------------------------------------

constexpr size_t STRING_BUFFER_SIZE = 512U;
constexpr float VEC2_SQR2 = 1.41421356237f;
constexpr float HALF_PIXEL = .5f;
constexpr float VEC2_PI = 3.14159265f;
constexpr uint32_t TESSELATION_STACK_MAX = 1024U;
constexpr float COLINEAR_THRESHOLD = .1f;
// quantized_aabb stores tile indices on 8 bits (max 256 tiles), so the screen is limited to 256 * TILE_SIZE pixels
constexpr uint32_t MAX_SCREEN_SIZE = (UINT8_MAX + 1U) * TILE_SIZE;   // 4096

// ---------------------------------------------------------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------------------------------------------------------
template<class T> void swap(T& a, T& b) {T tmp = a; a = b; b = tmp;}
template<class T> T min(T a, T b) {return (a<b) ? a : b;}
template<class T> T max(T a, T b) {return (a>b) ? a : b;}

template<typename T>
class DynamicBuffer
{
public:
    enum {MaxInflightBuffers = 3};

private:
    uint32_t GetIndex(uint32_t currentFrameIndex) {return currentFrameIndex % DynamicBuffer::MaxInflightBuffers;}
    MTL::Buffer* m_Buffers[MaxInflightBuffers];
    T* m_pData {nullptr};
    size_t m_NumElements {0};
    size_t m_MaxElements {0};

public:
    DynamicBuffer() {for(uint32_t i=0; i<DynamicBuffer::MaxInflightBuffers; ++i) m_Buffers[i] = nullptr;}

    void Init(MTL::Device* device, NS::UInteger length)
    {
        for(uint32_t i=0; i<DynamicBuffer::MaxInflightBuffers; ++i)
            m_Buffers[i] = device->newBuffer(length, MTL::ResourceStorageModeShared);
    
        m_pData = nullptr;
        m_NumElements = 0;
        m_MaxElements = length / sizeof(T);
    }

    T* Map(uint32_t currentFrameIndex)
    {
        m_pData = (T*)m_Buffers[GetIndex(currentFrameIndex)]->contents();
        m_NumElements = 0;
        return m_pData;
    }

    T* NewElement() {if (m_NumElements < m_MaxElements) return &m_pData[m_NumElements++]; return nullptr;}

    T* LastElement()
    {
        if (m_NumElements > 0)
            return &m_pData[m_NumElements-1];

        return nullptr;
    }

    T* NewMultiple(uint32_t count)
    {
        T* output = nullptr;
        if (m_NumElements + count <= m_MaxElements)
        {
            output = &m_pData[m_NumElements];
            m_NumElements += count;
        }
        return output;
    }

    void RemoveLast() {if (m_NumElements>0) m_NumElements--;}

    void Terminate()
    {
        for(uint32_t i=0; i<DynamicBuffer::MaxInflightBuffers; ++i)
        {
            if (m_Buffers[i] != nullptr)
            {
                m_Buffers[i]->release();
                m_Buffers[i] = nullptr;
            }
        }
    }

    size_t GetNumElements() const {return m_NumElements;}
    size_t GetMaxElements() const {return m_MaxElements;}
    MTL::Buffer* GetBuffer(uint32_t currentFrameIndex) {return m_Buffers[GetIndex(currentFrameIndex)];}
    NS::UInteger GetLength() const {return m_Buffers[0]->length();}
    size_t GetTotalSize() const {return m_Buffers[0]->allocatedSize() * DynamicBuffer::MaxInflightBuffers;}
};

// ---------------------------------------------------------------------------------------------------------------------------
void write_float(float* buffer, float value) {*buffer = value;};
template<typename...Args>
void write_float(float* buffer, float value, Args ... args)
{
    *buffer = value;
    write_float(++buffer, args...);
}

// ---------------------------------------------------------------------------------------------------------------------------
// private structures
// ---------------------------------------------------------------------------------------------------------------------------

typedef struct vec2 {float x, y;} vec2;
typedef struct aabb {vec2 min, max;} aabb;
typedef struct quadratic_bezier {vec2 c0, c1, c2;} quadratic_bezier;
typedef struct cubic_bezier {vec2 c0, c1, c2, c3;} cubic_bezier;

// the pre-build baker serializes struct alphabet into the default_font headers, fail at compile time if the layout drifted
static_assert(sizeof(alphabet) == default_font_size, "baked font layout mismatch");
static_assert(sizeof(alphabet) == default_font_highdpi_size, "baked font layout mismatch");

struct onedraw
{
    MTL::Device* device;
    MTL::CommandQueue* command_queue;
    MTL::CommandBuffer* command_buffer;
    dispatch_semaphore_t semaphore;

    struct
    {
        DynamicBuffer<draw_cmd_arguments> draw_arg;
        DynamicBuffer<tiles_data> bin_output_arg;
        DynamicBuffer<draw_command> buffer;
        DynamicBuffer<draw_color> colors;
        DynamicBuffer<quantized_aabb> aabb_buffer;
        DynamicBuffer<float> data_buffer;
        DynamicBuffer<clip_shape> clipshapes_buffer;
        uint32_t count;
        quantized_aabb* group_aabb {nullptr};
        quantized_aabb* draw_aabb {nullptr};
    } commands;

    // region binning
    struct
    {
        MTL::ComputePipelineState* predicate_pso {nullptr};
        MTL::ComputePipelineState* exclusive_scan_pso {nullptr};
        MTL::ComputePipelineState* binning_pso {nullptr};
        MTL::Buffer* indices {nullptr};
        MTL::Buffer* predicate {nullptr};
        MTL::Buffer* scan {nullptr};
        uint16_t num_width;
        uint16_t num_height;
        uint16_t count;
        uint32_t num_groups;
    } regions;

    // tile binning
    struct 
    {
        MTL::Buffer* head {nullptr};
        MTL::ComputePipelineState* binning_pso {nullptr};
        MTL::ComputePipelineState* write_icb_pso {nullptr};
        MTL::Buffer* counters_buffer {nullptr};
        MTL::Buffer* indirect_arg {nullptr};
        MTL::Buffer* indices {nullptr};
        MTL::Buffer* nodes {nullptr};
        MTL::IndirectCommandBuffer* indirect_cb {nullptr};
        uint16_t num_width;
        uint16_t num_height;
        uint32_t count;
        bool culling_debug {false};
    } tiles;

    // rasterizer
    struct
    {
        MTL::RenderPipelineState* pso {nullptr};
        MTL::DepthStencilState* depth_stencil_state {nullptr};
        MTL::Texture* atlas {nullptr};
        float4 clear_color {.x = 0.f, .y = 0.f, .z = 0.f, .w = 1.f};
        uint16_t width;
        uint16_t height;
        float aa_width {VEC2_SQR2};
        float group_smoothness {0.f};
        sdf_operator group_op;
        float outline_width {0.f};
        bool srgb_backbuffer {true}; 
    } rasterizer;

    // font
    struct
    {
        MTL::Texture* texture {nullptr};
        MTL::Buffer* glyphs {nullptr};
        alphabet desc;
        float scale {1.f};
        bool high_dpi {false};
    } font;

    // screenshot service
    struct
    {
        MTL::Texture* texture {nullptr};
        void* out_pixels {nullptr};
        uint32_t region_x, region_y;
        uint32_t region_width, region_height;
        bool show_region {false};
        bool capture_image {false};
        bool allocate_resources {false};
    } screenshot;

    // stats
    struct
    {
        uint32_t peak_num_draw_cmd {0};
        uint32_t num_draw_data {0};
        _Atomic(float) gpu_time;
        float average_gpu_time {0.f};
        float accumulated_gpu_time {0.f};
        uint32_t frame_index {0};
    } stats;

    void (*custom_log)(const char* string);
    char string_buffer[STRING_BUFFER_SIZE];
};


// ---------------------------------------------------------------------------------------------------------------------------
// private functions
// ---------------------------------------------------------------------------------------------------------------------------

static inline float float_max(float a, float b) {return (a>b) ? a : b;}
static inline float float_clamp(float f, float a, float b) {if (f<a) return a; if (f>b) return b; return f;}
static inline vec2 vec2_splat(float value) {return (vec2) {value, value};}
static inline vec2 vec2_set(float x, float y) {return (vec2) {x, y};}
static inline vec2 vec2_add(vec2 a, vec2 b) {return (vec2) {a.x + b.x, a.y + b.y};}
static inline vec2 vec2_sub(vec2 a, vec2 b) {return (vec2) {a.x - b.x, a.y - b.y};}
static inline vec2 vec2_min(vec2 v, vec2 op) {return (vec2) {.x = (v.x < op.x) ? v.x : op.x, .y = (v.y < op.y) ? v.y : op.y};}
static inline vec2 vec2_min3(vec2 a, vec2 b, vec2 c) {return vec2_min(a, vec2_min(b, c));}
static inline vec2 vec2_min4(vec2 a, vec2 b, vec2 c, vec2 d) {return vec2_min(a, vec2_min3(b, c, d));}
static inline vec2 vec2_max(vec2 v, vec2 op) {return (vec2) {.x = (v.x > op.x) ? v.x : op.x, .y = (v.y > op.y) ? v.y : op.y};}
static inline vec2 vec2_max3(vec2 a, vec2 b, vec2 c) {return vec2_max(a, vec2_max(b, c));}
static inline vec2 vec2_max4(vec2 a, vec2 b, vec2 c, vec2 d) {return vec2_max(a, vec2_max3(b, c, d));}
static inline vec2 vec2_skew(vec2 v) {return (vec2) {-v.y, v.x};}
static inline vec2 vec2_scale(vec2 a, float f) {return (vec2) {a.x * f, a.y * f};}
static inline float vec2_dot(vec2 a, vec2 b) {return fmaf(a.x, b.x, a.y * b.y);}
static inline float vec2_sq_length(vec2 v) {return vec2_dot(v, v);}
static inline float vec2_length(vec2 v) {return sqrtf(vec2_sq_length(v));}
static inline bool vec2_similar(vec2 a, vec2 b, float epsilon) {return (fabsf(a.x - b.x) < epsilon) && (fabsf(a.y - b.y) < epsilon);}
static inline vec2 vec2_direction(float angle) {return (vec2) {cosf(angle), sinf(angle)};}
static inline float vec2_distance(vec2 a, vec2 b) {return vec2_length(vec2_sub(b, a));}

//----------------------------------------------------------------------------------------------------------------------------
static inline float vec2_normalize(vec2* v)
{
    float norm = vec2_length(*v);
    if (norm <= FLT_EPSILON)
        return 0.f;

    *v = vec2_scale(*v, 1.f / norm);
    return norm;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
static inline vec2 vec2_lerp(vec2 a, vec2 b, float t) 
{
    float one_minus_t = 1.f - t;
    return (vec2) {.x = fmaf(a.x , one_minus_t, b.x * t), .y = fmaf(a.y , one_minus_t, b.y * t)};
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
static inline bool is_colinear(vec2 p0, vec2 p1, vec2 p2, float threshold)
{
    vec2 v0 = vec2_sub(p1, p0);
    vec2 v1 = vec2_sub(p2, p0);
    float squared_area = fabsf(v0.x * v1.y - v0.y * v1.x);
    float base2 = vec2_dot(v0, v0);

    if (base2 < FLT_EPSILON)
        return true;

    float squared_height = (squared_area * squared_area) / base2;
    return squared_height <= (threshold * threshold);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline void aabb_grow(aabb* box, vec2 amount)
{
    box->min = vec2_sub(box->min, amount);
    box->max = vec2_add(box->max, amount);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline aabb aabb_from_circle(vec2 center, float radius)
{
    return (aabb)
    {
        .min = vec2_sub(center, vec2_splat(radius)),
        .max = vec2_add(center, vec2_splat(radius))
    };
}

//----------------------------------------------------------------------------------------------------------------------------
static inline aabb aabb_from_triangle(vec2 v0, vec2 v1, vec2 v2)
{
    return (aabb)
    {
        .min = vec2_min3(v0, v1, v2),
        .max = vec2_max3(v0, v1, v2)
    };
}

//----------------------------------------------------------------------------------------------------------------------------
static inline aabb aabb_from_rounded_obb(vec2 p0, vec2 p1, float width, float border)
{
    aabb box;

    vec2 dir = vec2_sub(p1, p0);
    vec2_normalize(&dir);
    vec2 normal = vec2_skew(dir);

    normal = vec2_scale(normal, width*.5f + border);
    dir = vec2_scale(dir, border);
    p0 = vec2_sub(p0, dir);
    p1 = vec2_add(p1, dir);

    vec2 vertices[4];
    vertices[0] = vec2_add(p0, normal);
    vertices[1] = vec2_sub(p0, normal);
    vertices[2] = vec2_sub(p1, normal);
    vertices[3] = vec2_add(p1, normal);

    box.min = vec2_min4(vertices[0], vertices[1], vertices[2], vertices[3]);
    box.max = vec2_max4(vertices[0], vertices[1], vertices[2], vertices[3]);

    return box;
}

//----------------------------------------------------------------------------------------------------------------------------
static inline float srgb_to_linear(float c)
{
    if (c <= 0.04045f)
        return c / 12.92f;
    else
        return powf((c + 0.055f) / 1.055f, 2.4f);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline float bitcast_u32_to_float(uint32_t value)
{
    union {float f; uint32_t u;} c;
    c.u = value;
    return c.f;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_log(struct onedraw* r, const char* string, ...)
{
    if (r->custom_log != nullptr)
    {
        va_list args;
        va_start(args, string);
        vsnprintf(r->string_buffer, STRING_BUFFER_SIZE, string, args);
        va_end(args);

        r->custom_log(r->string_buffer);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void od_init_screenshot_resources(struct onedraw* r)
{
    if (r->screenshot.allocate_resources)
    {
        SAFE_RELEASE(r->screenshot.texture);

        MTL::TextureDescriptor* pTextureDesc = MTL::TextureDescriptor::alloc()->init();
        pTextureDesc->setWidth(r->rasterizer.width);
        pTextureDesc->setHeight(r->rasterizer.height);
        pTextureDesc->setPixelFormat(r->rasterizer.srgb_backbuffer ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
        pTextureDesc->setTextureType(MTL::TextureType2D);
        pTextureDesc->setMipmapLevelCount(1);
        pTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        pTextureDesc->setStorageMode(MTL::StorageModeShared);

        r->screenshot = 
        {
            .texture = r->device->newTexture(pTextureDesc),
            .out_pixels = nullptr, // null because defined by the user
            .region_x = 0,
            .region_y = 0,
            .region_width = r->rasterizer.width,
            .region_height = r->rasterizer.height
        };

        pTextureDesc->release();
    }
    else
    {
        r->screenshot = {};
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void od_create_atlas(struct onedraw* r, uint32_t width, uint32_t height, uint32_t slice_count)
{
    assert_msg(slice_count < UINT8_MAX, "too many slices");
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setTextureType(MTL::TextureType2DArray);
    desc->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB);
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setArrayLength(slice_count);
    desc->setMipmapLevelCount(1);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);

    r->rasterizer.atlas = r->device->newTexture(desc);
    desc->release();

    if (r->rasterizer.atlas == nullptr)
        od_log(r, "can't create texture array (width:%u height:%u slice_count%u)", width, height, slice_count);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_build_depthstencil_state(struct onedraw* r)
{
    MTL::DepthStencilDescriptor* pDsDesc = MTL::DepthStencilDescriptor::alloc()->init();

    pDsDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionAlways);
    pDsDesc->setDepthWriteEnabled(false);

    r->rasterizer.depth_stencil_state = r->device->newDepthStencilState(pDsDesc);

    pDsDesc->release();
}

//----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t optimal_num_threads(uint32_t num_elements, uint32_t simd_group_size, uint32_t max_threads)
{
    uint32_t rounded = (num_elements + simd_group_size - 1) / simd_group_size;
    rounded *= simd_group_size;
    return min(rounded, max_threads);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline void write_quantized_aabb(quantized_aabb* box, float min_x, float min_y, float max_x, float max_y)
{
    min_x = max(min_x, 0.f);
    min_y = max(min_y, 0.f);
    max_x = max(max_x, 0.f);
    max_y = max(max_y, 0.f);
    box->min_x = uint8_t(min(uint32_t(min_x) / TILE_SIZE, (uint32_t)UINT8_MAX));
    box->min_y = uint8_t(min(uint32_t(min_y) / TILE_SIZE, (uint32_t)UINT8_MAX));
    box->max_x = uint8_t(min(uint32_t(max_x) / TILE_SIZE, (uint32_t)UINT8_MAX));
    box->max_y = uint8_t(min(uint32_t(max_y) / TILE_SIZE, (uint32_t)UINT8_MAX));
}

//----------------------------------------------------------------------------------------------------------------------------
static inline void merge_quantized_aabb(quantized_aabb* merge, const quantized_aabb* other)
{
    if (merge != nullptr)
    {
        merge->min_x = min(merge->min_x, other->min_x);
        merge->min_y = min(merge->min_y, other->min_y);
        merge->max_x = max(merge->max_x, other->max_x);
        merge->max_y = max(merge->max_y, other->max_y);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static inline quantized_aabb invalid_quantized_aabb()
{
    return (quantized_aabb)
    {
        .min_x = UINT8_MAX,
        .min_y = UINT8_MAX,
        .max_x = 0,
        .max_y = 0
    };
}

//----------------------------------------------------------------------------------------------------------------------------
MTL::ComputePipelineState* create_pso(struct onedraw* r, MTL::Library* pLibrary, const char* function_name)
{
    MTL::Function *pFunction = pLibrary->newFunction(NS::String::string(function_name, NS::UTF8StringEncoding));
    NS::Error* pError = nullptr;

    MTL::ComputePipelineState* pso = r->device->newComputePipelineState(pFunction, &pError);

    if (pso == nullptr)
        od_log(r, "%s", pError->localizedDescription()->utf8String());

    pFunction->release();
    return pso;
}


//----------------------------------------------------------------------------------------------------------------------------
void od_build_pso(struct onedraw* r)
{
    SAFE_RELEASE(r->regions.binning_pso);
    SAFE_RELEASE(r->tiles.binning_pso);
    SAFE_RELEASE(r->rasterizer.pso);
    SAFE_RELEASE(r->tiles.write_icb_pso);
    SAFE_RELEASE(r->regions.exclusive_scan_pso);

    NS::Error* pError = nullptr;
    MTL::Library* pLibrary = r->device->newLibrary( NS::String::string(binning_shader, NS::UTF8StringEncoding), nullptr, &pError );
    if (pLibrary != nullptr)
    {
        MTL::Function* pTileBinningFunction = pLibrary->newFunction(NS::String::string("tile_bin", NS::UTF8StringEncoding));
        NS::Error* pError = nullptr;
        r->tiles.binning_pso = r->device->newComputePipelineState(pTileBinningFunction, &pError);

        if (r->tiles.binning_pso == nullptr)
        {
            od_log(r, "%s", pError->localizedDescription()->utf8String());
            return;
        }

        MTL::ArgumentEncoder* inputArgumentEncoder = pTileBinningFunction->newArgumentEncoder(0);
        MTL::ArgumentEncoder* outputArgumentEncoder = pTileBinningFunction->newArgumentEncoder(1);

        r->commands.draw_arg.Init(r->device, inputArgumentEncoder->encodedLength());
        r->commands.bin_output_arg.Init(r->device, outputArgumentEncoder->encodedLength());

        pTileBinningFunction->release();
        inputArgumentEncoder->release();
        outputArgumentEncoder->release();

        MTL::Function* pWriteIcbFunction = pLibrary->newFunction(NS::String::string("write_icb", NS::UTF8StringEncoding));
        r->tiles.write_icb_pso = r->device->newComputePipelineState(pWriteIcbFunction, &pError);
        if (r->tiles.write_icb_pso == nullptr)
        {
            od_log(r, "%s", pError->localizedDescription()->utf8String());
            return;
        }

        MTL::ArgumentEncoder* indirectArgumentEncoder = pWriteIcbFunction->newArgumentEncoder(1);
        r->tiles.indirect_arg = r->device->newBuffer(indirectArgumentEncoder->encodedLength(), MTL::ResourceStorageModeShared);
        indirectArgumentEncoder->setArgumentBuffer(r->tiles.indirect_arg, 0);
        indirectArgumentEncoder->setIndirectCommandBuffer(r->tiles.indirect_cb, 0);

        indirectArgumentEncoder->release();
        pWriteIcbFunction->release();

        r->regions.binning_pso = create_pso(r, pLibrary, "region_bin");
        r->regions.predicate_pso = create_pso(r, pLibrary, "predicate");
        r->regions.exclusive_scan_pso = create_pso(r, pLibrary, "exclusive_scan");
        pLibrary->release();
    }
    else
        od_log(r, "error while compiling binning shader : %s", pError->localizedDescription()->utf8String());

    pLibrary = r->device->newLibrary( NS::String::string(rasterization_shader, NS::UTF8StringEncoding), nullptr, &pError );
    if (pLibrary != nullptr)
    {
        MTL::Function* pVertexFunction = pLibrary->newFunction(NS::String::string("tile_vs", NS::UTF8StringEncoding));
        MTL::Function* pFragmentFunction = pLibrary->newFunction(NS::String::string("tile_fs", NS::UTF8StringEncoding));

        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pDesc->setVertexFunction(pVertexFunction);
        pDesc->setFragmentFunction(pFragmentFunction);
        pDesc->setSupportIndirectCommandBuffers(true);

        MTL::RenderPipelineColorAttachmentDescriptor *pRenderbufferAttachment = pDesc->colorAttachments()->object(0);
        pRenderbufferAttachment->setPixelFormat(r->rasterizer.srgb_backbuffer ? MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormat::PixelFormatBGRA8Unorm);
        pRenderbufferAttachment->setBlendingEnabled(false);
        r->rasterizer.pso = r->device->newRenderPipelineState( pDesc, &pError );

        if (r->rasterizer.pso == nullptr)
            od_log(r, "error while creating rasterizer pso : %s", pError->localizedDescription()->utf8String());

        pVertexFunction->release();
        pFragmentFunction->release();
        pDesc->release();
        pLibrary->release();
    }
    else
        od_log(r, "error while compiling rasterization shader : %s", pError->localizedDescription()->utf8String());
}

//----------------------------------------------------------------------------------------------------------------------------
static void od_upload_font_glyphs(struct onedraw* r);

//----------------------------------------------------------------------------------------------------------------------------
void od_build_font(struct onedraw* r)
{
    MTL::TextureDescriptor* pTextureDesc = MTL::TextureDescriptor::alloc()->init();
    pTextureDesc->setWidth(r->font.desc.texture_width);
    pTextureDesc->setHeight(r->font.desc.texture_height);
    pTextureDesc->setPixelFormat(MTL::PixelFormatBC4_RUnorm);
    pTextureDesc->setTextureType(MTL::TextureType2D);
    pTextureDesc->setMipmapLevelCount(1);
    pTextureDesc->setUsage(MTL::ResourceUsageRead );
    pTextureDesc->setStorageMode(MTL::StorageModeShared);

    r->font.texture = r->device->newTexture(pTextureDesc);

    const uint8_t* atlas_pixels = r->font.high_dpi ? default_font_atlas_highdpi : default_font_atlas;
    r->font.texture->replaceRegion( MTL::Region(
        0, 0, 0,
        r->font.desc.texture_width, r->font.desc.texture_height, 1 ),
        0, atlas_pixels, (r->font.desc.texture_width/4) * 8);
    pTextureDesc->release();

    od_upload_font_glyphs(r);
}

//----------------------------------------------------------------------------------------------------------------------------
static void od_upload_font_glyphs(struct onedraw* r)
{
    // fill the glyph description to be upload on the gpu
    // note: the glyph size is scaled by the font scale so text matches the
    //       (possibly high-dpi) framebuffer resolution, the uv's are untouched
    font_char cpu_buffer[MAX_GLYPHS];
    for(uint32_t i=0; i<r->font.desc.num_glyphs; i++)
    {
        const glyph& glyph = r->font.desc.glyphs[i];
        cpu_buffer[i] = 
        {
            .uv_topleft = {.x = float(glyph.x0) / float(r->font.desc.texture_width), 
                           .y = float(glyph.y0) / float(r->font.desc.texture_height)},
            .uv_bottomright = {.x = float(glyph.x1) / float(r->font.desc.texture_width),
                               .y = float(glyph.y1) / float(r->font.desc.texture_height)},
            .width = float(glyph.x1 - glyph.x0) * r->font.scale,
            .height = float(glyph.y1 - glyph.y0) * r->font.scale
        };
    }
    SAFE_RELEASE(r->font.glyphs);
    r->font.glyphs = r->device->newBuffer(cpu_buffer, sizeof(cpu_buffer), MTL::ResourceStorageModeShared);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_bin_commands(struct onedraw* r)
{
    if (r->tiles.binning_pso == nullptr || r->regions.binning_pso == nullptr || r->regions.exclusive_scan_pso == nullptr)
        return;

    assert(r->commands.buffer.GetNumElements() == r->commands.colors.GetNumElements());
    assert(r->commands.buffer.GetNumElements() == r->commands.aabb_buffer.GetNumElements());

    // clear buffers
    MTL::BlitCommandEncoder* blit_encoder = r->command_buffer->blitCommandEncoder();
    blit_encoder->fillBuffer(r->tiles.counters_buffer, NS::Range(0, r->tiles.counters_buffer->length()), 0);
    blit_encoder->fillBuffer(r->tiles.head, NS::Range(0, r->tiles.head->length()), 0xff);
    blit_encoder->fillBuffer(r->regions.indices, NS::Range(0, r->regions.indices->length()), 0xff);
    blit_encoder->endEncoding();


    // fill common structures
    draw_cmd_arguments* args = r->commands.draw_arg.Map(r->stats.frame_index);

    if (r->rasterizer.srgb_backbuffer)
        args->clear_color = r->rasterizer.clear_color;
    else
    {
        // clear color for the shader is linear the backbuffer is linear as we do
        // the conversion to srgb at the end of the fragment
        args->clear_color.x = srgb_to_linear(r->rasterizer.clear_color.x);
        args->clear_color.y = srgb_to_linear(r->rasterizer.clear_color.y);
        args->clear_color.z = srgb_to_linear(r->rasterizer.clear_color.z);
        args->clear_color.w = r->rasterizer.clear_color.w;
    }
    args->aa_width = r->rasterizer.aa_width;
    args->commands_aabb = (quantized_aabb*) r->commands.aabb_buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->commands = (draw_command*) r->commands.buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->colors = (draw_color*) r->commands.colors.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->draw_data = (float*) r->commands.data_buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->clips = (clip_shape*) r->commands.clipshapes_buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->glyphs = (font_char*) r->font.glyphs->gpuAddress();
    args->font = r->font.texture->gpuResourceID()._impl;
    args->atlas = r->rasterizer.atlas->gpuResourceID()._impl;
    args->max_nodes = MAX_NODES_COUNT;
    args->num_commands = r->commands.count;
    args->num_tile_height = r->tiles.num_height;
    args->num_tile_width = r->tiles.num_width;
    args->num_region_width = r->regions.num_width;
    args->num_region_height = r->regions.num_height;
    args->num_groups = r->regions.num_groups;
    args->screen_div = (float2) {.x = 1.f / (float)r->rasterizer.width, .y = 1.f / (float) r->rasterizer.height};
    args->culling_debug = r->tiles.culling_debug;
    args->srgb_backbuffer = r->rasterizer.srgb_backbuffer;
    args->num_elements_per_thread = (r->commands.count + MAX_THREADS_PER_THREADGROUP-1) / MAX_THREADS_PER_THREADGROUP;

    const uint32_t simd_group_count = MAX_THREADS_PER_THREADGROUP / SIMD_GROUP_SIZE;
    const uint32_t threads_for_commands = optimal_num_threads(r->commands.count, SIMD_GROUP_SIZE, MAX_THREADS_PER_THREADGROUP);

    // predicate
    MTL::ComputeCommandEncoder* compute_encoder = r->command_buffer->computeCommandEncoder();
    compute_encoder->setComputePipelineState(r->regions.predicate_pso);
    compute_encoder->setBuffer(r->commands.draw_arg.GetBuffer(r->stats.frame_index), 0, 0);
    compute_encoder->setBuffer(r->regions.predicate, 0, 1);
    compute_encoder->useResource(r->commands.aabb_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->dispatchThreads(MTL::Size(r->commands.count, 1, 1), MTL::Size(threads_for_commands, 1, 1));

    // must be a multiple of SIMD_GROUP_SIZE (simd_prefix_exclusive_sum requires a full simdgroup)
    uint32_t threads_per_region = optimal_num_threads((r->commands.count + args->num_elements_per_thread - 1) / args->num_elements_per_thread,
                                                      SIMD_GROUP_SIZE, MAX_THREADS_PER_THREADGROUP);

    compute_encoder->setComputePipelineState(r->regions.exclusive_scan_pso);
    compute_encoder->setBuffer(r->regions.scan, 0, 2);
    compute_encoder->setThreadgroupMemoryLength(simd_group_count * sizeof(uint16_t), 0);
    compute_encoder->setThreadgroupMemoryLength(simd_group_count * sizeof(uint16_t), 1);
    compute_encoder->dispatchThreads(MTL::Size(threads_per_region, r->regions.count, 1), MTL::Size(threads_per_region, 1, 1));

    // region binning
    compute_encoder->setComputePipelineState(r->regions.binning_pso);
    compute_encoder->setBuffer(r->regions.indices, 0, 1);
    compute_encoder->setBuffer(r->regions.predicate, 0, 3);
    compute_encoder->dispatchThreads(MTL::Size(r->commands.count, r->regions.count, 1), MTL::Size(16, 16, 1));

    // tile binning
    compute_encoder->setComputePipelineState(r->tiles.binning_pso);

    tiles_data* output = (tiles_data*) r->commands.bin_output_arg.Map(r->stats.frame_index);
    output->head = (uint32_t*) r->tiles.head->gpuAddress();
    output->nodes = (tile_node*) r->tiles.nodes->gpuAddress();
    output->tile_indices = (uint16_t*) r->tiles.indices->gpuAddress();

    compute_encoder->setBuffer(r->commands.bin_output_arg.GetBuffer(r->stats.frame_index), 0, 1);
    compute_encoder->setBuffer(r->tiles.counters_buffer, 0, 2);
    compute_encoder->setBuffer(r->regions.indices, 0, 3);
    compute_encoder->useResource(r->commands.aabb_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.data_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.clipshapes_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->tiles.head, MTL::ResourceUsageRead|MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.nodes, MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.indices, MTL::ResourceUsageWrite);
    compute_encoder->dispatchThreads(MTL::Size(REGION_SIZE, REGION_SIZE, r->regions.count), MTL::Size(16, 16, 1));
    compute_encoder->setComputePipelineState(r->tiles.write_icb_pso);
    compute_encoder->setBuffer(r->tiles.counters_buffer, 0, 0);
    compute_encoder->setBuffer(r->tiles.indirect_arg, 0, 1);
    compute_encoder->useResource(r->tiles.indirect_cb, MTL::ResourceUsageWrite);
    compute_encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
    compute_encoder->endEncoding();
}

//----------------------------------------------------------------------------------------------------------------------------
void od_flush(struct onedraw* r, void* drawable)
{
    assert_msg((uint16_t)((CA::MetalDrawable*)drawable)->texture()->width() == r->rasterizer.width, "drawable/renderer size mismatch");
    assert_msg((uint16_t)((CA::MetalDrawable*)drawable)->texture()->height() == r->rasterizer.height, "drawable/renderer size mismatch");

    r->command_buffer = r->command_queue->commandBuffer();

    dispatch_semaphore_wait(r->semaphore, DISPATCH_TIME_FOREVER);

    if (r->commands.count)
        od_bin_commands(r);

    MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* cd = renderPassDescriptor->colorAttachments()->object(0);
    cd->setTexture(((CA::MetalDrawable*)drawable)->texture());
    cd->setLoadAction(MTL::LoadActionClear);
    cd->setClearColor(MTL::ClearColor(r->rasterizer.clear_color.x, r->rasterizer.clear_color.y, r->rasterizer.clear_color.z, r->rasterizer.clear_color.w));
    cd->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* render_encoder = r->command_buffer->renderCommandEncoder(renderPassDescriptor);
    if (r->commands.count)
    {
        render_encoder->setViewport((MTL::Viewport){.originX = 0, .originY = 0, .width = (double)r->rasterizer.width, .height = (double)r->rasterizer.height});
        render_encoder->setCullMode(MTL::CullModeNone);
        render_encoder->setDepthStencilState(r->rasterizer.depth_stencil_state);
        render_encoder->setVertexBuffer(r->commands.draw_arg.GetBuffer(r->stats.frame_index), 0, 0);
        render_encoder->setVertexBuffer(r->tiles.indices, 0, 1);
        render_encoder->setFragmentBuffer(r->commands.draw_arg.GetBuffer(r->stats.frame_index), 0, 0);
        render_encoder->setFragmentBuffer(r->commands.bin_output_arg.GetBuffer(r->stats.frame_index), 0, 1);
        render_encoder->useResource(r->commands.draw_arg.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.colors.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.data_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.clipshapes_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.head, MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.nodes, MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.indices, MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.indirect_cb, MTL::ResourceUsageRead);
        render_encoder->useResource(r->font.texture, MTL::ResourceUsageRead);
        if (r->rasterizer.atlas != nullptr)
            render_encoder->useResource(r->rasterizer.atlas, MTL::ResourceUsageRead);
        render_encoder->setRenderPipelineState(r->rasterizer.pso);
        render_encoder->executeCommandsInBuffer(r->tiles.indirect_cb, NS::Range(0, 1));
    }
    render_encoder->endEncoding();

    const bool take_screenshot = (r->screenshot.out_pixels != nullptr) && (r->screenshot.capture_image) && (r->screenshot.texture != nullptr);

    r->command_buffer->addCompletedHandler(^void( MTL::CommandBuffer* pCmd )
    {
        UNUSED_VARIABLE(pCmd);
        dispatch_semaphore_signal( r->semaphore );

        atomic_store(&r->stats.gpu_time, (float)(pCmd->GPUEndTime() - pCmd->GPUStartTime()));

        if (take_screenshot)
        {
            MTL::Region region = MTL::Region(r->screenshot.region_x, r->screenshot.region_y,
                                             r->screenshot.region_width, r->screenshot.region_height);
            r->screenshot.texture->getBytes(r->screenshot.out_pixels, r->screenshot.region_width * 4, region, 0);
            r->screenshot.capture_image = false;
            r->screenshot.out_pixels = nullptr;
        }
    });

    if (take_screenshot)
    {
        MTL::BlitCommandEncoder* blit = r->command_buffer->blitCommandEncoder();
        blit->copyFromTexture(((CA::MetalDrawable*)drawable)->texture(),0, 0, r->screenshot.texture, 0, 0, 1, 1);
        blit->endEncoding();
    }

    r->command_buffer->presentDrawable((CA::MetalDrawable*)drawable);
    r->command_buffer->commit();
    r->command_buffer->waitUntilCompleted();

    renderPassDescriptor->release();
}

// ---------------------------------------------------------------------------------------------------------------------------
// public functions
// ---------------------------------------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------------------------------------
size_t od_min_memory_size(void)
{
    return sizeof(struct onedraw);
}

//----------------------------------------------------------------------------------------------------------------------------
struct onedraw* od_init(onedraw_def* def)
{
    if (def == nullptr || def->preallocated_buffer == nullptr || def->metal_device == nullptr)
        return nullptr;

    // a failure must not crash the host, return nullptr instead
    if (((uintptr_t)def->preallocated_buffer) % sizeof(uintptr_t) != 0)
    {
        if (def->log_func != nullptr)
            def->log_func("onedraw: preallocated_buffer must be aligned on sizeof(uintptr_t)");
        return nullptr;
    }

    // release build has no assert, check the GPU family in all cases
    if (!((MTL::Device*)def->metal_device)->supportsFamily(MTL::GPUFamilyApple7))
    {
        if (def->log_func != nullptr)
            def->log_func("onedraw: requires a M1/A14 (or later) GPU, can't init");
        return nullptr;
    }

    if (def->viewport_width == 0 || def->viewport_height == 0)
    {
        if (def->log_func != nullptr)
            def->log_func("onedraw: viewport width and height must be non-zero, can't init");
        return nullptr;
    }

    onedraw* r = new(def->preallocated_buffer) onedraw;

    r->custom_log = def->log_func;
    r->device = (MTL::Device*)def->metal_device;
    r->command_queue = r->device->newCommandQueue();
    r->screenshot.allocate_resources = def->allow_screenshot;
    r->rasterizer.srgb_backbuffer = def->srgb_backbuffer;

    if (r->command_queue == nullptr)
    {
        od_log(r, "can't create a command queue");
        SAFE_RELEASE(r->command_queue);
        r->commands.buffer.Terminate();
        r->commands.colors.Terminate();
        r->commands.data_buffer.Terminate();
        r->commands.aabb_buffer.Terminate();
        r->commands.clipshapes_buffer.Terminate();
        return nullptr;
    }

    r->commands.buffer.Init(r->device, sizeof(draw_command) * MAX_COMMANDS);
    r->commands.colors.Init(r->device, sizeof(draw_color) * MAX_COMMANDS);
    r->commands.data_buffer.Init(r->device, sizeof(float) * MAX_DRAWDATA);
    r->commands.aabb_buffer.Init(r->device, sizeof(quantized_aabb) * MAX_COMMANDS);
    r->commands.clipshapes_buffer.Init(r->device, sizeof(clip_shape) * MAX_CLIPS);
    r->tiles.counters_buffer = r->device->newBuffer(sizeof(counters), MTL::ResourceStorageModePrivate);
    r->tiles.nodes = r->device->newBuffer(sizeof(tile_node) * MAX_NODES_COUNT, MTL::ResourceStorageModePrivate);

    MTL::IndirectCommandBufferDescriptor* icb_desc = MTL::IndirectCommandBufferDescriptor::alloc()->init();
    icb_desc->setCommandTypes(MTL::IndirectCommandTypeDraw);
    icb_desc->setInheritBuffers(true);
    icb_desc->setInheritPipelineState(true);
    icb_desc->setMaxVertexBufferBindCount(2);
    icb_desc->setMaxFragmentBufferBindCount(2);
    r->tiles.indirect_cb = r->device->newIndirectCommandBuffer(icb_desc, 1, MTL::ResourceStorageModePrivate);
    icb_desc->release();

    r->semaphore = dispatch_semaphore_create(DynamicBuffer<float>::MaxInflightBuffers);
    r->stats.average_gpu_time = 0.f;
    r->stats.accumulated_gpu_time = 0.f;
    atomic_store(&r->stats.gpu_time, 0.f);

    r->font.high_dpi = def->high_dpi;

    if (r->font.high_dpi)
        r->font.desc = *((alphabet*) default_font_highdpi);
    else
        r->font.desc = *((alphabet*) default_font);

    od_build_pso(r);
    od_build_font(r);
    od_build_depthstencil_state(r);
    od_resize(r, def->viewport_width, def->viewport_height);

    if (def->atlas.width != 0)
        od_create_atlas(r, def->atlas.width, def->atlas.height, def->atlas.num_slices);

    return r;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_upload_slice(struct onedraw* r, const void* pixel_data, uint32_t slice_index)
{
    if (r->rasterizer.atlas == nullptr)
    {
        od_log(r, "can't upload a slice, atlas is not allocated (set atlas width/height in od_init)");
        return;
    }

    assert_msg(slice_index<r->rasterizer.atlas->arrayLength(), "slice_index is out of bound");

    const NS::UInteger bpp = 4;   // MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB
    const NS::UInteger bytes_per_row = r->rasterizer.atlas->width() * bpp;

    MTL::Region region = MTL::Region::Make2D(0, 0, r->rasterizer.atlas->width(), r->rasterizer.atlas->height());

    r->rasterizer.atlas->replaceRegion(
        region,
        0,  // no mipmap
        slice_index,
        pixel_data,
        bytes_per_row,
        bytes_per_row * r->rasterizer.atlas->height()
    );
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_capture_region(struct onedraw* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    assert_msg(x+width <= r->rasterizer.width && y+height <= r->rasterizer.height,
               "capture region must be inside the rendertarget");
    r->screenshot.region_x = x;
    r->screenshot.region_y = y;
    r->screenshot.region_width = width;
    r->screenshot.region_height = height;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_get_capture_region_dimensions(struct onedraw *r, uint32_t* width, uint32_t* height)
{
    *width = r->screenshot.region_width;
    *height = r->screenshot.region_height;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_take_screenshot(struct onedraw* r, void* out_pixels)
{
    assert_msg(r->screenshot.texture != nullptr, "set allow_screenshot to true when calling od_init()");
    r->screenshot.capture_image = true;
    r->screenshot.out_pixels = out_pixels;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_resize(struct onedraw* r, uint32_t width, uint32_t height)
{
    // see MAX_SCREEN_SIZE: the aabb buffer stores tile indices on 8 bits
    if (width > MAX_SCREEN_SIZE || height > MAX_SCREEN_SIZE)
        od_log(r, "warning: resolution clamped to %ux%u (max supported resolution)", MAX_SCREEN_SIZE, MAX_SCREEN_SIZE);
    width = min(width, MAX_SCREEN_SIZE);
    height = min(height, MAX_SCREEN_SIZE);

    od_log(r, "resizing the framebuffer to %dx%d", width, height);
    r->rasterizer.width = (uint16_t) width;
    r->rasterizer.height = (uint16_t) height;
    r->tiles.num_width = (uint16_t)((width + TILE_SIZE - 1) / TILE_SIZE);
    r->tiles.num_height = (uint16_t)((height + TILE_SIZE - 1) / TILE_SIZE);
    r->tiles.count = r->tiles.num_width * r->tiles.num_height;
    r->regions.num_width = (r->tiles.num_width + REGION_SIZE - 1) / REGION_SIZE;
    r->regions.num_height = (r->tiles.num_height + REGION_SIZE - 1) / REGION_SIZE;
    r->regions.count = r->regions.num_width * r->regions.num_height;

    SAFE_RELEASE(r->regions.indices);
    SAFE_RELEASE(r->regions.predicate);
    SAFE_RELEASE(r->regions.scan);

    size_t num_indices = r->regions.count * MAX_COMMANDS;
    r->regions.indices = r->device->newBuffer(num_indices * sizeof(uint16_t), MTL::ResourceStorageModePrivate);
    r->regions.predicate = r->device->newBuffer(num_indices * sizeof(uint8_t), MTL::ResourceStorageModePrivate);
    r->regions.scan = r->device->newBuffer(num_indices * sizeof(uint16_t), MTL::ResourceStorageModePrivate);

    SAFE_RELEASE(r->tiles.head);
    SAFE_RELEASE(r->tiles.indices);
    r->tiles.head = r->device->newBuffer(r->tiles.count * sizeof(uint32_t), MTL::ResourceStorageModePrivate);
    r->tiles.indices = r->device->newBuffer(r->tiles.num_width * r->tiles.num_height * sizeof(uint16_t), MTL::ResourceStorageModePrivate);

    od_log(r, "%ux%u tiles", r->tiles.num_width, r->tiles.num_height);
    od_log(r, "%ux%u regions", r->regions.num_width, r->regions.num_height);

    od_init_screenshot_resources(r);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_begin_frame(struct onedraw* r)
{
    assert_msg(r->commands.group_aabb == nullptr, "previous frame was not ended properly with od_end_frame");
    r->stats.frame_index++;
    r->commands.buffer.Map(r->stats.frame_index);
    r->commands.colors.Map(r->stats.frame_index);
    r->commands.draw_aabb = r->commands.aabb_buffer.Map(r->stats.frame_index);
    r->commands.data_buffer.Map(r->stats.frame_index);
    r->commands.clipshapes_buffer.Map(r->stats.frame_index);
    od_set_cliprect(r, 0, 0, (uint16_t) r->rasterizer.width, (uint16_t) r->rasterizer.height);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_end_frame(struct onedraw* r, void* drawable)
{
    assert_msg(r->commands.group_aabb == nullptr, "you need to call od_end_group, before od_end_frame");
    if (r->screenshot.show_region)
    {
        aabb capture_region = {.min = {(float)r->screenshot.region_x, (float)r->screenshot.region_y}};
        capture_region.max = vec2_add(capture_region.min, vec2_set(r->screenshot.region_width, r->screenshot.region_height));
        od_draw_box(r, capture_region.min.x, capture_region.min.y, capture_region.max.x, capture_region.max.y, 0.f, 0x802020ff);
    }

    r->commands.count = (uint32_t)r->commands.buffer.GetNumElements();
    r->stats.peak_num_draw_cmd = max(r->stats.peak_num_draw_cmd, r->commands.count);
    r->stats.num_draw_data = (uint32_t)r->commands.data_buffer.GetNumElements();
    r->stats.accumulated_gpu_time += atomic_load(&r->stats.gpu_time);
    if (r->stats.frame_index%60 == 0)
    {
        r->stats.average_gpu_time = r->stats.accumulated_gpu_time / 60.f;
        r->stats.accumulated_gpu_time = 0;
    }
    r->regions.num_groups = (r->commands.count + SIMD_GROUP_SIZE - 1) / SIMD_GROUP_SIZE;

    od_flush(r, drawable);
}

//----------------------------------------------------------------------------------------------------------------------------
float od_get_average_gputime(struct onedraw* r)
{
    return r->stats.average_gpu_time;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_terminate(struct onedraw* r)
{
    r->commands.buffer.Terminate();
    r->commands.colors.Terminate();
    r->commands.data_buffer.Terminate();
    r->commands.aabb_buffer.Terminate();
    r->commands.draw_arg.Terminate();
    r->commands.bin_output_arg.Terminate();
    r->commands.clipshapes_buffer.Terminate();
    SAFE_RELEASE(r->tiles.counters_buffer);
    SAFE_RELEASE(r->tiles.binning_pso);
    SAFE_RELEASE(r->tiles.head);
    SAFE_RELEASE(r->tiles.nodes);
    SAFE_RELEASE(r->tiles.indices);
    SAFE_RELEASE(r->tiles.indirect_arg);
    SAFE_RELEASE(r->tiles.indirect_cb);
    SAFE_RELEASE(r->regions.predicate_pso);
    SAFE_RELEASE(r->regions.exclusive_scan_pso);
    SAFE_RELEASE(r->regions.indices);
    SAFE_RELEASE(r->regions.predicate);
    SAFE_RELEASE(r->regions.scan);
    SAFE_RELEASE(r->tiles.write_icb_pso);
    SAFE_RELEASE(r->rasterizer.pso);
    SAFE_RELEASE(r->rasterizer.depth_stencil_state);
    SAFE_RELEASE(r->rasterizer.atlas);
    SAFE_RELEASE(r->command_queue);
    SAFE_RELEASE(r->font.texture);
    SAFE_RELEASE(r->font.glyphs);
    SAFE_RELEASE(r->screenshot.texture);
    if (r->semaphore != nullptr)
    {
        dispatch_release(r->semaphore);
        r->semaphore = nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void od_get_stats(struct onedraw* r, od_stats* stats)
{
    stats->frame_index = r->stats.frame_index;
    stats->num_draw_cmd = r->commands.count;
    stats->peak_num_draw_cmd = r->stats.peak_num_draw_cmd;
    stats->gpu_time_ms = r->stats.average_gpu_time * 1000.f;
    size_t gpu_mem = r->commands.aabb_buffer.GetTotalSize();
    gpu_mem += r->commands.bin_output_arg.GetTotalSize();
    gpu_mem += r->commands.buffer.GetTotalSize();
    gpu_mem += r->commands.clipshapes_buffer.GetTotalSize();
    gpu_mem += r->commands.colors.GetTotalSize();
    gpu_mem += r->commands.data_buffer.GetTotalSize();
    gpu_mem += r->commands.draw_arg.GetTotalSize();
    gpu_mem += r->font.texture->allocatedSize();
    gpu_mem += r->font.glyphs->allocatedSize();
    gpu_mem += r->rasterizer.atlas->allocatedSize();
    gpu_mem += r->regions.indices->allocatedSize();
    gpu_mem += r->regions.predicate->allocatedSize();
    gpu_mem += r->regions.scan->allocatedSize();
    gpu_mem += (r->screenshot.texture != nullptr) ? r->screenshot.texture->allocatedSize() : 0;
    gpu_mem += r->tiles.counters_buffer->allocatedSize();
    gpu_mem += r->tiles.head->allocatedSize();
    gpu_mem += r->tiles.indices->allocatedSize();
    gpu_mem += r->tiles.indirect_arg->allocatedSize();
    gpu_mem += r->tiles.nodes->allocatedSize();
    stats->gpu_memory_usage = gpu_mem;
}

//----------------------------------------------------------------------------------------------------------------------------
// check that every buffer used by a draw command has enough space before reserving anything,
// so the callers can call NewElement/NewMultiple without checking the result
static inline bool has_space_for_draw_command(struct onedraw* r, uint32_t num_data)
{
    return r->commands.buffer.GetNumElements() < r->commands.buffer.GetMaxElements()
        && r->commands.colors.GetNumElements() < r->commands.colors.GetMaxElements()
        && r->commands.data_buffer.GetNumElements() + num_data <= r->commands.data_buffer.GetMaxElements()
        && r->commands.aabb_buffer.GetNumElements() < r->commands.aabb_buffer.GetMaxElements();
}

//----------------------------------------------------------------------------------------------------------------------------
void od_begin_group(struct onedraw* r, bool smoothblend, float group_smoothness, float outline_width)
{
    assert_msg(r->commands.group_aabb == nullptr, "cannot call a second time od_begin_group without closing the previous group");
    assert_msg(group_smoothness >= 0.f, "smoothness cannot be negative");

    if (!smoothblend)
        group_smoothness = 0.f;

    if (!has_space_for_draw_command(r, 2))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    enum sdf_operator op = smoothblend ? op_blend : op_overwrite;

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->type = begin_group;
    cmd->fillmode = fill_solid;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->extra = (uint8_t)op;
    *color = draw_color();    // placeholder, the rasterizer reads the color of every command (the begin_group sdf branch ignores it)

    write_float(r->commands.data_buffer.NewMultiple(2), group_smoothness + outline_width, outline_width);

    // keep values for the end group command
    r->rasterizer.outline_width = outline_width;
    r->rasterizer.group_smoothness = group_smoothness;
    r->rasterizer.group_op = op;

    // reserve a aabb that we're going to update depending on the coming shapes
    r->commands.group_aabb = r->commands.aabb_buffer.NewElement();
    *r->commands.group_aabb = invalid_quantized_aabb();
}

//----------------------------------------------------------------------------------------------------------------------------
void od_end_group(struct onedraw* r, draw_color outline_color)
{
    assert(r->commands.group_aabb != nullptr);

    if (!has_space_for_draw_command(r, 1))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->type = end_group;
    cmd->fillmode = (r->rasterizer.outline_width > 0.f) ? fill_outline : fill_solid;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->extra = (uint8_t) r->rasterizer.group_op;
    *color = outline_color;

    // we put also the smooth value as we traverse the list in reverse order on the gpu
    *r->commands.data_buffer.NewElement() = r->rasterizer.group_smoothness + r->rasterizer.outline_width;

    quantized_aabb* aabb = r->commands.aabb_buffer.NewElement();
    *aabb = *r->commands.group_aabb;

    r->commands.group_aabb = nullptr;
    r->rasterizer.group_smoothness = 0.f;
    r->rasterizer.group_op = op_overwrite;
    r->rasterizer.outline_width = 0.f;
}

//----------------------------------------------------------------------------------------------------------------------------
static inline float draw_cmd_aabb_bump(struct onedraw* r)
{
    float result = r->rasterizer.aa_width + r->rasterizer.outline_width;
    if (r->rasterizer.group_op == op_blend)
        result += r->rasterizer.group_smoothness;
    return result;
}

//----------------------------------------------------------------------------------------------------------------------------
void private_draw_disc(struct onedraw* r, vec2 center, float radius, float thickness, enum primitive_fillmode fillmode,
                       draw_color primary_color, draw_color secondary_color)
{
    thickness *= .5f;

    uint32_t num_data = (fillmode == fill_hollow || fillmode == fill_gradient) ? 4 : 3;
    if (!has_space_for_draw_command(r, num_data))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fillmode;
    cmd->type = primitive_disc;
    *color = primary_color;

    float* data = r->commands.data_buffer.NewMultiple(num_data);
    quantized_aabb* aabb = r->commands.aabb_buffer.NewElement();
    float max_radius = radius + draw_cmd_aabb_bump(r);

    if (fillmode == fill_hollow)
    {
        max_radius += thickness;
        write_float(data, center.x, center.y, radius, thickness);
    }
    else if (fillmode == fill_gradient)
        write_float(data, center.x, center.y, radius, bitcast_u32_to_float(secondary_color));
    else
        write_float(data, center.x, center.y, radius);

    write_quantized_aabb(aabb, center.x - max_radius, center.y - max_radius, center.x + max_radius, center.y + max_radius);
    merge_quantized_aabb(r->commands.group_aabb, aabb);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_ring(struct onedraw* r, float cx, float cy, float radius, float thickness, draw_color color)
{
    private_draw_disc(r, vec2_set(cx, cy), radius, thickness, fill_hollow, color, 0);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_disc(struct onedraw* r, float cx, float cy, float radius, draw_color color)
{
    private_draw_disc(r, vec2_set(cx, cy), radius, 0.f, fill_solid, color, 0);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_disc_gradient(struct onedraw* r, float cx, float cy, float radius, draw_color outter_color, draw_color inner_color)
{
    private_draw_disc(r, vec2_set(cx, cy), radius, 0.f, fill_gradient, outter_color, inner_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void private_draw_oriented_box(struct onedraw* r, vec2 p0, vec2 p1, float width, float roundness, float thickness,
                              enum primitive_fillmode fillmode, draw_color primary_color, draw_color secondary_color)
{
    if (vec2_similar(p0, p1, HALF_PIXEL))
        return;

    thickness *= .5f;

    uint32_t num_data = (fillmode == fill_gradient) ? 7 : 6;
    if (!has_space_for_draw_command(r, num_data))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fillmode;
    cmd->type = primitive_oriented_box;
    *color = primary_color;

    float* data = r->commands.data_buffer.NewMultiple(num_data);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    float roundness_thickness = (fillmode == fill_hollow) ? thickness : roundness;
    aabb bb = aabb_from_rounded_obb(p0, p1, width, roundness_thickness + draw_cmd_aabb_bump(r));

    if (fillmode == fill_gradient)
        write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness, bitcast_u32_to_float(secondary_color));
    else
        write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness);

    write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_oriented_box(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness, draw_color color)
{
    private_draw_oriented_box(r, vec2_set(ax, ay), vec2_set(bx, by), width, roundness, 0.f, fill_solid, color, 0);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_oriented_rect(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness, float thickness, draw_color color)
{
    private_draw_oriented_box(r, vec2_set(ax, ay), vec2_set(bx, by), width, roundness, thickness, fill_hollow, color, 0);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_line(struct onedraw* r, float ax, float ay, float bx, float by, float width, draw_color srgb_color)
{
    private_draw_oriented_box(r, vec2_set(ax, ay), vec2_set(bx, by), width, 0.f, 0.f, fill_solid, srgb_color, 0);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_capsule(struct onedraw* r, float ax, float ay, float bx, float by, float radius, draw_color srgb_color)
{
    // capsule uses a specific sdf (see rasterizer shader) more efficient that oriented box
    private_draw_oriented_box(r, vec2_set(ax, ay), vec2_set(bx, by), 0.f, radius, 0.f, fill_solid, srgb_color, 0);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_capsule_gradient(struct onedraw* r, float ax, float ay, float bx, float by, float radius, draw_color primary_color, draw_color secondary_color)
{
    private_draw_oriented_box(r, vec2_set(ax, ay), vec2_set(bx, by), 0.f, radius, 0.f, fill_gradient, primary_color, secondary_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void private_draw_ellipse(struct onedraw* r, vec2 p0, vec2 p1, float width, float thickness, enum primitive_fillmode fillmode, draw_color srgb_color)
{
    if (vec2_similar(p0, p1, HALF_PIXEL))
        return;

    if (width <= HALF_PIXEL)
    {
        private_draw_oriented_box(r, p0, p1, 0.f, 0.f, 0.f, fill_solid, srgb_color, 0);
        return;
    }

    thickness = float_max(thickness * .5f, 0.f);

    uint32_t num_data = (fillmode == fill_hollow) ? 6 : 5;
    if (!has_space_for_draw_command(r, num_data))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fillmode;
    cmd->type = primitive_ellipse;
    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(num_data);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    aabb bb = aabb_from_rounded_obb(p0, p1, width, draw_cmd_aabb_bump(r) + thickness);
    if (fillmode == fill_hollow)
        write_float(data, p0.x, p0.y, p1.x, p1.y, width, thickness);
    else
        write_float(data, p0.x, p0.y, p1.x, p1.y, width);

    write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_ellipse(struct onedraw* r, float ax, float ay, float bx, float by, float width, draw_color srgb_color)
{
    private_draw_ellipse(r, vec2_set(ax, ay), vec2_set(bx, by), width, 0.f, fill_solid, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_ellipse_ring(struct onedraw* r, float ax, float ay, float bx, float by, float width, float thickness, draw_color srgb_color)
{
    private_draw_ellipse(r, vec2_set(ax, ay), vec2_set(bx, by), width, thickness, fill_hollow, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void private_draw_triangle(struct onedraw* r, const vec2* v, float roundness, float thickness, enum primitive_fillmode fillmode, draw_color srgb_color)
{
    // exclude invalid triangle
    if (vec2_similar(v[0], v[1], HALF_PIXEL) || vec2_similar(v[2], v[1], HALF_PIXEL) ||
        vec2_similar(v[0], v[2], HALF_PIXEL)) return;

    thickness *= .5f;

    if (!has_space_for_draw_command(r, 7))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fillmode;
    cmd->type = primitive_triangle;
    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(7);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    float roundness_thickness = (fillmode != fill_hollow) ? roundness : thickness;
    aabb bb = aabb_from_triangle(v[0], v[1], v[2]);
    aabb_grow(&bb, vec2_splat(roundness_thickness + draw_cmd_aabb_bump(r)));
    write_float(data, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, roundness_thickness);
    write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_triangle(struct onedraw* r, const float* vertices, float roundness, draw_color srgb_color)
{
    private_draw_triangle(r, (const vec2*) vertices, roundness, 0.f, fill_solid, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_triangle_ring(struct onedraw* r, const float* vertices, float roundness, float thickness, draw_color srgb_color)
{
    private_draw_triangle(r, (const vec2*) vertices, roundness, thickness, fill_hollow, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void private_draw_pie(struct onedraw* r, vec2 center, vec2 direction, float radius, float aperture, float thickness, enum primitive_fillmode fillmode, draw_color srgb_color)
{
    if (aperture <= FLT_EPSILON)
        return;
    
    aperture = float_clamp(aperture, 0.f, VEC2_PI);
    thickness = float_max(thickness * .5f, 0.f);

    uint32_t num_data = (fillmode != fill_hollow) ? 7 : 8;
    if (!has_space_for_draw_command(r, num_data))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fillmode;
    cmd->type = primitive_pie;
    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(num_data);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    aabb bb = aabb_from_circle(center, radius);
    aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r)));

    if (fillmode != fill_hollow)
        write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture));
    else
        write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
        
    write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_sector(struct onedraw* r, float cx, float cy, float radius, float start_angle, float sweep_angle, draw_color srgb_color)
{
    vec2 center = {cx, cy};
    float aperture = sweep_angle * .5f;
    vec2 direction = vec2_direction(start_angle + aperture);
    private_draw_pie(r, center, direction, radius, fabs(aperture), 0.f, fill_solid, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_sector_ring(struct onedraw* r, float cx, float cy, float radius, float start_angle, float sweep_angle, float thickness, draw_color srgb_color)
{
    vec2 center = {cx, cy};
    float aperture = sweep_angle * .5f;
    vec2 direction = vec2_direction(start_angle + aperture);
    private_draw_pie(r, center, direction, radius, fabs(aperture), thickness, fill_hollow, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_arc(struct onedraw* r, float cx, float cy, float dx, float dy, float aperture, float radius, float thickness, draw_color srgb_color)
{
    vec2 center = {cx, cy};
    vec2 direction = {dx, dy};

    aperture = float_clamp(aperture, 0.f, VEC2_PI);
    thickness = float_max(thickness, 0.f);

    if (!has_space_for_draw_command(r, 8))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fill_solid;
    cmd->type = primitive_arc;

    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(8);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    aabb bb = aabb_from_circle(center, radius);
    aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r)));

    write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
    write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_box(struct onedraw* r, float x0, float y0, float x1, float y1, float radius, draw_color srgb_color)
{
    if (x0>x1) swap(x0, x1);
    if (y0>y1) swap(y0, y1);

    aabb box = {.min = {x0, y0}, .max = {x1, y1}};

    if (!has_space_for_draw_command(r, 5))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fill_solid;
    cmd->type = primitive_aabox;

    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(5);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    vec2 center = vec2_scale(vec2_add(box.min, box.max), .5f);
    vec2 half_extents = vec2_scale(vec2_sub(box.max, box.min), .5f);
    aabb_grow(&box, vec2_splat(draw_cmd_aabb_bump(r)));
    write_float(data, center.x, center.y, half_extents.x, half_extents.y, radius);
    write_quantized_aabb(aabox, box.min.x, box.min.y, box.max.x, box.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_blurred_box(struct onedraw* r, float cx, float cy, float width, float height, float roundness, draw_color srgb_color)
{
    float half_width = width * .5f;
    float half_height = height * .5f;

    if (!has_space_for_draw_command(r, 5))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->type = primitive_blurred_box;
    cmd->fillmode = fill_solid;

    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(5);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    write_float(data, cx, cy, half_width, half_height, roundness);
    write_quantized_aabb(aabox, cx - half_width - roundness, cy - half_height - roundness,
                         cx + half_width + roundness, cy + half_height + roundness);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_char(struct onedraw* r, float x, float y, char c, draw_color srgb_color)
{
    if (c < r->font.desc.first_glyph || c >= (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        return;

    uint32_t glyph_index = c - r->font.desc.first_glyph;

    if (!has_space_for_draw_command(r, 2))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fill_solid;
    cmd->type = primitive_char;
    cmd->extra = (uint8_t) glyph_index;

    *color = srgb_color;

    const float scale = r->font.scale;
    const glyph& glyph = r->font.desc.glyphs[glyph_index];
    x += glyph.bearing_x * scale;
    y += (glyph.bearing_y + r->font.desc.font_height) * scale;
    float glyph_width = float(glyph.x1 - glyph.x0) * scale;
    float glyph_height = float(glyph.y1 - glyph.y0) * scale;

    float* data = r->commands.data_buffer.NewMultiple(2);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    write_float(data, x, y);
    write_quantized_aabb(aabox, x, y, x + glyph_width, y + glyph_height);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_text(struct onedraw* r, float x, float y, const char* text, draw_color srgb_color)
{
    float left = x;
    const float scale = r->font.scale;
    for(const char *c = text; *c != 0; c++)
    {
        if (*c == '\n')
        {
            y += r->font.desc.font_height * scale;
            x = left;
        }
        else if (*c >= r->font.desc.first_glyph && *c < (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        {
            od_draw_char(r, x, y, *c, srgb_color);
            uint32_t glyph_index = *c - r->font.desc.first_glyph;
            x += r->font.desc.glyphs[glyph_index].advance_x * scale;
        }
        else
            x += r->font.desc.glyphs['_'- r->font.desc.first_glyph].advance_x * .65f * scale;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_quad(struct onedraw* r, float x0, float y0, float x1, float y1, od_quad_uv uv, uint32_t slice_index, draw_color srgb_color)
{
    if (r->rasterizer.atlas == nullptr)
    {
        od_log(r, "can't draw a quad, atlas is not allocated (set atlas width/height in od_init)");
        return;
    }
    assert_msg(slice_index < r->rasterizer.atlas->arrayLength(), "slice index out of bound");

    // keep the corners coherent with the uv
    if (x0 > x1)
    {
        swap(x0, x1);
        swap(uv.u0, uv.u1);
    }
    if (y0 > y1)
    {
        swap(y0, y1);
        swap(uv.v0, uv.v1);
    }

    if (fabsf(x0 - x1) < HALF_PIXEL || fabsf(y0 - y1) < HALF_PIXEL)
        return;

    if (!has_space_for_draw_command(r, 8))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fill_solid;
    cmd->type = primitive_quad;
    cmd->extra = (uint8_t) slice_index;

    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(8);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    write_float(data, x0, y0, x1, y1, uv.u0, uv.v0, uv.u1, uv.v1);
    write_quantized_aabb(aabox, x0, y0, x1, y1);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_oriented_quad(struct onedraw* r, float cx, float cy, float width, float height, float angle, od_quad_uv uv, uint32_t slice_index, draw_color srgb_color)
{
    if (r->rasterizer.atlas == nullptr)
    {
        od_log(r, "can't draw an oriented quad, atlas is not allocated (set atlas width/height in od_init)");
        return;
    }
    assert_msg(slice_index < r->rasterizer.atlas->arrayLength(), "slice index out of bound");

    if (width < HALF_PIXEL || height < HALF_PIXEL)
        return;

    vec2 center = {cx, cy};
    vec2 axis = vec2_direction(angle);
    vec2 dir = vec2_scale(axis, width*.5f);
    vec2 p0 = vec2_sub(center, dir);
    vec2 p1 = vec2_add(center, dir);

    if (!has_space_for_draw_command(r, 10))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    cmd->clip_index = LAST_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fill_solid;
    cmd->type = primitive_oriented_quad;
    cmd->extra = (uint8_t) slice_index;

    *color = srgb_color;

    float* data = r->commands.data_buffer.NewMultiple(10);
    quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
    write_float(data, cx, cy, 1.f/width, 1.f/height, axis.x, axis.y, uv.u0, uv.v0, uv.u1, uv.v1);
    aabb bb = aabb_from_rounded_obb(p0, p1, height, 0.f);
    write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
    merge_quantized_aabb(r->commands.group_aabb, aabox);
}

//----------------------------------------------------------------------------------------------------------------------------
// Breaks the bezier quadratic curve into multiple capsules, using De Casteljau’s algorithm and colinear detection
uint32_t od_draw_quadratic_bezier(struct onedraw* r, const float* control_points, float width, draw_color srgb_color)
{
    quadratic_bezier stack[TESSELATION_STACK_MAX];
    uint32_t stack_index = 0;

    const float radius = width * .5f;
    uint32_t num_capsules = 0;

    stack[stack_index++] = 
    {
        .c0 = {control_points[0], control_points[1]},
        .c1 = {control_points[2], control_points[3]},
        .c2 = {control_points[4], control_points[5]},
    };

    while (stack_index != 0)
    {
        quadratic_bezier c = stack[--stack_index];

        // splits proportionally to segment lengths
        float d0 = vec2_distance(c.c0, c.c1);
        float d1 = vec2_distance(c.c1, c.c2);
        float split = d0 / (d0 + d1);

        vec2 left = vec2_lerp( c.c0, c.c1, split);
        vec2 right = vec2_lerp(c.c1, c.c2, split);
        vec2 middle = vec2_lerp(left, right, split);
        
        if (is_colinear(c.c0, c.c2, middle, COLINEAR_THRESHOLD))
        {
            od_draw_capsule(r, c.c0.x, c.c0.y, c.c2.x, c.c2.y, radius, srgb_color);
            num_capsules++;
        }
        else
        {
            if (stack_index + 2 <= TESSELATION_STACK_MAX)
            {
                stack[stack_index++] =
                {
                    .c0 = c.c0,
                    .c1 = left,
                    .c2 = middle,
                };

                stack[stack_index++] =
                {
                    .c0 = middle,
                    .c1 = right,
                    .c2 = c.c2,
                };
            }
            else
                return UINT32_MAX;
        }
    }
    return num_capsules;
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_cubic_bezier(struct onedraw* r, const float* control_points, float width, draw_color srgb_color)
{
    cubic_bezier stack[TESSELATION_STACK_MAX];
    uint32_t stack_index = 0;

    const float radius = width * .5f;
    uint32_t num_capsules = 0;

    stack[stack_index++] = 
    {
        .c0 = {control_points[0], control_points[1]},
        .c1 = {control_points[2], control_points[3]},
        .c2 = {control_points[4], control_points[5]},
        .c3 = {control_points[6], control_points[7]}
    };

    while (stack_index != 0)
    {
        cubic_bezier c = stack[--stack_index];

        // the halfway point along the control polygon roughly corresponds to halfway along the curve arc length
        float d0 = vec2_distance(c.c0, c.c1);
        float d1 = vec2_distance(c.c1, c.c2);
        float d2 = vec2_distance(c.c2, c.c3);
        float total = d0 + d1 + d2;
        float split = (d0 + 0.5f * d1) / total;

        vec2 c01 = vec2_lerp(c.c0, c.c1, split);
        vec2 c12 = vec2_lerp(c.c1, c.c2, split);
        vec2 c23 = vec2_lerp(c.c2, c.c3, split);
        vec2 c01c12 = vec2_lerp(c01, c12, split);
        vec2 c12c23 = vec2_lerp(c12, c23, split);
        vec2 middle = vec2_lerp(c01c12, c12c23, split);

        if (is_colinear(c.c0, c.c3, middle, COLINEAR_THRESHOLD))
        {
            // the geometric endpoints of a cubic are c0 and c3 (c2 is a control point)
            od_draw_capsule(r, c.c0.x, c.c0.y, c.c3.x, c.c3.y, radius, srgb_color);
            num_capsules++;
        }
        else
        {
            if (stack_index + 2 <= TESSELATION_STACK_MAX)
            {
                stack[stack_index++] =
                {
                    .c0 = c.c0,
                    .c1 = c01,
                    .c2 = c01c12,
                    .c3 = middle
                };

                stack[stack_index++] =
                {
                    .c0 = middle,
                    .c1 = c12c23,
                    .c2 = c23,
                    .c3 = c.c3,
                };
            }
            else
                return UINT32_MAX;
        }
    }

    return num_capsules;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Dashed shapes
//
// The dash pattern is cut on the CPU into small primitives that are emitted along the path, so the shaders don't need
// dashed shape support : each dash becomes a capsule (round ends) or a line (flat ends), both use the exact oriented
// box culling. Dashes are placed by arc length along a polyline approximation of the path :
//  - a line is the segment itself
//  - a triangle ring is the 3 edges
//  - a circle/ellipse is sampled in DASH_SAMPLES steps (an ellipse has no closed form arc length)
//  - a bezier reuses the solid curve De Casteljau tesselation, but the leaves come out in curve order
//
// A closed path rescales the pattern so it closes without a stub at the seam, and the walk starts at a pattern
// boundary, so no dash is split by the seam.
// ---------------------------------------------------------------------------------------------------------------------------

constexpr float FULL_TURN = VEC2_PI * 2.f;
constexpr uint32_t DASH_SAMPLES = 512U;    // circle/ellipse sampling, the chord error stays < 0.1px on a 4096px screen
constexpr uint32_t DASH_CORNER_SAMPLES = DASH_SAMPLES / 4U;    // box corner sampling, the same density per swept angle
constexpr float DASH_EPSILON = 0.01f;      // distance below which a "zero length" dash is not emitted

enum dash_path_kind
{
    path_line,
    path_triangle,
    path_circle,
    path_ellipse,
    path_quadratic,
    path_cubic,
    path_box
};

typedef struct dash_path
{
    enum dash_path_kind kind;

    bool  closed;
    float perimeter;                    // total path length
    vec2  point;                        // current position on the path
    vec2  start;                        // walk start position (a closed path walks back to it for a full lap)
    uint32_t remaining;                 // segments left to emit (ignored by the beziers, gated on the tesselation stack)
    uint32_t index;                     // triangle : current edge, circle/ellipse : current sample
    bool  failed;                       // tesselation stack overflow
    bool  turns_after;                 // the path turns a corner at the current [point], a dash spanning it is split in two

    vec2  a, b;                         // line
    vec2  vertices[3];                  // triangle
    vec2  center;                       // circle, ellipse
    float radius;                       // circle
    float angle;                        // circle : current angle
    vec2  semi_axes;                    // ellipse : (semi major, semi minor)
    vec2  axis;                         // ellipse : major axis direction, normalized
    float cumulative[DASH_SAMPLES];     // ellipse : cumulative length of the samples
    vec2  control[4];                   // bezier control points (root of the tesselation stack)
    union
    {
        quadratic_bezier quad[TESSELATION_STACK_MAX];
        cubic_bezier     cubic[TESSELATION_STACK_MAX];
    };
    uint32_t stack_index;                  // bezier : tesselation stack depth, box : samples consumed in the current corner
} dash_path;

//----------------------------------------------------------------------------------------------------------------------------
static inline vec2 dash_ellipse_point(dash_path* p, float t)
{
    vec2 local = vec2_set(p->semi_axes.x * cosf(t), p->semi_axes.y * sinf(t));
    return vec2_add(p->center, vec2_add(vec2_scale(p->axis, local.x), vec2_scale(vec2_skew(p->axis), local.y)));
}

//----------------------------------------------------------------------------------------------------------------------------
// The box lives in its local frame : x along [axis], y along the skew, relative to [center], so the same path
// serves the axis-aligned box (axis = (1, 0)) and the oriented box. [a]/[b] are the local min/max corners.
static inline vec2 dash_box_point(dash_path* p, vec2 local)
{
    return vec2_add(p->center, vec2_add(vec2_scale(p->axis, local.x), vec2_scale(vec2_skew(p->axis), local.y)));
}

//----------------------------------------------------------------------------------------------------------------------------
// The box is 8 pieces : 0 top edge, 1 top-right corner, 2 right edge, 3 bottom-right corner, 4 bottom edge,
// 5 bottom-left corner, 6 left edge, 7 top-left corner
static inline float dash_box_piece_length(dash_path* p, uint32_t piece)
{
    float w = p->b.x - p->a.x, h = p->b.y - p->a.y;
    return (piece & 1) ? (VEC2_PI * .5f * p->radius) : (((piece & 2) == 0) ? (w - 2.f * p->radius) : (h - 2.f * p->radius));
}

static inline vec2 dash_box_edge_point(dash_path* p, uint32_t edge, bool end)
{
    float x0 = p->a.x, y0 = p->a.y, x1 = p->b.x, y1 = p->b.y, r = p->radius;

    switch (edge)    // 0 top, 1 right, 2 bottom, 3 left
    {
    case 0:
        return end ? vec2_set(x1 - r, y0) : vec2_set(x0 + r, y0);
    case 1:
        return end ? vec2_set(x1, y1 - r) : vec2_set(x1, y0 + r);
    case 2:
        return end ? vec2_set(x0 + r, y1) : vec2_set(x1 - r, y1);
    default:
        return end ? vec2_set(x0, y0 + r) : vec2_set(x0, y1 - r);
    }
}

static inline vec2 dash_box_corner_center(dash_path* p, uint32_t corner)
{
    float x0 = p->a.x, y0 = p->a.y, x1 = p->b.x, y1 = p->b.y, r = p->radius;

    switch (corner)    // 0 top-right, 1 bottom-right, 2 bottom-left, 3 top-left
    {
    case 0:
        return vec2_set(x1 - r, y0 + r);
    case 1:
        return vec2_set(x1 - r, y1 - r);
    case 2:
        return vec2_set(x0 + r, y1 - r);
    default:
        return vec2_set(x0 + r, y0 + r);
    }
}

// A corner sweeps a quarter turn, corner 0 starts at -90 degrees (top edge end) and the sweep is clockwise
static inline float dash_box_corner_angle(uint32_t corner, bool end)
{
    return -VEC2_PI * .5f + (corner + (end ? 1U : 0U)) * VEC2_PI * .5f;
}

//----------------------------------------------------------------------------------------------------------------------------
// Fills the cumulative sample lengths, the ellipse has no closed form arc length
static void dash_path_init_ellipse(dash_path* p)
{
    p->cumulative[0] = 0.f;

    float length = 0.f;
    vec2 previous = dash_ellipse_point(p, 0.f);
    for (uint32_t sample = 1; sample < DASH_SAMPLES; sample++)
    {
        vec2 current = dash_ellipse_point(p, sample * FULL_TURN / (float)DASH_SAMPLES);
        length += vec2_distance(previous, current);
        p->cumulative[sample] = length;
        previous = current;
    }
    p->perimeter = length + vec2_distance(previous, dash_ellipse_point(p, 0.f));
}

//----------------------------------------------------------------------------------------------------------------------------
// Places the path cursor at an arc length position, a closed path then walks a full lap back to the start
static void dash_path_start_at(dash_path* p, float arc)
{
    switch (p->kind)
    {
    case path_line:
    {
        p->point = p->a;
        p->start = p->a;
        p->index = 0;
        p->remaining = 1;
        break;
    }

    case path_triangle:
    {
        float u = arc;
        uint32_t edge = 2;
        for (uint32_t i = 0; i < 3; i++)
        {
            float edge_length = vec2_distance(p->vertices[i], p->vertices[(i + 1) % 3]);
            if (u < edge_length || i == 2)
            {
                edge = i;
                break;
            }
            u -= edge_length;
        }

        vec2 to = p->vertices[(edge + 1) % 3];
        p->start = (u <= 0.f) ? p->vertices[edge] : vec2_lerp(p->vertices[edge], to, u / vec2_distance(p->vertices[edge], to));
        p->point = p->start;
        p->index = edge;
        p->remaining = 4;    // one partial edge + 2 full edges + the final partial edge back to the start
        break;
    }

    case path_circle:
    {
        p->angle = arc / p->radius;
        p->start = vec2_add(p->center, vec2_scale(vec2_direction(p->angle), p->radius));
        p->point = p->start;
        p->index = 0;
        p->remaining = DASH_SAMPLES + 1;    // the final "partial" segment degenerates to the seam drift
        break;
    }

    case path_ellipse:
    {
        uint32_t sample = 0;
        while (sample + 1 < DASH_SAMPLES && p->cumulative[sample + 1] <= arc)
            sample++;

        float cum_from = (sample == 0) ? 0.f : p->cumulative[sample - 1];
        float cum_to = (sample + 1 == DASH_SAMPLES) ? p->perimeter : p->cumulative[sample + 1];
        float segment_length = cum_to - cum_from;

        vec2 from = dash_ellipse_point(p, sample * FULL_TURN / (float)DASH_SAMPLES);
        vec2 to = dash_ellipse_point(p, ((sample + 1) % DASH_SAMPLES) * FULL_TURN / (float)DASH_SAMPLES);
        p->start = (segment_length > 0.f) ? vec2_lerp(from, to, (arc - cum_from) / segment_length) : to;
        p->point = p->start;
        p->index = sample;
        p->remaining = DASH_SAMPLES + 1;    // one partial sample + full samples + the final partial sample back to the start
        break;
    }

    case path_box:
    {
        float u = arc;
        uint32_t piece = 7;
        for (uint32_t i = 0; i < 8; i++)
        {
            float piece_length = dash_box_piece_length(p, i);
            if (u < piece_length || i == 7)
            {
                piece = i;
                break;
            }
            u -= piece_length;
        }

        p->index = piece;
        if ((piece & 1) == 0)    // edge
        {
            float piece_length = dash_box_piece_length(p, piece);
            vec2 from = dash_box_edge_point(p, piece / 2, false);
            vec2 local = (u <= 0.f || piece_length <= 0.f) ? from : vec2_lerp(from, dash_box_edge_point(p, piece / 2, true), u / piece_length);
            p->start = dash_box_point(p, local);
            p->point = p->start;
            p->stack_index = 0;
            // arm the corner that follows this edge : its first sample increments from the corner start angle,
            // so a walk that starts on the edge is correct no matter what [angle] held before the start
            p->angle = dash_box_corner_angle(piece / 2, false);
        }
        else    // corner
        {
            float piece_length = dash_box_piece_length(p, piece);
            float t = (u <= 0.f || piece_length <= 0.f) ? 0.f : u / piece_length;
            if (t >= 1.f)
                t = 1.f - 1.f / (float)DASH_CORNER_SAMPLES;    // the last sample is left for the walk

            uint32_t corner = piece / 2;
            p->angle = dash_box_corner_angle(corner, false) + t * VEC2_PI * .5f;
            vec2 local = vec2_add(dash_box_corner_center(p, corner), vec2_scale(vec2_direction(p->angle), p->radius));
            p->start = dash_box_point(p, local);
            p->point = p->start;
            p->stack_index = (uint32_t)(t * (float)DASH_CORNER_SAMPLES);
        }

        p->remaining = 4U + 4U * DASH_CORNER_SAMPLES + 1U - p->stack_index;
        break;
    }

    case path_quadratic:
    case path_cubic:
    {
        // an open path always starts at its first control point
        p->point = p->control[0];
        p->start = p->point;
        p->index = 0;
        p->remaining = 0;

        p->stack_index = 0;
        if (p->kind == path_quadratic)
            p->quad[p->stack_index++] = (quadratic_bezier) { .c0 = p->control[0], .c1 = p->control[1], .c2 = p->control[2] };
        else
            p->cubic[p->stack_index++] = (cubic_bezier) { .c0 = p->control[0], .c1 = p->control[1], .c2 = p->control[2], .c3 = p->control[3] };
        break;
    }
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// Returns the next segment of the path (in curve order), returns false when the path is exhausted or the tesselation stack overflowed
static bool dash_path_next_segment(dash_path* p, vec2* p0, vec2* p1, float* length)
{
    *p0 = p->point;

    // the beziers don't use [remaining] (their tesselation stack gates the walk), every other kind walks exactly
    // [remaining] segments
    if (p->kind != path_quadratic && p->kind != path_cubic)
    {
        if (p->remaining == 0)
            return false;
        p->remaining--;
    }

    // a closed path ends with a final segment back to its start, so the walk covers exactly one lap
    const bool final_segment = p->closed && (p->remaining == 1);

    switch (p->kind)
    {
    case path_line:
    {
        *p1 = p->b;
        p->turns_after = false;
        break;
    }

    case path_triangle:
    {
        *p1 = final_segment ? p->start : p->vertices[(p->index + 1) % 3];
        p->index = (p->index + 1) % 3;
        p->turns_after = true;    // each edge ends at a vertex
        break;
    }

    case path_circle:
    {
        if (final_segment)
            *p1 = p->start;
        else
        {
            p->angle += FULL_TURN / (float)DASH_SAMPLES;
            *p1 = vec2_add(p->center, vec2_scale(vec2_direction(p->angle), p->radius));
        }
        p->turns_after = false;    // smooth arc
        break;
    }

    case path_ellipse:
    {
        uint32_t next = (p->index + 1) % DASH_SAMPLES;
        *p1 = final_segment ? p->start : dash_ellipse_point(p, next * FULL_TURN / (float)DASH_SAMPLES);
        p->index = next;
        p->turns_after = false;    // smooth arc
        break;
    }

    case path_box:
    {
        if (final_segment)
        {
            *p1 = p->start;
            p->turns_after = true;    // a box corner is just a vertex when the radius is 0
            break;
        }

        if ((p->index & 1) == 0)    // edge : a single chord to the piece end
        {
            *p1 = dash_box_point(p, dash_box_edge_point(p, p->index / 2, true));
            p->index = (p->index + 1) & 7;
            p->turns_after = true;    // every piece end is a path corner, a zero length edge still turns 90 degrees there
        }
        else    // corner : one sampled chord, the last one lands exactly on the corner end
        {
            uint32_t corner = p->index / 2;
            p->stack_index++;
            if (p->stack_index >= DASH_CORNER_SAMPLES)
            {
                p->angle = dash_box_corner_angle(corner, true);
                p->stack_index = 0;
                p->index = (p->index + 1) & 7;
            }
            else
                p->angle += VEC2_PI * .5f / (float)DASH_CORNER_SAMPLES;

            *p1 = dash_box_point(p, vec2_add(dash_box_corner_center(p, corner), vec2_scale(vec2_direction(p->angle), p->radius)));
            p->turns_after = true;    // every sample : a dash must follow the corner arc, never cut it with a chord
        }
        break;
    }

    case path_quadratic:
    {
        // re-runs the solid curve De Casteljau tesselation, but the halves are popped in reverse order so the segments
        // come out in curve order, required to place the dashes along the path
        p->turns_after = false;    // a smooth curve
        bool leaf = false;
        while (p->stack_index > 0 && !leaf)
        {
            quadratic_bezier c = p->quad[--p->stack_index];

            // splits proportionally to segment lengths
            float d0 = vec2_distance(c.c0, c.c1);
            float d1 = vec2_distance(c.c1, c.c2);
            float split = d0 / (d0 + d1);

            vec2 left = vec2_lerp(c.c0, c.c1, split);
            vec2 right = vec2_lerp(c.c1, c.c2, split);
            vec2 middle = vec2_lerp(left, right, split);

            if (is_colinear(c.c0, c.c2, middle, COLINEAR_THRESHOLD))
            {
                *p1 = c.c2;
                leaf = true;
            }
            else
            {
                if (p->stack_index + 2 > TESSELATION_STACK_MAX)
                {
                    p->failed = true;
                    return false;
                }

                // push the right half first so the left half is popped first
                p->quad[p->stack_index++] = (quadratic_bezier) { .c0 = middle, .c1 = right, .c2 = c.c2 };
                p->quad[p->stack_index++] = (quadratic_bezier) { .c0 = c.c0, .c1 = left, .c2 = middle };
            }
        }
        if (!leaf)
            return false;    // the curve is fully tesselated
        break;
    }

    case path_cubic:
    {
        p->turns_after = false;    // a smooth curve
        bool leaf = false;
        while (p->stack_index > 0 && !leaf)
        {
            cubic_bezier c = p->cubic[--p->stack_index];

            // the halfway point along the control polygon roughly corresponds to halfway along the curve arc length
            float d0 = vec2_distance(c.c0, c.c1);
            float d1 = vec2_distance(c.c1, c.c2);
            float d2 = vec2_distance(c.c2, c.c3);
            float total = d0 + d1 + d2;
            float split = (d0 + 0.5f * d1) / total;

            vec2 c01 = vec2_lerp(c.c0, c.c1, split);
            vec2 c12 = vec2_lerp(c.c1, c.c2, split);
            vec2 c23 = vec2_lerp(c.c2, c.c3, split);
            vec2 c01c12 = vec2_lerp(c01, c12, split);
            vec2 c12c23 = vec2_lerp(c12, c23, split);
            vec2 middle = vec2_lerp(c01c12, c12c23, split);

            if (is_colinear(c.c0, c.c3, middle, COLINEAR_THRESHOLD))
            {
                *p1 = c.c3;
                leaf = true;
            }
            else
            {
                if (p->stack_index + 2 > TESSELATION_STACK_MAX)
                {
                    p->failed = true;
                    return false;
                }

                // push the right half first so the left half is popped first
                p->cubic[p->stack_index++] = (cubic_bezier) { .c0 = middle, .c1 = c12c23, .c2 = c23, .c3 = c.c3 };
                p->cubic[p->stack_index++] = (cubic_bezier) { .c0 = c.c0, .c1 = c01, .c2 = c01c12, .c3 = middle };
            }
        }
        if (!leaf)
            return false;    // the curve is fully tesselated
        break;
    }
    }

    *length = vec2_distance(*p0, *p1);
    p->point = *p1;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
typedef struct dash_emitter
{
    struct onedraw* r;
    bool  closed;
    bool  failed;                       // the command budget ran out (shouldn't happen, it's checked up front)
    bool  dry_run;                      // count the primitives without emitting them (budget check)
    od_dash_style style;
    float dash_length;                  // the pattern is rescaled for closed paths
    float period;
    float phase;                        // pattern position at [point]
    bool  in_dash;                      // [point] is inside a dash
    vec2  point;                        // current position on the path
    vec2  dash_start;                   // path position where the current dash started
    float width;
    draw_color color;
    uint32_t num_commands;
} dash_emitter;

//----------------------------------------------------------------------------------------------------------------------------
static bool has_space_for_dash_commands(struct onedraw* r, uint32_t num_commands, uint32_t num_data)
{
    return r->commands.buffer.GetMaxElements() - r->commands.buffer.GetNumElements() >= num_commands
        && r->commands.colors.GetMaxElements() - r->commands.colors.GetNumElements() >= num_commands
        && r->commands.data_buffer.GetMaxElements() - r->commands.data_buffer.GetNumElements() >= num_data
        && r->commands.aabb_buffer.GetMaxElements() - r->commands.aabb_buffer.GetNumElements() >= num_commands;
}

//----------------------------------------------------------------------------------------------------------------------------
static bool dash_solid_segment(struct onedraw* r, vec2 a, vec2 b, float width, bool round_caps, draw_color color)
{
    // one command : a capsule for round ends, a line for flat ends (both use the exact oriented box culling)
    if (!has_space_for_dash_commands(r, 1, 6))
        return false;

    if (round_caps)
        od_draw_capsule(r, a.x, a.y, b.x, b.y, width * .5f, color);
    else
        od_draw_line(r, a.x, a.y, b.x, b.y, width, color);
    return true;
}

static void dash_emitter_emit(dash_emitter* e)
{
    if (e->dry_run)
    {
        e->num_commands++;
        return;
    }

    if (!dash_solid_segment(e->r, e->dash_start, e->point, e->width, e->style.round_caps, e->color))
        e->failed = true;
    else
        e->num_commands++;
}

//----------------------------------------------------------------------------------------------------------------------------
// Emits the dash that just ended, or opens a new one, [point] is at a pattern boundary
static void dash_emitter_boundary(dash_emitter* e)
{
    if (e->in_dash)
    {
        dash_emitter_emit(e);
        e->in_dash = false;
        e->phase = e->dash_length;
    }
    else
    {
        e->in_dash = true;
        e->dash_start = e->point;
        e->phase = 0.f;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// The path leaves the dash's straight line at the current point (a box corner, a triangle vertex) : a dash in
// progress is emitted up to that point, its remainder starts there and continues along the new direction, so a
// dash is never drawn as a straight chord cutting across a corner. [in_dash]/[phase] are preserved, the pattern
// rhythm is unchanged : the dash becomes a chain of primitives sharing the split points (round caps give a
// smooth join, flat caps a sharp one).
//
// The corner samples are sub half pixel chords, and the oriented box primitive silently rejects endpoints closer
// than half a pixel : a dash piece therefore accumulates across the samples and is only emitted once it is long
// enough to actually be drawn (the same half pixel guard), so the corner renders as a smooth arc of short
// pieces instead of a blank corner (and a zero radius corner emits at most one piece per dash)
static void dash_emitter_kink(dash_emitter* e)
{
    if (e->in_dash && e->phase < e->dash_length && !vec2_similar(e->dash_start, e->point, HALF_PIXEL))
    {
        dash_emitter_emit(e);
        e->dash_start = e->point;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// Walks one path segment, emitting the dashes it contains, the dash/gap phase carries across segments. When the
// path turns a corner after the segment, a dash in progress is split at the corner (dash_emitter_kink), the
// split also applies to zero length segments (a sharp box corner is a zero length 90 degree turn)
static void dash_emitter_segment(dash_emitter* e, vec2 p0, vec2 p1, float length, bool turns_after)
{
    if (e->failed)
        return;

    e->point = p0;
    if (length <= 0.f)
    {
        if (turns_after)
            dash_emitter_kink(e);
        return;
    }

    vec2 dir = vec2_scale(vec2_sub(p1, p0), 1.f / length);
    float u = 0.f;

    while (u < length)
    {
        float to_boundary = e->in_dash ? (e->dash_length - e->phase) : (e->period - e->phase);
        if (to_boundary <= 0.f)
        {
            // on a pattern boundary (segment start, or a floating point drift)
            dash_emitter_boundary(e);
            if (u >= length)
                break;
            continue;
        }

        float remaining = length - u;
        float step = (to_boundary < remaining) ? to_boundary : remaining;
        float new_u = u + step;
        if (new_u <= u)
            new_u = length;    // a pattern smaller than a float ulp at this scale, avoid an infinite loop

        u = new_u;
        e->phase += step;
        e->point = vec2_add(p0, vec2_scale(dir, u));

        if (step == to_boundary)
            dash_emitter_boundary(e);
    }

    e->point = p1;
    if (turns_after)
        dash_emitter_kink(e);
}

//----------------------------------------------------------------------------------------------------------------------------
// Closes the dash still open at the end of the path
static void dash_emitter_end(dash_emitter* e)
{
    if (!e->in_dash)
        return;

    // a closed path pattern closes exactly at the seam : a dash that (re)starts there is the first dash, already
    // emitted, only a floating point drift can leave a degenerate dash behind
    if (e->closed && vec2_similar(e->dash_start, e->point, DASH_EPSILON))
        return;

    dash_emitter_emit(e);
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_emitter_init(dash_emitter* e, struct onedraw* r, bool closed, vec2 start, od_dash_style style, float period, float width,
                              draw_color color)
{
    e->r = r;
    e->closed = closed;
    e->failed = false;
    e->dry_run = false;
    e->style = style;
    e->dash_length = style.dash_length;
    e->period = period;
    e->point = start;
    e->dash_start = start;
    e->width = width;
    e->color = color;
    e->num_commands = 0;

    if (closed)
    {
        // the walk starts at a pattern boundary (the offset was applied to the walk origin)
        e->in_dash = true;
        e->phase = 0.f;
    }
    else
    {
        e->phase = fmodf(style.offset, period);
        if (e->phase < 0.f)
            e->phase += period;
        e->in_dash = (e->phase < e->dash_length);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// A zero gap means the whole path is one dash : draw the solid shape
static uint32_t dash_path_solid(struct onedraw* r, dash_path* path, float width, od_dash_style style, draw_color color)
{
    switch (path->kind)
    {
    case path_line:
        if (style.round_caps)
            od_draw_capsule(r, path->a.x, path->a.y, path->b.x, path->b.y, width * .5f, color);
        else
            od_draw_line(r, path->a.x, path->a.y, path->b.x, path->b.y, width, color);
        return 1;

    case path_triangle:
        od_draw_triangle_ring(r, (const float*) path->vertices, 0.f, width, color);
        return 1;

    case path_circle:
        od_draw_ring(r, path->center.x, path->center.y, path->radius, width, color);
        return 1;

    case path_ellipse:
    {
        vec2 p0 = vec2_sub(path->center, vec2_scale(path->axis, path->semi_axes.x));
        vec2 p1 = vec2_add(path->center, vec2_scale(path->axis, path->semi_axes.x));
        od_draw_ellipse_ring(r, p0.x, p0.y, p1.x, p1.y, path->semi_axes.y * 2.f, width, color);
        return 1;
    }

    case path_box:
    {
        // there is no solid rounded box outline primitive : walk the 8 pieces (4 edges + 4 sampled corner arcs)
        // emitting a line/capsule per piece, wrapped in a group when translucent so the shared endpoints aren't
        // double blended. Degenerate pieces are skipped (the oriented box primitive would reject them anyway),
        // so a zero radius corner costs nothing and a sharp box is its 4 edges only
        bool rounded = path->radius > 0.f;
        uint32_t num = 4U + (rounded ? 4U * DASH_CORNER_SAMPLES : 0U);
        bool wrap_group = (((color >> 24) & 0xFF) < 0xFF) && (r->commands.group_aabb == nullptr);

        if (!has_space_for_dash_commands(r, num + (wrap_group ? 2U : 0U), num * 6U + (wrap_group ? 3U : 0U)))
            return UINT32_MAX;

        if (wrap_group)
            od_begin_group(r, false, 0.f, 0.f);

        uint32_t emitted = 0;
        vec2 a, b;
        float segment_length;
        dash_path_start_at(path, 0.f);
        while (dash_path_next_segment(path, &a, &b, &segment_length))
        {
            if (vec2_similar(a, b, HALF_PIXEL))
                continue;    // degenerate, the primitive would reject it

            if (!dash_solid_segment(r, a, b, width, style.round_caps, color))
            {
                if (wrap_group)
                    od_end_group(r, 0);
                return UINT32_MAX;
            }
            emitted++;
        }

        if (wrap_group)
            od_end_group(r, 0);
        return emitted;
    }

    case path_quadratic:
        return od_draw_quadratic_bezier(r, (const float*) path->control, width, color);

    case path_cubic:
        return od_draw_cubic_bezier(r, (const float*) path->control, width, color);
    }
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------------
// Walks the tesselation to measure the path length, the beziers have no closed form length
static float dash_path_measure(dash_path* p)
{
    vec2 a, b;
    float length = 0.f;
    float segment_length;
    while (dash_path_next_segment(p, &a, &b, &segment_length))
        length += segment_length;
    return length;
}

//----------------------------------------------------------------------------------------------------------------------------
// Walks the whole path through the emitter (the caller must have placed the path cursor with dash_path_start_at),
// in dry_run mode the primitives are only counted
static void dash_path_walk(dash_path* path, dash_emitter* e)
{
    vec2 a, b;
    float length;
    while (dash_path_next_segment(path, &a, &b, &length))
        dash_emitter_segment(e, a, b, length, path->turns_after);

    dash_emitter_end(e);
}

//----------------------------------------------------------------------------------------------------------------------------
// Emits the dash pattern along the path, returns the number of commands emitted or UINT32_MAX on failure
static uint32_t dash_path_draw(struct onedraw* r, dash_path* path, float width, od_dash_style style, draw_color color)
{
    if (width <= 0.f)
        return 0;

    if (style.gap_length <= 0.f)
        return dash_path_solid(r, path, width, style, color);

    if (style.dash_length <= 0.f)
        return 0;

    float period = style.dash_length + style.gap_length;
    float walk_start = 0.f;

    if (path->closed)
    {
        // rescale the pattern so it closes without a stub at the seam
        float num = floorf(path->perimeter / period + 0.5f);
        if (num < 1.f)
            return 0;    // the pattern doesn't fit even once

        style.dash_length *= path->perimeter / (num * period);
        period = path->perimeter / num;

        // walk from a pattern boundary so no dash is split by the seam
        walk_start = fmodf(-style.offset, period);
        if (walk_start < 0.f)
            walk_start += period;
    }

    // a translucent color is wrapped in a group, so the overlapping dash ends aren't double blended
    // (a caller already in a group composites everything in its group, no wrap needed)
    bool wrap_group = (((color >> 24) & 0xFF) < 0xFF) && (r->commands.group_aabb == nullptr);

    // a dash uses one command, one color, 6 data floats and one aabb, but a dash that spans a box corner is split
    // into a capsule per corner sample : count the exact number of primitives with a dry run walk, check the
    // capacity up front, then re-walk to emit, so the function never emits a partial shape
    dash_path_start_at(path, walk_start);

    dash_emitter e;
    dash_emitter_init(&e, r, path->closed, path->point, style, period, width, color);
    e.dry_run = true;
    dash_path_walk(path, &e);

    if (path->failed)
        return UINT32_MAX;    // the tesselation stack overflowed, nothing was emitted

    uint32_t num_commands = e.num_commands;
    if (!has_space_for_dash_commands(r, num_commands + (wrap_group ? 2U : 0U), num_commands * 6U + (wrap_group ? 3U : 0U)))
        return UINT32_MAX;

    if (wrap_group)
        od_begin_group(r, false, 0.f, 0.f);

    // the real emission, the same deterministic walk
    dash_path_start_at(path, walk_start);
    dash_emitter_init(&e, r, path->closed, path->point, style, period, width, color);
    dash_path_walk(path, &e);

    if (wrap_group)
        od_end_group(r, 0);

    if (path->failed || e.failed)
        return UINT32_MAX;

    return e.num_commands;
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_line(struct onedraw* r, float ax, float ay, float bx, float by, float width, od_dash_style style, draw_color srgb_color)
{
    dash_path path = {};
    path.kind = path_line;
    path.a = vec2_set(ax, ay);
    path.b = vec2_set(bx, by);
    path.perimeter = vec2_distance(path.a, path.b);

    if (path.perimeter <= HALF_PIXEL)
        return 0;

    return dash_path_draw(r, &path, width, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_ring(struct onedraw* r, float cx, float cy, float radius, float thickness, od_dash_style style, draw_color srgb_color)
{
    if (radius <= 0.f)
        return 0;

    dash_path path = {};
    path.kind = path_circle;
    path.closed = true;
    path.center = vec2_set(cx, cy);
    path.radius = radius;
    path.perimeter = FULL_TURN * radius;

    return dash_path_draw(r, &path, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_box(struct onedraw* r, float x0, float y0, float x1, float y1, float radius, float thickness,
                            od_dash_style style, draw_color srgb_color)
{
    vec2 lo = vec2_set(fminf(x0, x1), fminf(y0, y1));
    vec2 hi = vec2_set(fmaxf(x0, x1), fmaxf(y0, y1));
    float w = hi.x - lo.x, h = hi.y - lo.y;

    if (w <= 0.f || h <= 0.f)
        return 0;

    dash_path path = {};
    path.kind = path_box;
    path.closed = true;
    vec2 center = vec2_scale(vec2_add(lo, hi), .5f);
    path.a = vec2_sub(lo, center);
    path.b = vec2_sub(hi, center);
    path.center = center;
    path.axis = vec2_set(1.f, 0.f);
    path.radius = fminf(radius, fminf(w, h) * .5f);
    path.perimeter = 2.f * (w + h) - 8.f * path.radius + FULL_TURN * path.radius;

    return dash_path_draw(r, &path, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_oriented_box(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness,
                                     float thickness, od_dash_style style, draw_color srgb_color)
{
    vec2 a = vec2_set(ax, ay), b = vec2_set(bx, by);
    float length = vec2_distance(a, b);

    if (length <= 0.f || width <= 0.f)
        return 0;

    dash_path path = {};
    path.kind = path_box;
    path.closed = true;
    vec2 center = vec2_scale(vec2_add(a, b), .5f);
    path.a = vec2_set(-length * .5f, -width * .5f);
    path.b = vec2_set(length * .5f, width * .5f);
    path.center = center;
    path.axis = vec2_scale(vec2_sub(b, a), 1.f / length);
    path.radius = fminf(roundness, fminf(length, width) * .5f);
    path.perimeter = 2.f * (length + width) - 8.f * path.radius + FULL_TURN * path.radius;

    return dash_path_draw(r, &path, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_ellipse_ring(struct onedraw* r, float ax, float ay, float bx, float by, float width, float thickness,
                                     od_dash_style style, draw_color srgb_color)
{
    vec2 p0 = vec2_set(ax, ay);
    vec2 p1 = vec2_set(bx, by);

    float semi_major = vec2_distance(p0, p1) * .5f;
    if (semi_major <= HALF_PIXEL || width <= 0.f)
        return 0;

    dash_path path = {};
    path.kind = path_ellipse;
    path.closed = true;
    path.center = vec2_scale(vec2_add(p0, p1), .5f);
    path.semi_axes = vec2_set(semi_major, width * .5f);

    // a needle like ellipse is a line segment along its main axis
    if (path.semi_axes.y < 0.01f)
    {
        path.kind = path_line;
        path.closed = false;
        path.a = p0;
        path.b = p1;
        path.perimeter = vec2_distance(p0, p1);
        return dash_path_draw(r, &path, thickness, style, srgb_color);
    }

    path.axis = vec2_scale(vec2_sub(p1, p0), 1.f / (2.f * semi_major));
    dash_path_init_ellipse(&path);

    return dash_path_draw(r, &path, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_triangle_ring(struct onedraw* r, const float* vertices, float thickness, od_dash_style style, draw_color srgb_color)
{
    vec2 v[3];
    v[0] = vec2_set(vertices[0], vertices[1]);
    v[1] = vec2_set(vertices[2], vertices[3]);
    v[2] = vec2_set(vertices[4], vertices[5]);

    // exclude an invalid triangle, same guard as od_draw_triangle_ring
    if (vec2_similar(v[0], v[1], HALF_PIXEL) || vec2_similar(v[2], v[1], HALF_PIXEL) || vec2_similar(v[0], v[2], HALF_PIXEL))
        return 0;

    dash_path path = {};
    path.kind = path_triangle;
    path.closed = true;
    path.vertices[0] = v[0];
    path.vertices[1] = v[1];
    path.vertices[2] = v[2];
    path.perimeter = vec2_distance(v[0], v[1]) + vec2_distance(v[1], v[2]) + vec2_distance(v[2], v[0]);

    return dash_path_draw(r, &path, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_quadratic_bezier(struct onedraw* r, const float* control_points, float width, od_dash_style style, draw_color srgb_color)
{
    dash_path path = {};
    path.kind = path_quadratic;
    path.control[0] = vec2_set(control_points[0], control_points[1]);
    path.control[1] = vec2_set(control_points[2], control_points[3]);
    path.control[2] = vec2_set(control_points[4], control_points[5]);

    // the path length requires the tesselation, run it once to measure it before checking the budget
    dash_path_start_at(&path, 0.f);
    path.perimeter = dash_path_measure(&path);

    if (path.failed)
        return UINT32_MAX;    // nothing was emitted

    return dash_path_draw(r, &path, width, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_cubic_bezier(struct onedraw* r, const float* control_points, float width, od_dash_style style, draw_color srgb_color)
{
    dash_path path = {};
    path.kind = path_cubic;
    path.control[0] = vec2_set(control_points[0], control_points[1]);
    path.control[1] = vec2_set(control_points[2], control_points[3]);
    path.control[2] = vec2_set(control_points[4], control_points[5]);
    path.control[3] = vec2_set(control_points[6], control_points[7]);

    // the path length requires the tesselation, run it once to measure it before checking the budget
    dash_path_start_at(&path, 0.f);
    path.perimeter = dash_path_measure(&path);

    if (path.failed)
        return UINT32_MAX;    // nothing was emitted

    return dash_path_draw(r, &path, width, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
float od_text_height(struct onedraw* r)
{
    return r->font.desc.font_height * r->font.scale;
}

//----------------------------------------------------------------------------------------------------------------------------
float od_text_width(struct onedraw* r, const char* text)
{
    float width = 0.f;
    const float scale = r->font.scale;
    for(const char *c = text; *c != 0; c++)
    {
        if (*c >= r->font.desc.first_glyph && *c < (r->font.desc.first_glyph + r->font.desc.num_glyphs))
            width += r->font.desc.glyphs[*c - r->font.desc.first_glyph].advance_x * scale;
        else
            width += r->font.desc.glyphs['_'- r->font.desc.first_glyph].advance_x * .65f * scale;
    }
    return width;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_font_scale(struct onedraw* r, float scale)
{
    if (r->font.scale != scale)
    {
        r->font.scale = scale;
        od_upload_font_glyphs(r);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_clear_color(struct onedraw* r, draw_color srgb_color)
{
    float r8 = (float)(srgb_color & 0xFF) / 255.f;
    float g8 = (float)((srgb_color >> 8) & 0xFF) / 255.f;
    float b8 = (float)((srgb_color >> 16) & 0xFF) / 255.f;
    float a8 = (float)((srgb_color >> 24) & 0xFF) / 255.f;

    if (r->rasterizer.srgb_backbuffer)
    {
        r->rasterizer.clear_color.x = srgb_to_linear(r8);
        r->rasterizer.clear_color.y = srgb_to_linear(g8);
        r->rasterizer.clear_color.z = srgb_to_linear(b8);
        r->rasterizer.clear_color.w = a8;
    }
    else
    {
        r->rasterizer.clear_color.x = r8;
        r->rasterizer.clear_color.y = g8;
        r->rasterizer.clear_color.z = b8;
        r->rasterizer.clear_color.w = a8;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static void push_clip_shape(struct onedraw* r, clip_shape clip)
{
    if (r->commands.clipshapes_buffer.GetNumElements() < MAX_CLIPS)
        *r->commands.clipshapes_buffer.NewElement() = clip;
    else
        od_log(r, "too many clip shapes! maximum is %d", MAX_CLIPS);
}

//----------------------------------------------------------------------------------------------------------------------------
static void set_cliprect(struct onedraw* r, float min_x, float min_y, float max_x, float max_y, float radius)
{
    if (max_x < min_x) {float t = min_x; min_x = max_x; max_x = t;}
    if (max_y < min_y) {float t = min_y; min_y = max_y; max_y = t;}

    float half_x = (max_x - min_x) * 0.5f;
    float half_y = (max_y - min_y) * 0.5f;

    // clamp the radius to the half of the smaller side, a larger radius would leave the shape undefined
    radius = min(min(radius, half_x), half_y);

    clip_shape clip =
    {
        .rect =
        {
            .center_x = min_x + half_x,
            .center_y = min_y + half_y,
            .half_x = half_x,
            .half_y = half_y,
            .radius = radius
        },
        .type = clip_rect
    };

    // avoid redundant clip rect
    if (r->commands.clipshapes_buffer.GetNumElements()>0)
    {
        clip_shape* last = r->commands.clipshapes_buffer.LastElement();

        if (last->type == clip_rect && last->rect.center_x == clip.rect.center_x && last->rect.center_y == clip.rect.center_y &&
            last->rect.half_x == clip.rect.half_x && last->rect.half_y == clip.rect.half_y && last->rect.radius == clip.rect.radius)
            return;
    }

    push_clip_shape(r, clip);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_cliprect(struct onedraw* r, float min_x, float min_y, float max_x, float max_y)
{
    set_cliprect(r, min_x, min_y, max_x, max_y, 0.f);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_cliprect_rounded(struct onedraw* r, float min_x, float min_y, float max_x, float max_y, float radius)
{
    set_cliprect(r, min_x, min_y, max_x, max_y, radius);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_clipdisc(struct onedraw* r, float cx, float cy, float radius)
{
    clip_shape clip =
    {
        .disc = {.center_x = cx, .center_y = cy, .radius = radius},
        .type = clip_disc
    };

    // avoid redundant clip disc
    if (r->commands.clipshapes_buffer.GetNumElements()>0)
    {
        clip_shape* last = r->commands.clipshapes_buffer.LastElement();

        if (last->type == clip_disc && last->disc.center_x == clip.disc.center_x &&
            last->disc.center_y == clip.disc.center_y && last->disc.radius == clip.disc.radius)
            return;
    }

    push_clip_shape(r, clip);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_culling_debug(struct onedraw* r, bool b)
{
    r->tiles.culling_debug = b;
}

