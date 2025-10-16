#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"

#include "renderer.h"
#include "../system/format.h"
#include "../math/arc.h"
#include "../system/log.h"
#include "../system/lean_ui.h"
#include "../math/ortho.h"
#include "MetalLoader.h"
#include "default_font.h"
#include "default_font_atlas.h"
#include <stdatomic.h>

#define SAFE_RELEASE(p) if (p!=nullptr) {p->release(); p = nullptr;}
#define UNUSED_VARIABLE(a) (void)(a)

// ---------------------------------------------------------------------------------------------------------------------------
// templates definition
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
};

void write_float(float* buffer, float value) {*buffer = value;};
template<typename...Args>
void write_float(float* buffer, float value, Args ... args)
{
    *buffer = value;
    write_float(++buffer, args...);
}

template<typename T>
void distance_screen_space(float scale, T& var){var *= scale;}

template<typename T, typename... Args>
void distance_screen_space(float scale, T& var, Args & ... args)
{
    var *= scale;
    distance_screen_space(scale, args...);
}

// ---------------------------------------------------------------------------------------------------------------------------
// private structures
// ---------------------------------------------------------------------------------------------------------------------------
struct glyph
{
    uint16_t x0, y0, x1, y1;
    float bearing_x, bearing_y;
    float advance_x;
};

struct alphabet
{
    glyph glyphs[MAX_GLYPHS];
    float font_height;
    uint16_t num_glyphs;
    uint16_t first_glyph;
    uint16_t texture_width;
    uint16_t texture_height;
};

struct renderer
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
        DynamicBuffer<quantized_aabb> aabb_buffer;
        DynamicBuffer<float> data_buffer;
        DynamicBuffer<clip_rect> cliprects_buffer;
        uint32_t count;
        quantized_aabb* combination_aabb {nullptr};
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
        MTL::Fence* clear_buffers_fence {nullptr};
        MTL::Fence* write_icb_fence {nullptr};
        MTL::Buffer* indirect_arg {nullptr};
        MTL::Buffer* indices {nullptr};
        MTL::Buffer* nodes {nullptr};
        MTL::IndirectCommandBuffer* indirect_cb {nullptr};
        uint16_t num_width;
        uint16_t num_height;
        uint32_t count;
    } tiles;

    // rasterizer
    struct
    {
        MTL::RenderPipelineState* pso {nullptr};
        MTL::DepthStencilState* depth_stencil_state {nullptr};
        float4 clear_color {.x = 0.f, .y = 0.f, .z = 0.f, .w = 1.f};
        uint16_t width;
        uint16_t height;
        float aa_width {VEC2_SQR2};
        float smooth_value {0.f};
        float outline_width {1.f};
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
        MTL::Texture* pTexture {nullptr};
        uint8_t* pRawBytes {nullptr};
        uint32_t region_x, region_y;
        uint32_t region_width, region_height;
        bool show_region {false};
        bool capture_image {false};
        bool capture_video {false};
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
        float time;
    } stats;

    struct view_proj view_proj;
    bool m_CullingDebug {false};
};


// ---------------------------------------------------------------------------------------------------------------------------
// private functions
// ---------------------------------------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------------------------------------
void renderer_init_screenshot_resources(struct renderer* r)
{
    MTL::TextureDescriptor* pTextureDesc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm_sRGB,
                                           r->rasterizer.width, r->rasterizer.height, false);
    pTextureDesc->setStorageMode(MTL::StorageModeShared);
    pTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);

    r->screenshot = 
    {
        .pTexture = r->device->newTexture(pTextureDesc),
        .pRawBytes = new uint8_t[r->rasterizer.width * r->rasterizer.height],
        .region_x = 0,
        .region_y = 0,
        .region_width = r->rasterizer.width,
        .region_height = r->rasterizer.height
    };

    pTextureDesc->release();
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_build_depthstencil_state(struct renderer* r)
{
    MTL::DepthStencilDescriptor* pDsDesc = MTL::DepthStencilDescriptor::alloc()->init();

    pDsDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionAlways);
    pDsDesc->setDepthWriteEnabled(false);

    r->rasterizer.depth_stencil_state = r->device->newDepthStencilState(pDsDesc);

    pDsDesc->release();
}

//----------------------------------------------------------------------------------------------------------------------------
void writeTGA(const char* filename, uint8_t* pixels, uint32_t width, uint32_t height) 
{
    // TGA Header (18 bytes)
    uint8_t header[18] = {};
    header[2]  = 2;                        // Image type: uncompressed true-color
    header[12] = width & 0xFF;
    header[13] = (width >> 8) & 0xFF;
    header[14] = height & 0xFF;
    header[15] = (height >> 8) & 0xFF;
    header[16] = 32;                       // Bits per pixel
    header[17] = 0x20;                     // Image origin: top-left (bit 5)

    FILE* f = fopen(filename, "wb");
    if (f)
    {
        fwrite(header, sizeof(header), 1, f);
        fwrite(pixels, width * height * 4, 1, f);
        fclose(f);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
MTL::ComputePipelineState* create_pso(struct renderer* r, MTL::Library* pLibrary, const char* function_name)
{
    MTL::Function *pFunction = pLibrary->newFunction(NS::String::string(function_name, NS::UTF8StringEncoding));
    NS::Error* pError = nullptr;

    MTL::ComputePipelineState* pso = r->device->newComputePipelineState(pFunction, &pError);

    if (pso == nullptr)
        log_error( "%s", pError->localizedDescription()->utf8String());

    pFunction->release();
    return pso;
}


//----------------------------------------------------------------------------------------------------------------------------
void renderer_build_pso(struct renderer* r)
{
    SAFE_RELEASE(r->regions.binning_pso);
    SAFE_RELEASE(r->tiles.binning_pso);
    SAFE_RELEASE(r->rasterizer.pso);
    SAFE_RELEASE(r->tiles.write_icb_pso);
    SAFE_RELEASE(r->regions.exclusive_scan_pso);

    MTL::Library* pLibrary = load_metal_library(r->device, "shaders.metallib");
    if (pLibrary != nullptr)
    {
        MTL::Function* pTileBinningFunction = pLibrary->newFunction(NS::String::string("tile_bin", NS::UTF8StringEncoding));
        NS::Error* pError = nullptr;
        r->tiles.binning_pso = r->device->newComputePipelineState(pTileBinningFunction, &pError);

        if (r->tiles.binning_pso == nullptr)
        {
            log_error( "%s", pError->localizedDescription()->utf8String());
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
            log_error( "%s", pError->localizedDescription()->utf8String());
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

        MTL::Function* pVertexFunction = pLibrary->newFunction(NS::String::string("tile_vs", NS::UTF8StringEncoding));
        MTL::Function* pFragmentFunction = pLibrary->newFunction(NS::String::string("tile_fs", NS::UTF8StringEncoding));

        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pDesc->setVertexFunction(pVertexFunction);
        pDesc->setFragmentFunction(pFragmentFunction);
        pDesc->setSupportIndirectCommandBuffers(true);

        MTL::RenderPipelineColorAttachmentDescriptor *pRenderbufferAttachment = pDesc->colorAttachments()->object(0);
        pRenderbufferAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
        pRenderbufferAttachment->setBlendingEnabled(false);
        r->rasterizer.pso = r->device->newRenderPipelineState( pDesc, &pError );

        if (r->rasterizer.pso == nullptr)
            log_error( "%s", pError->localizedDescription()->utf8String());

        pVertexFunction->release();
        pFragmentFunction->release();
        pDesc->release();
        pLibrary->release();
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_build_font(struct renderer* r)
{
    MTL::TextureDescriptor* pTextureDesc = MTL::TextureDescriptor::alloc()->init();
    pTextureDesc->setWidth(r->font.desc.texture_width);
    pTextureDesc->setHeight(r->font.desc.texture_height);
    pTextureDesc->setPixelFormat(MTL::PixelFormatBC4_RUnorm);
    pTextureDesc->setTextureType(MTL::TextureType2D);
    pTextureDesc->setMipmapLevelCount(1);
    pTextureDesc->setUsage( MTL::ResourceUsageSample | MTL::ResourceUsageRead );
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
        const glyph& glyph = r->font.desc.glyphs[i];
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
static inline uint32_t optimal_num_threads(uint32_t num_elements, uint32_t simd_group_size, uint32_t max_threads)
{
    uint32_t rounded = (num_elements + simd_group_size - 1) / simd_group_size;
    rounded *= simd_group_size;
    return min(rounded, max_threads);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline void write_aabb(quantized_aabb* box, float min_x, float min_y, float max_x, float max_y)
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
static inline void merge_aabb(quantized_aabb* merge, const quantized_aabb* other)
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
static inline quantized_aabb invalid_aabb()
{
    return (quantized_aabb)
    {
        .min_x = UINT8_MAX,
        .min_y = UINT8_MAX,
        .max_x = 0,
        .max_y = 0
    };
}

// ---------------------------------------------------------------------------------------------------------------------------
// public functions
// ---------------------------------------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------------------------------------
size_t renderer_min_memory_size()
{
    return sizeof(struct renderer);
}

//----------------------------------------------------------------------------------------------------------------------------
struct renderer* renderer_init(void* buffer, void* device, uint32_t width, uint32_t height)
{
    renderer* r = new(buffer) renderer;

    assert(device!=nullptr);
    r->device = (MTL::Device*)device;
    assert(r->device->supportsFamily(MTL::GPUFamilyApple7));
    r->command_queue = r->device->newCommandQueue();

    if (r->command_queue == nullptr)
    {
        log_fatal("can't create a command queue");
        exit(EXIT_FAILURE);
    }

    r->commands.buffer.Init(r->device, sizeof(draw_command) * MAX_COMMANDS);
    r->commands.data_buffer.Init(r->device, sizeof(float) * MAX_DRAWDATA);
    r->commands.aabb_buffer.Init(r->device, sizeof(quantized_aabb) * MAX_COMMANDS);
    r->commands.cliprects_buffer.Init(r->device, sizeof(clip_rect) * MAX_CLIPS);
    r->tiles.counters_buffer = r->device->newBuffer(sizeof(counters), MTL::ResourceStorageModePrivate);
    r->tiles.nodes = r->device->newBuffer(sizeof(tile_node) * MAX_NODES_COUNT, MTL::ResourceStorageModePrivate);
    r->tiles.clear_buffers_fence = r->device->newFence();
    r->tiles.write_icb_fence = r->device->newFence();

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

    renderer_build_pso(r);
    renderer_build_font(r);
    renderer_build_depthstencil_state(r);
    renderer_resize(r, width, height);
    renderer_init_screenshot_resources(r);
    ortho_set_viewport(&r->view_proj, vec2_set((float)width, (float)height), vec2_set((float)r->rasterizer.width, (float)r->rasterizer.height), vec2_zero());

    return r;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_capture_region(struct renderer* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    assert(x<=r->rasterizer.width && width<=r->rasterizer.width && y<=r->rasterizer.height && height<=r->rasterizer.height);
    r->screenshot.region_x = x;
    r->screenshot.region_y = y;
    r->screenshot.region_width = width;
    r->screenshot.region_height = height;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_resize(struct renderer* r, uint32_t width, uint32_t height)
{
    log_info("resizing the framebuffer to %dx%d", width, height);
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

    log_info("%ux%u tiles", r->tiles.num_width, r->tiles.num_height);
    log_info("%ux%u regions", r->regions.num_width, r->regions.num_height);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_begin_frame(struct renderer* r, float time)
{
    assert(r->commands.combination_aabb == nullptr);
    r->stats.frame_index++;
    r->stats.time = time;
    r->commands.buffer.Map(r->stats.frame_index);
    r->commands.draw_aabb = r->commands.aabb_buffer.Map(r->stats.frame_index);
    r->commands.data_buffer.Map(r->stats.frame_index);
    r->commands.cliprects_buffer.Map(r->stats.frame_index);
    renderer_set_cliprect(r, 0, 0, (uint16_t) r->rasterizer.width, (uint16_t) r->rasterizer.height);
    r->commands.combination_aabb = nullptr;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_end_frame(struct renderer* r)
{
    assert(r->commands.combination_aabb == nullptr);

    if (r->screenshot.show_region)
    {
        aabb capture_region = {.min = {(float)r->screenshot.region_x, (float)r->screenshot.region_y}};
        capture_region.max = vec2_add(capture_region.min, vec2_set(r->screenshot.region_width, r->screenshot.region_height));
        renderer_draw_aabb(r, capture_region, (draw_color)(0x802020ff));
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
}

//----------------------------------------------------------------------------------------------------------------------------
float renderer_get_average_gputime(struct renderer* r)
{
    return r->stats.average_gpu_time;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_bin_commands(struct renderer* r)
{
    if (r->tiles.binning_pso == nullptr || r->regions.binning_pso == nullptr || r->regions.exclusive_scan_pso == nullptr)
        return;

    // clear buffers
    MTL::BlitCommandEncoder* pBlitEncoder = r->command_buffer->blitCommandEncoder();
    pBlitEncoder->fillBuffer(r->tiles.counters_buffer, NS::Range(0, r->tiles.counters_buffer->length()), 0);
    pBlitEncoder->fillBuffer(r->tiles.head, NS::Range(0, r->tiles.head->length()), 0xff);
    pBlitEncoder->fillBuffer(r->regions.indices, NS::Range(0, r->regions.indices->length()), 0xff);
    pBlitEncoder->updateFence(r->tiles.clear_buffers_fence);
    pBlitEncoder->endEncoding();

    // compute wait for clears to be done
    MTL::ComputeCommandEncoder* compute_encoder = r->command_buffer->computeCommandEncoder();
    compute_encoder->waitForFence(r->tiles.clear_buffers_fence);

    // fill common structures
    draw_cmd_arguments* args = r->commands.draw_arg.Map(r->stats.frame_index);
    args->clear_color = r->rasterizer.clear_color;
    args->aa_width = r->rasterizer.aa_width;
    args->commands_aabb = (quantized_aabb*) r->commands.aabb_buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->commands = (draw_command*) r->commands.buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->draw_data = (float*) r->commands.data_buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->clips = (clip_rect*) r->commands.cliprects_buffer.GetBuffer(r->stats.frame_index)->gpuAddress();
    args->glyphs = (font_char*) r->font.glyphs->gpuAddress();
    args->font = (texture_half) r->font.texture->gpuResourceID()._impl;
    args->max_nodes = MAX_NODES_COUNT;
    args->num_commands = r->commands.count;
    args->num_tile_height = r->tiles.num_height;
    args->num_tile_width = r->tiles.num_width;
    args->num_region_width = r->regions.num_width;
    args->num_region_height = r->regions.num_height;
    args->num_groups = r->regions.num_groups;
    args->screen_div = (float2) {.x = 1.f / (float)r->rasterizer.width, .y = 1.f / (float) r->rasterizer.height};
    args->outline_width = r->rasterizer.outline_width;
    args->outline_color = draw_color(0xff000000);
    args->culling_debug = r->m_CullingDebug;
    args->time = r->stats.time;
    args->num_elements_per_thread = (r->commands.count + MAX_THREADS_PER_THREADGROUP-1) / MAX_THREADS_PER_THREADGROUP;

    const uint32_t simd_group_count = MAX_THREADS_PER_THREADGROUP / SIMD_GROUP_SIZE;
    const uint32_t threads_for_commands = optimal_num_threads(r->commands.count, SIMD_GROUP_SIZE, MAX_THREADS_PER_THREADGROUP);

    const NS::UInteger w = r->tiles.binning_pso->threadExecutionWidth();
    const NS::UInteger h = r->tiles.binning_pso->maxTotalThreadsPerThreadgroup() / w;
    const MTL::Size default_2d_threadgroup_size(w, h, 1);

    // predicate
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
    compute_encoder->dispatchThreads(MTL::Size(r->commands.count, r->regions.count, 1), default_2d_threadgroup_size);

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
    compute_encoder->useResource(r->commands.cliprects_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
    compute_encoder->useResource(r->tiles.head, MTL::ResourceUsageRead|MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.nodes, MTL::ResourceUsageWrite);
    compute_encoder->useResource(r->tiles.indices, MTL::ResourceUsageWrite);

    compute_encoder->dispatchThreads(MTL::Size(r->tiles.num_width, r->tiles.num_height, 1), default_2d_threadgroup_size);

    compute_encoder->setComputePipelineState(r->tiles.write_icb_pso);
    compute_encoder->setBuffer(r->tiles.counters_buffer, 0, 0);
    compute_encoder->setBuffer(r->tiles.indirect_arg, 0, 1);
    compute_encoder->useResource(r->tiles.indirect_cb, MTL::ResourceUsageWrite);
    compute_encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
    compute_encoder->updateFence(r->tiles.write_icb_fence);
    compute_encoder->endEncoding();
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_flush(struct renderer* r, void* drawable)
{
    r->command_buffer = r->command_queue->commandBuffer();

    dispatch_semaphore_wait(r->semaphore, DISPATCH_TIME_FOREVER);

    renderer_bin_commands(r);

    MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* cd = renderPassDescriptor->colorAttachments()->object(0);
    cd->setTexture(((CA::MetalDrawable*)drawable)->texture());
    cd->setLoadAction(MTL::LoadActionClear);
    cd->setClearColor(MTL::ClearColor(r->rasterizer.clear_color.x, r->rasterizer.clear_color.y, r->rasterizer.clear_color.z, r->rasterizer.clear_color.w));
    cd->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* render_encoder = r->command_buffer->renderCommandEncoder(renderPassDescriptor);

    if (r->rasterizer.pso != nullptr && r->tiles.binning_pso != nullptr)
    {
        render_encoder->waitForFence(r->tiles.write_icb_fence, MTL::RenderStageVertex|MTL::RenderStageFragment|MTL::RenderStageMesh|MTL::RenderStageObject);
        render_encoder->setCullMode(MTL::CullModeNone);
        render_encoder->setDepthStencilState(r->rasterizer.depth_stencil_state);
        render_encoder->setVertexBuffer(r->commands.draw_arg.GetBuffer(r->stats.frame_index), 0, 0);
        render_encoder->setVertexBuffer(r->tiles.indices, 0, 1);
        render_encoder->setFragmentBuffer(r->commands.draw_arg.GetBuffer(r->stats.frame_index), 0, 0);
        render_encoder->setFragmentBuffer(r->commands.bin_output_arg.GetBuffer(r->stats.frame_index), 0, 1);
        render_encoder->useResource(r->commands.draw_arg.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.data_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->commands.cliprects_buffer.GetBuffer(r->stats.frame_index), MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.head, MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.nodes, MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.indices, MTL::ResourceUsageRead);
        render_encoder->useResource(r->tiles.indirect_cb, MTL::ResourceUsageRead);
        render_encoder->useResource(r->font.texture, MTL::ResourceUsageRead);
        render_encoder->setRenderPipelineState(r->rasterizer.pso);
        render_encoder->executeCommandsInBuffer(r->tiles.indirect_cb, NS::Range(0, 1));
        render_encoder->endEncoding();
    }

    r->command_buffer->addCompletedHandler(^void( MTL::CommandBuffer* pCmd )
    {
        UNUSED_VARIABLE(pCmd);
        dispatch_semaphore_signal( r->semaphore );

        atomic_store(&r->stats.gpu_time, (float)(pCmd->GPUEndTime() - pCmd->GPUStartTime()));
    });

    if (r->screenshot.capture_image || r->screenshot.capture_video)
    {
        MTL::BlitCommandEncoder* blit = r->command_buffer->blitCommandEncoder();
        blit->copyFromTexture(((CA::MetalDrawable*)drawable)->texture(),0, 0, r->screenshot.pTexture, 0, 0, 1, 1);
        blit->endEncoding();
    }

    r->command_buffer->presentDrawable((CA::MetalDrawable*)drawable);
    r->command_buffer->commit();
    r->command_buffer->waitUntilCompleted();

    if (r->screenshot.capture_image || r->screenshot.capture_video)
    {
        MTL::Region region = MTL::Region(r->screenshot.region_x, r->screenshot.region_y,
                                         r->screenshot.region_width, r->screenshot.region_height);
        r->screenshot.pTexture->getBytes(r->screenshot.pRawBytes, r->screenshot.region_width * 4, region, 0);
        writeTGA(format("screenshot_%05d.tga", r->stats.frame_index), r->screenshot.pRawBytes, r->screenshot.region_width, r->screenshot.region_height);
        r->screenshot.capture_image = false;
    }
    
    renderPassDescriptor->release();
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_terminate(struct renderer* r)
{
    r->commands.buffer.Terminate();
    r->commands.data_buffer.Terminate();
    r->commands.aabb_buffer.Terminate();
    r->commands.draw_arg.Terminate();
    r->commands.bin_output_arg.Terminate();
    r->commands.cliprects_buffer.Terminate();
    SAFE_RELEASE(r->tiles.write_icb_fence);
    SAFE_RELEASE(r->tiles.counters_buffer);
    SAFE_RELEASE(r->tiles.clear_buffers_fence);
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
    SAFE_RELEASE(r->command_queue);
    SAFE_RELEASE(r->font.texture);
    SAFE_RELEASE(r->font.glyphs);
    SAFE_RELEASE(r->screenshot.pTexture);
    delete[] r->screenshot.pRawBytes;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_debug_interface(struct renderer* r, struct ui_context* gui_context)
{
    size_t total_buffers_usage = (r->stats.num_draw_data * sizeof(float)) + (r->commands.count * sizeof(draw_command));
    size_t total_buffers_capacity = (r->commands.data_buffer.GetMaxElements()  * sizeof(float)) + (r->commands.buffer.GetLength());

    ui_begin_window(gui_context, "sdf2d stats", 0, 0, 600, 600, 0);
    ui_value(gui_context, "frame count", "%d", r->stats.frame_index);
    ui_value(gui_context, "draw cmds", "%d/%d", r->commands.count, r->commands.buffer.GetMaxElements());
    ui_value(gui_context, "peak cmds", "%d", r->stats.peak_num_draw_cmd);
    ui_value(gui_context, "buffers usage", "%d/%d kb", total_buffers_usage>>10, total_buffers_capacity>>10);
    ui_value(gui_context, "gpu time", "%2.2f ms", r->stats.average_gpu_time * 1000.f);
    ui_toggle(gui_context, "debug culling", &r->m_CullingDebug);
    ui_separator(gui_context);
    ui_toggle(gui_context, "show capture region", &r->screenshot.show_region);
    ui_toggle(gui_context, "capture video", &r->screenshot.capture_video);
    r->screenshot.capture_image = ui_button(gui_context, "take screenshot", align_right);
    ui_end_window(gui_context);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_begin_combination(struct renderer* r, float smooth_value)
{
    assert(r->commands.combination_aabb == nullptr);
    assert(smooth_value >= 0.f);

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->type = pack_type(combination_begin, fill_solid);
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;

        float* k = r->commands.data_buffer.NewElement(); // just one float for the smooth
        r->commands.combination_aabb = r->commands.aabb_buffer.NewElement();
        
        if (r->commands.combination_aabb != nullptr && k != nullptr)
        {
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), smooth_value);
            *k = smooth_value;
            r->rasterizer.smooth_value = smooth_value;

            // reserve a aabb that we're going to update depending on the coming shapes
            *r->commands.combination_aabb = invalid_aabb();
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_end_combination(struct renderer* r, bool outline)
{
    assert(r->commands.combination_aabb != nullptr);

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->type = pack_type(combination_end, outline ? fill_outline : fill_solid);
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;

        // we put also the smooth value as we traverse the list in reverse order on the gpu
        float* k = r->commands.data_buffer.NewElement();

        quantized_aabb* aabb = r->commands.aabb_buffer.NewElement();
        if (aabb != nullptr && k != nullptr)
        {
            *aabb = *r->commands.combination_aabb;
            *k = r->rasterizer.smooth_value;
            r->commands.combination_aabb = nullptr;
            r->rasterizer.smooth_value = 0.f;
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
static inline float draw_cmd_aabb_bump(struct renderer* r, enum sdf_operator op)
{
    if (op == op_blend)
        return max(r->rasterizer.aa_width, r->rasterizer.smooth_value);
    else
        return r->rasterizer.aa_width;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_disc(struct renderer* r, vec2 center, float radius, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    thickness *= .5f;
    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_disc, fillmode);

        float* data = r->commands.data_buffer.NewMultiple((fillmode == fill_hollow) ? 4 : 3);
        quantized_aabb* aabb = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabb != nullptr)
        {
            center = ortho_to_screen_space(&r->view_proj, center);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), radius, thickness);

            float max_radius = radius + draw_cmd_aabb_bump(r, op);

            if (fillmode == fill_hollow)
            {
                max_radius += thickness;
                write_float(data, center.x, center.y, radius, thickness);
            }
            else
                write_float(data, center.x, center.y, radius);

            write_aabb(aabb, center.x - max_radius, center.y - max_radius, center.x + max_radius, center.y + max_radius);
            merge_aabb(r->commands.combination_aabb, aabb);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_orientedbox(struct renderer* r, vec2 p0, vec2 p1, float width, float roundness, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    if (vec2_similar(p0, p1, VEC2_EASY_EPSILON))
        return;

    thickness *= .5f;

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_oriented_box, fillmode);

        float* data = r->commands.data_buffer.NewMultiple(6);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            float roundness_thickness = (fillmode == fill_hollow) ? thickness : roundness;

            p0 = ortho_to_screen_space(&r->view_proj, p0);
            p1 = ortho_to_screen_space(&r->view_proj, p1);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), width, roundness_thickness);

            aabb bb = aabb_from_rounded_obb(p0, p1, width, roundness_thickness + draw_cmd_aabb_bump(r, op));
            write_float(data, p0.x, p0.y, p1.x, p1.y, width, roundness_thickness);
            write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_line(struct renderer* r, vec2 p0, vec2 p1, float width, draw_color color, enum sdf_operator op)
{
    renderer_draw_orientedbox(r, p0, p1, width, 0.f, 0.f, fill_solid, color, op);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_arrow(struct renderer* r, vec2 p0, vec2 p1, float width, draw_color color)
{
    float ratio = float_min((width * 3.f) / vec2_distance(p0, p1), 0.15f);
    vec2 delta = vec2_scale(vec2_sub(p0, p1), ratio);
    vec2 arrow_edge0 = vec2_add(p1, vec2_add(delta, vec2_skew(delta)));
    vec2 arrow_edge1 = vec2_add(p1, vec2_sub(delta, vec2_skew(delta)));

    renderer_draw_line(r, p0, p1, width, color, op_blend);
    renderer_draw_line(r, p1, arrow_edge0, width, color, op_blend);
    renderer_draw_line(r, p1, arrow_edge1, width, color, op_blend);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_arrow_solid(struct renderer* r, vec2 p0, vec2 p1, float width, draw_color color)
{
    float ratio = float_min((width * 3.f) / vec2_distance(p0, p1), 0.15f);
    vec2 delta = vec2_scale(vec2_sub(p0, p1), ratio);
    vec2 arrow_edge0 = vec2_add(p1, vec2_add(delta, vec2_skew(delta)));
    vec2 arrow_edge1 = vec2_add(p1, vec2_sub(delta, vec2_skew(delta)));

    renderer_draw_line(r, p0, vec2_add(p1, delta), width, color, op_blend);
    renderer_draw_triangle(r, p1, arrow_edge0, arrow_edge1, 0.f, 0.f, fill_solid, color, op_blend);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_doublearrow(struct renderer* r, vec2 p0, vec2 p1, float width, draw_color color)
{
    float ratio = float_min((width * 3.f) / vec2_distance(p0, p1), 0.15f);
    vec2 delta = vec2_scale(vec2_sub(p0, p1), ratio);
    vec2 arrow_edge0 = vec2_add(p1, vec2_add(delta, vec2_skew(delta)));
    vec2 arrow_edge1 = vec2_add(p1, vec2_sub(delta, vec2_skew(delta)));

    renderer_draw_line(r, p0, p1, width, color, op_blend);
    renderer_draw_line(r, p1, arrow_edge0, width, color, op_blend);
    renderer_draw_line(r, p1, arrow_edge1, width, color, op_blend);

    arrow_edge0 = vec2_sub(p0, vec2_add(delta, vec2_skew(delta)));
    arrow_edge1 = vec2_sub(p0, vec2_sub(delta, vec2_skew(delta)));
    renderer_draw_line(r, p0, arrow_edge0, width, color, op_blend);
    renderer_draw_line(r, p0, arrow_edge1, width, color, op_blend);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_doublearrow_solid(struct renderer* r, vec2 p0, vec2 p1, float width, draw_color color)
{
    float ratio = float_min((width * 3.f) / vec2_distance(p0, p1), 0.15f);
    vec2 delta = vec2_scale(vec2_sub(p0, p1), ratio);
    vec2 arrow_edge0 = vec2_add(p1, vec2_add(delta, vec2_skew(delta)));
    vec2 arrow_edge1 = vec2_add(p1, vec2_sub(delta, vec2_skew(delta)));

    renderer_draw_line(r, vec2_sub(p0, delta), vec2_add(p1, delta), width, color, op_blend);
    renderer_draw_triangle(r, p1, arrow_edge0, arrow_edge1, 0.f, 0.f, fill_solid, color, op_blend);

    arrow_edge0 = vec2_sub(p0, vec2_add(delta, vec2_skew(delta)));
    arrow_edge1 = vec2_sub(p0, vec2_sub(delta, vec2_skew(delta)));
    renderer_draw_triangle(r, p0, arrow_edge0, arrow_edge1, 0.f, 0.f, fill_solid, color, op_blend);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_ellipse(struct renderer* r, vec2 p0, vec2 p1, float width, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    if (vec2_similar(p0, p1, VEC2_EASY_EPSILON))
        return;

    if (width <= VEC2_EASY_EPSILON)
        renderer_draw_orientedbox(r, p0, p1, 0.f, 0.f, -1.f, fill_solid, color, op);
    else
    {
        thickness = float_max(thickness * .5f, 0.f);
        draw_command* cmd = r->commands.buffer.NewElement();
        if (cmd != nullptr)
        {
            cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
            cmd->color = color;
            cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
            cmd->op = op;
            cmd->type = pack_type(primitive_ellipse, fillmode);

            float* data = r->commands.data_buffer.NewMultiple((fillmode == fill_hollow) ? 6 : 5);
            quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
            if (data != nullptr && aabox != nullptr)
            {
                p0 = ortho_to_screen_space(&r->view_proj, p0);
                p1 = ortho_to_screen_space(&r->view_proj, p1);
                distance_screen_space(ortho_get_radius_scale(&r->view_proj), width, thickness);

                aabb bb = aabb_from_rounded_obb(p0, p1, width, draw_cmd_aabb_bump(r, op) + thickness);
                if (fillmode == fill_hollow)
                    write_float(data, p0.x, p0.y, p1.x, p1.y, width, thickness);
                else
                    write_float(data, p0.x, p0.y, p1.x, p1.y, width);

                write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
                merge_aabb(r->commands.combination_aabb, aabox);
                return;
            }
            r->commands.buffer.RemoveLast();
        }
        log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_triangle(struct renderer* r, vec2 p0, vec2 p1, vec2 p2, float roundness, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    // exclude invalid triangle
    if (vec2_similar(p0, p1, VEC2_EASY_EPSILON) || vec2_similar(p2, p1, VEC2_EASY_EPSILON) || vec2_similar(p0, p2, VEC2_EASY_EPSILON))
        return;

    thickness *= .5f;

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_triangle, fillmode);

        float* data = r->commands.data_buffer.NewMultiple(7);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            p0 = ortho_to_screen_space(&r->view_proj, p0);
            p1 = ortho_to_screen_space(&r->view_proj, p1);
            p2 = ortho_to_screen_space(&r->view_proj, p2);
            
            float roundness_thickness = (fillmode != fill_hollow) ? roundness : thickness;
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), roundness_thickness);

            aabb bb = aabb_from_triangle(p0, p1, p2);
            aabb_grow(&bb, vec2_splat(roundness_thickness + draw_cmd_aabb_bump(r, op)));
            write_float(data, p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, roundness_thickness);
            write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_pie(struct renderer* r, vec2 center, vec2 point, float aperture, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    if (vec2_similar(center, point, VEC2_EASY_EPSILON))
        return;

    if (aperture <= VEC2_EASY_EPSILON)
        return;
    
    aperture = float_clamp(aperture, 0.f, VEC2_PI);
    thickness = float_max(thickness * .5f, 0.f);

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_pie, fillmode);

        float* data = r->commands.data_buffer.NewMultiple((fillmode != fill_hollow) ? 7 : 8);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            center = ortho_to_screen_space(&r->view_proj, center);
            point = ortho_to_screen_space(&r->view_proj, point);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj),  thickness);

            vec2 direction = point - center;
            float radius = vec2_normalize(&direction);
            
            aabb bb = aabb_from_circle(center, radius);
            aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r, op)));

            if (fillmode != fill_hollow)
                write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture));
            else
                write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
                
            write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_arc_from_circle(struct renderer* r, vec2 p0, vec2 p1, vec2 p2, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    vec2 center, direction;
    float aperture, radius;
    arc_from_points(p0, p1, p2, &center, &direction, &aperture, &radius);

    // colinear points
    if (radius<0.f)
    {
        renderer_draw_orientedbox(r, p0, p2, thickness, 0.f, -1.f, fill_solid, color, op);
        return;
    }

    renderer_draw_arc(r, center, direction, aperture, radius, thickness, fillmode, color, op);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_arc(struct renderer* r, vec2 center, vec2 direction, float aperture, float radius, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    // don't support fill_hollow
    if (fillmode == fill_hollow)
        fillmode = fill_solid;

    aperture = float_clamp(aperture, 0.f, VEC2_PI);
    thickness = float_max(thickness, 0.f);

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_arc, fillmode);

        float* data = r->commands.data_buffer.NewMultiple(8);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            center = ortho_to_screen_space(&r->view_proj, center);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), radius, thickness);

            aabb bb = aabb_from_circle(center, radius);
            aabb_grow(&bb, vec2_splat(thickness + draw_cmd_aabb_bump(r, op)));

            write_float(data, center.x, center.y, radius, direction.x, direction.y, sinf(aperture), cosf(aperture), thickness);
            write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_unevencapsule(struct renderer* r, vec2 p0, vec2 p1, float radius0, float radius1, float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    float delta = vec2_distance(p0, p1);

    if (radius0>radius1 && radius0 > radius1 + delta)
    {
        renderer_draw_disc(r, p0, radius0, thickness, fillmode, color, op);
        return;
    }

    if (radius1>radius0 && radius1 > radius0 + delta)
    {
        renderer_draw_disc(r, p1, radius1, thickness, fillmode, color, op);
        return;
    }

    thickness = float_max(thickness * .5f, 0.f);
    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_uneven_capsule, fillmode);

        float* data = r->commands.data_buffer.NewMultiple((fillmode != fill_hollow) ? 6 : 7);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            p0 = ortho_to_screen_space(&r->view_proj, p0);
            p1 = ortho_to_screen_space(&r->view_proj, p1);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), radius0, radius1, thickness);

            aabb bb = aabb_from_capsule(p0, p1, float_max(radius0, radius1));
            aabb_grow(&bb, vec2_splat(draw_cmd_aabb_bump(r, op) + thickness));

            if (fillmode != fill_hollow)
                write_float(data, p0.x, p0.y, p1.x, p1.y, radius0, radius1);
            else
                write_float(data, p0.x, p0.y, p1.x, p1.y, radius0, radius1, thickness);

            write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_trapezoid(struct renderer* r, vec2 p0, vec2 p1, float radius0, float radius1, float roundness,
                             float thickness, enum primitive_fillmode fillmode, draw_color color, enum sdf_operator op)
{
    if (vec2_similar(p0, p1, vec2_relative_epsilon(vec2_max(p0, p1), VEC2_EASY_EPSILON)))
        return;

    if (radius0 < VEC2_EASY_EPSILON && radius1 < VEC2_EASY_EPSILON)
        return;

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op;
        cmd->type = pack_type(primitive_trapezoid, fillmode);

        float* data = r->commands.data_buffer.NewMultiple((fillmode != fill_hollow) ? 7 : 8);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            float roundness_thickness = (fillmode == fill_hollow) ? thickness : roundness;
            p0 = ortho_to_screen_space(&r->view_proj, p0);
            p1 = ortho_to_screen_space(&r->view_proj, p1);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), radius0, radius1, roundness_thickness);

            aabb bb = aabb_from_trapezoid(p0, p1, radius0, radius1);
            aabb_grow(&bb, vec2_splat(draw_cmd_aabb_bump(r, op) + roundness_thickness));

            write_float(data, p0.x, p0.y, p1.x, p1.y, radius0, radius1, roundness_thickness);
            write_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
            merge_aabb(r->commands.combination_aabb, aabox);

            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_box(struct renderer* r, float x0, float y0, float x1, float y1, float radius, draw_color color)
{
    if (x0>x1) swap(x0, x1);
    if (y0>y1) swap(y0, y1);

    vec2 p0 = vec2_set(x0, y0);
    vec2 p1 = vec2_set(x1, y1);

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op_add;
        cmd->type = pack_type(primitive_aabox, fill_solid);

        float* data = r->commands.data_buffer.NewMultiple(5);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            p0 = ortho_to_screen_space(&r->view_proj, p0);
            p1 = ortho_to_screen_space(&r->view_proj, p1);
            radius *= ortho_get_radius_scale(&r->view_proj);
            
            vec2 center = vec2_scale(vec2_add(p0, p1), .5f);
            vec2 half_extents = vec2_scale(vec2_sub(p1, p0), .5f);

            write_float(data, center.x, center.y, half_extents.x, half_extents.y, radius);
            write_aabb(aabox, p0.x, p0.y, p1.x, p1.y);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_blurred_box(struct renderer* r, vec2 center, vec2 half_extents, float roundness, draw_color color)
{
    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        *cmd = 
        {
            .clip_index = (uint8_t) (r->commands.cliprects_buffer.GetNumElements()-1),
            .color = color,
            .data_index = (uint32_t) r->commands.data_buffer.GetNumElements(),
            .op = op_add,
            .type = pack_type(primitive_blurred_box, fill_solid)
        };

        float* data = r->commands.data_buffer.NewMultiple(5);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            center = ortho_to_screen_space(&r->view_proj, center);
            distance_screen_space(ortho_get_radius_scale(&r->view_proj), half_extents.x, half_extents.y, roundness);
            write_float(data, center.x, center.y, half_extents.x, half_extents.y, roundness);
            write_aabb(aabox, center.x - half_extents.x - roundness, center.y - half_extents.y - roundness,
                              center.x + half_extents.x + roundness, center.y + half_extents.y + roundness);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_aabb(struct renderer* r, aabb box, draw_color color)
{
    renderer_draw_box(r, box.min.x, box.min.y, box.max.x, box.max.y, 0.f, color);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_char(struct renderer* r, float x, float y, char c, draw_color color)
{
    if (c < r->font.desc.first_glyph || c > (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        return;

    draw_command* cmd = r->commands.buffer.NewElement();
    if (cmd != nullptr)
    {
        uint32_t glyph_index = c - r->font.desc.first_glyph;
        cmd->clip_index = (uint8_t) r->commands.cliprects_buffer.GetNumElements()-1;
        cmd->color = color;
        cmd->data_index = (uint32_t)r->commands.data_buffer.GetNumElements();
        cmd->op = op_blend;
        cmd->type = primitive_char;
        cmd->custom_data = (uint8_t) glyph_index;

        const glyph& glyph = r->font.desc.glyphs[glyph_index];
        x += glyph.bearing_x;
        y += glyph.bearing_y + r->font.desc.font_height;
        float glyph_width = float(glyph.x1 - glyph.x0);
        float glyph_height = float(glyph.y1 - glyph.y0);

        float* data = r->commands.data_buffer.NewMultiple(2);
        quantized_aabb* aabox = r->commands.aabb_buffer.NewElement();
        if (data != nullptr && aabox != nullptr)
        {
            write_float(data, x, y);
            write_aabb(aabox, x, y, x + glyph_width, y + glyph_height);
            merge_aabb(r->commands.combination_aabb, aabox);
            return;
        }
        r->commands.buffer.RemoveLast();
    }
    log_warn("out of draw commands/draw data buffer, expect graphical artefacts");
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_draw_text(struct renderer* r, float x, float y, const char* text, draw_color color)
{
    vec2 p = ortho_to_screen_space(&r->view_proj, vec2_set(x, y));
    for(const char *c = text; *c != 0; c++)
    {
        if (*c >= r->font.desc.first_glyph && *c <= (r->font.desc.first_glyph + r->font.desc.num_glyphs))
        {
            renderer_draw_char(r, p.x, p.y, *c, color);
            uint32_t glyph_index = *c - r->font.desc.first_glyph;
            p.x += r->font.desc.glyphs[glyph_index].advance_x;
        }
        else
            p.x += r->font.desc.glyphs['_'- r->font.desc.first_glyph].advance_x * .65f;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
float renderer_text_height(struct renderer* r)
{
    return r->font.desc.font_height;
}

//----------------------------------------------------------------------------------------------------------------------------
float renderer_text_width(struct renderer* r, const char* text)
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
void renderer_set_clear_color(struct renderer* r, draw_color color)
{
    r->rasterizer.clear_color.x = color.r();
    r->rasterizer.clear_color.y = color.g();
    r->rasterizer.clear_color.z = color.b();
    r->rasterizer.clear_color.w = color.a();
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_set_cliprect(struct renderer* r, uint16_t min_x, uint16_t min_y, uint16_t max_x, uint16_t max_y)
{
    // avoid redundant clip rect
    if (r->commands.cliprects_buffer.GetNumElements()>0)
    {
        clip_rect* rect = r->commands.cliprects_buffer.LastElement();

        if (rect->min_x == min_x && rect->min_y == min_y &&
            rect->max_x == max_x && rect->max_y == max_y)
            return;
    }

    if (r->commands.cliprects_buffer.GetNumElements() < MAX_CLIPS)
        *r->commands.cliprects_buffer.NewElement() = (clip_rect) {.min_x = (float)min_x, .min_y = (float)min_y, .max_x = (float)max_x, .max_y = (float)max_y};
    else
        log_error("too many clip rectangle! maximum is %d", MAX_CLIPS);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_set_cliprect_relative(struct renderer * r, aabb const* box)
{
    vec2 top_left = ortho_to_screen_space(&r->view_proj, box->min) + vec2_splat(.5f);
    vec2 bottom_right = ortho_to_screen_space(&r->view_proj, box->max) + vec2_splat(.5f);

    renderer_set_cliprect(r, (uint16_t) top_left.x, (uint16_t) top_left.y, (uint16_t) bottom_right.x, (uint16_t) bottom_right.y);
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_set_culling_debug(struct renderer* r, bool b)
{
    r->m_CullingDebug = b;
}

//----------------------------------------------------------------------------------------------------------------------------
void renderer_set_viewproj(struct renderer* r, const struct view_proj* vp)
{
    r->view_proj = *vp;
    ortho_set_window_size(&r->view_proj, vec2_set((float)r->rasterizer.width, (float)r->rasterizer.height));
}

