#ifndef __COMPARE_H__
#define __COMPARE_H__

#include <stdint.h>
#include <stdbool.h>

// Swaps the b and r channels of a BGRA capture buffer in place
void test_bgra_to_rgba(uint8_t* pixels, uint32_t width, uint32_t height);

// Regression check: compares [pixels] against the committed reference image matching its size.
// Identical images are a clean pass; otherwise the PSNR (all RGBA channels) must stay above the
// threshold.
//      [compare]      true : a mismatch makes the process exit non-zero
//      [update_refs]  true : the matching reference is rewritten from the current capture
void test_compare_reference(const uint8_t* pixels, uint32_t width, uint32_t height, bool compare, bool update_refs);

#endif
