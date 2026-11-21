/*
 * hvf-probe - report which public Hypervisor.framework calls the provider on this Mac supports.
 *
 * hvf-smoke proves that one VMM path works end to end. This tool is the complement: it walks the
 * API surface a Virtualization.framework client can reach and prints what each call answers, so a
 * build can be compared before and after a kext change without guessing from the kernel log.
 *
 * It never treats an error as a test failure. On a Hackintosh the interesting answers are
 * HV_UNSUPPORTED (a call the provider does not implement) and HV_BAD_ARGUMENT/HV_NO_DEVICE (a call
 * it implements but refuses here); on Intel, Apple's own framework answers the same calls.
 *
 * Build:  make tools
 * Run:    ./build/hvf-probe            (needs the com.apple.security.hypervisor entitlement)
 *
 * Two probes need a task with no VM of its own and run in a forked child:
 *   - hv_vm_create(HV_VM_ACCEL_APIC), which a client that wants the in-kernel APIC passes;
 *   - the vCPU-count probe, which creates one vCPU per thread (the API binds a vCPU to a thread).
 */
#include <Hypervisor/Hypervisor.h>
#include <errno.h>
#include <inttypes.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096u
#define RAM_SIZE (1u << 20)

static const char *hvName(hv_return_t r)
{
    switch (r) {
    case HV_SUCCESS: return "ok";
    case HV_ERROR: return "HV_ERROR";
    case HV_BUSY: return "HV_BUSY";
    case HV_BAD_ARGUMENT: return "HV_BAD_ARGUMENT";
    case HV_NO_RESOURCES: return "HV_NO_RESOURCES";
    case HV_NO_DEVICE: return "HV_NO_DEVICE";
    case HV_DENIED: return "HV_DENIED";
    case HV_FAULT: return "HV_FAULT";
    case HV_UNSUPPORTED: return "HV_UNSUPPORTED";
    default: break;
    }
    static __thread char buf[24];
    snprintf(buf, sizeof(buf), "%#x", (unsigned)r);
    return buf;
}

static void line(const char *name, hv_return_t r)
{
    printf("  %-52s %s\n", name, hvName(r));
}

static uint64_t nanos(uint64_t ticks)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return ticks * tb.numer / tb.denom;
}

static uint64_t rvmcs(hv_vcpuid_t v, uint32_t f)
{
    uint64_t x = 0;
    if (hv_vmx_vcpu_read_vmcs(v, f, &x) != HV_SUCCESS)
        return 0;
    return x;
}

static void wvmcs(hv_vcpuid_t v, uint32_t f, uint64_t x)
{
    hv_vmx_vcpu_write_vmcs(v, f, x);
}

static void wreg(hv_vcpuid_t v, hv_x86_reg_t r, uint64_t x)
{
    hv_vcpu_write_register(v, r, x);
}

/* QEMU's cap2ctrl: the controls that must be 1, plus the wanted ones the CPU allows. */
static uint64_t cap2ctrl(uint64_t cap, uint64_t ctrl)
{
    return (ctrl | (cap & 0xffffffff)) & (cap >> 32);
}

/* The controls hvf-smoke uses: HLT and MWAIT exit to the client, TSC offsetting, secondary controls. */
static void setupControls(hv_vcpuid_t v)
{
    uint64_t pin = 0, proc = 0, proc2 = 0, entry = 0;
    hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &pin);
    hv_vmx_read_capability(HV_VMX_CAP_PROCBASED, &proc);
    hv_vmx_read_capability(HV_VMX_CAP_PROCBASED2, &proc2);
    hv_vmx_read_capability(HV_VMX_CAP_ENTRY, &entry);
    wvmcs(v, VMCS_CTRL_PIN_BASED, cap2ctrl(pin, 0));
    wvmcs(v, VMCS_CTRL_CPU_BASED,
          cap2ctrl(proc, CPU_BASED_HLT | CPU_BASED_MWAIT | CPU_BASED_TSC_OFFSET) | CPU_BASED_SECONDARY_CTLS);
    wvmcs(v, VMCS_CTRL_CPU_BASED2, cap2ctrl(proc2, 0));
    wvmcs(v, VMCS_CTRL_VMENTRY_CONTROLS, cap2ctrl(entry, 0));
    wvmcs(v, VMCS_CTRL_EXC_BITMAP, 0);
}

/* A real-mode machine with a flat 64 KiB segment, the same shape hvf-smoke uses. */
static void setupRealMode(hv_vcpuid_t v, uint16_t ip)
{
    static const struct { uint32_t sel, lim, ar, base; } seg[] = {
        { VMCS_GUEST_ES, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR, VMCS_GUEST_ES_BASE },
        { VMCS_GUEST_CS, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_AR, VMCS_GUEST_CS_BASE },
        { VMCS_GUEST_SS, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR, VMCS_GUEST_SS_BASE },
        { VMCS_GUEST_DS, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR, VMCS_GUEST_DS_BASE },
        { VMCS_GUEST_FS, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR, VMCS_GUEST_FS_BASE },
        { VMCS_GUEST_GS, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR, VMCS_GUEST_GS_BASE },
        { VMCS_GUEST_LDTR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, VMCS_GUEST_LDTR_BASE },
        { VMCS_GUEST_TR, VMCS_GUEST_TR_LIMIT, VMCS_GUEST_TR_AR, VMCS_GUEST_TR_BASE },
    };
    for (int i = 0; i < 8; i++) {
        wvmcs(v, seg[i].sel, 0);
        wvmcs(v, seg[i].lim, 0xFFFF);
        wvmcs(v, seg[i].base, 0);
        wvmcs(v, seg[i].ar, i == 1 ? 0x9B : i == 6 ? 0x82 : i == 7 ? 0x8B : 0x93);
    }
    wvmcs(v, VMCS_GUEST_GDTR_BASE, 0);
    wvmcs(v, VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    wvmcs(v, VMCS_GUEST_IDTR_BASE, 0);
    wvmcs(v, VMCS_GUEST_IDTR_LIMIT, 0xFFFF);
    wvmcs(v, VMCS_GUEST_CR0, 0x20 | 0x10);
    wvmcs(v, VMCS_CTRL_CR0_MASK, 0);
    wvmcs(v, VMCS_CTRL_CR0_SHADOW, 0);
    wvmcs(v, VMCS_GUEST_CR3, 0);
    wvmcs(v, VMCS_GUEST_CR4, 0);
    wvmcs(v, VMCS_GUEST_IA32_EFER, 0);
    wreg(v, HV_X86_RFLAGS, 2);
    wreg(v, HV_X86_RSP, 0);
    wreg(v, HV_X86_RIP, ip);
}

/* A kick from another thread, cancelled by the main thread when the run returns on its own. */
static pthread_mutex_t g_kickLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kickCond = PTHREAD_COND_INITIALIZER;
static int g_kickCancel;
static hv_vcpuid_t g_kickVcpu;

static void *kicker(void *arg)
{
    const long ms = (long)(intptr_t)arg;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_kickLock);
    while (!g_kickCancel) {
        if (pthread_cond_timedwait(&g_kickCond, &g_kickLock, &ts) == ETIMEDOUT)
            break;
    }
    const int cancelled = g_kickCancel;
    pthread_mutex_unlock(&g_kickLock);
    if (!cancelled) {
        hv_vcpuid_t v = g_kickVcpu;
        hv_vcpu_interrupt(&v, 1);
    }
    return NULL;
}

static void cancelKicker(void)
{
    pthread_mutex_lock(&g_kickLock);
    g_kickCancel = 1;
    pthread_cond_broadcast(&g_kickCond);
    pthread_mutex_unlock(&g_kickLock);
}

/* Probe hv_vm_create(HV_VM_ACCEL_APIC) in a fresh task: a task can hold only one VM, and the parent
 * creates its own later. Re-exec rather than fork alone so no Hypervisor.framework state is inherited. */
static const char *g_self;

static void runSelf(const char *arg)
{
    pid_t pid = fork();
    if (pid < 0) {
        line(arg, HV_ERROR);
        return;
    }
    if (pid == 0) {
        execl(g_self, g_self, arg, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        printf("  %-52s child failed\n", arg);
}

/* How many vCPUs can one VM hold? One thread per vCPU, because the API binds a vCPU to a thread. */
#define MAX_PROBE_VCPUS 64

typedef struct {
    hv_vcpuid_t id;
    hv_return_t result;
} VcpuThread;

static pthread_mutex_t g_countLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_countCond = PTHREAD_COND_INITIALIZER;
static int g_ready;
static int g_created;
static int g_release;

static void *vcpuWorker(void *arg)
{
    VcpuThread *t = (VcpuThread *)arg;
    t->result = hv_vcpu_create(&t->id, HV_VCPU_DEFAULT);
    pthread_mutex_lock(&g_countLock);
    g_ready++;
    if (t->result == HV_SUCCESS)
        g_created++;
    pthread_cond_broadcast(&g_countCond);
    while (!g_release)
        pthread_cond_wait(&g_countCond, &g_countLock);
    pthread_mutex_unlock(&g_countLock);
    if (t->result == HV_SUCCESS)
        hv_vcpu_destroy(t->id);
    return NULL;
}

static void probeVcpuCount(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) {
        line("vCPU-count probe: mmap", HV_NO_RESOURCES);
        return;
    }
    memset(ram, 0, RAM_SIZE);
    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    if (r != HV_SUCCESS) {
        line("vCPU-count probe: hv_vm_create", r);
        munmap(ram, RAM_SIZE);
        return;
    }
    if (hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS) {
        line("vCPU-count probe: hv_vm_map", HV_ERROR);
        hv_vm_destroy();
        munmap(ram, RAM_SIZE);
        return;
    }

    pthread_t th[MAX_PROBE_VCPUS];
    VcpuThread t[MAX_PROBE_VCPUS];
    memset(t, 0, sizeof(t));
    int started = 0;
    for (int i = 0; i < MAX_PROBE_VCPUS; i++) {
        if (pthread_create(&th[i], NULL, vcpuWorker, &t[i]) != 0)
            break;
        started++;
    }
    pthread_mutex_lock(&g_countLock);
    while (g_ready < started)
        pthread_cond_wait(&g_countCond, &g_countLock);
    const int created = g_created;
    pthread_mutex_unlock(&g_countLock);
    printf("  %-52s %d of %d\n", "vCPUs created in one VM (per-thread)", created, MAX_PROBE_VCPUS);

    pthread_mutex_lock(&g_countLock);
    g_release = 1;
    pthread_cond_broadcast(&g_countCond);
    pthread_mutex_unlock(&g_countLock);
    for (int i = 0; i < started; i++)
        pthread_join(th[i], NULL);

    hv_vm_destroy();
    munmap(ram, RAM_SIZE);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    static char selfPath[4096];
    uint32_t selfLen = sizeof(selfPath);
    g_self = _NSGetExecutablePath(selfPath, &selfLen) == 0 ? selfPath : argv[0];
    if (argc > 1 && strcmp(argv[1], "--accel-apic") == 0) {
        hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
        printf("  %-52s %s\n", "hv_vm_create(HV_VM_ACCEL_APIC)", hvName(r));
        if (r == HV_SUCCESS)
            hv_vm_destroy();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--vcpu-count") == 0) {
        probeVcpuCount();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--double-create") == 0) {
        /* A second hv_vm_create must answer HV_BUSY. Stop before hv_vm_destroy: on this framework a
         * task that asked twice aborts inside hv_vmx_vm_destroy (a userspace heap error, not the kext). */
        hv_vm_create(HV_VM_DEFAULT);
        hv_return_t again = hv_vm_create(HV_VM_DEFAULT);
        printf("  %-52s %s\n", "hv_vm_create twice in one task", hvName(again));
        fflush(stdout);
        _exit(0);
    }

    printf("Hypervisor.framework provider probe\n");

    printf("\n[1] VM creation\n");
    runSelf("--accel-apic");
    runSelf("--double-create");
    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    line("hv_vm_create(HV_VM_DEFAULT)", r);
    if (r != HV_SUCCESS) {
        printf("\nNo VM, so the rest cannot run. Is kern.hv_support=1 and the kext loaded?\n");
        return 0;
    }

    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0xF4;                       /* HLT */
    ram[0x1100] = 0xEB;                       /* jmp $ */
    ram[0x1101] = 0xFE;

    printf("\n[2] memory\n");
    line("hv_vm_map (1 MiB, R|W|X)", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    line("hv_vm_map over an existing mapping", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ));
    line("hv_vm_map unaligned user address", hv_vm_map(ram + 1, 0x200000, PAGE, HV_MEMORY_READ));
    line("hv_vm_protect", hv_vm_protect(0x1000, PAGE, HV_MEMORY_READ | HV_MEMORY_EXEC));
    line("hv_vm_sync_tsc", hv_vm_sync_tsc(1));

    hv_vm_space_t asid = 0;
    r = hv_vm_space_create(&asid);
    line("hv_vm_space_create", r);
    if (r == HV_SUCCESS) {
        uint8_t *extra = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        memset(extra, 0, PAGE);
        line("hv_vm_map_space", hv_vm_map_space(asid, extra, 0x300000, PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE));
        line("hv_vm_protect_space", hv_vm_protect_space(asid, 0x300000, PAGE, HV_MEMORY_READ));
        line("hv_vm_unmap_space", hv_vm_unmap_space(asid, 0x300000, PAGE));
        line("hv_vm_space_destroy", hv_vm_space_destroy(asid));
        munmap(extra, PAGE);
    }

    printf("\n[3] vCPU\n");
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    line("hv_vcpu_create", r);
    if (r != HV_SUCCESS) {
        hv_vm_destroy();
        munmap(ram, RAM_SIZE);
        return 0;
    }
    setupControls(v);
    setupRealMode(v, 0x1000);

    uint64_t t = 0;
    line("hv_vcpu_read_register(RIP)", hv_vcpu_read_register(v, HV_X86_RIP, &t));
    line("hv_vcpu_write_msr(LSTAR)", hv_vcpu_write_msr(v, 0xC0000082, 0xffffff8000100000ull));
    line("hv_vcpu_read_msr(LSTAR)", hv_vcpu_read_msr(v, 0xC0000082, &t));
    line("hv_vcpu_enable_native_msr(FS_BASE)", hv_vcpu_enable_native_msr(v, 0xC0000100, true));
    line("hv_vcpu_set_msr_access(FS_BASE, off)", hv_vcpu_set_msr_access(v, 0xC0000100, false));
    uint8_t fp[4096];
    line("hv_vcpu_read_fpstate", hv_vcpu_read_fpstate(v, fp, sizeof(fp)));
    line("hv_vcpu_invalidate_tlb", hv_vcpu_invalidate_tlb(v));
    line("hv_vcpu_inject_extint", hv_vcpu_inject_extint(v));
    uint32_t vmxStatus = 0;
    line("hv_vcpu_vmx_status", hv_vcpu_vmx_status(v, &vmxStatus));
    uint32_t exitCode = 0;
    line("hv_vcpu_exit_info", hv_vcpu_exit_info(v, &exitCode));

    printf("\n[4] run and run_until\n");
    setupRealMode(v, 0x1000);
    r = hv_vcpu_run(v);
    printf("  %-52s %s, exit %" PRIu64 "\n", "hv_vcpu_run (guest executes HLT)", hvName(r),
           rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);

    /* A deadline that has already passed: an Intel provider returns at once with the preemption-timer
     * reason (52). A provider that ignores the deadline runs the guest until the next exit. */
    setupRealMode(v, 0x1100);
    g_kickVcpu = v;
    g_kickCancel = 0;
    pthread_t th;
    pthread_create(&th, NULL, kicker, (void *)(intptr_t)200);
    uint64_t t0 = mach_absolute_time();
    r = hv_vcpu_run_until(v, t0 - 1);
    uint64_t t1 = mach_absolute_time();
    cancelKicker();
    pthread_join(th, NULL);
    printf("  %-52s %s, %.3f ms, exit %" PRIu64 "\n", "hv_vcpu_run_until(past deadline)", hvName(r),
           nanos(t1 - t0) / 1e6, rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);

    /* HV_DEADLINE_FOREVER must behave like hv_vcpu_run: block until an exit or a kick. */
    setupRealMode(v, 0x1100);
    g_kickCancel = 0;
    pthread_create(&th, NULL, kicker, (void *)(intptr_t)100);
    t0 = mach_absolute_time();
    r = hv_vcpu_run_until(v, HV_DEADLINE_FOREVER);
    t1 = mach_absolute_time();
    cancelKicker();
    pthread_join(th, NULL);
    printf("  %-52s %s, %.3f ms, exit %" PRIu64 "\n", "hv_vcpu_run_until(FOREVER) + interrupt", hvName(r),
           nanos(t1 - t0) / 1e6, rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);

    printf("\n[5] time and APIC\n");
    uint64_t exec = 0, idle = 0;
    line("hv_vcpu_get_exec_time", hv_vcpu_get_exec_time(v, &exec));
    line("hv_vcpu_get_idle_time", hv_vcpu_get_idle_time(v, &idle));
    printf("  %-52s %" PRIu64 " ns / %" PRIu64 " ns\n", "  (exec / idle)", exec, idle);

    uint32_t data = 0;
    line("hv_vcpu_apic_read(0x80)", hv_vcpu_apic_read(v, 0x80, &data));
    bool noSideEffect = false;
    line("hv_vcpu_apic_write(0x80)", hv_vcpu_apic_write(v, 0x80, 0, &noSideEffect));
    line("hv_vm_lapic_set_intr(0x20)", hv_vm_lapic_set_intr(v, 0x20, HV_APIC_EDGE_TRIGGER));
    line("hv_vm_lapic_msi", hv_vm_lapic_msi(0xFEE00000, 0x4000));
    line("hv_vm_ioapic_read", hv_vm_ioapic_read(0xFEC00000, &data));
    line("hv_vm_ioapic_write", hv_vm_ioapic_write(0xFEC00000, 0));
    line("hv_vm_ioapic_assert_irq(0)", hv_vm_ioapic_assert_irq(0));
    line("hv_vm_ioapic_deassert_irq(0)", hv_vm_ioapic_deassert_irq(0));

    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS) {
        line("hv_vm_add_pio_notifier(0x3f8)", hv_vm_add_pio_notifier(0x3F8, 1, 0, port, 0));
        line("hv_vm_remove_pio_notifier(0x3f8)", hv_vm_remove_pio_notifier(0x3F8, 1, 0, port, 0));
        mach_port_deallocate(mach_task_self(), port);
    }

    printf("\n[6] teardown\n");
    line("hv_vcpu_destroy", hv_vcpu_destroy(v));
    line("hv_vm_destroy", hv_vm_destroy());
    munmap(ram, RAM_SIZE);

    printf("\n[7] vCPU limit (separate task)\n");
    runSelf("--vcpu-count");

    printf("\ndone. HV_UNSUPPORTED = the provider does not implement the call; HV_BAD_ARGUMENT/\n"
           "HV_NO_DEVICE = it implements it but refuses this request. Compare this output before and\n"
           "after loading a new kext (a new kext needs a reboot to take effect).\n");
    return 0;
}
