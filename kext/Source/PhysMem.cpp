#include <IOKit/IOLib.h>

#include "Log.hpp"
#include "PhysMem.hpp"

namespace amdhv {

bool physAlloc(PhysMem *m, size_t size)
{
    bzero(m, sizeof(*m));
    size = (size + 4095) & ~(size_t)4095;

    /* kIOMemoryMapperNone: these pages are addressed by physical address, never DMA-mapped. Without
     * it IOBufferMemoryDescriptor calls IOMapper::checkForSystemMapper(), which during early boot
     * waits for the system mapper and reports "waitForSystemMapper" with the calling kext. */
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, size,
        0x000FFFFFFFFFF000ULL);
    if (!md) {
        AMDHV_ERR("physAlloc(%zu): out of contiguous memory", size);
        return false;
    }

    void *va = md->getBytesNoCopy();
    bzero(va, size);

    IOByteCount len = 0;
    addr64_t pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
    if (!pa || (pa & 0xFFF) || len < size) {
        AMDHV_ERR("physAlloc(%zu): bad physical segment pa=0x%llx len=%llu", size,
                (unsigned long long)pa, (unsigned long long)len);
        md->release();
        return false;
    }

    m->md   = md;
    m->va   = va;
    m->pa   = pa;
    m->size = size;
    return true;
}

void physFree(PhysMem *m)
{
    if (m->md)
        m->md->release();
    bzero(m, sizeof(*m));
}

} // namespace amdhv
