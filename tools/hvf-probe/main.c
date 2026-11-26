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
 * Four probes need a task with no VM of its own and run in a forked child:
 *   - hv_vm_create(HV_VM_ACCEL_APIC), which a client that wants the in-kernel APIC passes;
 *   - a second hv_vm_create in the same task, which must answer HV_BUSY;
 *   - the vCPU-count probe, which creates one vCPU per thread (the API binds a vCPU to a thread);
 *   - the full API matrix, which creates its own VM/vCPU to measure all 79 exported functions.
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

/**
 * @brief Return a printable name for a Hypervisor.framework return code.
 *
 * @param r Hypervisor.framework return code.
 */
static const char *hv_name(hv_return_t r)
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

/**
 * @brief Print the name and result of one framework call.
 *
 * @param name API call name.
 * @param r Return code from the call.
 */
static void print_call(const char *name, hv_return_t r)
{
    printf("  %-52s %s\n", name, hv_name(r));
}

/**
 * @brief Convert mach absolute time ticks to nanoseconds.
 *
 * @param ticks Mach absolute time ticks.
 *
 * @return The uint64_t result.
 */
static uint64_t nanos(uint64_t ticks)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return ticks * tb.numer / tb.denom;
}

/**
 * @brief Read a VMCS field, reporting a failed read on stderr.
 *
 * @param v vCPU identifier.
 * @param f VMCS field encoding.
 *
 * @return The uint64_t result.
 */
static uint64_t rvmcs(hv_vcpuid_t v, uint32_t f)
{
    uint64_t x = 0;
    hv_return_t r = hv_vmx_vcpu_read_vmcs(v, f, &x);
    if (r != HV_SUCCESS)
        fprintf(stderr, "warning: read VMCS %#x failed: %s\n", f, hv_name(r));
    return x;
}

/**
 * @brief Write a VMCS field, reporting a failed write on stderr.
 *
 * @param v vCPU identifier.
 * @param f VMCS field encoding.
 * @param x Value to write.
 */
static void wvmcs(hv_vcpuid_t v, uint32_t f, uint64_t x)
{
    hv_return_t r = hv_vmx_vcpu_write_vmcs(v, f, x);
    if (r != HV_SUCCESS)
        fprintf(stderr, "warning: write VMCS %#x failed: %s\n", f, hv_name(r));
}

/**
 * @brief Write a guest register, reporting a failed write on stderr.
 *
 * @param v vCPU identifier.
 * @param r Guest register.
 * @param x Value to write.
 */
static void wreg(hv_vcpuid_t v, hv_x86_reg_t r, uint64_t x)
{
    hv_return_t k = hv_vcpu_write_register(v, r, x);
    if (k != HV_SUCCESS)
        fprintf(stderr, "warning: write register %d failed: %s\n", (int)r, hv_name(k));
}

/** QEMU's cap2ctrl: the controls that must be 1, plus the wanted ones the CPU allows. */
static uint64_t cap2ctrl(uint64_t cap, uint64_t ctrl)
{
    return (ctrl | (cap & 0xffffffff)) & (cap >> 32);
}

/** The controls hvf-smoke uses: HLT and MWAIT exit to the client, TSC offsetting, secondary controls. */
static void setup_controls(hv_vcpuid_t v)
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

/** A real-mode machine with a flat 64 KiB segment, the same shape hvf-smoke uses. */
static void setup_real_mode(hv_vcpuid_t v, uint16_t ip)
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
static pthread_mutex_t g_kick_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kick_cond = PTHREAD_COND_INITIALIZER;
static int g_kick_cancel;
static hv_vcpuid_t g_kick_vcpu;

/**
 * @brief Interrupt a vCPU after a delay unless the main thread cancels it.
 *
 * @param arg Delay in milliseconds.
 */
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
    pthread_mutex_lock(&g_kick_lock);
    while (!g_kick_cancel) {
        if (pthread_cond_timedwait(&g_kick_cond, &g_kick_lock, &ts) == ETIMEDOUT)
            break;
    }
    const int cancelled = g_kick_cancel;
    pthread_mutex_unlock(&g_kick_lock);
    if (!cancelled) {
        hv_vcpuid_t v = g_kick_vcpu;
        hv_vcpu_interrupt(&v, 1);
    }
    return NULL;
}

/**
 * @brief Cancel a pending kicker thread.
 */
static void cancel_kicker(void)
{
    pthread_mutex_lock(&g_kick_lock);
    g_kick_cancel = 1;
    pthread_cond_broadcast(&g_kick_cond);
    pthread_mutex_unlock(&g_kick_lock);
}

/* Probe hv_vm_create(HV_VM_ACCEL_APIC) in a fresh task: a task can hold only one VM, and the parent
 * creates its own later. Re-exec rather than fork alone so no Hypervisor.framework state is inherited. */
static const char *g_self;

/**
 * @brief Re-exec this tool with an internal argument in a forked child task.
 *
 * @param arg Internal switch passed to the child process.
 */
static void run_self(const char *arg)
{
    pid_t pid = fork();
    if (pid < 0) {
        print_call(arg, HV_ERROR);
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

/**
 * @brief Create one vCPU in a worker thread and wait for a signal.
 *
 * @param arg Worker signal counter.
 */
static void *vcpu_worker(void *arg)
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

/**
 * @brief Probe how many vCPUs a VM can create.
 */
static void probe_vcpu_count(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) {
        print_call("vCPU-count probe: mmap", HV_NO_RESOURCES);
        return;
    }
    memset(ram, 0, RAM_SIZE);
    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    if (r != HV_SUCCESS) {
        print_call("vCPU-count probe: hv_vm_create", r);
        munmap(ram, RAM_SIZE);
        return;
    }
    if (hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS) {
        print_call("vCPU-count probe: hv_vm_map", HV_ERROR);
        hv_vm_destroy();
        munmap(ram, RAM_SIZE);
        return;
    }

    pthread_t th[MAX_PROBE_VCPUS];
    VcpuThread t[MAX_PROBE_VCPUS];
    memset(t, 0, sizeof(t));
    int started = 0;
    for (int i = 0; i < MAX_PROBE_VCPUS; i++) {
        if (pthread_create(&th[i], NULL, vcpu_worker, &t[i]) != 0)
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

/* --------------------------------------------------------------------------
 * Full public API matrix (--matrix).
 *
 * The probes above follow real client flows, but a few exported functions are
 * never reached that way. This pass calls every function exported by the x86
 * Hypervisor.framework headers exactly once with benign arguments and prints
 * the answer, turning "unverified" into a measured result. It runs in a fresh
 * task so its VM/vCPU cannot disturb the exploratory probes. An error is a
 * result here, never a test failure.
 */
#define API_COUNT 79
static const char *const k_api_names[API_COUNT] = {
    "hv_capability", "hv_vm_create", "hv_vm_destroy", "hv_vm_space_create",
    "hv_vm_space_destroy", "hv_vm_map", "hv_vm_unmap", "hv_vm_protect",
    "hv_vm_map_space", "hv_vm_unmap_space", "hv_vm_protect_space",
    "hv_vm_sync_tsc", "hv_vm_add_pio_notifier", "hv_vm_remove_pio_notifier",
    "hv_vcpu_create", "hv_vcpu_destroy", "hv_vcpu_set_space",
    "hv_vcpu_read_register", "hv_vcpu_write_register", "hv_vcpu_read_fpstate",
    "hv_vcpu_write_fpstate", "hv_vcpu_enable_native_msr",
    "hv_vcpu_enable_managed_msr", "hv_vcpu_set_msr_access", "hv_vcpu_read_msr",
    "hv_vcpu_write_msr", "hv_vcpu_flush", "hv_vcpu_invalidate_tlb",
    "hv_vcpu_run", "hv_vcpu_run_until", "hv_vcpu_interrupt",
    "hv_vcpu_get_exec_time", "hv_vcpu_get_idle_time", "hv_tsc_clock",
    "hv_vcpu_set_tsc_relative", "hv_vcpu_vmx_status", "hv_vm_lapic_set_intr",
    "hv_vm_lapic_msi", "hv_vm_ioapic_assert_irq", "hv_vm_ioapic_deassert_irq",
    "hv_vm_ioapic_pulse_irq", "hv_vm_ioapic_read", "hv_vm_ioapic_write",
    "hv_vm_ioapic_get_state", "hv_vm_ioapic_put_state", "hv_vm_send_ioapic_intr",
    "hv_vm_atpic_assert_irq", "hv_vm_atpic_deassert_irq",
    "hv_vm_atpic_port_read", "hv_vm_atpic_port_write", "hv_vm_atpic_get_state",
    "hv_vm_atpic_put_state", "hv_vm_set_apic_bus_freq", "hv_vcpu_inject_extint",
    "hv_vcpu_apic_read", "hv_vcpu_apic_write", "hv_vcpu_apic_get_state",
    "hv_vcpu_apic_put_state", "hv_vcpu_exit_info", "hv_vcpu_exit_init_ap",
    "hv_vcpu_exit_startup_ap", "hv_vcpu_exit_ioapic_eoi",
    "hv_vcpu_exit_apic_access_read", "hv_vcpu_exit_inject_excp",
    "hv_vcpu_apic_lsc_enter_r32", "hv_vcpu_apic_lsc_enter_imm32",
    "hv_vcpu_apic_lsc_invalidate", "hv_vcpu_apic_ctrl",
    "hv_vcpu_apic_trigger_lvt", "hv_vmx_vcpu_read_vmcs",
    "hv_vmx_vcpu_write_vmcs", "hv_vmx_vcpu_get_cap_write_vmcs",
    "hv_vmx_vcpu_read_shadow_vmcs", "hv_vmx_vcpu_write_shadow_vmcs",
    "hv_vmx_vcpu_set_shadow_access", "hv_vmx_read_capability",
    "hv_vmx_get_msr_info", "hv_vmx_vcpu_set_apic_address",
    "hv_vmx_vcpu_set_apic_address_space",
};

static int g_api_seen[API_COUNT];

/**
 * @brief Mark an API as observed and print its return code.
 *
 * @param name API call name.
 * @param r Return code from the call.
 */
static void record_and_print(const char *name, hv_return_t r)
{
    for (int i = 0; i < API_COUNT; i++) {
        if (!strcmp(k_api_names[i], name)) {
            g_api_seen[i] = 1;
            break;
        }
    }
    print_call(name, r);
}

/**
 * @brief Print the public APIs that were not reached by the probes.
 */
static void api_summary(void)
{
    int seen = 0;
    for (int i = 0; i < API_COUNT; i++) {
        if (g_api_seen[i])
            seen++;
        else
            printf("  %-52s NOT PROBED\n", k_api_names[i]);
    }
    printf("  %-52s %d of %d\n", "public API functions observed", seen, API_COUNT);
}

/**
 * @brief Probe the full public API surface in a fresh VM and vCPU.
 */
static void probe_api_matrix(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap");
        return;
    }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0xF4;
    uint8_t *extra = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (extra == MAP_FAILED) {
        perror("mmap (extra)");
        munmap(ram, RAM_SIZE);
        return;
    }
    memset(extra, 0, 2 * PAGE);

    printf("Public x86 Hypervisor.framework API matrix (SDK macOS 15)\n");
    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    record_and_print("hv_vm_create", r);
    if (r != HV_SUCCESS) {
        printf("  no VM: the vCPU functions below cannot be probed\n");
        api_summary();
        hv_vm_destroy();
        munmap(extra, 2 * PAGE);
        munmap(ram, RAM_SIZE);
        return;
    }

    hv_vm_space_t space = 0;
    record_and_print("hv_vm_space_create", hv_vm_space_create(&space));
    if (space == 0)
        space = 1;

    uint64_t value = 0;
    record_and_print("hv_capability", hv_capability(HV_CAP_VCPUMAX, &value));
    printf("  %-52s VCPUMAX=%" PRIu64 "\n", "  (hv_capability value)", value);
    record_and_print("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    record_and_print("hv_vm_protect", hv_vm_protect(0, PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    record_and_print("hv_vm_map_space", hv_vm_map_space(space, extra, 0x200000, PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE));
    record_and_print("hv_vm_protect_space", hv_vm_protect_space(space, 0x200000, PAGE, HV_MEMORY_READ));
    record_and_print("hv_vm_unmap_space", hv_vm_unmap_space(space, 0x200000, PAGE));
    hv_vm_map(extra + PAGE, 0x400000, PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE);    /* setup for hv_vm_unmap */
    record_and_print("hv_vm_unmap", hv_vm_unmap(0x400000, PAGE));
    record_and_print("hv_vm_sync_tsc", hv_vm_sync_tsc(1));

    uint64_t clock = hv_tsc_clock();
    for (int i = 0; i < API_COUNT; i++)
        if (!strcmp(k_api_names[i], "hv_tsc_clock"))
            g_api_seen[i] = 1;
    printf("  %-52s %" PRIu64 " (uint64_t, not hv_return_t)\n", "hv_tsc_clock", clock);

    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS &&
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS) {
        record_and_print("hv_vm_add_pio_notifier", hv_vm_add_pio_notifier(0x3F8, 1, 0, port, 0));
        record_and_print("hv_vm_remove_pio_notifier", hv_vm_remove_pio_notifier(0x3F8, 1, 0, port, 0));
        mach_port_deallocate(mach_task_self(), port);
    }

    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    record_and_print("hv_vcpu_create", r);
    if (r != HV_SUCCESS) {
        printf("  no vCPU: the per-vCPU functions below cannot be probed\n");
        api_summary();
        hv_vm_destroy();
        munmap(extra, 2 * PAGE);
        munmap(ram, RAM_SIZE);
        return;
    }

    setup_controls(v);
    setup_real_mode(v, 0x1000);
    record_and_print("hv_vcpu_set_space", hv_vcpu_set_space(v, space));
    hv_vm_space_destroy(space);                       /* extra call: expect HV_BUSY while bound */
    hv_vcpu_set_space(v, HV_VM_SPACE_DEFAULT);        /* extra call: unbind before the next probe */
    record_and_print("hv_vm_space_destroy", hv_vm_space_destroy(space));

    uint64_t reg = 0;
    record_and_print("hv_vcpu_read_register", hv_vcpu_read_register(v, HV_X86_RIP, &reg));
    record_and_print("hv_vcpu_write_register", hv_vcpu_write_register(v, HV_X86_RIP, 0x1000));
    uint8_t fp[4096];
    memset(fp, 0, sizeof(fp));
    record_and_print("hv_vcpu_read_fpstate", hv_vcpu_read_fpstate(v, fp, sizeof(fp)));
    record_and_print("hv_vcpu_write_fpstate", hv_vcpu_write_fpstate(v, fp, sizeof(fp)));
    record_and_print("hv_vcpu_enable_native_msr", hv_vcpu_enable_native_msr(v, 0xC0000100, true));
    record_and_print("hv_vcpu_enable_managed_msr", hv_vcpu_enable_managed_msr(v, 0xC0000100, true));
    record_and_print("hv_vcpu_set_msr_access", hv_vcpu_set_msr_access(v, 0xC0000100, 0));
    record_and_print("hv_vcpu_write_msr", hv_vcpu_write_msr(v, 0xC0000082, 0xffffff8000100000ull));
    record_and_print("hv_vcpu_read_msr", hv_vcpu_read_msr(v, 0xC0000082, &value));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    record_and_print("hv_vcpu_flush", hv_vcpu_flush(v));
#pragma clang diagnostic pop
    record_and_print("hv_vcpu_invalidate_tlb", hv_vcpu_invalidate_tlb(v));
    record_and_print("hv_vcpu_interrupt", hv_vcpu_interrupt(&v, 1));
    uint64_t off = 0;
    hv_vmx_vcpu_read_vmcs(v, VMCS_CTRL_TSC_OFFSET, &off);
    uint64_t offBefore = off;
    record_and_print("hv_vcpu_set_tsc_relative", hv_vcpu_set_tsc_relative(v, 0x1000));

    /* The provider applies the relative offset while entering the guest, so read it back after a run. */
    setup_real_mode(v, 0x1000);
    record_and_print("hv_vcpu_run", hv_vcpu_run(v));
    hv_vmx_vcpu_read_vmcs(v, VMCS_CTRL_TSC_OFFSET, &off);
    /* Our commpage starts with base = adjust = 0, so hv_tsc_clock() is rdtsc and the VMCB offset
     * must be exactly the requested 0x1000. hv_vm_sync_tsc earlier in this matrix put a different
     * baseline in the field, which is why a delta check would be wrong here. */
    printf("  %-52s %s (offset %#" PRIx64 " -> %#" PRIx64 ")\n",
           "  (set_tsc_relative effect)", off == 0x1000 ? "wired" : "not 0x1000", offBefore, off);
    setup_real_mode(v, 0x1100);
    record_and_print("hv_vcpu_run_until", hv_vcpu_run_until(v, mach_absolute_time()));

    uint64_t time = 0;
    record_and_print("hv_vcpu_get_exec_time", hv_vcpu_get_exec_time(v, &time));
    record_and_print("hv_vcpu_get_idle_time", hv_vcpu_get_idle_time(v, &time));
    uint32_t status = 0;
    record_and_print("hv_vcpu_vmx_status", hv_vcpu_vmx_status(v, &status));
    record_and_print("hv_vcpu_exit_info", hv_vcpu_exit_info(v, &status));

    record_and_print("hv_vm_lapic_set_intr", hv_vm_lapic_set_intr(v, 0x20, HV_APIC_EDGE_TRIGGER));
    record_and_print("hv_vm_lapic_msi", hv_vm_lapic_msi(0xFEE00000, 0x4000));
    record_and_print("hv_vm_ioapic_assert_irq", hv_vm_ioapic_assert_irq(0));
    record_and_print("hv_vm_ioapic_deassert_irq", hv_vm_ioapic_deassert_irq(0));
    record_and_print("hv_vm_ioapic_pulse_irq", hv_vm_ioapic_pulse_irq(0));
    uint32_t data = 0;
    record_and_print("hv_vm_ioapic_read", hv_vm_ioapic_read(0xFEC00000, &data));
    record_and_print("hv_vm_ioapic_write", hv_vm_ioapic_write(0xFEC00000, 0));
    hv_ioapic_state_ext_t ioapic = { 0 };
    record_and_print("hv_vm_ioapic_get_state", hv_vm_ioapic_get_state(&ioapic));
    record_and_print("hv_vm_ioapic_put_state", hv_vm_ioapic_put_state(&ioapic));
    record_and_print("hv_vm_send_ioapic_intr", hv_vm_send_ioapic_intr(0x4000));
    record_and_print("hv_vm_atpic_assert_irq", hv_vm_atpic_assert_irq(0));
    record_and_print("hv_vm_atpic_deassert_irq", hv_vm_atpic_deassert_irq(0));
    uint8_t byte = 0;
    record_and_print("hv_vm_atpic_port_read", hv_vm_atpic_port_read(0x21, &byte));
    record_and_print("hv_vm_atpic_port_write", hv_vm_atpic_port_write(0x21, 0));
    hv_atpic_state_ext_t atpic = { 0 };
    record_and_print("hv_vm_atpic_get_state", hv_vm_atpic_get_state(&atpic, true));
    record_and_print("hv_vm_atpic_put_state", hv_vm_atpic_put_state(&atpic, true));
    record_and_print("hv_vm_set_apic_bus_freq", hv_vm_set_apic_bus_freq(1000000000));

    record_and_print("hv_vcpu_inject_extint", hv_vcpu_inject_extint(v));
    bool noSideEffect = false;
    record_and_print("hv_vcpu_apic_read", hv_vcpu_apic_read(v, 0x80, &data));
    record_and_print("hv_vcpu_apic_write", hv_vcpu_apic_write(v, 0x80, 0, &noSideEffect));
    hv_apic_state_ext_t apic = { 0 };
    record_and_print("hv_vcpu_apic_get_state", hv_vcpu_apic_get_state(v, &apic));
    record_and_print("hv_vcpu_apic_put_state", hv_vcpu_apic_put_state(v, &apic));
    bool active[1] = { false };
    uint64_t rip = 0;
    uint8_t vec = 0;
    bool valid = false, restart = false;
    record_and_print("hv_vcpu_exit_init_ap", hv_vcpu_exit_init_ap(v, active, 1));
    record_and_print("hv_vcpu_exit_startup_ap", hv_vcpu_exit_startup_ap(v, active, 1, &rip));
    record_and_print("hv_vcpu_exit_ioapic_eoi", hv_vcpu_exit_ioapic_eoi(v, &vec));
    record_and_print("hv_vcpu_exit_apic_access_read", hv_vcpu_exit_apic_access_read(v, &data));
    record_and_print("hv_vcpu_exit_inject_excp", hv_vcpu_exit_inject_excp(v, &vec, &valid, &data, &restart));
    record_and_print("hv_vcpu_apic_lsc_enter_r32", hv_vcpu_apic_lsc_enter_r32(v, true, 0x1000, 1, 8, HV_X86_RAX, NULL, 0));
    record_and_print("hv_vcpu_apic_lsc_enter_imm32", hv_vcpu_apic_lsc_enter_imm32(v, 0x1000, 1, 8, 0, NULL, 0));
    record_and_print("hv_vcpu_apic_lsc_invalidate", hv_vcpu_apic_lsc_invalidate(v));
    record_and_print("hv_vcpu_apic_ctrl", hv_vcpu_apic_ctrl(v, HV_APIC_CTRL_DEFAULT));
    record_and_print("hv_vcpu_apic_trigger_lvt", hv_vcpu_apic_trigger_lvt(v, HV_APIC_LVT_FLAVOR_TIMER));

    uint64_t allowed0 = 0, allowed1 = 0;
    record_and_print("hv_vmx_vcpu_read_vmcs", hv_vmx_vcpu_read_vmcs(v, VMCS_GUEST_ES, &value));
    record_and_print("hv_vmx_vcpu_write_vmcs", hv_vmx_vcpu_write_vmcs(v, VMCS_GUEST_ES, 0));
    record_and_print("hv_vmx_vcpu_get_cap_write_vmcs", hv_vmx_vcpu_get_cap_write_vmcs(v, VMCS_GUEST_ES, &allowed0, &allowed1));
    record_and_print("hv_vmx_vcpu_read_shadow_vmcs", hv_vmx_vcpu_read_shadow_vmcs(v, VMCS_GUEST_ES, &value));
    record_and_print("hv_vmx_vcpu_write_shadow_vmcs", hv_vmx_vcpu_write_shadow_vmcs(v, VMCS_GUEST_ES, 0x1234));
    uint64_t shadow = 0;
    hv_return_t sr = hv_vmx_vcpu_read_shadow_vmcs(v, VMCS_GUEST_ES, &shadow);
    printf("  %-52s %s (read-back %#" PRIx64 ")\n", "  (shadow VMCS round-trip)",
           sr == HV_SUCCESS && shadow == 0x1234 ? "wired" : "not observed", shadow);
    record_and_print("hv_vmx_vcpu_set_shadow_access", hv_vmx_vcpu_set_shadow_access(v, VMCS_GUEST_ES, HV_SHADOW_VMCS_READ));
    record_and_print("hv_vmx_read_capability", hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &value));
    record_and_print("hv_vmx_get_msr_info", hv_vmx_get_msr_info(HV_VMX_INFO_MSR_IA32_ARCH_CAPABILITIES, &value));
    printf("  %-52s %#" PRIx64 "\n", "  (msr_info value)", value);
    record_and_print("hv_vmx_vcpu_set_apic_address", hv_vmx_vcpu_set_apic_address(v, 0xFEE00000));
    record_and_print("hv_vmx_vcpu_set_apic_address_space", hv_vmx_vcpu_set_apic_address_space(v, HV_VM_SPACE_DEFAULT, 0xFEE00000));

    record_and_print("hv_vcpu_destroy", hv_vcpu_destroy(v));
    record_and_print("hv_vm_destroy", hv_vm_destroy());
    api_summary();
    munmap(extra, 2 * PAGE);
    munmap(ram, RAM_SIZE);
}

/* Exercise the in-kernel APIC family under a VM that actually asked for it. The provider logs each
 * trap with its argument block, so this is the way to learn the sub-op encodings before wiring
 * handlers. Run with `amd-info log -f` to capture them. */
#define APIC(N, E)                                                              \
    do {                                                                        \
        hv_return_t rr = HV_SUCCESS;                                            \
        for (int i = 0; i < 1; i++)                                             \
            rr = (E);                                                           \
        print_call((N), rr);                                                          \
    } while (0)

/**
 * @brief Probe the APIC-related VMX capability words.
 */
static void probe_apic_family(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap");
        return;
    }
    memset(ram, 0, RAM_SIZE);
    printf("APIC family probe (HV_VM_ACCEL_APIC)\n");
    hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
    print_call("hv_vm_create(HV_VM_ACCEL_APIC)", r);
    if (r != HV_SUCCESS) {
        munmap(ram, RAM_SIZE);
        return;
    }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);

    uint32_t data = 0;
    uint8_t byte = 0;
    bool noSideEffect = false;
    bool active[1] = { false };
    bool valid = false, restart = false;
    uint64_t rip = 0;
    uint8_t vec = 0;
    hv_apic_state_ext_t apic = { 0 };
    hv_atpic_state_ext_t atpic = { 0 };
    hv_ioapic_state_ext_t ioapic = { 0 };

    printf("  %-52s %p %p %p %p\n", "  (locals data/noSide/apic/active)", &data, &noSideEffect, &apic, active);

    APIC("hv_vm_atpic_assert_irq(0)", hv_vm_atpic_assert_irq(0));
    APIC("hv_vm_atpic_deassert_irq(0)", hv_vm_atpic_deassert_irq(0));
    APIC("hv_vm_atpic_port_read(0x21)", hv_vm_atpic_port_read(0x21, &byte));
    APIC("hv_vm_atpic_port_write(0x21)", hv_vm_atpic_port_write(0x21, 0));
    APIC("hv_vm_atpic_get_state", hv_vm_atpic_get_state(&atpic, true));
    APIC("hv_vm_atpic_put_state", hv_vm_atpic_put_state(&atpic, true));
    APIC("hv_vm_ioapic_assert_irq(0)", hv_vm_ioapic_assert_irq(0));
    APIC("hv_vm_ioapic_deassert_irq(0)", hv_vm_ioapic_deassert_irq(0));
    APIC("hv_vm_ioapic_pulse_irq(0)", hv_vm_ioapic_pulse_irq(0));
    APIC("hv_vm_ioapic_read", hv_vm_ioapic_read(0xFEC00000, &data));
    APIC("hv_vm_ioapic_write", hv_vm_ioapic_write(0xFEC00000, 0));
    APIC("hv_vm_ioapic_get_state", hv_vm_ioapic_get_state(&ioapic));
    APIC("hv_vm_ioapic_put_state", hv_vm_ioapic_put_state(&ioapic));
    APIC("hv_vm_send_ioapic_intr", hv_vm_send_ioapic_intr(0x4000));
    APIC("hv_vm_lapic_set_intr(0x20)", hv_vm_lapic_set_intr(v, 0x20, HV_APIC_EDGE_TRIGGER));
    APIC("hv_vm_lapic_msi", hv_vm_lapic_msi(0xFEE00000, 0x4000));
    APIC("hv_vm_set_apic_bus_freq", hv_vm_set_apic_bus_freq(1000000000));
    if (r == HV_SUCCESS) {
        APIC("hv_vcpu_inject_extint", hv_vcpu_inject_extint(v));
        APIC("hv_vcpu_apic_read", hv_vcpu_apic_read(v, 0x80, &data));
        APIC("hv_vcpu_apic_write", hv_vcpu_apic_write(v, 0x80, 0, &noSideEffect));
        APIC("hv_vcpu_apic_get_state", hv_vcpu_apic_get_state(v, &apic));
        APIC("hv_vcpu_apic_put_state", hv_vcpu_apic_put_state(v, &apic));
        APIC("hv_vcpu_apic_ctrl", hv_vcpu_apic_ctrl(v, HV_APIC_CTRL_DEFAULT));
        APIC("hv_vcpu_apic_trigger_lvt", hv_vcpu_apic_trigger_lvt(v, HV_APIC_LVT_FLAVOR_TIMER));
        APIC("hv_vcpu_apic_lsc_enter_r32", hv_vcpu_apic_lsc_enter_r32(v, true, 0x1000, 1, 8, HV_X86_RAX, NULL, 0));
        APIC("hv_vcpu_apic_lsc_enter_imm32", hv_vcpu_apic_lsc_enter_imm32(v, 0x1000, 1, 8, 0, NULL, 0));
        APIC("hv_vcpu_apic_lsc_invalidate", hv_vcpu_apic_lsc_invalidate(v));
        uint32_t rt = 0;
        hv_return_t wr = hv_vcpu_apic_write(v, 0x80, 0x55, &noSideEffect);
        hv_return_t rd = hv_vcpu_apic_read(v, 0x80, &rt);
        printf("  %-52s write=%d read=%d val=%#x (no-op API)\n", "  (apic TPR round-trip)", wr, rd, rt);
        APIC("hv_vcpu_exit_init_ap", hv_vcpu_exit_init_ap(v, active, 1));
        APIC("hv_vcpu_exit_startup_ap", hv_vcpu_exit_startup_ap(v, active, 1, &rip));
        APIC("hv_vcpu_exit_ioapic_eoi", hv_vcpu_exit_ioapic_eoi(v, &vec));
        APIC("hv_vcpu_exit_apic_access_read", hv_vcpu_exit_apic_access_read(v, &data));
        APIC("hv_vcpu_exit_inject_excp", hv_vcpu_exit_inject_excp(v, &vec, &valid, &data, &restart));
        hv_vcpu_destroy(v);
    }
    print_call("hv_vm_destroy", hv_vm_destroy());
    munmap(ram, RAM_SIZE);
}

/** Functional check for APIC delivery: a real-mode guest with an IVT entry for vector 0x30 and an
 * IOAPIC redirect for IRQ0 to that vector. If the provider injected the interrupt, the HLT exit
 * comes from the handler at 0x1200 rather than the guest entry at 0x1000. */
static void probe_apic_run(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap"); return; }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0xF4;                       /* entry: HLT */
    ram[0x1200] = 0xF4;                       /* vector 0x30 handler: HLT */
    ram[0xC0] = 0x00; ram[0xC1] = 0x12;       /* IVT[0x30].offset = 0x1200 */
    ram[0xC2] = 0x00; ram[0xC3] = 0x00;       /* IVT[0x30].segment = 0 */

    hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
    print_call("hv_vm_create(HV_VM_ACCEL_APIC)", r);
    if (r != HV_SUCCESS) { munmap(ram, RAM_SIZE); return; }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) { hv_vm_destroy(); munmap(ram, RAM_SIZE); return; }
    setup_controls(v);
    setup_real_mode(v, 0x1000);
    wreg(v, HV_X86_RFLAGS, 0x202);            /* real mode masks external interrupts when IF=0 */
    wreg(v, HV_X86_TPR, 0);                   /* no priority mask in the APIC model */

    hv_ioapic_state_ext_t io = { 0 };
    r = hv_vm_ioapic_get_state(&io);
    print_call("ioapic get_state (setup)", r);
    printf("  %-52s %#llx\n", "  (rtbl0 before)", (unsigned long long)io.state.rtbl[0]);
    if (r == HV_SUCCESS) {
        io.state.rtbl[0] = 0x30;              /* vector 0x30, unmasked, physical destination */
        io.state.ioregsel = 0x10;
        print_call("ioapic put_state (setup)", hv_vm_ioapic_put_state(&io));
    }
    hv_ioapic_state_ext_t check = { 0 };
    if (hv_vm_ioapic_get_state(&check) == HV_SUCCESS)
        printf("  %-52s %#llx, irr %#x\n", "  (rtbl0 readback)", (unsigned long long)check.state.rtbl[0],
               check.state.irr);
    print_call("ioapic assert_irq(0)", hv_vm_ioapic_assert_irq(0));
    r = hv_vcpu_run(v);
    const uint64_t reason = rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF;
    const uint64_t rip = rvmcs(v, VMCS_GUEST_RIP);
    printf("  %-52s %s, exit %" PRIu64 ", RIP %#" PRIx64 " (%s)\n", "guest run with IRQ0 pending",
           hv_name(r), reason, rip, rip == 0x1200 ? "handler reached" : "entry HLT");
    hv_ioapic_state_ext_t after = { 0 };
    if (hv_vm_ioapic_get_state(&after) == HV_SUCCESS)
        printf("  %-52s %#llx, irr %#x\n", "  (state after run)", (unsigned long long)after.state.rtbl[0],
               after.state.irr);
    hv_vcpu_destroy(v);
    hv_vm_destroy();
    munmap(ram, RAM_SIZE);
}

/** Direct LAPIC injection through the three provider APIs. Vector 0x30 reaches 0x1200 via
 * hv_vm_lapic_set_intr, 0x50 reaches 0x1250 via hv_vm_send_ioapic_intr (an IOAPIC RTE), and 0x60
 * reaches 0x1260 via hv_vm_lapic_msi (physical destination 0). Each successive vector has a higher
 * class than the one left in ISR by the previous leg, so the LAPIC arbiter does not block it. */
static void probe_apic_inject(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap"); return; }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0xF4;                       /* entry HLT */
    ram[0x1200] = 0xF4;                       /* vector 0x30 handler */
    ram[0x1250] = 0xF4;                       /* vector 0x50 handler */
    ram[0x1260] = 0xF4;                       /* vector 0x60 handler */
    ram[0xC0] = 0x00; ram[0xC1] = 0x12;       /* IVT[0x30] -> 0x1200 */
    ram[0x140] = 0x50; ram[0x141] = 0x12;     /* IVT[0x50] -> 0x1250 */
    ram[0x180] = 0x60; ram[0x181] = 0x12;     /* IVT[0x60] -> 0x1260 */

    hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
    print_call("hv_vm_create(HV_VM_ACCEL_APIC)", r);
    if (r != HV_SUCCESS) { munmap(ram, RAM_SIZE); return; }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) { hv_vm_destroy(); munmap(ram, RAM_SIZE); return; }
    setup_controls(v);
    setup_real_mode(v, 0x1000);
    wreg(v, HV_X86_RFLAGS, 0x202);            /* IF=1: external interrupts are not masked */
    wreg(v, HV_X86_TPR, 0);

    print_call("lapic_set_intr(vcpu0, 0x30)", hv_vm_lapic_set_intr(0, 0x30, HV_APIC_EDGE_TRIGGER));
    hv_vcpu_run(v);
    uint64_t rip = rvmcs(v, VMCS_GUEST_RIP);
    printf("  %-52s RIP %#llx (%s)\n", "lapic_set_intr guest run", (unsigned long long)rip,
           rip == 0x1200 ? "handler reached" : "MISSED");

    wreg(v, HV_X86_RIP, 0x1000);
    print_call("send_ioapic_intr(RTE 0x50)", hv_vm_send_ioapic_intr(0x50));
    hv_vcpu_run(v);
    rip = rvmcs(v, VMCS_GUEST_RIP);
    printf("  %-52s RIP %#llx (%s)\n", "send_ioapic_intr guest run", (unsigned long long)rip,
           rip == 0x1250 ? "handler reached" : "MISSED");

    wreg(v, HV_X86_RIP, 0x1000);
    print_call("lapic_msi(RTE 0x60)", hv_vm_lapic_msi(0xFEE00000ull, 0x60ull));
    hv_vcpu_run(v);
    rip = rvmcs(v, VMCS_GUEST_RIP);
    printf("  %-52s RIP %#llx (%s)\n", "lapic_msi guest run", (unsigned long long)rip,
           rip == 0x1260 ? "handler reached" : "MISSED");

    hv_vcpu_destroy(v);
    hv_vm_destroy();
    munmap(ram, RAM_SIZE);
}

/** No-info regression check: without an APIC event, hv_vcpu_exit_info and all five helpers
 * must answer HV_ERROR, as on Apple. With an in-kernel APIC event the front end publishes the
 * matching payload in the ro page and the helpers decode it. */
static void probe_exitinfo(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap"); return; }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0xF4;                       /* hlt */
    hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
    print_call("hv_vm_create(HV_VM_ACCEL_APIC)", r);
    if (r != HV_SUCCESS) { munmap(ram, RAM_SIZE); return; }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) { hv_vm_destroy(); munmap(ram, RAM_SIZE); return; }
    setup_controls(v);
    setup_real_mode(v, 0x1000);
    setup_real_mode(v, 0x1000);
    hv_vcpu_run(v);
    hv_vm_exitinfo_t code = 0;
    hv_return_t er = hv_vcpu_exit_info(v, &code);
    printf("exit_info       ret=%d code=%u\n", er, code);
    bool a[4] = {false, false, false, false};
    printf("exit_init_ap    ret=%d\n", hv_vcpu_exit_init_ap(v, a, 4));
    uint64_t rip = 0;
    printf("exit_startup_ap ret=%d rip=%#llx\n", hv_vcpu_exit_startup_ap(v, a, 4, &rip),
           (unsigned long long)rip);
    uint8_t vec = 0;
    printf("exit_ioapic_eoi ret=%d vec=%#x\n", hv_vcpu_exit_ioapic_eoi(v, &vec), vec);
    uint32_t value = 0;
    printf("exit_apic_read  ret=%d value=%#x\n", hv_vcpu_exit_apic_access_read(v, &value), value);
    bool valid = false, restart = false; uint32_t err = 0;
    printf("exit_inject_exc ret=%d vec=%#x valid=%d code=%#x restart=%d\n",
           hv_vcpu_exit_inject_excp(v, &vec, &valid, &err, &restart), vec, valid, err, restart);

    hv_vcpu_destroy(v);
    hv_vm_destroy();
    munmap(ram, RAM_SIZE);
}

/** Trigger an in-kernel IOAPIC MMIO read so the front end publishes HV_VM_EXITINFO_APIC_ACCESS_READ. */
static void probe_exitinfo_mmio(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap"); return; }
    memset(ram, 0, RAM_SIZE);
    /* 66 = 32-bit operand, 67 = 32-bit address in real mode (see ApicMmio.hpp). */
    unsigned off = 0;
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0xB8; /* mov eax, 0x10 */
    ram[0x1000 + off++] = 0x10; ram[0x1000 + off++] = 0; ram[0x1000 + off++] = 0; ram[0x1000 + off++] = 0;
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0x67; ram[0x1000 + off++] = 0x89; ram[0x1000 + off++] = 0x05;
    ram[0x1000 + off++] = 0x00; ram[0x1000 + off++] = 0x00; ram[0x1000 + off++] = 0xC0; ram[0x1000 + off++] = 0xFE; /* mov [0xFEC00000],eax */
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0x67; ram[0x1000 + off++] = 0x8B; ram[0x1000 + off++] = 0x05;
    ram[0x1000 + off++] = 0x10; ram[0x1000 + off++] = 0x00; ram[0x1000 + off++] = 0xC0; ram[0x1000 + off++] = 0xFE; /* mov eax,[0xFEC00010] */
    ram[0x1000 + off++] = 0xF4;                                 /* hlt */

    hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
    print_call("hv_vm_create(HV_VM_ACCEL_APIC)", r);
    if (r != HV_SUCCESS) { munmap(ram, RAM_SIZE); return; }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) { hv_vm_destroy(); munmap(ram, RAM_SIZE); return; }
    setup_controls(v);
    setup_real_mode(v, 0x1000);
    wvmcs(v, VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);   /* unreal mode: 0xFEC00000 is beyond 64 KiB */
    wvmcs(v, VMCS_GUEST_DS_LIMIT, 0xFFFFFFFF);
    printf("  %-52s %#llx %02x %02x %02x %02x %02x %02x %02x\n", "code at 0x1000",
           (unsigned long long)rvmcs(v, VMCS_GUEST_RIP), ram[0x1000], ram[0x1001], ram[0x1002],
           ram[0x1003], ram[0x1004], ram[0x1005], ram[0x1006]);
    hv_return_t rr = hv_vcpu_run(v);
    uint64_t rax = 0;
    hv_vcpu_read_register(v, HV_X86_RAX, &rax);
    printf("  %-52s ret=%d reason=%llu RIP %#llx EAX %#llx GPA %#llx\n", "guest run", rr,
           (unsigned long long)(rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF),
           (unsigned long long)rvmcs(v, VMCS_GUEST_RIP), (unsigned long long)rax,
           (unsigned long long)rvmcs(v, 0x2400));
    hv_vm_exitinfo_t code = 0;
    uint8_t vec = 0;
    printf("  %-52s ret=%d code=%u\n", "exit_info", hv_vcpu_exit_info(v, &code), code);
    uint32_t value = 0;
    printf("  %-52s ret=%d value=%#x\n", "exit_apic_access_read",
           hv_vcpu_exit_apic_access_read(v, &value), value);

    /* Second leg: write EOI (vector 0x30) through the IOAPIC window -> HV_VM_EXITINFO_IOAPIC_EOI. */
    off = 0;
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0xB8;   /* mov eax, 0x10 */
    ram[0x1000 + off++] = 0x10; ram[0x1000 + off++] = 0; ram[0x1000 + off++] = 0; ram[0x1000 + off++] = 0;
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0x67; ram[0x1000 + off++] = 0x89; ram[0x1000 + off++] = 0x05;
    ram[0x1000 + off++] = 0x00; ram[0x1000 + off++] = 0x00; ram[0x1000 + off++] = 0xC0; ram[0x1000 + off++] = 0xFE;
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0xB8;   /* mov eax, 0x30 */
    ram[0x1000 + off++] = 0x30; ram[0x1000 + off++] = 0; ram[0x1000 + off++] = 0; ram[0x1000 + off++] = 0;
    ram[0x1000 + off++] = 0x66; ram[0x1000 + off++] = 0x67; ram[0x1000 + off++] = 0x89; ram[0x1000 + off++] = 0x05;
    ram[0x1000 + off++] = 0x10; ram[0x1000 + off++] = 0x00; ram[0x1000 + off++] = 0xC0; ram[0x1000 + off++] = 0xFE;
    ram[0x1000 + off++] = 0xF4;
    wreg(v, HV_X86_RIP, 0x1000);
    hv_vcpu_run(v);
    code = 0;
    printf("  %-52s ret=%d code=%u RIP %#llx\n", "EOI run", hv_vcpu_exit_info(v, &code), code,
           (unsigned long long)rvmcs(v, VMCS_GUEST_RIP));
    vec = 0;
    printf("  %-52s ret=%d vec=%#x\n", "exit_ioapic_eoi", hv_vcpu_exit_ioapic_eoi(v, &vec), vec);
    hv_vcpu_destroy(v);
    hv_vm_destroy();
    munmap(ram, RAM_SIZE);
}

/** PIO notifier: a real-mode guest writes 0x42 to port 0x3f8. The provider should send an
 * hv_ion_message_t to the registered Mach port, suppress the I/O exit and step over the OUT, so
 * the run ends at the following HLT (0x1006) instead of at the OUT (0x1005). */
static void probe_pio_notifier(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap"); return; }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0xBA; ram[0x1001] = 0xF8; ram[0x1002] = 0x03; /* mov dx, 0x3f8 */
    ram[0x1003] = 0xB0; ram[0x1004] = 0x42;                     /* mov al, 0x42  */
    ram[0x1005] = 0xEE;                                          /* out dx, al    */
    ram[0x1006] = 0xF4;                                          /* hlt           */

    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    printf("mach_port_allocate                              kr=0x%x port=%u\n", kr, port);
    if (kr != KERN_SUCCESS) { munmap(ram, RAM_SIZE); return; }
    /* hv_vm_add_pio_notifier requires a send right; the receive right alone is not enough. */
    kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    printf("mach_port_insert_right(MAKE_SEND)               kr=0x%x\n", kr);
    if (kr != KERN_SUCCESS) { mach_port_deallocate(mach_task_self(), port); munmap(ram, RAM_SIZE); return; }

    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    print_call("hv_vm_create", r);
    if (r != HV_SUCCESS) {
        mach_port_deallocate(mach_task_self(), port);
        munmap(ram, RAM_SIZE);
        return;
    }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) {
        hv_vm_destroy();
        mach_port_deallocate(mach_task_self(), port);
        munmap(ram, RAM_SIZE);
        return;
    }
    setup_controls(v);
    setup_real_mode(v, 0x1000);
    wreg(v, HV_X86_RFLAGS, 2);

    print_call("hv_vm_add_pio_notifier(0x3f8)", hv_vm_add_pio_notifier(0x3F8, 1, 0x42, port, 0));
    hv_vcpu_run(v);
    uint64_t rip = rvmcs(v, VMCS_GUEST_RIP);
    printf("  %-52s RIP %#llx (%s)\n", "guest run after OUT", (unsigned long long)rip,
           rip == 0x1006 ? "OUT suppressed, HLT reached" : "NOT suppressed");

    /* The kernel appends a mach_msg_trailer_t to the 48-byte notifier message. */
    struct { mach_msg_header_t h; uint64_t addr, size, value; uint8_t trailer[64]; } msg;
    kr = mach_msg(&msg.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), port, 500, MACH_PORT_NULL);
    printf("  %-52s kr=0x%x msgh_size=%u addr=%llx size=%llu value=%llx\n", "notifier message", kr,
           msg.h.msgh_size, (unsigned long long)msg.addr, (unsigned long long)msg.size,
           (unsigned long long)msg.value);

    print_call("hv_vm_remove_pio_notifier(0x3f8)", hv_vm_remove_pio_notifier(0x3F8, 1, 0x42, port, 0));
    hv_vcpu_destroy(v);
    hv_vm_destroy();
    mach_port_deallocate(mach_task_self(), port);
    munmap(ram, RAM_SIZE);
}

/** Guest-side LAPIC MMIO: MOV EAX,[0xFEE00080] then HLT. The model's TPR is set to 0x55 first,
 * so an in-kernel NPF emulation returns 0x55 in EAX and resumes at the HLT (0x1006). */
static void probe_apic_mmio(void)
{
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap"); return; }
    memset(ram, 0, RAM_SIZE);
    ram[0x1000] = 0x66; ram[0x1001] = 0x8B; ram[0x1002] = 0x05;
    ram[0x1003] = 0x80; ram[0x1004] = 0x00; ram[0x1005] = 0xE0; ram[0x1006] = 0xFE;
    ram[0x1007] = 0xF4;                       /* HLT after the emulated load */

    hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
    print_call("hv_vm_create(HV_VM_ACCEL_APIC)", r);
    if (r != HV_SUCCESS) { munmap(ram, RAM_SIZE); return; }
    print_call("hv_vm_map", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) { hv_vm_destroy(); munmap(ram, RAM_SIZE); return; }
    setup_controls(v);
    setup_real_mode(v, 0x1000);

    /* The front end mirrors HV_X86_TPR into the LAPIC model on every run, so this is enough to give
     * the emulated MMIO read a non-zero value. */
    print_call("write TPR", hv_vcpu_write_register(v, HV_X86_TPR, 0x55));
    uint64_t tprRead = 0;
    hv_vcpu_read_register(v, HV_X86_TPR, &tprRead);
    printf("  %-52s %#llx\n", "  (TPR readback)", (unsigned long long)tprRead);

    uint64_t ripBefore = 0;
    hv_vcpu_read_register(v, HV_X86_RIP, &ripBefore);
    printf("  %-52s %#llx\n", "  (RIP before run)", (unsigned long long)ripBefore);
    r = hv_vcpu_run(v);
    for (int i = 0; i < 1000 && r == HV_SUCCESS && (rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF) == 1; i++)
        r = hv_vcpu_run(v);
    uint64_t rip = 0, rax = 0;
    hv_vmx_vcpu_read_vmcs(v, VMCS_GUEST_RIP, &rip);
    hv_vcpu_read_register(v, HV_X86_RAX, &rax);
    printf("  %-52s %s, exit %" PRIu64 ", RIP %#" PRIx64 ", EAX %#" PRIx64 " (%s)\n",
           "guest MOV EAX,[LAPIC TPR]", hv_name(r), rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF,
           rip, rax, rip == 0x1007 && (rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF) == 12
               ? "instruction completed" : "unexpected exit");
    hv_vcpu_destroy(v);
    hv_vm_destroy();
    munmap(ram, RAM_SIZE);
}

/**
 * @brief Handle an internal switch used when this tool re-executes itself in a child task.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return True when the switch was ours and main should return immediately.
 */
static bool handle_internal_mode(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--accel-apic") == 0) {
        hv_return_t r = hv_vm_create(HV_VM_ACCEL_APIC);
        printf("  %-52s %s\n", "hv_vm_create(HV_VM_ACCEL_APIC)", hv_name(r));
        if (r == HV_SUCCESS)
            hv_vm_destroy();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--vcpu-count") == 0) {
        probe_vcpu_count();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--double-create") == 0) {
        /* A second hv_vm_create must answer HV_BUSY. Stop before hv_vm_destroy: on this framework a
         * task that asked twice aborts inside hv_vmx_vm_destroy (a userspace heap error, not the kext). */
        hv_vm_create(HV_VM_DEFAULT);
        hv_return_t again = hv_vm_create(HV_VM_DEFAULT);
        printf("  %-52s %s\n", "hv_vm_create twice in one task", hv_name(again));
        fflush(stdout);
        _exit(0);
    }
    if (argc > 1 && strcmp(argv[1], "--matrix") == 0) {
        probe_api_matrix();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--apic") == 0) {
        probe_apic_family();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--apic-run") == 0) {
        probe_apic_run();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--apic-inject") == 0) {
        probe_apic_inject();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--pio-notifier") == 0) {
        probe_pio_notifier();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--exitinfo") == 0) {
        probe_exitinfo();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--exitinfo-mmio") == 0) {
        probe_exitinfo_mmio();
        return true;
    }
    if (argc > 1 && strcmp(argv[1], "--apic-mmio") == 0) {
        probe_apic_mmio();
        return true;
    }
    return false;
}

/**
 * @brief Tool entry point: dispatch the command or run the probe sequence.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 *
 * @return Process exit status.
 */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    static char self_path[4096];
    uint32_t self_len = sizeof(self_path);
    g_self = _NSGetExecutablePath(self_path, &self_len) == 0 ? self_path : argv[0];
    if (handle_internal_mode(argc, argv))
        return 0;

    printf("Hypervisor.framework provider probe\n");

    printf("\n[1] VM creation\n");
    run_self("--accel-apic");
    run_self("--double-create");
    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    print_call("hv_vm_create(HV_VM_DEFAULT)", r);
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
    print_call("hv_vm_map (1 MiB, R|W|X)", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    print_call("hv_vm_map over an existing mapping", hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ));
    print_call("hv_vm_map unaligned user address", hv_vm_map(ram + 1, 0x200000, PAGE, HV_MEMORY_READ));
    print_call("hv_vm_protect", hv_vm_protect(0x1000, PAGE, HV_MEMORY_READ | HV_MEMORY_EXEC));
    print_call("hv_vm_sync_tsc", hv_vm_sync_tsc(1));

    hv_vm_space_t asid = 0;
    r = hv_vm_space_create(&asid);
    print_call("hv_vm_space_create", r);
    if (r == HV_SUCCESS) {
        uint8_t *extra = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        memset(extra, 0, PAGE);
        print_call("hv_vm_map_space", hv_vm_map_space(asid, extra, 0x300000, PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE));
        print_call("hv_vm_protect_space", hv_vm_protect_space(asid, 0x300000, PAGE, HV_MEMORY_READ));
        print_call("hv_vm_unmap_space", hv_vm_unmap_space(asid, 0x300000, PAGE));
        print_call("hv_vm_space_destroy", hv_vm_space_destroy(asid));
        munmap(extra, PAGE);
    }

    printf("\n[3] vCPU\n");
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    print_call("hv_vcpu_create", r);
    if (r != HV_SUCCESS) {
        hv_vm_destroy();
        munmap(ram, RAM_SIZE);
        return 0;
    }
    setup_controls(v);
    setup_real_mode(v, 0x1000);

    hv_vm_space_t vspace = 0;
    r = hv_vm_space_create(&vspace);
    print_call("hv_vm_space_create (for set_space)", r);
    if (r == HV_SUCCESS) {
        print_call("hv_vcpu_set_space(space)", hv_vcpu_set_space(v, vspace));
        print_call("hv_vm_space_destroy while bound", hv_vm_space_destroy(vspace));
        print_call("hv_vcpu_set_space(default)", hv_vcpu_set_space(v, HV_VM_SPACE_DEFAULT));
        print_call("hv_vcpu_set_space(unknown)", hv_vcpu_set_space(v, vspace + 1000));
        print_call("hv_vm_space_destroy", hv_vm_space_destroy(vspace));
    }

    uint64_t t = 0;
    print_call("hv_vcpu_read_register(RIP)", hv_vcpu_read_register(v, HV_X86_RIP, &t));
    print_call("hv_vcpu_write_msr(LSTAR)", hv_vcpu_write_msr(v, 0xC0000082, 0xffffff8000100000ull));
    print_call("hv_vcpu_read_msr(LSTAR)", hv_vcpu_read_msr(v, 0xC0000082, &t));
    print_call("hv_vcpu_enable_native_msr(FS_BASE)", hv_vcpu_enable_native_msr(v, 0xC0000100, true));
    print_call("hv_vcpu_set_msr_access(FS_BASE, off)", hv_vcpu_set_msr_access(v, 0xC0000100, false));
    uint8_t fp[4096];
    print_call("hv_vcpu_read_fpstate", hv_vcpu_read_fpstate(v, fp, sizeof(fp)));
    print_call("hv_vcpu_invalidate_tlb", hv_vcpu_invalidate_tlb(v));
    print_call("hv_vcpu_inject_extint", hv_vcpu_inject_extint(v));
    uint32_t vmxStatus = 0;
    print_call("hv_vcpu_vmx_status", hv_vcpu_vmx_status(v, &vmxStatus));
    uint32_t exitCode = 0;
    print_call("hv_vcpu_exit_info", hv_vcpu_exit_info(v, &exitCode));

    printf("\n[4] run and run_until\n");
    setup_real_mode(v, 0x1000);
    r = hv_vcpu_run(v);
    printf("  %-52s %s, exit %" PRIu64 "\n", "hv_vcpu_run (guest executes HLT)", hv_name(r),
           rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);

    /* A deadline that has already passed: an Intel provider returns at once with the preemption-timer
     * reason (52). A provider that ignores the deadline runs the guest until the next exit. */
    setup_real_mode(v, 0x1100);
    g_kick_vcpu = v;
    g_kick_cancel = 0;
    pthread_t th;
    pthread_create(&th, NULL, kicker, (void *)(intptr_t)200);
    uint64_t t0 = mach_absolute_time();
    r = hv_vcpu_run_until(v, t0 - 1);
    uint64_t t1 = mach_absolute_time();
    cancel_kicker();
    pthread_join(th, NULL);
    printf("  %-52s %s, %.3f ms, exit %" PRIu64 "\n", "hv_vcpu_run_until(past deadline)", hv_name(r),
           nanos(t1 - t0) / 1e6, rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);

    /* HV_DEADLINE_FOREVER must behave like hv_vcpu_run: block until an exit or a kick. */
    setup_real_mode(v, 0x1100);
    g_kick_cancel = 0;
    pthread_create(&th, NULL, kicker, (void *)(intptr_t)100);
    t0 = mach_absolute_time();
    r = hv_vcpu_run_until(v, HV_DEADLINE_FOREVER);
    t1 = mach_absolute_time();
    cancel_kicker();
    pthread_join(th, NULL);
    printf("  %-52s %s, %.3f ms, exit %" PRIu64 "\n", "hv_vcpu_run_until(FOREVER) + interrupt", hv_name(r),
           nanos(t1 - t0) / 1e6, rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);

    printf("\n[5] time and APIC\n");
    uint64_t exec = 0, idle = 0;
    print_call("hv_vcpu_get_exec_time", hv_vcpu_get_exec_time(v, &exec));
    print_call("hv_vcpu_get_idle_time", hv_vcpu_get_idle_time(v, &idle));
    printf("  %-52s %" PRIu64 " ns / %" PRIu64 " ns\n", "  (exec / idle)", exec, idle);

    uint32_t data = 0;
    print_call("hv_vcpu_apic_read(0x80)", hv_vcpu_apic_read(v, 0x80, &data));
    bool noSideEffect = false;
    print_call("hv_vcpu_apic_write(0x80)", hv_vcpu_apic_write(v, 0x80, 0, &noSideEffect));
    print_call("hv_vm_lapic_set_intr(0x20)", hv_vm_lapic_set_intr(v, 0x20, HV_APIC_EDGE_TRIGGER));
    print_call("hv_vm_lapic_msi", hv_vm_lapic_msi(0xFEE00000, 0x4000));
    print_call("hv_vm_ioapic_read", hv_vm_ioapic_read(0xFEC00000, &data));
    print_call("hv_vm_ioapic_write", hv_vm_ioapic_write(0xFEC00000, 0));
    print_call("hv_vm_ioapic_assert_irq(0)", hv_vm_ioapic_assert_irq(0));
    print_call("hv_vm_ioapic_deassert_irq(0)", hv_vm_ioapic_deassert_irq(0));

    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS &&
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS) {
        print_call("hv_vm_add_pio_notifier(0x3f8)", hv_vm_add_pio_notifier(0x3F8, 1, 0, port, 0));
        print_call("hv_vm_remove_pio_notifier(0x3f8)", hv_vm_remove_pio_notifier(0x3F8, 1, 0, port, 0));
        mach_port_deallocate(mach_task_self(), port);
    }

    printf("\n[6] teardown\n");
    print_call("hv_vcpu_destroy", hv_vcpu_destroy(v));
    print_call("hv_vm_destroy", hv_vm_destroy());
    munmap(ram, RAM_SIZE);

    printf("\n[7] vCPU limit (separate task)\n");
    run_self("--vcpu-count");

    printf("\n[8] full public API matrix (separate task)\n");
    run_self("--matrix");

    printf("\ndone. HV_UNSUPPORTED = the provider does not implement the call; HV_BAD_ARGUMENT/\n"
           "HV_NO_DEVICE = it implements it but refuses this request. Compare this output before and\n"
           "after loading a new kext (a new kext needs a reboot to take effect).\n");
    return 0;
}
