#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"

#define UNUSED_VARIABLE(a) (void)(a)

// ---------------------------------------------------------------------------------------------------------------------------
sapp_desc sokol_main(int argc, char* argv[])
{
    UNUSED_VARIABLE(argc);
    UNUSED_VARIABLE(argv);

    return (sapp_desc) 
    {
        .width = 1280,
        .height = 720,
    };
}

