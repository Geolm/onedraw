#import <Metal/Metal.h>

// onedraw requires a m1/a14 (or later) gpu ; returns the shared system default device, or null
// when no such gpu is available (a null device makes the test skip instead of failing)
extern "C" void* metal_default_device(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();

    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7])
        return NULL;

    return (void*)device;
}
