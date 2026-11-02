// Expected output of probe.bin: index and value of each word at 0x80000.
#include <stdint.h>
#include <stdio.h>
static uint32_t rotl7(uint32_t x){return (x<<7)|(x>>25);}
static uint32_t rbit_loop(uint32_t x){uint32_t r=0;for(int b=0;b<32;b++)r|=((x>>b)&1u)<<(31-b);return r;}
int main(void){
  uint32_t x=0x12345678;
  uint32_t e[14]={rotl7(x),31,0,8,__builtin_popcount(x),32,__builtin_bswap32(x),rbit_loop(x),
                  31-5,0x100|(3u<<8),0x80000000u>>4,x,0x1010101u*0x12345678u,rotl7(rotl7(1))};
  for(int i=0;i<14;i++)printf("%d %08x\n",i,e[i]);
  return 0;
}
