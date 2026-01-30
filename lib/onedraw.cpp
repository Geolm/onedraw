#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"

#include "onedraw.h"
#include "common.h"
#include "default_font.h"
#include "default_font_atlas.h"
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
        if (m_NumElements + count < m_MaxElements)
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

struct alphabet
{
    od_glyph glyphs[MAX_GLYPHS];
    float font_height;
    uint16_t num_glyphs;
    uint16_t first_glyph;
    uint16_t texture_width;
    uint16_t texture_height;
};

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
    r->font.texture->replaceRegion( MTL::Region(
        0, 0, 0,
        r->font.desc.texture_width, r->font.desc.texture_height, 1 ),
        0, default_font_atlas, (r->font.desc.texture_width/4) * 8);
    pTextureDesc->release();

    // fill the glyph description to be upload on the gpu
    font_char cpu_buffer[MAX_GLYPHS];
    for(uint32_t i=0; i<r->font.desc.num_glyphs; i++)
    {
        const od_glyph& glyph = r->font.desc.glyphs[i];
        cpu_buffer[i] = 
        {
            .width = float(glyph.x1 - glyph.x0),
            .height = float(glyph.y1 - glyph.y0),
            .uv_topleft = {.x = float(glyph.x0) / float(r->font.desc.texture_width), 
                           .y = float(glyph.y0) / float(r->font.desc.texture_height)},
            .uv_bottomright = {.x = float(glyph.x1) / float(r->font.desc.texture_width),
                               .y = float(glyph.y1) / float(r->font.desc.texture_height)}
        };
    }
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

    uint32_t threads_per_region = (r->commands.count + args->num_elements_per_thread - 1) / args->num_elements_per_thread;

    compute_encoder->setComputePipelineState(r->regions.exclusive_scan_pso);
    compute_encoder->setBuffer(r->regions.scan, 0, 2);
    compute_encoder->setThreadgroupMemoryLength(simd_group_count * sizeof(uint16_t), 0);
    compute_encoder->setThreadgroupMemoryLength(simd_group_count * sizeof(uint16_t), 1);
    compute_encoder->dispatchThreads(MTL::Size(threads_per_region, r->regions.count, 1), MTL::Size(min(threads_per_region, (uint32_t)MAX_THREADS_PER_THREADGROUP), 1, 1));

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
    assert_msg(def->preallocated_buffer != nullptr, "forgot to allocate memory?");
    assert_msg(((uintptr_t)def->preallocated_buffer)%sizeof(uintptr_t) == 0, "preallocated_buffer must be aligned on sizeof(uintptr_t)");
    assert(def->metal_device != nullptr);
    assert_msg(((MTL::Device*)def->metal_device)->supportsFamily(MTL::GPUFamilyApple7), "onedraw supports only M1/A14 GPU and later");

    onedraw* r = new(def->preallocated_buffer) onedraw;

    r->custom_log = def->log_func;
    r->device = (MTL::Device*)def->metal_device;
    r->command_queue = r->device->newCommandQueue();
    r->screenshot.allocate_resources = def->allow_screenshot;
    r->rasterizer.srgb_backbuffer = def->srgb_backbuffer;

    if (r->command_queue == nullptr)
    {
        od_log(r, "can't create a command queue");
        exit(EXIT_FAILURE);
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

    assert(sizeof(alphabet) == default_font_size);
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
    assert_msg(x<=r->rasterizer.width && width<=r->rasterizer.width && y<=r->rasterizer.height && height<=r->rasterizer.height,
               "capture region cannot be bigger than the rendertarget");
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
}

//----------------------------------------------------------------------------------------------------------------------------
void od_get_stats(struct onedraw* r, od_stats* stats)
{
    stats->frame_index = r->stats.frame_index;
    stats->num_draw_cmd = r->commands.count, r->commands.buffer.GetMaxElements();
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
void od_begin_group(struct onedraw* r, bool smoothblend, float group_smoothness, float outline_width)
{
    assert_msg(r->commands.group_aabb == nullptr, "cannot call a second time od_begin_group without closing the previous group");
    assert_msg(group_smoothness >= 0.f, "smoothness cannot be negative");

    if (!smoothblend)
        group_smoothness = 0.f;

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        enum sdf_operator op = smoothblend ? op_blend : op_overwrite;
        cmd->type = begin_group;
        cmd->fillmode = fill_solid;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->extra = (uint8_t)op;

        float* data = r->commands.data_buffer.NewMultiple(2);
        r->commands.group_aabb = r->commands.aabb_buffer.NewElement();
        
        if (r->commands.group_aabb != nullptr && data != nullptr)
        {
            write_float(data, group_smoothness + outline_width, outline_width);

            // keep values for the end group command
            r->rasterizer.outline_width = outline_width;
            r->rasterizer.group_smoothness = group_smoothness;
            r->rasterizer.group_op = op;

            // reserve a aabb that we're going to update depending on the coming shapes
            *r->commands.group_aabb = invalid_quantized_aabb();
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void od_end_group(struct onedraw* r, draw_color outline_color)
{
    assert(r->commands.group_aabb != nullptr);

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->type = end_group;
        cmd->fillmode = (r->rasterizer.outline_width > 0.f) ? fill_outline : fill_solid;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->extra = (uint8_t) r->rasterizer.group_op;
        *color = outline_color;

        // we put also the smooth value as we traverse the list in reverse order on the gpu
        float* k = r->commands.data_buffer.NewElement();

        quantized_aabb* aabb = r->commands.aabb_buffer.NewElement();
        if (aabb != nullptr && k != nullptr)
        {
            *aabb = *r->commands.group_aabb;
            *k = r->rasterizer.group_smoothness + r->rasterizer.outline_width;

            r->commands.group_aabb = nullptr;
            r->rasterizer.group_smoothness = 0.f;
            r->rasterizer.group_op = op_overwrite;
            r->rasterizer.outline_width = 0.f;

            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
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

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fillmode;
        cmd->type = primitive_disc;
        *color = primary_color;

        float* data = r->commands.data_buffer.NewMultiple((fillmode == fill_hollow || fillmode == fill_gradient) ? 4 : 3);
        quantized_aabb* aabb = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabb != nullptr)
        {
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
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
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

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fillmode;
        cmd->type = primitive_oriented_box;
        *color = primary_color;

        float* data = r->commands.data_buffer.NewMultiple((fillmode == fill_gradient) ? 7 : 6);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            float roundness_thickness = (fillmode == fill_hollow) ? thickness : roundness;
            aabb bb = aabb_from_rounded_obb(p0, p1, width, roundness_thickness + draw_cmd_aabb_bump(r));

            if (fillmode == fill_gradient)
                write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness, bitcast_u32_to_float(secondary_color));
            else
                write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness);

            write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
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
        private_draw_oriented_box(r, p0, p1, 0.f, 0.f, 0.f, fill_solid, srgb_color, 0);
    else
    {
        thickness = float_max(thickness * .5f, 0.f);
        draw_command* cmd = r->commands.buffer.NewElement();
        draw_color* color = r->commands.colors.NewElement();
        if (cmd != nullptr && color != nullptr)
        {
            cmd->clip_index = LAST_CLIP_INDEX;
            cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
            cmd->fillmode = fillmode;
            cmd->type = primitive_ellipse;
            *color = srgb_color;

            float* data = r->commands.data_buffer.NewMultiple((fillmode == fill_hollow) ? 6 : 5);
            quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
            if (data != nullptr && aabox != nullptr)
            {
                aabb bb = aabb_from_rounded_obb(p0, p1, width, draw_cmd_aabb_bump(r) + thickness);
                if (fillmode == fill_hollow)
                    write_float(data, p0.x, p0.y, p1.x, p1.y, width, thickness);
                else
                    write_float(data, p0.x, p0.y, p1.x, p1.y, width);

                write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
                merge_quantized_aabb(r->commands.group_aabb, aabox);
                return;
            }
            r->commands.buffer.RemoveLast();
            r->commands.colors.RemoveLast();
        }
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
    }
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

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fillmode;
        cmd->type = primitive_triangle;
        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple(7);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            float roundness_thickness = (fillmode != fill_hollow) ? roundness : thickness;
            aabb bb = aabb_from_triangle(v[0], v[1], v[2]);
            aabb_grow(&bb, vec2_splat(roundness_thickness + draw_cmd_aabb_bump(r)));
            write_float(data, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, roundness_thickness);
            write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
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

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fillmode;
        cmd->type = primitive_pie;
        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple((fillmode != fill_hollow) ? 7 : 8);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            aabb bb = aabb_from_circle(center, radius);
            aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r)));

            if (fillmode != fill_hollow)
                write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture));
            else
                write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
                
            write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
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

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fill_solid;
        cmd->type = primitive_arc;

        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple(8);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            aabb bb = aabb_from_circle(center, radius);
            aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r)));

            write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
            write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_box(struct onedraw* r, float x0, float y0, float x1, float y1, float radius, draw_color srgb_color)
{
    if (x0>x1) swap(x0, x1);
    if (y0>y1) swap(y0, y1);

    aabb box = {.min = {x0, y0}, .max = {x1, y1}};

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fill_solid;
        cmd->type = primitive_aabox;

        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple(5);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            vec2 center = vec2_scale(vec2_add(box.min, box.max), .5f);
            vec2 half_extents = vec2_scale(vec2_sub(box.max, box.min), .5f);
            aabb_grow(&box, vec2_splat(draw_cmd_aabb_bump(r)));
            write_float(data, center.x, center.y, half_extents.x, half_extents.y, radius);
            write_quantized_aabb(aabox, box.min.x, box.min.y, box.max.x, box.max.y);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_blurred_box(struct onedraw* r, float cx, float cy, float width, float height, float roundness, draw_color srgb_color)
{
    float half_width = width * .5f;
    float half_height = height * .5f;

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->type = primitive_blurred_box;
        cmd->fillmode = fill_solid;

        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple(5);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            write_float(data, cx, cy, half_width, half_height, roundness);
            write_quantized_aabb(aabox, cx - half_width - roundness, cy - half_height - roundness,
                                 cx + half_width + roundness, cy + half_height + roundness);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_char(struct onedraw* r, float x, float y, char c, draw_color srgb_color)
{
    if (c < r->font.desc.first_glyph || c > (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        return;

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        uint32_t glyph_index = c - r->font.desc.first_glyph;

        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fill_solid;
        cmd->type = primitive_char;
        cmd->extra = (uint8_t) glyph_index;

        *color = srgb_color;

        const od_glyph& glyph = r->font.desc.glyphs[glyph_index];
        x += glyph.bearing_x;
        y += glyph.bearing_y + r->font.desc.font_height;
        float glyph_width = float(glyph.x1 - glyph.x0);
        float glyph_height = float(glyph.y1 - glyph.y0);

        float* data = r->commands.data_buffer.NewMultiple(2);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            write_float(data, x, y);
            write_quantized_aabb(aabox, x, y, x + glyph_width, y + glyph_height);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_text(struct onedraw* r, float x, float y, const char* text, draw_color srgb_color)
{
    float left = x;
    for(const char *c = text; *c != 0; c++)
    {
        if (*c == '\n')
        {
            y += r->font.desc.font_height;
            x = left;
        }
        else if (*c >= r->font.desc.first_glyph && *c <= (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        {
            od_draw_char(r, x, y, *c, srgb_color);
            uint32_t glyph_index = *c - r->font.desc.first_glyph;
            x += r->font.desc.glyphs[glyph_index].advance_x;
        }
        else
            x += r->font.desc.glyphs['_'- r->font.desc.first_glyph].advance_x * .65f;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_quad(struct onedraw* r, float x0, float y0, float x1, float y1, od_quad_uv uv, uint32_t slice_index, draw_color srgb_color)
{
    assert_msg(slice_index < r->rasterizer.atlas->arrayLength(), "slice index out of bound");

    if (fabsf(x0 - x1) < HALF_PIXEL || fabsf(y0 - y1) < HALF_PIXEL)
        return;

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fill_solid;
        cmd->type = primitive_quad;
        cmd->extra = (uint8_t) slice_index;

        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple(8);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            write_float(data, x0, y0, x1, y1, uv.u0, uv.v0, uv.u1, uv.v1);
            write_quantized_aabb(aabox, x0, y0, x1, y1);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_oriented_quad(struct onedraw* r, float cx, float cy, float width, float height, float angle, od_quad_uv uv, uint32_t slice_index, draw_color srgb_color)
{
    assert_msg(slice_index < r->rasterizer.atlas->arrayLength(), "slice index out of bound");

    if (width < HALF_PIXEL || height < HALF_PIXEL)
        return;

    vec2 center = {cx, cy};
    vec2 axis = vec2_direction(angle);
    vec2 dir = vec2_scale(axis, width*.5f);
    vec2 p0 = vec2_sub(center, dir);
    vec2 p1 = vec2_add(center, dir);

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color = r->commands.colors.NewElement();
    if (cmd != nullptr && color != nullptr)
    {
        cmd->clip_index = LAST_CLIP_INDEX;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->fillmode = fill_solid;
        cmd->type = primitive_oriented_quad;
        cmd->extra = (uint8_t) slice_index;

        *color = srgb_color;

        float* data = r->commands.data_buffer.NewMultiple(10);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            write_float(data, cx, cy, 1.f/width, 1.f/height, axis.x, axis.y, uv.u0, uv.v0, uv.u1, uv.v1);
            aabb bb = aabb_from_rounded_obb(p0, p1, height, 0.f);
            write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_quantized_aabb(r->commands.group_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
        r->commands.colors.RemoveLast();
    }
    od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
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

//----------------------------------------------------------------------------------------------------------------------------
float od_text_height(struct onedraw* r)
{
    return r->font.desc.font_height;
}

//----------------------------------------------------------------------------------------------------------------------------
float od_text_width(struct onedraw* r, const char* text)
{
    float width = 0.f;
    for(const char *c = text; *c != 0; c++)
    {
        if (*c >= r->font.desc.first_glyph && *c <= (r->font.desc.first_glyph + r->font.desc.num_glyphs))
            width += r->font.desc.glyphs[*c - r->font.desc.first_glyph].advance_x;
        else
            width += r->font.desc.glyphs['_'- r->font.desc.first_glyph].advance_x * .65f;
    }
    return width;
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
void od_set_cliprect(struct onedraw* r, float min_x, float min_y, float max_x, float max_y)
{
    // avoid redundant clip rect
    if (r->commands.clipshapes_buffer.GetNumElements()>0)
    {
        clip_shape* clip = r->commands.clipshapes_buffer.LastElement();

        if (clip->rect.min_x == min_x && clip->rect.min_y == min_y &&
            clip->rect.max_x == max_x && clip->rect.max_y == max_y && clip->type == clip_rect)
            return;
    }

    if (r->commands.clipshapes_buffer.GetNumElements() < MAX_CLIPS)
    {
        *r->commands.clipshapes_buffer.NewElement() = (clip_shape) 
        {
            .rect = {.min_x = min_x, .min_y = min_y, .max_x = max_x, .max_y = max_y},
            .type = clip_rect
        };
    }
    else
        od_log(r, "too many clip shapes! maximum is %d", MAX_CLIPS);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_clipdisc(struct onedraw* r, float cx, float cy, float radius)
{
    if (r->commands.clipshapes_buffer.GetNumElements()>0)
    {
        clip_shape* clip = r->commands.clipshapes_buffer.LastElement();

        if (clip->type == clip_disc && clip->disc.center_x == cx && clip->disc.center_y == cy &&
            clip->disc.squared_radius == (radius * radius))
            return;
    }

    if (r->commands.clipshapes_buffer.GetNumElements() < MAX_CLIPS)
    {
        *r->commands.clipshapes_buffer.NewElement() = (clip_shape) 
        {
            .disc = {.center_x = cx, .center_y = cy, .squared_radius = radius * radius},
            .type = clip_disc
        };
    }
    else
        od_log(r, "too many clip shapes! maximum is %d", MAX_CLIPS);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_culling_debug(struct onedraw* r, bool b)
{
    r->tiles.culling_debug = b;
}

