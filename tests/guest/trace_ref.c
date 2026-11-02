// Expected output of trace.bin: x after each step of two test_bits iterations.
#include <stdint.h>
#include <stdio.h>
static uint32_t rotl7(uint32_t x){return (x<<7)|(x>>25);}
int main(void){
  uint32_t x=0x12345678, r;
  for (int it=0; it<2; it++) {
    x=rotl7(x); printf("%08x\n",x);
    x^=__builtin_clz(x|1); printf("%08x\n",x);
    x+=__builtin_popcount(x); printf("%08x\n",x);
    x^=__builtin_bswap32(x)>>3; printf("%08x\n",x);
    r=0; for(int b=0;b<32;b++) r|=((x>>b)&1u)<<(31-b); printf("%08x\n",r);
    x+=r>>5; printf("%08x\n",x);
  }
  return 0;
}
