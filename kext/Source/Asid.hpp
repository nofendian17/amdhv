/*
 * Asid.hpp - allocates SVM address space identifiers (ASIDs).
 *
 * ASID 0 belongs to the host. Every VM gets its own ASID, so its guest TLB entries are tagged and
 * survive VMRUN without colliding with the host's entries (ASID 0) or another VM's. That is what
 * lets a vCPU stop flushing the whole TLB on every entry (see Vm.cpp).
 *
 * Pure C++ and tested on the host (tests/asid_test.cpp); the kext wraps it with a lock.
 */
#ifndef AMDHV_ASID_HPP
#define AMDHV_ASID_HPP

#include <stdint.h>

namespace amdhv {
namespace asid {

constexpr uint32_t kMax = 65536;   /* 8 KiB of bitmap; a larger CPUID count is capped to this */

class Pool {
public:
    /* `count` is CPUID Fn8000_000A EBX: the number of ASIDs, including the host's 0. */
    void init(uint32_t count)
    {
        for (uint32_t i = 0; i < kMax / 64; i++)
            fBits[i] = 0;
        fCount = count < kMax ? count : kMax;
        fUsed  = 0;
        fNext  = 1;
        if (fCount > 0) {
            fBits[0] = 1;              /* ASID 0 is the host's and is never handed out */
            fUsed    = 1;
        }
    }

    /* A free ASID, or 0 when none is left (or the CPU offers only the host's). */
    uint32_t alloc()
    {
        if (fCount < 2 || fUsed >= fCount)
            return 0;
        const uint32_t span = fCount - 1;          /* ids 1 .. fCount-1 */
        for (uint32_t scanned = 0; scanned < span; scanned++) {
            const uint32_t id = 1 + (fNext - 1 + scanned) % span;
            const uint64_t bit = 1ull << (id % 64);
            if (!(fBits[id / 64] & bit)) {
                fBits[id / 64] |= bit;
                fUsed++;
                fNext = id + 1;
                if (fNext >= fCount)
                    fNext = 1;
                return id;
            }
        }
        return 0;
    }

    /* Returns `id` to the pool. Idempotent and safe for 0/out-of-range: those are ignored. */
    void free(uint32_t id)
    {
        if (id == 0 || id >= fCount)
            return;
        const uint64_t bit = 1ull << (id % 64);
        if (!(fBits[id / 64] & bit))
            return;                                /* double free: the count stays right */
        fBits[id / 64] &= ~bit;
        fUsed--;
    }

    uint32_t capacity() const { return fCount; }   /* includes the host's ASID 0 */
    uint32_t inUse() const { return fUsed; }

private:
    uint64_t fBits[kMax / 64];
    uint32_t fCount;
    uint32_t fUsed;
    uint32_t fNext;
};

} // namespace asid
} // namespace amdhv

#endif /* AMDHV_ASID_HPP */
