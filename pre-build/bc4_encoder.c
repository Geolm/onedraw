#include "bc4_encoder.h"
#include <assert.h>


// ---------------------------------------------------------------------------------------------------------------------------
// Alpha block compression (this is easy for a change)
static void stb__compress_bc4_block(const unsigned char *src, unsigned char *dest)
{
   int i,dist,bias,dist4,dist2,bits,mask;

   // find min/max color
   int mn,mx;
   mn = mx = src[0];

   for (i=1;i<16;i++)
   {
      if (src[i] < mn) mn = src[i];
      else if (src[i] > mx) mx = src[i];
   }

   // encode them
   dest[0] = (unsigned char)mx;
   dest[1] = (unsigned char)mn;
   dest += 2;

   // determine bias and emit color indices
   // given the choice of mx/mn, these indices are optimal:
   // http://fgiesen.wordpress.com/2009/12/15/dxt5-alpha-block-index-determination/
   dist = mx-mn;
   dist4 = dist*4;
   dist2 = dist*2;
   bias = (dist < 8) ? (dist - 1) : (dist/2 + 2);
   bias -= mn * 7;
   bits = 0,mask=0;

   for (i=0;i<16;i++) {
      int a = src[i]*7 + bias;
      int ind,t;

      // select index. this is a "linear scale" lerp factor between 0 (val=min) and 7 (val=max).
      t = (a >= dist4) ? -1 : 0; ind =  t & 4; a -= dist4 & t;
      t = (a >= dist2) ? -1 : 0; ind += t & 2; a -= dist2 & t;
      ind += (a >= dist);

      // turn linear scale into DXT index (0/1 are extremal pts)
      ind = -ind & 7;
      ind ^= (2 > ind);

      // write index
      mask |= ind << bits;
      if((bits += 3) >= 8) {
         *dest++ = (unsigned char)mask;
         mask >>= 8;
         bits -= 8;
      }
   }
}

// ---------------------------------------------------------------------------------------------------------------------------
static inline void fill_block(const uint8_t* input, uint32_t width, uint8_t* block)
{
   for(uint32_t y=0; y<4; ++y)
      for(uint32_t x=0; x<4; ++x)
         block[y * 4 + x] = input[y*width + x];
}

// ---------------------------------------------------------------------------------------------------------------------------
void bc4_encode(const uint8_t* input, uint8_t* output, uint32_t width, uint32_t height)
{
   assert((width%4) == 0);
   assert((height%4) == 0);

   for(uint32_t y=0; y<height; y+=4)
   {
      for(uint32_t x=0; x<width; x+=4)
      {
         uint8_t block[16];
         fill_block(&input[y * width + x], width, block);
         stb__compress_bc4_block(block, output);
         output += 8;
      }
   }
}