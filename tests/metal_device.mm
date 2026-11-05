#import <Metal/Metal.h>

// The system default device is a shared singleton : it is handed to onedraw as-is, nobody releases it
extern "C" void* metal_default_device(void)
{
    return (void*)MTLCreateSystemDefaultDevice();
}
