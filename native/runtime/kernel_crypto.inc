// Xbox SHA-1/HMAC ABI for original XAPI save signing. Locally authored
// SHA-1 per FIPS 180-4, sections 5/6.1. Context layout from Cxbx 585c49a:
// 24 reserved bytes followed by SHA1_CTX (state[5], bit count[2], block[64]).
// Keeping the complete state in guest memory permits XAPI to copy contexts.
// No host pointer/handle is embedded in guest data. See docs/SOURCES.md.
struct NativeSha1State { uint32_t h[5], bits[2]; uint8_t block[64]; };
static_assert(sizeof(NativeSha1State) == 92);
uint32_t ShaRol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
void ShaBlock(NativeSha1State& s, const uint8_t* block) {
    uint32_t w[80];
    for (unsigned i=0;i<16;++i) w[i]=(uint32_t(block[i*4])<<24)|(uint32_t(block[i*4+1])<<16)|(uint32_t(block[i*4+2])<<8)|block[i*4+3];
    for (unsigned i=16;i<80;++i) w[i]=ShaRol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=s.h[0], b=s.h[1], c=s.h[2], d=s.h[3], e=s.h[4];
    for (unsigned i=0;i<80;++i) {
        uint32_t f,k;
        if(i<20) { f=(b&c)|(~b&d); k=0x5a827999; }
        else if(i<40) { f=b^c^d; k=0x6ed9eba1; }
        else if(i<60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdc; }
        else { f=b^c^d; k=0xca62c1d6; }
        uint32_t next=ShaRol(a,5)+f+e+k+w[i]; e=d; d=c; c=ShaRol(b,30); b=a; a=next;
    }
    s.h[0]+=a; s.h[1]+=b; s.h[2]+=c; s.h[3]+=d; s.h[4]+=e;
}
void ShaInit(NativeSha1State& s) {
    s={}; s.h[0]=0x67452301; s.h[1]=0xefcdab89; s.h[2]=0x98badcfe; s.h[3]=0x10325476; s.h[4]=0xc3d2e1f0;
}
void ShaUpdate(NativeSha1State& s, const uint8_t* data, uint32_t length) {
    unsigned used=(s.bits[0]>>3)&63;
    uint64_t bits=(uint64_t(s.bits[1])<<32)|s.bits[0]; bits+=uint64_t(length)*8;
    s.bits[0]=uint32_t(bits); s.bits[1]=uint32_t(bits>>32);
    while(length) {
        unsigned n=std::min<uint32_t>(64-used,length);
        memcpy(s.block+used,data,n); used+=n; data+=n; length-=n;
        if(used==64) { ShaBlock(s,s.block); used=0; }
    }
}
void ShaFinal(NativeSha1State& s, uint8_t* digest) {
    uint64_t bits=(uint64_t(s.bits[1])<<32)|s.bits[0];
    uint8_t pad[72]={0x80}; unsigned used=(s.bits[0]>>3)&63;
    unsigned padding=used<56 ? 56-used : 120-used;
    for(unsigned i=0;i<8;++i) pad[padding+i]=uint8_t(bits>>(56-i*8));
    ShaUpdate(s,pad,padding+8);
    for(unsigned i=0;i<20;++i) digest[i]=uint8_t(s.h[i/4]>>(24-(i%4)*8));
    s={};
}
void __stdcall XcSHAInit(uint8_t* context) {
    if(!context) Stop(290,"XcSHAInit null context");
    NativeSha1State s; ShaInit(s); memcpy(context+24,&s,sizeof s);
}
void __stdcall XcSHAUpdate(uint8_t* context, const uint8_t* data, uint32_t length) {
    if(!context || (length && !data)) Stop(290,"XcSHAUpdate null input");
    NativeSha1State s; memcpy(&s,context+24,sizeof s); ShaUpdate(s,data,length); memcpy(context+24,&s,sizeof s);
}
void __stdcall XcSHAFinal(uint8_t* context, uint8_t* digest) {
    if(!context || !digest) Stop(290,"XcSHAFinal null input");
    NativeSha1State s; memcpy(&s,context+24,sizeof s); ShaFinal(s,digest); memcpy(context+24,&s,sizeof s);
}
void __stdcall XcHMAC(const uint8_t* key, uint32_t key_length, const uint8_t* data, uint32_t length,
                      const uint8_t* data2, uint32_t length2, uint8_t* digest) {
    if((key_length && !key) || (length && !data) || (length2 && !data2) || !digest) Stop(290,"XcHMAC null input");
    // Xbox XcHMAC truncates keys over 64 bytes (Cxbx JumpedHMAC), unlike
    // standard HMAC's long-key prehash. Save keys here are sixteen bytes.
    key_length=std::min<uint32_t>(key_length,64);
    uint8_t inner_pad[64], outer_pad[64], inner[20];
    for(unsigned i=0;i<64;++i) { uint8_t k=i<key_length ? key[i] : 0; inner_pad[i]=k^0x36; outer_pad[i]=k^0x5c; }
    NativeSha1State s; ShaInit(s); ShaUpdate(s,inner_pad,64); ShaUpdate(s,data,length); ShaUpdate(s,data2,length2); ShaFinal(s,inner);
    ShaInit(s); ShaUpdate(s,outer_pad,64); ShaUpdate(s,inner,20); ShaFinal(s,digest);
}
