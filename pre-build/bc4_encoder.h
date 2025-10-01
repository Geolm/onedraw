#ifndef __BC4_ENCODER_H__
#define __BC4_ENCODER_H__

#include <stdint.h>

// code extracted from stb_dxt.h
void bc4_encode(const uint8_t* input, uint8_t* output, uint32_t width, uint32_t height);

#endif