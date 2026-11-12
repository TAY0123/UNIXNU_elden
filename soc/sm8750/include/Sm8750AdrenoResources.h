/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8750_ADRENO_RESOURCES_H
#define SM8750_ADRENO_RESOURCES_H

/* Register apertures from the SM8750P Linux handoff. The GMU register XML
 * uses GPU-relative dword indices, including for registers beyond the GPU
 * node's own MMIO aperture. Convert them to offsets within the GMU mapping. */
#define SM8750_ADRENO_GPU_BASE          0x03d00000U
#define SM8750_ADRENO_GMU_BASE          0x03d37000U
#define SM8750_ADRENO_GMU_BYTES         0x00068000U
#define SM8750_ADRENO_GMU_OFFSET(word)  (4U * (word) - (SM8750_ADRENO_GMU_BASE - SM8750_ADRENO_GPU_BASE))
#define SM8750_ADRENO_GMU_ITCM          SM8750_ADRENO_GMU_OFFSET(0x1b400U)
#define SM8750_ADRENO_GMU_DTCM          SM8750_ADRENO_GMU_OFFSET(0x1c400U)
#define SM8750_ADRENO_GMU_CM3_SYSRESET  SM8750_ADRENO_GMU_OFFSET(0x1f800U)

/* CM3 firmware address space. TCM is accessible through MMIO. The other
 * firmware windows require a GMU SMMU mapping and CPU-accessible backing. */
#define SM8750_GMU_ITCM_ADDRESS         0x00000000U
#define SM8750_GMU_DTCM_ADDRESS         0x10004000U
#define SM8750_GMU_TCM_BYTES            0x00004000U
#define SM8750_GMU_ICACHE_IOVA          0x00004000U
#define SM8750_GMU_ICACHE_BYTES         0x00ffc000U
#define SM8750_GMU_SCRATCH_IOVA         0x60000000U
#define SM8750_GMU_SCRATCH_BYTES        0x00004000U
#define SM8750_GMU_DEBUG_IOVA           0x60400000U
#define SM8750_GMU_DEBUG_BYTES          0x00007000U
#define SM8750_GMU_LOG_BYTES            0x00004000U
#define SM8750_GMU_HFI_BYTES            0x00004000U
#define SM8750_GMU_PAGE_BYTES           0x00001000U
#define SM8750_GPU_SMMU_BASE            0x03da0000U
#define SM8750_GPU_SMMU_BYTES           0x00040000U
#define SM8750_GMU_STREAM_ID            5U

/* GPUCC CX register page. Offsets are relative to this page. */
#define SM8750_GPUCC_CX_PAGE            0x03d99000U
#define SM8750_GPUCC_CX_GDSCR           0x080U
#define SM8750_GPUCC_CX_GDS_HW_CTRL      0x094U
#define SM8750_GPUCC_AHB_CBCR           0x0bcU
#define SM8750_GPUCC_CX_GMU_CBCR        0x0d4U
#define SM8750_GPUCC_CXO_CBCR           0x0e4U
#define SM8750_GPUCC_HUB_CX_INT_CBCR     0x0e8U

/* AP RPMh RSC DRV-2. The Elden Linux DT selects DRV-2 and assigns the
 * first three TCSes to active requests. The other TCSes are for sleep/wake. */
#define SM8750_APPS_RSC_DRV2_BASE       0x16520000U
#define SM8750_APPS_RSC_DRV2_PROBE_SIZE 0x00002000U
#define SM8750_APPS_RSC_TCS_BASE        0x0d00U
#define SM8750_APPS_RSC_TCS_STRIDE      0x02a0U
#define SM8750_APPS_RSC_ACTIVE_TCS      3U

#endif
