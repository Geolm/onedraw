#ifndef __BIN2H_H__
#define __BIN2H_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool bin2h(const char* filename, const char* variable, const void* buffer, size_t length);
bool uint2h(const char* filename, const char* variable, const uint32_t* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif // __BIN2H_H__