/*
 * SpacePool.hpp - allocates guest address space IDs (hv_vm_space_t).
 *
 * Space 0 is the default address space of the VM and always exists. Every other space has its own
 * nested page table root, so a vCPU can switch address spaces without rebuilding the tables
 * (hv_vcpu_set_space). The IDs are the ones Hypervisor.framework hands back to the client, so they
 * are not reused while a space is alive. A destroyed ID is not handed out again until the counter
 * wraps, which keeps a stale hv_vcpu_set_space call from silently binding a new space.
 *
 * Pure C++ and tested on the host (tests/space_pool_test.cpp); the kext wraps it with the page-table
 * lock.
 */
#ifndef AMDHV_SPACE_POOL_HPP
#define AMDHV_SPACE_POOL_HPP

#include <stdint.h>

namespace amdhv {
namespace space {

constexpr unsigned kMax = 64;      /* one nested page table root each; plenty for VZ's use */

class Pool {
public:
    Pool() { reset(); }

    void reset()
    {
        for (unsigned i = 0; i < kMax; i++)
            fIds[i] = 0;
        fUsed = 0;
        fNext = 1;
    }

    /*
     * Allocates an ID and returns the slot whose page table it names. Fails when the pool is full.
     * The caller initialises the table in `*slot`; if that fails it must call release().
     */
    bool create(uint32_t *id, unsigned *slot)
    {
        if (!id || !slot || fUsed >= kMax)
            return false;

        unsigned freeSlot = kMax;
        for (unsigned i = 0; i < kMax; i++) {
            if (!fIds[i]) {
                freeSlot = i;
                break;
            }
        }
        if (freeSlot == kMax)
            return false;

        /* Skip IDs that are still alive; fNext is never 0, so space 0 stays the default. */
        uint32_t candidate = fNext;
        while (index(candidate) >= 0) {
            candidate++;
            if (!candidate)
                candidate = 1;
        }

        fIds[freeSlot] = candidate;
        fNext = candidate + 1;
        if (!fNext)
            fNext = 1;
        fUsed++;
        *id = candidate;
        *slot = freeSlot;
        return true;
    }

    /* The slot of `id`, or -1 for space 0, an unknown ID or an already destroyed one. */
    int index(uint32_t id) const
    {
        if (!id)
            return -1;
        for (unsigned i = 0; i < kMax; i++)
            if (fIds[i] == id)
                return (int)i;
        return -1;
    }

    /* True for the default space 0 and for every live space. */
    bool valid(uint32_t id) const { return id == 0 || index(id) >= 0; }

    /* Returns the ID to the pool. Idempotent; space 0 and unknown IDs are ignored. */
    bool release(uint32_t id)
    {
        const int slot = index(id);
        if (slot < 0)
            return false;
        fIds[slot] = 0;
        fUsed--;
        return true;
    }

    unsigned used() const { return fUsed; }

private:
    uint32_t fIds[kMax];
    uint32_t fNext;
    unsigned fUsed;
};

} // namespace space
} // namespace amdhv

#endif /* AMDHV_SPACE_POOL_HPP */
