#define HL_NAME(n) native_args_##n
#include <hl.h>

// more than 8 integer arguments : the 9th and following are on the stack
// (4 bytes slots for int32 on Apple ARM64, 8 bytes slots in the other ABIs)
HL_PRIM int HL_NAME(sum10)( int a, int b, int c, int d, int e, int f, int g, int h, int i, int j ) {
	return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}
HL_PRIM int HL_NAME(sum12)( int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k, int l ) {
	return a + b + c + d + e + f + g + h + i * 100 + j * 1000 + k * 10000 + l * 100000;
}
// mixed sizes past the registers, natural alignment
HL_PRIM double HL_NAME(mixed)( int a, int b, int c, int d, int e, int f, int g, int h, bool p9, int p10, unsigned char p11, unsigned short p12, double p13, float p14, vbyte *p15, double a1, double a2, double a3, double a4, double a5, double a6, double a7, double a8, double fs9, float fs10, int p16 ) {
	return a+b+c+d+e+f+g+h + (p9?1000.:0.) + p10*2. + p11*3. + p12*4. + p13*5. + p14*6. + (p15 ? 7.0 : 0.0) + a1+a2+a3+a4+a5+a6+a7+a8 + fs9*100. + fs10*1000. + p16*11.;
}
HL_PRIM vdynamic *HL_NAME(dyn9)( int a, int b, int c, int d, int e, int f, int g, int h, vdynamic *i ) {
	return i;
}
#define _ARGS10 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32
DEFINE_PRIM(_I32, sum10, _ARGS10);
DEFINE_PRIM(_I32, sum12, _ARGS10 _I32 _I32);
DEFINE_PRIM(_F64, mixed, _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BOOL _I32 _I8 _I16 _F64 _F32 _BYTES _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F32 _I32);
DEFINE_PRIM(_DYN, dyn9, _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _DYN);
