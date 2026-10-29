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
#define CURRENT_CLIP_INDEX (r->commands.clip_stack[r->commands.clip_depth - 1])
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
constexpr uint32_t MAX_SCREEN_SIZE = (UINT8_MAX + 1U) * TILE_SIZE;   // 4096
constexpr uint32_t MAX_CLIP_DEPTH = 16U;
constexpr uint32_t STATS_AVERAGE_FRAMES = 60U;

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

    T* At(size_t index) {if (index < m_NumElements) return &m_pData[index]; return nullptr;}

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

struct gpu_timer
{
    _Atomic(float) last = 0.f;
    float accumulated {0.f};
    float average {0.f};
};

static void gpu_timer_end_frame(gpu_timer* t, uint32_t frame_index)
{
    t->accumulated += atomic_load(&t->last);
    if (frame_index % STATS_AVERAGE_FRAMES == 0)
    {
        t->average = t->accumulated / (float)STATS_AVERAGE_FRAMES;
        t->accumulated = 0.f;
    }
}

struct onedraw
{
    MTL::Device* device;
    MTL::CommandQueue* command_queue;
    MTL::CommandBuffer* command_buffer;
    dispatch_semaphore_t semaphore;
    MTL::CounterSampleBuffer* sample_buffer {nullptr};

    struct
    {
        DynamicBuffer<draw_cmd_arguments> draw_arg;
        DynamicBuffer<tiles_data> bin_output_arg;
        DynamicBuffer<draw_command> buffer;
        DynamicBuffer<draw_color> colors;
        DynamicBuffer<quantized_aabb> aabb_buffer;
        DynamicBuffer<float> data_buffer;
        DynamicBuffer<clip_shape> clipshapes_buffer;
        uint8_t clip_stack[MAX_CLIP_DEPTH] {0};
        uint32_t clip_depth {0};
        MTL::Buffer* command_hashes {nullptr};
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
        uint16_t num_width;
        uint16_t num_height;
        uint32_t count;
        MTL::Buffer* hashes {nullptr};
        MTL::Buffer* clean_indices {nullptr};
        MTL::ComputePipelineState* hash_pso {nullptr};
        uint32_t invalidate_generation {0};
        bool culling_debug {false};
    } tiles;

    // rasterizer
    struct
    {
        MTL::RenderPipelineState* pso {nullptr};
        MTL::RenderPipelineState* clean_pso {nullptr};
        MTL::DepthStencilState* depth_stencil_state {nullptr};
        MTL::Texture* atlas {nullptr};
        MTL::Texture* cache_texture {nullptr};
        MTL::Texture* cache_placeholder {nullptr};
        uint32_t atlas_generation {0};
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
        uint32_t generation {0};
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
        gpu_timer total;
        gpu_timer binning;
        gpu_timer rasterization;
        double seconds_per_tick {0.0};
        uint32_t frame_index {0};
    } stats;

    bool cache {false};
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
static inline uint32_t float_bits(float f)
{
    union {float f; uint32_t u;} c;
    c.f = f;
    return c.u;
}

//----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t hash_combine(uint32_t state, uint32_t value)
{
    state ^= value + 0x9e3779b9u + (state << 6u) + (state >> 2u);
    return state;
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
static inline void write_quantized_aabb(quantized_aabb* box, aabb bb)
{
    float min_x = max(bb.min.x, 0.f);
    float min_y = max(bb.min.y, 0.f);
    float max_x = max(bb.max.x, 0.f);
    float max_y = max(bb.max.y, 0.f);
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
    SAFE_RELEASE(r->tiles.hash_pso);
    SAFE_RELEASE(r->rasterizer.pso);
    SAFE_RELEASE(r->rasterizer.clean_pso);
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

        r->tiles.indirect_arg = r->device->newBuffer(2 * sizeof(MTL::DrawPrimitivesIndirectArguments), MTL::ResourceStorageModeShared);

        pWriteIcbFunction->release();

        r->regions.binning_pso = create_pso(r, pLibrary, "region_bin");
        r->regions.predicate_pso = create_pso(r, pLibrary, "predicate");
        r->regions.exclusive_scan_pso = create_pso(r, pLibrary, "exclusive_scan");
        r->tiles.hash_pso = create_pso(r, pLibrary, "hash_commands");
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

        MTL::RenderPipelineColorAttachmentDescriptor *pRenderbufferAttachment = pDesc->colorAttachments()->object(0);
        pRenderbufferAttachment->setPixelFormat(r->rasterizer.srgb_backbuffer ? MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormat::PixelFormatBGRA8Unorm);
        pRenderbufferAttachment->setBlendingEnabled(false);
        r->rasterizer.pso = r->device->newRenderPipelineState( pDesc, &pError );

        if (r->rasterizer.pso == nullptr)
            od_log(r, "error while creating rasterizer pso : %s", pError->localizedDescription()->utf8String());

        if (r->cache)
        {
            MTL::Function* pCleanFragmentFunction = pLibrary->newFunction(NS::String::string("clean_fs", NS::UTF8StringEncoding));

            MTL::RenderPipelineDescriptor* pCleanDesc = MTL::RenderPipelineDescriptor::alloc()->init();
            pCleanDesc->setVertexFunction(pVertexFunction);
            pCleanDesc->setFragmentFunction(pCleanFragmentFunction);

            MTL::RenderPipelineColorAttachmentDescriptor* pCleanAttachment = pCleanDesc->colorAttachments()->object(0);
            pCleanAttachment->setPixelFormat(r->rasterizer.srgb_backbuffer ? MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormat::PixelFormatBGRA8Unorm);
            pCleanAttachment->setBlendingEnabled(false);
            r->rasterizer.clean_pso = r->device->newRenderPipelineState( pCleanDesc, &pError );

            if (r->rasterizer.clean_pso == nullptr)
                od_log(r, "error while creating clean rasterizer pso : %s", pError->localizedDescription()->utf8String());

            pCleanFragmentFunction->release();
            pCleanDesc->release();
        }

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
    args->cache = r->cache;
    args->num_draw_data = (uint32_t) r->commands.data_buffer.GetNumElements();
    args->command_hashes = (uint32_t*) r->commands.command_hashes->gpuAddress();
    MTL::Texture* cache_bound = r->cache ? r->rasterizer.cache_texture : r->rasterizer.cache_placeholder;
    args->cache_texture = cache_bound->gpuResourceID()._impl;
    args->cache_read = cache_bound->gpuResourceID()._impl;

    // tile cache : the frame seed mixes everything that can change a pixel without changing the commands
    uint32_t seed = r->tiles.invalidate_generation;
    seed = hash_combine(seed, r->rasterizer.atlas_generation);
    seed = hash_combine(seed, r->font.generation);
    seed = hash_combine(seed, float_bits(r->rasterizer.clear_color.x));
    seed = hash_combine(seed, float_bits(r->rasterizer.clear_color.y));
    seed = hash_combine(seed, float_bits(r->rasterizer.clear_color.z));
    seed = hash_combine(seed, float_bits(r->rasterizer.clear_color.w));
    seed = hash_combine(seed, float_bits(r->rasterizer.aa_width));
    seed = hash_combine(seed, (uint32_t) r->tiles.culling_debug);
    seed = hash_combine(seed, (uint32_t) r->rasterizer.srgb_backbuffer);
    args->frame_seed = seed;

    const uint32_t simd_group_count = MAX_THREADS_PER_THREADGROUP / SIMD_GROUP_SIZE;
    const uint32_t threads_for_commands = optimal_num_threads(r->commands.count, SIMD_GROUP_SIZE, MAX_THREADS_PER_THREADGROUP);

    MTL::ComputeCommandEncoder* compute_encoder = nullptr;
    if (r->sample_buffer != nullptr)
    {
        MTL::ComputePassDescriptor* pass_desc = MTL::ComputePassDescriptor::alloc()->init();
        MTL::ComputePassSampleBufferAttachmentDescriptor* attachment = pass_desc->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(r->sample_buffer);
        attachment->setStartOfEncoderSampleIndex(0);
        attachment->setEndOfEncoderSampleIndex(1);
        compute_encoder = r->command_buffer->computeCommandEncoder(pass_desc);
        pass_desc->release();
    }
    else
        compute_encoder = r->command_buffer->computeCommandEncoder();

    if (r->cache)
    {
        compute_encoder->setComputePipelineState(r->tiles.hash_pso);
        compute_encoder->setBuffer(r->commands.draw_arg.GetBuffer(r->stats.frame_index), 0, 0);
        compute_encoder->setBuffer(r->commands.command_hashes, 0, 1);
        compute_encoder->useResource(r->commands.buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        compute_encoder->useResource(r->commands.colors.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        compute_encoder->useResource(r->commands.data_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        compute_encoder->useResource(r->commands.clipshapes_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        compute_encoder->useResource(r->commands.aabb_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        compute_encoder->useResource(r->commands.command_hashes, MTL::ResourceUsageWrite);
        compute_encoder->dispatchThreads(MTL::Size(r->commands.count, 1, 1), MTL::Size(threads_for_commands, 1, 1));
    }

    // predicate
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
    output->clean_indices = (uint16_t*) r->tiles.clean_indices->gpuAddress();
    output->tile_hashes = (uint32_t*) r->tiles.hashes->gpuAddress();

    compute_encoder->setBuffer(r->commands.bin_output_arg.GetBuffer(r->stats.frame_index), 0, 1);
    compute_encoder->setBuffer(r->tiles.counters_buffer, 0, 2);
    compute_encoder->setBuffer(r->regions.indices, 0, 3);
    compute_encoder->useResource(r->commands.aabb_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.data_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.clipshapes_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->commands.command_hashes, MTL::ResourceUsageRead);
    compute_encoder->useResource(r->tiles.hashes, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.head, MTL::ResourceUsageRead|MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.nodes, MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.indices, MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.clean_indices, MTL::ResourceUsageWrite);
    compute_encoder->dispatchThreads(MTL::Size(REGION_SIZE, REGION_SIZE, r->regions.count), MTL::Size(16, 16, 1));
    compute_encoder->setComputePipelineState(r->tiles.write_icb_pso);
    compute_encoder->setBuffer(r->tiles.counters_buffer, 0, 0);
    compute_encoder->setBuffer(r->tiles.indirect_arg, 0, 1);
    compute_encoder->useResource(r->tiles.indirect_arg, MTL::ResourceUsageWrite);
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

    const bool timed = (r->sample_buffer != nullptr) && (r->commands.count != 0);
    if (timed)
    {
        MTL::RenderPassSampleBufferAttachmentDescriptor* attachment = renderPassDescriptor->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(r->sample_buffer);
        attachment->setStartOfVertexSampleIndex(2);
        attachment->setEndOfFragmentSampleIndex(3);
    }

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
        render_encoder->useResource(r->tiles.clean_indices, MTL::ResourceUsageRead);
        MTL::Texture* cache_bound = r->cache ? r->rasterizer.cache_texture : r->rasterizer.cache_placeholder;
        render_encoder->useResource(cache_bound, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
        render_encoder->useResource(r->tiles.indirect_arg, MTL::ResourceUsageRead);
        render_encoder->useResource(r->font.texture, MTL::ResourceUsageRead);
        if (r->rasterizer.atlas != nullptr)
            render_encoder->useResource(r->rasterizer.atlas, MTL::ResourceUsageRead);
        render_encoder->setRenderPipelineState(r->rasterizer.pso);
        render_encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip, r->tiles.indirect_arg, 0);

        if (r->cache)
        {
            render_encoder->setRenderPipelineState(r->rasterizer.clean_pso);
            render_encoder->setVertexBuffer(r->tiles.clean_indices, 0, 1);
            render_encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip, r->tiles.indirect_arg, 16);
        }
    }
    render_encoder->endEncoding();

    const bool take_screenshot = (r->screenshot.out_pixels != nullptr) && (r->screenshot.capture_image) && (r->screenshot.texture != nullptr);

    r->command_buffer->addCompletedHandler(^void( MTL::CommandBuffer* pCmd )
    {
        UNUSED_VARIABLE(pCmd);
        dispatch_semaphore_signal( r->semaphore );

        atomic_store(&r->stats.total.last, (float)(pCmd->GPUEndTime() - pCmd->GPUStartTime()));

        if (timed)
        {
            NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

            NS::Data* samples = r->sample_buffer->resolveCounterRange(NS::Range(0, 4));
            const MTL::CounterResultTimestamp* s = (const MTL::CounterResultTimestamp*)samples->bytes();
            const bool valid = (samples->length() >= 4 * sizeof(MTL::CounterResultTimestamp)) &&
                               (s[0].timestamp != MTL::CounterErrorValue) && (s[1].timestamp != MTL::CounterErrorValue) &&
                               (s[2].timestamp != MTL::CounterErrorValue) && (s[3].timestamp != MTL::CounterErrorValue) &&
                               (s[1].timestamp >= s[0].timestamp) && (s[3].timestamp >= s[2].timestamp) &&
                               (s[1].timestamp <= s[3].timestamp);

            // the timestamp counter ticks on the gpu core clock, which varies in frequency,
            // so the tick -> seconds rate is derived per frame from the buffer span itself :
            // the same gpu interval expressed in seconds and in counter ticks
            if (valid && s[3].timestamp > s[0].timestamp)
                r->stats.seconds_per_tick = (double)(pCmd->GPUEndTime() - pCmd->GPUStartTime()) / (double)(s[3].timestamp - s[0].timestamp);

            const float binning_time = valid ? (float)((double)(s[1].timestamp - s[0].timestamp) * r->stats.seconds_per_tick) : 0.f;
            const float raster_time = valid ? (float)((double)(s[3].timestamp - s[2].timestamp) * r->stats.seconds_per_tick) : 0.f;
            atomic_store(&r->stats.binning.last, binning_time);
            atomic_store(&r->stats.rasterization.last, raster_time);

            pool->release();
        }
        else
        {
            atomic_store(&r->stats.binning.last, 0.f);
            atomic_store(&r->stats.rasterization.last, 0.f);
        }

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
    r->cache = def->cache;

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

    // sized on LAST_COMMAND, not MAX_COMMANDS : the region lists are terminated with LAST_COMMAND,
    // which must never be a real command index, so the newest command is LAST_COMMAND - 1
    r->commands.buffer.Init(r->device, sizeof(draw_command) * LAST_COMMAND);
    r->commands.colors.Init(r->device, sizeof(draw_color) * LAST_COMMAND);
    r->commands.data_buffer.Init(r->device, sizeof(float) * MAX_DRAWDATA);
    r->commands.aabb_buffer.Init(r->device, sizeof(quantized_aabb) * LAST_COMMAND);
    r->commands.clipshapes_buffer.Init(r->device, sizeof(clip_shape) * MAX_CLIPS);
    r->tiles.counters_buffer = r->device->newBuffer(sizeof(counters), MTL::ResourceStorageModePrivate);
    r->tiles.nodes = r->device->newBuffer(sizeof(tile_node) * MAX_NODES_COUNT, MTL::ResourceStorageModePrivate);
    r->commands.command_hashes = r->device->newBuffer(sizeof(uint32_t) * MAX_COMMANDS, MTL::ResourceStorageModePrivate);

    // 1x1 placeholder bound to the cache slot when the cache is off; the shader never dereferences it (uniform branch)
    MTL::TextureDescriptor* cache_placeholder_desc = MTL::TextureDescriptor::alloc()->init();
    cache_placeholder_desc->setWidth(1);
    cache_placeholder_desc->setHeight(1);
    cache_placeholder_desc->setPixelFormat(r->rasterizer.srgb_backbuffer ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
    cache_placeholder_desc->setTextureType(MTL::TextureType2D);
    cache_placeholder_desc->setMipmapLevelCount(1);
    cache_placeholder_desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    cache_placeholder_desc->setStorageMode(MTL::StorageModeShared);
    r->rasterizer.cache_placeholder = r->device->newTexture(cache_placeholder_desc);
    cache_placeholder_desc->release();

    r->semaphore = dispatch_semaphore_create(DynamicBuffer<float>::MaxInflightBuffers);

    // a single counter sample buffer is enough : od_flush ends with waitUntilCompleted(), frames never overlap on the gpu
    NS::Array* counter_sets = r->device->counterSets();
    MTL::CounterSet* timestamp_set = nullptr;
    for (NS::UInteger i = 0; i < counter_sets->count(); ++i)
    {
        MTL::CounterSet* set = counter_sets->object<MTL::CounterSet>(i);
        if (set->name()->isEqualToString(MTL::CommonCounterSetTimestamp))
        {
            timestamp_set = set;
            break;
        }
    }
    if (timestamp_set != nullptr)
    {
        MTL::CounterSampleBufferDescriptor* sample_desc = MTL::CounterSampleBufferDescriptor::alloc()->init();
        sample_desc->setCounterSet(timestamp_set);
        sample_desc->setStorageMode(MTL::StorageModeShared);
        sample_desc->setSampleCount(4);
        r->sample_buffer = r->device->newCounterSampleBuffer(sample_desc, nullptr);
        sample_desc->release();
        if (r->sample_buffer == nullptr)
            od_log(r, "warning: couldn't create the counter sample buffer, binning/rasterization time will report 0");
    }
    else
        od_log(r, "warning: the device exposes no timestamp counter set, binning/rasterization time will report 0");

    r->stats.seconds_per_tick = 0.0;

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

    // tile cache : a slice upload can change every textured quad, invalidate the whole cache
    r->rasterizer.atlas_generation++;
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
    SAFE_RELEASE(r->tiles.hashes);
    SAFE_RELEASE(r->tiles.clean_indices);
    SAFE_RELEASE(r->rasterizer.cache_texture);
    r->tiles.head = r->device->newBuffer(r->tiles.count * sizeof(uint32_t), MTL::ResourceStorageModePrivate);
    r->tiles.indices = r->device->newBuffer(r->tiles.num_width * r->tiles.num_height * sizeof(uint16_t), MTL::ResourceStorageModePrivate);
    r->tiles.clean_indices = r->device->newBuffer(r->tiles.num_width * r->tiles.num_height * sizeof(uint16_t), MTL::ResourceStorageModePrivate);
    r->tiles.hashes = r->device->newBuffer(r->tiles.count * sizeof(uint32_t), MTL::ResourceStorageModePrivate);

    if (r->cache)
    {
        MTL::TextureDescriptor* cache_desc = MTL::TextureDescriptor::alloc()->init();
        cache_desc->setWidth(width);
        cache_desc->setHeight(height);
        cache_desc->setPixelFormat(r->rasterizer.srgb_backbuffer ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
        cache_desc->setTextureType(MTL::TextureType2D);
        cache_desc->setMipmapLevelCount(1);
        cache_desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
        cache_desc->setStorageMode(MTL::StorageModePrivate);
        r->rasterizer.cache_texture = r->device->newTexture(cache_desc);
        cache_desc->release();
    }

    // the generation is mixed into the frame seed : a resize must change every tile hash to force a full redraw
    r->tiles.invalidate_generation++;

    // a fresh tile hash buffer must read as "never cached" : fill it with the sentinel the shader checks for
    MTL::CommandBuffer* fill_cb = r->command_queue->commandBuffer();
    MTL::BlitCommandEncoder* fill_blit = fill_cb->blitCommandEncoder();
    fill_blit->fillBuffer(r->tiles.hashes, NS::Range(0, r->tiles.hashes->length()), 0xff);
    fill_blit->endEncoding();
    fill_cb->commit();
    fill_cb->waitUntilCompleted();
    // commandBuffer() returns an autoreleased buffer : it is released by the current autorelease pool,
    // a manual release would be a double release (zombie) when the pool pops

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
    r->commands.clip_depth = 0;
    od_push_cliprect(r, 0, 0, (float)r->rasterizer.width, (float)r->rasterizer.height);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_end_frame(struct onedraw* r, void* drawable)
{
    assert_msg(r->commands.group_aabb == nullptr, "you need to call od_end_group, before od_end_frame");
    assert_msg(r->commands.clip_depth == 1, "unbalanced clip stack, every od_push_clip* must be matched by od_pop_clip");
    if (r->screenshot.show_region)
    {
        aabb capture_region = {.min = {(float)r->screenshot.region_x, (float)r->screenshot.region_y}};
        capture_region.max = vec2_add(capture_region.min, vec2_set(r->screenshot.region_width, r->screenshot.region_height));
        od_draw_box(r, capture_region.min.x, capture_region.min.y, capture_region.max.x, capture_region.max.y, 0.f, 0x802020ff);
    }

    r->commands.count = (uint32_t)r->commands.buffer.GetNumElements();
    r->stats.peak_num_draw_cmd = max(r->stats.peak_num_draw_cmd, r->commands.count);
    r->stats.num_draw_data = (uint32_t)r->commands.data_buffer.GetNumElements();
    gpu_timer_end_frame(&r->stats.total, r->stats.frame_index);
    gpu_timer_end_frame(&r->stats.binning, r->stats.frame_index);
    gpu_timer_end_frame(&r->stats.rasterization, r->stats.frame_index);
    r->regions.num_groups = (r->commands.count + SIMD_GROUP_SIZE - 1) / SIMD_GROUP_SIZE;

    od_flush(r, drawable);
}

//----------------------------------------------------------------------------------------------------------------------------
float od_get_average_gputime(struct onedraw* r)
{
    return r->stats.total.average;
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
    SAFE_RELEASE(r->tiles.hash_pso);
    SAFE_RELEASE(r->tiles.head);
    SAFE_RELEASE(r->tiles.nodes);
    SAFE_RELEASE(r->tiles.indices);
    SAFE_RELEASE(r->tiles.hashes);
    SAFE_RELEASE(r->tiles.clean_indices);
    SAFE_RELEASE(r->commands.command_hashes);
    SAFE_RELEASE(r->tiles.indirect_arg);
    SAFE_RELEASE(r->regions.predicate_pso);
    SAFE_RELEASE(r->regions.exclusive_scan_pso);
    SAFE_RELEASE(r->regions.indices);
    SAFE_RELEASE(r->regions.predicate);
    SAFE_RELEASE(r->regions.scan);
    SAFE_RELEASE(r->tiles.write_icb_pso);
    SAFE_RELEASE(r->rasterizer.pso);
    SAFE_RELEASE(r->rasterizer.clean_pso);
    SAFE_RELEASE(r->rasterizer.depth_stencil_state);
    SAFE_RELEASE(r->rasterizer.atlas);
    SAFE_RELEASE(r->rasterizer.cache_texture);
    SAFE_RELEASE(r->rasterizer.cache_placeholder);
    SAFE_RELEASE(r->command_queue);
    SAFE_RELEASE(r->font.texture);
    SAFE_RELEASE(r->font.glyphs);
    SAFE_RELEASE(r->screenshot.texture);
    SAFE_RELEASE(r->sample_buffer);
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
    stats->gpu_time_ms = r->stats.total.average * 1000.f;
    stats->binning_time_ms = r->stats.binning.average * 1000.f;
    stats->rasterization_time_ms = r->stats.rasterization.average * 1000.f;
    size_t gpu_mem = r->commands.aabb_buffer.GetTotalSize();
    gpu_mem += r->commands.bin_output_arg.GetTotalSize();
    gpu_mem += r->commands.buffer.GetTotalSize();
    gpu_mem += r->commands.clipshapes_buffer.GetTotalSize();
    gpu_mem += r->commands.colors.GetTotalSize();
    gpu_mem += r->commands.data_buffer.GetTotalSize();
    gpu_mem += r->commands.draw_arg.GetTotalSize();
    gpu_mem += r->font.texture->allocatedSize();
    gpu_mem += r->font.glyphs->allocatedSize();
    gpu_mem += (r->rasterizer.atlas != nullptr) ? r->rasterizer.atlas->allocatedSize() : 0;
    gpu_mem += r->regions.indices->allocatedSize();
    gpu_mem += r->regions.predicate->allocatedSize();
    gpu_mem += r->regions.scan->allocatedSize();
    gpu_mem += (r->screenshot.texture != nullptr) ? r->screenshot.texture->allocatedSize() : 0;
    gpu_mem += r->tiles.counters_buffer->allocatedSize();
    gpu_mem += r->tiles.head->allocatedSize();
    gpu_mem += r->tiles.indices->allocatedSize();
    gpu_mem += r->tiles.indirect_arg->allocatedSize();
    gpu_mem += r->tiles.nodes->allocatedSize();
    gpu_mem += r->tiles.hashes->allocatedSize();
    gpu_mem += r->tiles.clean_indices->allocatedSize();
    gpu_mem += r->commands.command_hashes->allocatedSize();
    gpu_mem += (r->cache ? r->rasterizer.cache_texture : r->rasterizer.cache_placeholder)->allocatedSize();
    stats->gpu_memory_usage = gpu_mem;
}

//----------------------------------------------------------------------------------------------------------------------------
// check that every buffer used by a draw command has enough space before reserving anything,
// so the callers can call NewElement/NewMultiple without checking the result
static bool has_space_for_draw_commands(struct onedraw* r, uint32_t num_commands, uint32_t num_data)
{
    return r->commands.buffer.GetMaxElements() - r->commands.buffer.GetNumElements() >= num_commands
        && r->commands.colors.GetMaxElements() - r->commands.colors.GetNumElements() >= num_commands
        && r->commands.data_buffer.GetMaxElements() - r->commands.data_buffer.GetNumElements() >= num_data
        && r->commands.aabb_buffer.GetMaxElements() - r->commands.aabb_buffer.GetNumElements() >= num_commands;
}

static inline bool has_space_for_draw_command(struct onedraw* r, uint32_t num_data)
{
    return has_space_for_draw_commands(r, 1, num_data);
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
// the aabb slot is reserved unwritten, the caller fills it (begin_group / end_group write it themselves)
static float* new_command(struct onedraw* r, enum command_type type, enum primitive_fillmode fillmode, uint8_t extra,
                          draw_color color, uint32_t num_data, quantized_aabb** out_aabb)
{
    *out_aabb = nullptr;

    if (!has_space_for_draw_command(r, num_data))
    {
        od_log(r, "out of draw commands/draw data buffer, expect graphical artefacts");
        return nullptr;
    }

    draw_command* cmd = r->commands.buffer.NewElement();
    draw_color* color_ptr = r->commands.colors.NewElement();
    cmd->clip_index = CURRENT_CLIP_INDEX;
    cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
    cmd->fillmode = fillmode;
    cmd->type = type;
    cmd->extra = extra;
    *color_ptr = color;

    float* data = r->commands.data_buffer.NewMultiple(num_data);
    *out_aabb = r->commands.aabb_buffer.NewElement();
    return data;
}

//----------------------------------------------------------------------------------------------------------------------------
// [bb] is passed already grown by draw_cmd_aabb_bump(r) when the shape needs it
static float* new_primitive(struct onedraw* r, enum command_type type, enum primitive_fillmode fillmode, uint8_t extra,
                            draw_color color, uint32_t num_data, aabb bb)
{
    quantized_aabb* aabb;
    float* data = new_command(r, type, fillmode, extra, color, num_data, &aabb);
    if (data == nullptr)
        return nullptr;

    write_quantized_aabb(aabb, bb);
    merge_quantized_aabb(r->commands.group_aabb, aabb);
    return data;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_begin_group(struct onedraw* r, bool smoothblend, float group_smoothness, float outline_width)
{
    assert_msg(r->commands.group_aabb == nullptr, "cannot call a second time od_begin_group without closing the previous group");
    assert_msg(group_smoothness >= 0.f, "smoothness cannot be negative");

    if (!smoothblend)
        group_smoothness = 0.f;

    enum sdf_operator op = smoothblend ? op_blend : op_overwrite;

    // placeholder color, the rasterizer reads the color of every command (the begin_group sdf branch ignores it)
    quantized_aabb* group_aabb;
    float* data = new_command(r, begin_group, fill_solid, (uint8_t)op, draw_color(), 2, &group_aabb);
    if (data == nullptr)
        return;

    write_float(data, group_smoothness + outline_width, outline_width);

    // keep values for the end group command
    r->rasterizer.outline_width = outline_width;
    r->rasterizer.group_smoothness = group_smoothness;
    r->rasterizer.group_op = op;

    // the aabb that the coming shapes will grow, end_group copies it into its command
    r->commands.group_aabb = group_aabb;
    *r->commands.group_aabb = invalid_quantized_aabb();
}

//----------------------------------------------------------------------------------------------------------------------------
void od_end_group(struct onedraw* r, draw_color outline_color)
{
    assert(r->commands.group_aabb != nullptr);

    enum primitive_fillmode fillmode = (r->rasterizer.outline_width > 0.f) ? fill_outline : fill_solid;

    // we put also the smooth value as we traverse the list in reverse order on the gpu
    quantized_aabb* aabb;
    float* data = new_command(r, end_group, fillmode, (uint8_t)r->rasterizer.group_op, outline_color, 1, &aabb);
    if (data == nullptr)
        return;

    *data = r->rasterizer.group_smoothness + r->rasterizer.outline_width;
    *aabb = *r->commands.group_aabb;

    r->commands.group_aabb = nullptr;
    r->rasterizer.group_smoothness = 0.f;
    r->rasterizer.group_op = op_overwrite;
    r->rasterizer.outline_width = 0.f;
}

//----------------------------------------------------------------------------------------------------------------------------
void private_draw_disc(struct onedraw* r, vec2 center, float radius, float thickness, enum primitive_fillmode fillmode,
                       draw_color primary_color, draw_color secondary_color)
{
    thickness *= .5f;

    uint32_t num_data = (fillmode == fill_hollow || fillmode == fill_gradient) ? 4 : 3;
    float max_radius = radius + draw_cmd_aabb_bump(r);
    if (fillmode == fill_hollow)
        max_radius += thickness;

    aabb bb = aabb_from_circle(center, max_radius);
    float* data = new_primitive(r, primitive_disc, fillmode, 0, primary_color, num_data, bb);
    if (data == nullptr)
        return;

    if (fillmode == fill_hollow)
        write_float(data, center.x, center.y, radius, thickness);
    else if (fillmode == fill_gradient)
        write_float(data, center.x, center.y, radius, bitcast_u32_to_float(secondary_color));
    else
        write_float(data, center.x, center.y, radius);
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
    float roundness_thickness = (fillmode == fill_hollow) ? thickness : roundness;
    aabb bb = aabb_from_rounded_obb(p0, p1, width, roundness_thickness + draw_cmd_aabb_bump(r));

    float* data = new_primitive(r, primitive_oriented_box, fillmode, 0, primary_color, num_data, bb);
    if (data == nullptr)
        return;

    if (fillmode == fill_gradient)
        write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness, bitcast_u32_to_float(secondary_color));
    else
        write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness);
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
    aabb bb = aabb_from_rounded_obb(p0, p1, width, draw_cmd_aabb_bump(r) + thickness);

    float* data = new_primitive(r, primitive_ellipse, fillmode, 0, srgb_color, num_data, bb);
    if (data == nullptr)
        return;

    if (fillmode == fill_hollow)
        write_float(data, p0.x, p0.y, p1.x, p1.y, width, thickness);
    else
        write_float(data, p0.x, p0.y, p1.x, p1.y, width);
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

    float roundness_thickness = (fillmode != fill_hollow) ? roundness : thickness;
    aabb bb = aabb_from_triangle(v[0], v[1], v[2]);
    aabb_grow(&bb, vec2_splat(roundness_thickness + draw_cmd_aabb_bump(r)));

    float* data = new_primitive(r, primitive_triangle, fillmode, 0, srgb_color, 7, bb);
    if (data == nullptr)
        return;

    write_float(data, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, roundness_thickness);
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
    aabb bb = aabb_from_circle(center, radius);
    aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r)));

    float* data = new_primitive(r, primitive_pie, fillmode, 0, srgb_color, num_data, bb);
    if (data == nullptr)
        return;

    if (fillmode != fill_hollow)
        write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture));
    else
        write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
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

    aabb bb = aabb_from_circle(center, radius);
    aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r)));

    float* data = new_primitive(r, primitive_arc, fill_solid, 0, srgb_color, 8, bb);
    if (data == nullptr)
        return;

    write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_box(struct onedraw* r, float x0, float y0, float x1, float y1, float radius, draw_color srgb_color)
{
    if (x0>x1) swap(x0, x1);
    if (y0>y1) swap(y0, y1);

    aabb box = {.min = {x0, y0}, .max = {x1, y1}};

    vec2 center = vec2_scale(vec2_add(box.min, box.max), .5f);
    vec2 half_extents = vec2_scale(vec2_sub(box.max, box.min), .5f);
    aabb_grow(&box, vec2_splat(draw_cmd_aabb_bump(r)));

    float* data = new_primitive(r, primitive_aabox, fill_solid, 0, srgb_color, 5, box);
    if (data == nullptr)
        return;

    write_float(data, center.x, center.y, half_extents.x, half_extents.y, radius);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_blurred_box(struct onedraw* r, float cx, float cy, float width, float height, float roundness, draw_color srgb_color)
{
    float half_width = width * .5f;
    float half_height = height * .5f;

    aabb bb = {.min = vec2_set(cx - half_width - roundness, cy - half_height - roundness),
               .max = vec2_set(cx + half_width + roundness, cy + half_height + roundness)};

    float* data = new_primitive(r, primitive_blurred_box, fill_solid, 0, srgb_color, 5, bb);
    if (data == nullptr)
        return;

    write_float(data, cx, cy, half_width, half_height, roundness);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_draw_char(struct onedraw* r, float x, float y, char c, draw_color srgb_color)
{
    if (c < r->font.desc.first_glyph || c >= (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        return;

    uint32_t glyph_index = c - r->font.desc.first_glyph;

    const float scale = r->font.scale;
    const glyph& glyph = r->font.desc.glyphs[glyph_index];
    x += glyph.bearing_x * scale;
    y += (glyph.bearing_y + r->font.desc.font_height) * scale;
    float glyph_width = float(glyph.x1 - glyph.x0) * scale;
    float glyph_height = float(glyph.y1 - glyph.y0) * scale;

    aabb bb = {.min = vec2_set(x, y), .max = vec2_set(x + glyph_width, y + glyph_height)};
    float* data = new_primitive(r, primitive_char, fill_solid, (uint8_t)glyph_index, srgb_color, 2, bb);
    if (data == nullptr)
        return;

    write_float(data, x, y);
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

    aabb bb = {.min = vec2_set(x0, y0), .max = vec2_set(x1, y1)};
    float* data = new_primitive(r, primitive_quad, fill_solid, (uint8_t)slice_index, srgb_color, 8, bb);
    if (data == nullptr)
        return;

    write_float(data, x0, y0, x1, y1, uv.u0, uv.v0, uv.u1, uv.v1);
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

    aabb bb = aabb_from_rounded_obb(p0, p1, height, 0.f);
    float* data = new_primitive(r, primitive_oriented_quad, fill_solid, (uint8_t)slice_index, srgb_color, 10, bb);
    if (data == nullptr)
        return;

    write_float(data, cx, cy, 1.f/width, 1.f/height, axis.x, axis.y, uv.u0, uv.v0, uv.u1, uv.v1);
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
        r->font.generation++;    // tile cache : the glyph metrics changed, invalidate the whole cache
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
static inline bool clip_shape_equal(const clip_shape* a, const clip_shape* b)
{
    if (a->type == clip_rect)
        return a->type == b->type && a->rect.center_x == b->rect.center_x && a->rect.center_y == b->rect.center_y &&
               a->rect.half_x == b->rect.half_x && a->rect.half_y == b->rect.half_y && a->rect.radius == b->rect.radius;

    return a->type == b->type && a->disc.center_x == b->disc.center_x && a->disc.center_y == b->disc.center_y &&
           a->disc.radius == b->disc.radius;
}

//----------------------------------------------------------------------------------------------------------------------------
static void push_clip(struct onedraw* r, clip_shape clip)
{
    assert_msg(r->commands.clip_depth < MAX_CLIP_DEPTH, "clip stack overflow, maximum depth is 16");

    // dedupe against the current clip, so pushing the same shape twice doesn't grow the buffer;
    // at depth 0 (the base push of od_begin_frame, the buffer was just mapped) the shape is appended unconditionally
    uint8_t clip_index = (r->commands.clip_depth > 0) ? CURRENT_CLIP_INDEX : 0;
    clip_shape* current = r->commands.clipshapes_buffer.At(clip_index);

    if (current == nullptr || !clip_shape_equal(current, &clip))
    {
        if (r->commands.clipshapes_buffer.GetNumElements() < MAX_CLIPS)
        {
            *r->commands.clipshapes_buffer.NewElement() = clip;
            clip_index = (uint8_t)r->commands.clipshapes_buffer.GetNumElements() - 1;
        }
        else
            od_log(r, "too many clip shapes! maximum is %d", MAX_CLIPS);
    }

    r->commands.clip_stack[r->commands.clip_depth++] = clip_index;
}

//----------------------------------------------------------------------------------------------------------------------------
static void push_cliprect(struct onedraw* r, float min_x, float min_y, float max_x, float max_y, float radius)
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

    push_clip(r, clip);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_push_cliprect(struct onedraw* r, float min_x, float min_y, float max_x, float max_y)
{
    push_cliprect(r, min_x, min_y, max_x, max_y, 0.f);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_push_cliprect_rounded(struct onedraw* r, float min_x, float min_y, float max_x, float max_y, float radius)
{
    push_cliprect(r, min_x, min_y, max_x, max_y, radius);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_push_clipdisc(struct onedraw* r, float cx, float cy, float radius)
{
    clip_shape clip =
    {
        .disc = {.center_x = cx, .center_y = cy, .radius = radius},
        .type = clip_disc
    };

    push_clip(r, clip);
}

//----------------------------------------------------------------------------------------------------------------------------
void od_pop_clip(struct onedraw* r)
{
    assert_msg(r->commands.clip_depth > 1, "cannot pop the base clip");
    r->commands.clip_depth--;
}

//----------------------------------------------------------------------------------------------------------------------------
void od_set_culling_debug(struct onedraw* r, bool b)
{
    r->tiles.culling_debug = b;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Dashed shapes
// ---------------------------------------------------------------------------------------------------------------------------

constexpr float FULL_TURN = VEC2_PI * 2.f;
constexpr uint32_t DASH_SAMPLES = 512U;    // max arc samples per full turn
constexpr float DASH_TOLERANCE = 0.25f;    // max chord sagitta in px, a dash follows the arc within it
constexpr float DASH_EPSILON = 0.01f;      // distance below which a "zero length" dash is not emitted

typedef struct dash_path
{
    bool       closed;                     // a closed path walks a full lap back to its first point
    float      perimeter;                  // total path length, the closed pattern rescales to it
    vec2       point;                      // current position on the path
    uint32_t   remaining;                  // path_polyline : segments left to emit
    uint32_t   index;                      // path_polyline : next destination point

    vec2       points[DASH_SAMPLES + 8];   // path_polyline : the vertices, straight edges are implicit between them
    uint32_t   num_points;
} dash_path;

//----------------------------------------------------------------------------------------------------------------------------
static uint32_t dash_path_arc_samples(float sweep, float radius)
{
    if (radius <= 0.f)
        return 0;    // a sharp corner : a single point

    float delta = 2.f * acosf(float_clamp(1.f - DASH_TOLERANCE / radius, -1.f, 1.f));
    uint32_t samples = max((uint32_t)ceilf(sweep / delta), 1U);
    uint32_t cap = max((uint32_t)((float)DASH_SAMPLES * sweep / FULL_TURN), 1U);
    return min(samples, cap);
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_path_add_arc(dash_path* p, vec2 center, vec2 u, vec2 v, float start_angle, float sweep, uint32_t samples)
{
    for (uint32_t i = 0; i <= samples; i++)
    {
        float a = start_angle + ((samples > 0) ? (sweep * (float)i / (float)samples) : 0.f);
        p->points[p->num_points++] = vec2_add(center, vec2_add(vec2_scale(u, cosf(a)), vec2_scale(v, sinf(a))));
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_path_add_box(dash_path* p, vec2 center, vec2 axis, float half_w, float half_h, float radius)
{
    vec2 sk = vec2_skew(axis);
    vec2 u = vec2_scale(axis, radius);
    vec2 v = vec2_scale(sk, radius);

    // the top edge start : the walk goes top edge, then corner 0 (top-right), clockwise
    vec2 local = vec2_set(-half_w + radius, -half_h);
    p->points[0] = vec2_add(center, vec2_add(vec2_scale(axis, local.x), vec2_scale(sk, local.y)));
    p->num_points = 1;

    for (uint32_t corner = 0; corner < 4; corner++)
    {
        // corner 0 top-right, 1 bottom-right, 2 bottom-left, 3 top-left, each sweeps a quarter turn clockwise ;
        // the right half is corners 0-1, the bottom half is corners 1-2 (a wrong side here folds the box into a bowtie)
        float sx = (corner < 2) ? 1.f : -1.f;
        float sy = ((corner == 1) || (corner == 2)) ? 1.f : -1.f;
        vec2 c = vec2_add(center, vec2_add(vec2_scale(axis, sx * (half_w - radius)), vec2_scale(sk, sy * (half_h - radius))));
        dash_path_add_arc(p, c, u, v, -VEC2_PI * .5f + (float)corner * VEC2_PI * .5f, VEC2_PI * .5f, dash_path_arc_samples(VEC2_PI * .5f, radius));
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static float dash_path_polyline_perimeter(dash_path* p)
{
    float length = 0.f;
    uint32_t last = p->closed ? p->num_points : p->num_points - 1;
    for (uint32_t i = 0; i < last; i++)
        length += vec2_distance(p->points[i], p->points[(i + 1) % p->num_points]);
    return length;
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_path_start_at(dash_path* p)
{
    p->point = p->points[0];
    p->index = 1;
    p->remaining = p->closed ? p->num_points : p->num_points - 1;
}

//----------------------------------------------------------------------------------------------------------------------------
static bool dash_path_next_segment(dash_path* p, vec2* p0, vec2* p1, float* length)
{
    *p0 = p->point;

    if (p->remaining == 0)
        return false;
    p->remaining--;

    *p1 = p->points[p->index];

    p->index = p->closed ? (p->index + 1) % p->num_points : p->index + 1;

    *length = vec2_distance(*p0, *p1);
    p->point = *p1;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
typedef struct dash_emitter
{
    struct onedraw* r;
    bool  failed;                       // the command budget is exceeded, abort the walk
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
// a capsule (round ends) and a line (flat ends) both use the exact oriented box culling
static void draw_dash(struct onedraw* r, vec2 a, vec2 b, float width, bool round_caps, draw_color color)
{
    if (round_caps)
        od_draw_capsule(r, a.x, a.y, b.x, b.y, width * .5f, color);
    else
        od_draw_line(r, a.x, a.y, b.x, b.y, width, color);
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_emitter_emit(dash_emitter* e)
{
    // one dash per emit ; the capacity was reserved up front, but guard against an exhausted buffer so the walk
    // can never overflow it
    if (!has_space_for_draw_command(e->r, 6))
    {
        e->failed = true;
        return;
    }

    draw_dash(e->r, e->dash_start, e->point, e->width, e->style.round_caps, e->color);
    e->num_commands++;
}

//----------------------------------------------------------------------------------------------------------------------------
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
static void dash_emitter_kink(dash_emitter* e)
{
    if (e->in_dash && e->phase < e->dash_length && !vec2_similar(e->dash_start, e->point, HALF_PIXEL))
    {
        dash_emitter_emit(e);
        e->dash_start = e->point;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_emitter_segment(dash_emitter* e, vec2 p0, vec2 p1, float length)
{
    e->point = p0;
    if (length > 0.f)
    {
        vec2 dir = vec2_scale(vec2_sub(p1, p0), 1.f / length);
        float u = 0.f;

        while (u < length && !e->failed)
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
    }

    dash_emitter_kink(e);
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_emitter_end(dash_emitter* e)
{
    if (!e->in_dash)
        return;

    // a dash that (re)started at the end of the path is a floating point drift, not a dash :
    // a closed path pattern that closes exactly at the seam (the first dash, already emitted)
    if (vec2_similar(e->dash_start, e->point, DASH_EPSILON))
        return;

    dash_emitter_emit(e);
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_emitter_init(dash_emitter* e, struct onedraw* r, vec2 start, od_dash_style style, float period, float width, draw_color color)
{
    e->r = r;
    e->failed = false;
    e->style = style;
    e->dash_length = style.dash_length;
    e->period = period;
    e->point = start;
    e->dash_start = start;
    e->width = width;
    e->color = color;
    e->num_commands = 0;

    // a closed path walks from its first vertex, the offset rotates the pattern (closed paths rescale it to close
    // without a stub at the seam), so open and closed paths start on the same pattern position
    e->phase = fmodf(style.offset, period);
    if (e->phase < 0.f)
        e->phase += period;
    e->in_dash = (e->phase < e->dash_length);
}

//----------------------------------------------------------------------------------------------------------------------------
static void dash_path_walk(dash_path* path, dash_emitter* e)
{
    vec2 a, b;
    float length;
    while (dash_path_next_segment(path, &a, &b, &length) && !e->failed)
        dash_emitter_segment(e, a, b, length);

    dash_emitter_end(e);
}

//----------------------------------------------------------------------------------------------------------------------------
static uint32_t dash_path_draw(struct onedraw* r, dash_path* path, float width, od_dash_style style, draw_color color)
{
    if (width <= 0.f)
        return 0;

    if (style.dash_length <= 0.f)
        return 0;

    float period = style.dash_length + style.gap_length;

    if (path->closed)
    {
        // rescale the pattern so it closes without a stub at the seam ; a small path that doesn't fit even one
        // pattern still draws one rescaled dash
        float num = fmaxf(floorf(path->perimeter / period + 0.5f), 1.f);
        style.dash_length *= path->perimeter / (num * period);
        period = path->perimeter / num;
    }

    // a translucent color is wrapped in a group, so the overlapping dash ends aren't double blended
    // (a caller already in a group composites everything in its group, no wrap needed)
    bool wrap_group = (((color >> 24) & 0xFF) < 0xFF) && (r->commands.group_aabb == nullptr);

    // a dash is one primitive (1 command / 1 color / 6 data / 1 aabb), and a dash that spans a corner is split
    // into one primitive per kink : reserve that much up front and return without emitting if the shape cannot
    // fit (a path that needs more primitives than the buffers hold would otherwise overflow them), then emit
    // in a single walk
    uint32_t capacity = min((uint32_t)(r->commands.buffer.GetMaxElements() - r->commands.buffer.GetNumElements()),
                            (uint32_t)(r->commands.colors.GetMaxElements() - r->commands.colors.GetNumElements()));
    capacity = min(capacity, (uint32_t)(r->commands.aabb_buffer.GetMaxElements() - r->commands.aabb_buffer.GetNumElements()));
    capacity = min(capacity, (uint32_t)((r->commands.data_buffer.GetMaxElements() - r->commands.data_buffer.GetNumElements()) / 6));
    capacity = (capacity > (wrap_group ? 2U : 0U)) ? (capacity - (wrap_group ? 2U : 0U)) : 0U;    // the group begin/end, underflow safe

    // the pattern produces one dash per period (+2 for a dash split at the seam or the endpoint), and each path
    // vertex is a kink that can split a dash into an extra primitive
    uint32_t num_dashes = (uint32_t)(path->perimeter / period) + 2U;
    uint32_t num_kinks = path->closed ? path->num_points : ((path->num_points > 1U) ? (path->num_points - 1U) : 0U);

    if (num_dashes + num_kinks > capacity)
        return UINT32_MAX;

    dash_path_start_at(path);

    dash_emitter e;
    dash_emitter_init(&e, r, path->point, style, period, width, color);

    if (wrap_group)
        od_begin_group(r, false, 0.f, 0.f);

    dash_path_walk(path, &e);

    if (wrap_group)
        od_end_group(r, 0);

    return e.num_commands;
}

//----------------------------------------------------------------------------------------------------------------------------
// a closed path needs its perimeter set, dash_path_draw rescales the pattern to it
static uint32_t dash_closed_path_draw(struct onedraw* r, dash_path* path, float thickness, od_dash_style style, draw_color color)
{
    path->perimeter = dash_path_polyline_perimeter(path);
    return dash_path_draw(r, path, thickness, style, color);
}

//----------------------------------------------------------------------------------------------------------------------------
static uint32_t dash_box(struct onedraw* r, vec2 center, vec2 axis, float half_w, float half_h, float corner,
                         float thickness, od_dash_style style, draw_color color)
{
    // there is no solid rounded box outline primitive : a zero gap pattern is one dash = the whole outline, walked
    // through the generic emitter (the dash length value is irrelevant with a zero gap, it just must not be zero)
    if (style.gap_length <= 0.f)
        style.dash_length = fmaxf(style.dash_length, 1.f);

    dash_path path = { .closed = true };
    dash_path_add_box(&path, center, axis, half_w, half_h, corner);
    return dash_closed_path_draw(r, &path, thickness, style, color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_line(struct onedraw* r, float ax, float ay, float bx, float by, float width, od_dash_style style, draw_color srgb_color)
{
    vec2 a = vec2_set(ax, ay), b = vec2_set(bx, by);
    float perimeter = vec2_distance(a, b);

    if (perimeter <= HALF_PIXEL || width <= 0.f)
        return 0;

    // a zero gap is the solid line : one primitive, no walk
    if (style.gap_length <= 0.f)
    {
        draw_dash(r, a, b, width, style.round_caps, srgb_color);
        return 1;
    }

    dash_path path = { .points = { a, b } };
    path.num_points = 2;
    path.perimeter = perimeter;

    return dash_path_draw(r, &path, width, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_ring(struct onedraw* r, float cx, float cy, float radius, float thickness, od_dash_style style, draw_color srgb_color)
{
    if (radius <= 0.f || thickness <= 0.f)
        return 0;

    // a zero gap is the solid ring : one primitive, no walk
    if (style.gap_length <= 0.f)
    {
        od_draw_ring(r, cx, cy, radius, thickness, srgb_color);
        return 1;
    }

    dash_path path = { .closed = true };
    dash_path_add_arc(&path, vec2_set(cx, cy), vec2_set(radius, 0.f), vec2_set(0.f, radius), 0.f, FULL_TURN, dash_path_arc_samples(FULL_TURN, radius));
    return dash_closed_path_draw(r, &path, thickness, style, srgb_color);
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

    vec2 center = vec2_scale(vec2_add(lo, hi), .5f);
    float corner = fminf(fmaxf(0.f, radius), fminf(w, h) * .5f);    // a negative radius is clamped to a sharp corner

    return dash_box(r, center, vec2_set(1.f, 0.f), w * .5f, h * .5f, corner, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_oriented_box(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness,
                                     float thickness, od_dash_style style, draw_color srgb_color)
{
    vec2 a = vec2_set(ax, ay), b = vec2_set(bx, by);
    float length = vec2_distance(a, b);

    if (length <= 0.f || width <= 0.f || thickness <= 0.f)
        return 0;

    vec2 center = vec2_scale(vec2_add(a, b), .5f);
    vec2 axis = vec2_scale(vec2_sub(b, a), 1.f / length);
    float corner = fminf(fmaxf(0.f, roundness), fminf(length, width) * .5f);    // a negative roundness is clamped to a sharp corner

    return dash_box(r, center, axis, length * .5f, width * .5f, corner, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_ellipse_ring(struct onedraw* r, float ax, float ay, float bx, float by, float width, float thickness,
                                     od_dash_style style, draw_color srgb_color)
{
    vec2 p0 = vec2_set(ax, ay);
    vec2 p1 = vec2_set(bx, by);
    float length = vec2_distance(p0, p1);
    float semi_major = length * .5f;

    if (semi_major <= HALF_PIXEL || width <= 0.f || thickness <= 0.f)
        return 0;

    vec2 center = vec2_scale(vec2_add(p0, p1), .5f);
    vec2 axis = vec2_scale(vec2_sub(p1, p0), 1.f / length);
    float semi_minor = width * .5f;

    // a needle like ellipse is a line segment along its main axis
    if (semi_minor < 0.01f)
    {
        if (style.gap_length <= 0.f)
        {
            draw_dash(r, p0, p1, thickness, style.round_caps, srgb_color);
            return 1;
        }

        dash_path path = { .points = { p0, p1 } };
        path.num_points = 2;
        path.perimeter = length;
        return dash_path_draw(r, &path, thickness, style, srgb_color);
    }

    // a zero gap is the solid ellipse : one primitive, no walk
    if (style.gap_length <= 0.f)
    {
        od_draw_ellipse_ring(r, p0.x, p0.y, p1.x, p1.y, width, thickness, srgb_color);
        return 1;
    }

    dash_path path = { .closed = true };
    dash_path_add_arc(&path, center, vec2_scale(axis, semi_major), vec2_scale(vec2_skew(axis), semi_minor), 0.f, FULL_TURN,
                      dash_path_arc_samples(FULL_TURN, semi_minor * semi_minor / semi_major));
    return dash_closed_path_draw(r, &path, thickness, style, srgb_color);
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t od_draw_dashed_triangle_ring(struct onedraw* r, const float* vertices, float thickness, od_dash_style style, draw_color srgb_color)
{
    vec2 v[3];
    v[0] = vec2_set(vertices[0], vertices[1]);
    v[1] = vec2_set(vertices[2], vertices[3]);
    v[2] = vec2_set(vertices[4], vertices[5]);

    // exclude an invalid triangle, same guard as od_draw_triangle_ring
    if (vec2_similar(v[0], v[1], HALF_PIXEL) || vec2_similar(v[2], v[1], HALF_PIXEL) || vec2_similar(v[0], v[2], HALF_PIXEL) ||
        thickness <= 0.f)
        return 0;

    // a zero gap is the solid triangle : one primitive, no walk
    if (style.gap_length <= 0.f)
    {
        od_draw_triangle_ring(r, vertices, 0.f, thickness, srgb_color);
        return 1;
    }

    dash_path path = { .closed = true, .points = { v[0], v[1], v[2] }};
    path.num_points = 3;
    return dash_closed_path_draw(r, &path, thickness, style, srgb_color);
}

