/*
 * VmShared.cpp - the part of a VM all its vCPUs share: the nested page tables,
 * the per-VM address spaces and the per-task VM registry. Split out of Vm.cpp
 * so the vCPU run loop has a file of its own. No behaviour change.
 */
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#include "KernelDecls.hpp"
#include "Frontend.hpp"
#include "Asid.hpp"
#include "MsrMap.hpp"
#include "RunDeadline.hpp"
#include "SleepPolicy.hpp"
#include "TlbPolicy.hpp"
#include "X86.hpp"
#include "Log.hpp"
#include "SVM.hpp"
#include "Vm.hpp"
#include "ApicMmio.hpp"

#define super OSObject

OSDefineMetaClassAndStructors(com_amdhv_AMDHVVmShared, OSObject)

using namespace amdhv;
using namespace amdhv::vmcb;

/* ---- the part of a VM all its vCPUs share ---- */

static_assert((int)AMDHV_MEM_READ == (int)amdhv::npt::PERM_R && (int)AMDHV_MEM_WRITE == (int)amdhv::npt::PERM_W &&
                  (int)AMDHV_MEM_EXEC == (int)amdhv::npt::PERM_X,
              "AMDHV_MEM_* must match the NPT permission bits");

namespace {

/** Table pages are wired physically contiguous kernel pages. Node bookkeeping is plain kernel heap. */
bool nptAllocPage(void *, amdhv::npt::Page *out)
{
    amdhv::PhysMem *m = (amdhv::PhysMem *)IOMalloc(sizeof(amdhv::PhysMem));
    if (!m)
        return false;
    if (!amdhv::physAlloc(m, 4096)) {
        IOFree(m, sizeof(amdhv::PhysMem));
        return false;
    }
    out->va     = m->va;
    out->pa     = m->pa;
    out->handle = m;
    return true;
}

/**
 * @brief Release an NPT page through the allocator.
 *
 * @param p Page or pointer.
 */
void nptFreePage(void *, const amdhv::npt::Page *p)
{
    amdhv::PhysMem *m = (amdhv::PhysMem *)p->handle;
    amdhv::physFree(m);
    IOFree(m, sizeof(amdhv::PhysMem));
}

/**
 * @brief Allocate zeroed kernel heap memory for the NPT.
 *
 * @param n Number of bytes.
 */
void *nptAllocMem(void *, size_t n)
{
    void *p = IOMalloc(n);
    if (p)
        bzero(p, n);
    return p;
}

/**
 * @brief Release NPT heap memory through the allocator.
 *
 * @param p Page or pointer.
 * @param n Number of bytes.
 */
void nptFreeMem(void *, void *p, size_t n)
{
    IOFree(p, n);
}

const amdhv::npt::Allocator kNptAllocator = { nullptr, nptAllocPage, nptFreePage, nptAllocMem,
                                            nptFreeMem };

} // namespace

namespace {
IOLock *gSharedLock;                       /* guards the registry below */
com_amdhv_AMDHVVmShared *gShared[16];
uint64_t gNextId = 1;

/**
 * @brief Return the lazily created VM registry lock.
 */
IOLock *sharedLock()
{
    if (!gSharedLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, &gSharedLock))
            IOLockFree(l);
    }
    return gSharedLock;
}

} // namespace

com_amdhv_AMDHVVmShared *com_amdhv_AMDHVVmShared::create(task_t owner, IOReturn *err)
{
    IOLock *reg = sharedLock();
    if (!reg) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    com_amdhv_AMDHVVmShared *sh = OSTypeAlloc(com_amdhv_AMDHVVmShared);
    if (!sh) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    if (!sh->init()) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    if (!sh->setup(owner)) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    IOLockLock(reg);
    int slot = -1;
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++) {
        if (!gShared[i]) {
            slot = (int)i;
            break;
        }
    }
    if (slot >= 0) {
        sh->fId = gNextId++;
        gShared[slot] = sh;            /* not retained: free() takes it out again */
    }
    IOLockUnlock(reg);
    if (slot < 0) {
        AMDHV_WARN("VmCreate: too many VMs");
        sh->release();
        *err = kIOReturnNoResources;
        return nullptr;
    }
    *err = kIOReturnSuccess;
    return sh;
}

com_amdhv_AMDHVVmShared *com_amdhv_AMDHVVmShared::find(uint64_t id, task_t owner)
{
    IOLock *reg = sharedLock();
    if (!reg)
        return nullptr;
    com_amdhv_AMDHVVmShared *found = nullptr;
    IOLockLock(reg);
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++) {
        if (gShared[i] && gShared[i]->fId == id && gShared[i]->fOwner == owner) {
            found = gShared[i];
            found->retain();          /* free() takes the lock before it unlinks, so it cannot be mid-destruction here */
            break;
        }
    }
    IOLockUnlock(reg);
    return found;
}

bool com_amdhv_AMDHVVmShared::init()
{
    if (!OSObject::init())
        return false;
    fId = 0;
    fOwner = nullptr;
    fRw = nullptr;
    fRegLock = nullptr;
    bzero(fVcpus, sizeof(fVcpus));
    fPagesMapped = 0;
    fTlbGen = 1;                  /* 1, not 0: every vCPU's first run flushes the TLB */
    fSpaceIds.reset();            /* space 0 (fNpt) is the default; the rest start free */
    fRw = IORWLockAlloc();
    fRegLock = IOLockAlloc();
    return fRw && fRegLock;
}

void com_amdhv_AMDHVVmShared::free()
{
    IOLock *reg = gSharedLock;
    if (reg) {
        IOLockLock(reg);
        for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++)
            if (gShared[i] == this)
                gShared[i] = nullptr;
        IOLockUnlock(reg);
    }
    /* Unpins every buffer that is still mapped, then returns the tables. */
    fNpt.shutdown(&releaseOwner, this);
    for (unsigned i = 0; i < amdhv::space::kMax; i++)
        fSpaces[i].shutdown(&releaseOwner, this);
    if (fRw) {
        IORWLockFree(fRw);
        fRw = nullptr;
    }
    if (fRegLock) {
        IOLockFree(fRegLock);
        fRegLock = nullptr;
    }
    OSObject::free();
}

bool com_amdhv_AMDHVVmShared::setup(task_t owner)
{
    fOwner = owner;
    return fNpt.init(kNptAllocator);
}

IOReturn com_amdhv_AMDHVVmShared::attach(com_amdhv_AMDHVVm *vcpu, uint32_t *index)
{
    IOReturn rc = kIOReturnNoResources;
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < AMDHV_MAX_VCPUS_PER_VM; i++) {
        if (!fVcpus[i]) {
            fVcpus[i] = vcpu;
            *index = i;
            rc = kIOReturnSuccess;
            break;
        }
    }
    IOLockUnlock(fRegLock);
    return rc;
}

void com_amdhv_AMDHVVmShared::detach(com_amdhv_AMDHVVm *vcpu)
{
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < AMDHV_MAX_VCPUS_PER_VM; i++)
        if (fVcpus[i] == vcpu)
            fVcpus[i] = nullptr;
    IOLockUnlock(fRegLock);
}

/* Sends every vCPU that is inside the guest back to userland now, so a writer of the page tables need not wait a slice. */
void com_amdhv_AMDHVVmShared::kickAll()
{
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < AMDHV_MAX_VCPUS_PER_VM; i++)
        if (fVcpus[i])
            fVcpus[i]->kick();
    IOLockUnlock(fRegLock);
}

/* Called for every page that stops pointing into `b`. The last one unpins and frees it. */
void com_amdhv_AMDHVVmShared::dropPage(Backing *b)
{
    if (--b->live != 0)
        return;
    b->md->complete(b->dir);
    b->md->release();
    IOFree(b, sizeof(*b));
}

/* Table::clear/shutdown callback: one call per page that is still mapped. */
void com_amdhv_AMDHVVmShared::releaseOwner(void *ctx, void *owner)
{
    com_amdhv_AMDHVVmShared *vm = static_cast<com_amdhv_AMDHVVmShared *>(ctx);
    vm->fPagesMapped--;
    AMDHV_STAT_ADD(vmPagesMapped, -1);
    dropPage(static_cast<Backing *>(owner));
}

/** Where byte `offset` of `ctx` (an IOMemoryDescriptor) lives in physical memory, see amdhv::npt::SegmentFn. */
static bool mdSegment(void *ctx, uint64_t offset, uint64_t *pa, uint64_t *len)
{
    IOMemoryDescriptor *md = static_cast<IOMemoryDescriptor *>(ctx);
    IOByteCount seg = 0;
    addr64_t addr = md->getPhysicalSegment(offset, &seg, kIOMemoryMapperNone);
    if (!addr || !seg) {
        AMDHV_ERR("VM map: no physical segment at offset 0x%llx", (unsigned long long)offset);
        return false;
    }
    *pa  = addr;
    *len = seg;
    return true;
}

/*
 * Maps [0, size) of `md` at guest physical `gpa`, page by page, all or nothing.
 * Wires `md` (prepare) and holds a reference on it until the last page is unmapped.
 * Caller holds the lock or is still constructing the VM.
 */
IOReturn com_amdhv_AMDHVVmShared::mapDescriptorLocked(amdhv::npt::Table &table, IOMemoryDescriptor *md,
                                                        uint64_t gpa, uint64_t size, uint32_t perm)
{
    for (uint64_t off = 0; off < size; off += 4096) {
        if (table.isMapped(gpa + off)) {
            AMDHV_DBG("VM map: guest page 0x%llx is already mapped", (unsigned long long)(gpa + off));
            return kIOReturnBusy;
        }
    }

    IODirection dir = kIODirectionInOut;
    IOReturn rc = md->prepare(dir);
    if (rc != kIOReturnSuccess) {
        /* A range the process itself can only read (a firmware image mapped read-only from a file) cannot be wired for writing: protection
         * failure. Wire it for reading and take the write right away from the guest, which could not change that memory anyway (a write is
         * an NPT exit for the client to handle). Only after the normal way failed, so no other client changes. */
        const IOReturn first = rc;
        dir = kIODirectionOut;
        rc  = md->prepare(dir);
        if (rc == kIOReturnSuccess) {
            AMDHV_WARN("VM map: gpa 0x%llx cannot be pinned for writing (0x%x), mapped read-only (requested perm %u)",
                     (unsigned long long)gpa, first, perm);
            perm &= ~(uint32_t)amdhv::npt::PERM_W;
        }
    }
    if (rc != kIOReturnSuccess) {
        AMDHV_WARN("VM map: cannot pin the buffer: 0x%x (perm %u)", rc, perm);
        return rc;
    }
    Backing *b = (Backing *)IOMalloc(sizeof(Backing));
    if (!b) {
        md->complete(dir);
        return kIOReturnNoMemory;
    }
    md->retain();
    b->md   = md;
    b->live = 1;          /* the builder's own reference, dropped below */
    b->dir  = dir;

    uint64_t done = 0;    /* bytes of the range that are mapped so far */
    amdhv::npt::Status st = table.mapRange(gpa, size, perm, b, &mdSegment, md, &done);
    if (st != amdhv::npt::OK) {
        AMDHV_ERR("VM map: NPT mapRange failed (status %d) after %llu of %llu bytes at gpa 0x%llx", (int)st,
                (unsigned long long)done, (unsigned long long)size, (unsigned long long)gpa);
        switch (st) {
        case amdhv::npt::NO_MEMORY:   rc = kIOReturnNoMemory; break;
        case amdhv::npt::BUSY:        rc = kIOReturnBusy; break;
        case amdhv::npt::BAD_SEGMENT: rc = kIOReturnVMError; break;
        default:                    rc = kIOReturnBadArgument; break;
        }
    }
    /* Count what was actually mapped. */
    fPagesMapped += done / 4096;
    AMDHV_STAT_ADD(vmPagesMapped, done / 4096);
    b->live += done / 4096;

    if (rc != kIOReturnSuccess) {
        /* Take back what was mapped so far. */
        for (uint64_t q = 0; q < done; q += 4096) {
            void *owner = table.unmap(gpa + q);
            if (owner) {
                fPagesMapped--;
                AMDHV_STAT_ADD(vmPagesMapped, -1);
                dropPage(static_cast<Backing *>(owner));
            }
        }
    }
    dropPage(b);          /* frees the backing right away if nothing is mapped */
    return rc;
}

/* The table of `space`, or nullptr when the space does not exist. Caller holds fRw. */
amdhv::npt::Table *com_amdhv_AMDHVVmShared::tableFor(uint32_t space)
{
    if (!space)
        return &fNpt;
    const int slot = fSpaceIds.index(space);
    return slot < 0 ? nullptr : &fSpaces[slot];
}

IOReturn com_amdhv_AMDHVVmShared::spaceCreate(uint32_t *id)
{
    if (!id)
        return kIOReturnBadArgument;

    IORWLockWrite(fRw);
    uint32_t sid = 0;
    unsigned slot = 0;
    IOReturn rc = kIOReturnSuccess;
    if (!fSpaceIds.create(&sid, &slot)) {
        rc = kIOReturnNoResources;      /* 64 spaces are already alive */
    } else if (!fSpaces[slot].init(kNptAllocator)) {
        fSpaceIds.release(sid);
        rc = kIOReturnNoMemory;
    } else {
        fTlbGen++;
    }
    IORWLockUnlock(fRw);

    if (rc == kIOReturnSuccess)
        *id = sid;
    return rc;
}

IOReturn com_amdhv_AMDHVVmShared::spaceDestroy(uint32_t id)
{
    if (!id)
        return kIOReturnBadArgument;    /* the default space always exists */

    kickAll();
    IORWLockWrite(fRw);                 /* no map/unmap/protect or set_space may be in flight */
    const int slot = fSpaceIds.index(id);
    IOReturn rc;
    if (slot < 0) {
        rc = kIOReturnBadArgument;
    } else if (spaceInUse(id)) {
        rc = kIOReturnBusy;             /* a vCPU still runs in it: the client must rebind it first */
    } else {
        fSpaces[slot].shutdown(&releaseOwner, this);
        fSpaceIds.release(id);
        fTlbGen++;
        rc = kIOReturnSuccess;
    }
    IORWLockUnlock(fRw);
    return rc;
}

bool com_amdhv_AMDHVVmShared::spaceRootLocked(uint32_t space, uint64_t *pa)
{
    amdhv::npt::Table *table = tableFor(space);
    if (table && pa)
        *pa = table->rootPa();
    return table != nullptr;
}

bool com_amdhv_AMDHVVmShared::spaceRoot(uint32_t space, uint64_t *pa)
{
    IORWLockRead(fRw);
    const bool found = spaceRootLocked(space, pa);
    IORWLockUnlock(fRw);
    return found;
}

uint32_t com_amdhv_AMDHVVmShared::spacePerm(uint32_t space, uint64_t gpa)
{
    uint32_t perm = 0;
    IORWLockRead(fRw);
    amdhv::npt::Table *table = tableFor(space);
    if (table)
        table->lookup(gpa, nullptr, &perm);
    IORWLockUnlock(fRw);
    return perm;
}

/* Copy a guest-physical range out of the pinned backing descriptor. Caller holds the rw lock;
 * the read lock is recursive here, as in spacePerm(). */
bool com_amdhv_AMDHVVmShared::guestBytes(uint32_t space, uint64_t gpa, void *dst, size_t n)
{
    bool ok = false;
    IORWLockRead(fRw);
    amdhv::npt::Table *table = tableFor(space);
    if (table) {
        uint64_t hpa = 0;
        Backing *b = static_cast<Backing *>(table->lookup(gpa, &hpa, nullptr));
        if (b && b->md) {
            const IOByteCount total = b->md->getLength();
            for (IOByteCount cursor = 0; cursor < total;) {
                IOByteCount seg = 0;
                addr64_t pa = b->md->getPhysicalSegment(cursor, &seg, kIOMemoryMapperNone);
                if (!pa || !seg)
                    break;
                if (hpa >= pa && hpa < pa + seg) {
                    ok = b->md->readBytes(cursor + (hpa - pa), dst, n) == n;
                    break;
                }
                cursor += seg;
            }
        }
    }
    IORWLockUnlock(fRw);
    return ok;
}

bool com_amdhv_AMDHVVmShared::spaceInUse(uint32_t space)
{
    bool used = false;
    IOLockLock(fRegLock);
    for (unsigned i = 0; i < AMDHV_MAX_VCPUS_PER_VM; i++) {
        if (fVcpus[i] && fVcpus[i]->spaceId() == space) {
            used = true;
            break;
        }
    }
    IOLockUnlock(fRegLock);
    return used;
}

IOReturn com_amdhv_AMDHVVmShared::map(task_t task, uint32_t space, uint64_t uva, uint64_t gpa,
                                      uint64_t size, uint32_t flags)
{
    uint32_t perm = flags;
    if (!amdhv::npt::normalizePerm(&perm) || size == 0 || size > AMDHV_MAP_MAX_SIZE ||
        ((uva | gpa | size) & 0xFFF) || gpa >= amdhv::npt::kGpaLimit ||
        size > amdhv::npt::kGpaLimit - gpa || uva + size < uva)
        return kIOReturnBadArgument;

    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(uva, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnVMError;   /* typically an unmapped or non-writable range */

    kickAll();
    IORWLockWrite(fRw);            /* no vCPU is inside the guest while the tables change */
    amdhv::npt::Table *table = tableFor(space);
    IOReturn rc = table ? mapDescriptorLocked(*table, md, gpa, size, perm) : kIOReturnBadArgument;
    fTlbGen++;                     /* every vCPU flushes on its next entry: no translation may survive */
    IORWLockUnlock(fRw);

    md->release();                 /* the backing took its own reference */
    if (rc == kIOReturnSuccess) {
        AMDHV_STAT_INC(vmMaps);
        AMDHV_DBG("VM map: space %u uva 0x%llx -> gpa 0x%llx, %llu KiB, perm %u", space,
                (unsigned long long)uva, (unsigned long long)gpa, (unsigned long long)(size / 1024), perm);
    } else {
        AMDHV_STAT_INC(vmMapFailures);
        AMDHV_WARN("VM map failed (0x%x): space %u uva 0x%llx gpa 0x%llx size 0x%llx", rc, space,
                 (unsigned long long)uva, (unsigned long long)gpa, (unsigned long long)size);
    }
    return rc;
}

IOReturn com_amdhv_AMDHVVmShared::unmap(uint32_t space, uint64_t gpa, uint64_t size)
{
    if (size == 0 || gpa >= amdhv::npt::kGpaLimit ||
        size > amdhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    /*
     * HVF clients unmap sub-page MMIO ranges: QEMU's TPM TIS uses 0xfed45000, 0x400
     * bytes. Apple's framework rounds those to whole pages; do the same here instead
     * of refusing with HV_BAD_ARGUMENT (which aborts the guest's device setup).
     * Unmapping a page that was never mapped is a no-op in Npt::unmap().
     */
    const uint64_t first = gpa & ~(uint64_t)0xFFF;
    const uint64_t last  = (gpa + size + 0xFFF) & ~(uint64_t)0xFFF;

    kickAll();
    IORWLockWrite(fRw);
    amdhv::npt::Table *table = tableFor(space);
    if (!table) {
        IORWLockUnlock(fRw);
        return kIOReturnBadArgument;
    }
    for (uint64_t page = first; page < last; page += 4096) {
        void *owner = table->unmap(page);
        if (owner) {
            fPagesMapped--;
            AMDHV_STAT_ADD(vmPagesMapped, -1);
            dropPage(static_cast<Backing *>(owner));
        }
    }
    fTlbGen++;                     /* the unmapped pages must not stay in any vCPU's TLB */
    IORWLockUnlock(fRw);

    AMDHV_STAT_INC(vmUnmaps);
    return kIOReturnSuccess;
}

IOReturn com_amdhv_AMDHVVmShared::protect(uint32_t space, uint64_t gpa, uint64_t size, uint32_t flags)
{
    uint32_t perm = flags;
    if (!amdhv::npt::normalizePerm(&perm) || size == 0 || ((gpa | size) & 0xFFF) ||
        gpa >= amdhv::npt::kGpaLimit || size > amdhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    kickAll();
    IORWLockWrite(fRw);
    amdhv::npt::Table *table = tableFor(space);
    if (!table) {
        IORWLockUnlock(fRw);
        return kIOReturnBadArgument;
    }
    IOReturn rc = kIOReturnSuccess;
    for (uint64_t off = 0; off < size; off += 4096) {
        if (!table->isMapped(gpa + off)) {
            rc = kIOReturnNotFound;   /* check everything first so a failure changes nothing */
            break;
        }
    }
    if (rc == kIOReturnSuccess) {
        for (uint64_t off = 0; off < size; off += 4096)
            table->protect(gpa + off, perm);
        AMDHV_STAT_INC(vmProtects);
    }
    fTlbGen++;                     /* permissions changed: a stale TLB entry would keep the old rights */
    IORWLockUnlock(fRw);
    return rc;
}
