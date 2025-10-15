#ifndef __SHADER__READER__
#define __SHADER__READER__

#ifdef __cplusplus
extern "C" {
#endif

// returns a zero-terminated string or NULL if the file is not found. Caller needs to free the buffer
char* read_shader(const char* filename);

// merge all includes and the shader file in one buffer. Caller needs to free the buffer
// definitely not bullet proofed function, don't run this on invalid shader file
char* read_shader_include(const char* path, const char* filename);

#ifdef __cplusplus
}
#endif

#endif
