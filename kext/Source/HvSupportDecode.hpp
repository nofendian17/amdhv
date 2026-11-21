/*
 * HvSupportDecode.hpp - locate kern.hv_support from hv_get_support's machine code.
 *
 * hv_get_support() is normally `push rbp; mov rbp,rsp; mov eax,[rip+disp]; pop rbp; ret`,
 * but a build without the frame pointer omits the prologue/epilogue. Recognise both so a
 * different kernel build is not silently mis-decoded. The returned address is never
 * dereferenced here: the caller must verify it against the sysctl OID and against
 * hv_get_support() itself before writing a single byte.
 */
#ifndef AMDHV_HV_SUPPORT_DECODE_HPP
#define AMDHV_HV_SUPPORT_DECODE_HPP

#include <stdint.h>

namespace amdhv {
namespace hv {

inline const void *decodeSupportPtr(const uint8_t *f)
{
    static const uint8_t kFrame[6] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x05};
    static const uint8_t kPlain[2] = {0x8b, 0x05};
    unsigned mov;
    if (__builtin_memcmp(f, kFrame, sizeof(kFrame)) == 0 && f[10] == 0x5d && f[11] == 0xc3)
        mov = 4;                 /* push rbp; mov rbp,rsp; mov eax,[rip+disp32] */
    else if (__builtin_memcmp(f, kPlain, sizeof(kPlain)) == 0 && f[6] == 0xc3)
        mov = 0;                 /* mov eax,[rip+disp32]; ret */
    else
        return nullptr;

    int32_t disp = 0;
    __builtin_memcpy(&disp, f + mov + 2, sizeof(disp));
    return f + mov + 6 + disp;   /* RIP after the 6-byte mov */
}

} // namespace hv
} // namespace amdhv

#endif /* AMDHV_HV_SUPPORT_DECODE_HPP */
