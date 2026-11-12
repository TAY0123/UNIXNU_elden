/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750GpuPower.c"
#include "Sm8750Qup1Resources.h"

typedef struct {
    vm_offset_t gcc;
    vm_offset_t gpucc;
    Sm8750GpuPower power;
    const Sm8750RpmhGpuRailResult *rails;
    unsigned reported_step;
} EldenGpuPowerProbe;

static int
elden_gpu_probe_register(EldenGpuPowerProbe *probe, Sm8750GpuPowerBlock block,
    uint32_t reg, vm_offset_t *base)
{
    if (reg & 3U) return -1;
    if (block == Sm8750GpuGcc && reg < SM8750_GCC_GPU_MAP_BYTES) {
        *base = probe->gcc;
    } else if (block == Sm8750GpuGpucc && reg < SM8750_GPUCC_MAP_BYTES) {
        *base = probe->gpucc;
    } else {
        return -1;
    }
    if (probe->reported_step != (unsigned)probe->power.step) {
        probe->reported_step = (unsigned)probe->power.step;
        printf("ELDEN-A830: GPUCC entering step=%u block=%u reg=%05x\n",
            probe->reported_step, (unsigned)block, reg);
    }
    return 0;
}

static int
elden_gpu_probe_read(void *cookie, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t *value)
{
    vm_offset_t base;
    if (elden_gpu_probe_register(cookie, block, reg, &base)) return -1;
    *value = elden_adreno830_rpmh_read32((void *)base, reg);
    return 0;
}

static int
elden_gpu_probe_write(void *cookie, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t value)
{
    vm_offset_t base;
    if (elden_gpu_probe_register(cookie, block, reg, &base)) return -1;
    elden_adreno830_rpmh_write32((void *)base, reg, value);
    return 0;
}

static int
elden_gpu_probe_delay(void *cookie, unsigned us)
{
    elden_adreno830_rpmh_delay(cookie, us);
    return 0;
}

static int
elden_gpu_probe_rails(void *cookie)
{
    const EldenGpuPowerProbe *probe = cookie;
    return probe->rails->mx.acknowledged && probe->rails->cx.acknowledged ? 0 : -1;
}

static unsigned elden_gpu_gcc_vote_busy;

static int
elden_gpu_probe_vote(void *cookie, uint32_t mask, uint32_t value, uint32_t *prior)
{
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&elden_gpu_gcc_vote_busy, &expected, 1, false,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return -1;

    uint32_t current;
    int error = elden_gpu_probe_read(cookie, Sm8750GpuGcc, SM8750_GCC_GPU_VOTE_REG, &current);
    if (!error && current != UINT32_MAX) {
        *prior = current & mask;
        error = elden_gpu_probe_write(cookie, Sm8750GpuGcc, SM8750_GCC_GPU_VOTE_REG,
            (current & ~mask) | (value & mask));
    } else {
        error = -1;
    }
    __atomic_store_n(&elden_gpu_gcc_vote_busy, 0, __ATOMIC_RELEASE);
    return error;
}

static bool
elden_gpu_probe_counter(void *mapping, uint64_t *ticks)
{
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t high = elden_adreno830_rpmh_read32(mapping, SM8750_ADRENO_GMU_COUNTER_HIGH);
        uint32_t low = elden_adreno830_rpmh_read32(mapping, SM8750_ADRENO_GMU_COUNTER_LOW);
        uint32_t high_after = elden_adreno830_rpmh_read32(mapping, SM8750_ADRENO_GMU_COUNTER_HIGH);
        if (high == high_after) {
            *ticks = ((uint64_t)high << 32) | low;
            return *ticks != UINT64_MAX;
        }
    }
    return false;
}

static void
elden_gpu_probe_gmu(void)
{
    printf("ELDEN-A830: GPUCC clocks ready, reading GMU reset and counter\n");
    vm_offset_t address = io_map(SM8750_ADRENO_GMU_BASE, SM8750_ADRENO_GMU_BYTES,
        VM_WIMG_IO, VM_PROT_READ, true);
    if (!address) {
        printf("ELDEN-A830: GMU read-only map failed\n");
        return;
    }
    void *mapping = (void *)address;
    uint32_t reset = elden_adreno830_rpmh_read32(mapping, SM8750_ADRENO_GMU_CM3_SYSRESET);
    uint64_t start = 0, end = 0;
    bool valid = elden_gpu_probe_counter(mapping, &start);
    delay(100);
    valid = elden_gpu_probe_counter(mapping, &end) && valid;
    ml_io_unmap(address, SM8750_ADRENO_GMU_BYTES);
    printf("ELDEN-A830: GMU reset=%08x counter=%llx/%llx advanced=%u\n",
        reset, (unsigned long long)start, (unsigned long long)end, (unsigned)(valid && end > start));
}

/* The RAM probe is the sole GPUCC owner in this kernel. Other active Elden
 * drivers use different GCC registers. The managed reset ends the lease. */
static void
elden_adreno830_probe_power(const Sm8750RpmhGpuRailResult *rails)
{
    const vm_prot_t protections = VM_PROT_READ | VM_PROT_WRITE;
    EldenGpuPowerProbe probe = {.rails = rails};
    probe.gcc = io_map(SM8750_GCC_BASE, SM8750_GCC_GPU_MAP_BYTES, VM_WIMG_IO, protections, true);
    if (probe.gcc) {
        probe.gpucc = io_map(SM8750_GPUCC_BASE, SM8750_GPUCC_MAP_BYTES, VM_WIMG_IO, protections, true);
    }
    if (probe.gpucc) {
        const Sm8750GpuPowerIo io = {
            &probe, elden_gpu_probe_read, elden_gpu_probe_write, elden_gpu_probe_delay,
            elden_gpu_probe_rails, elden_gpu_probe_vote,
        };
        Sm8750GpuPowerStatus status = Sm8750GpuPowerOn(&probe.power, &io);
        printf("ELDEN-A830: GPUCC power result=%u step=%u block=%u reg=%05x value=%08x\n",
            (unsigned)status, (unsigned)probe.power.step, (unsigned)probe.power.block,
            probe.power.reg, probe.power.value);
        if (status == Sm8750GpuPowerOk) elden_gpu_probe_gmu();
        ml_io_unmap(probe.gpucc, SM8750_GPUCC_MAP_BYTES);
    } else {
        printf("ELDEN-A830: GPUCC map failed GCC=%u\n", (unsigned)(probe.gcc != 0));
    }
    if (probe.gcc) ml_io_unmap(probe.gcc, SM8750_GCC_GPU_MAP_BYTES);
}
