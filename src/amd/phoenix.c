/*
Vendor Reset - Vendor Specific Reset
Copyright (C) 2024 Vendor Reset Contributors

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

/*
 * AMD Phoenix / Hawk Point (RDNA 3 / GC 11.0.3) PSP Mode 1 Reset
 *
 * Supports Radeon 780M iGPU found in Ryzen 7040 (Phoenix) and
 * Ryzen 8040 (Hawk Point) series APUs.
 *
 * The reset mechanism is identical to Navi10: use the PSP's Mode 1
 * reset command to perform a BACO-like reset of the entire GPU.
 * IP Discovery Table in VRAM provides register base offsets, so no
 * hardcoded register base init is needed for these ASICs.
 *
 * SMN addresses for Phoenix (SMU v13.x / NBIO 4.3.x):
 *   MP1_Public            = 0x03b00000  (same as SMU v11)
 *   smnMP1_FIRMWARE_FLAGS = 0x3010028   (different from SMU v11: 0x3010024)
 *   smnMP1_PUB_CTRL       = 0x3010b14   (same as SMU v11)
 */

#include <linux/delay.h>

#include "vendor-reset-dev.h"

#include "amd.h"
#include "common_defs.h"
#include "common.h"
#include "firmware.h"
#include "amdgpu_discovery.h"

/* Phoenix uses MP0 v13.0.0 with 'reg' prefix naming */
#include "mp/mp_13_0_0_offset.h"
#include "mp/mp_13_0_0_sh_mask.h"
#include "nbio_4_3_0_offset.h"

/* SOC15 register access macros (RREG32_SOC15, WREG32_SOC15, etc.) */
#include "soc15_common.h"

/* GFX_CTRL_CMD_ID_MODE1_RST for PSP mode 1 reset */
#include "psp_gfx_if.h"

/*
 * PCIe config space access registers.
 * These have fixed MMIO offsets and don't vary by NBIO IP version.
 */
#define mmPCIE_INDEX2  0x000e
#define mmPCIE_DATA2   0x000f

/* SMU v11 defines MP1_Public and smnMP1_PUB_CTRL at the same addresses,
 * but smnMP1_FIRMWARE_FLAGS is different on SMU v13 (Phoenix) */
#include "smu_v11_0.h"
#undef smnMP1_FIRMWARE_FLAGS
#define smnMP1_FIRMWARE_FLAGS 0x3010028

extern bool amdgpu_get_bios(struct amd_fake_dev *adev);

static int amd_phoenix_reset(struct vendor_reset_dev *dev)
{
  struct amd_vendor_private *priv = amd_private(dev);
  struct amd_fake_dev *adev;
  int ret = 0, timeout;
  u32 sol, tmp, offset;

  adev = &priv->adev;
  ret = amd_fake_dev_init(adev, dev);
  if (ret)
    return ret;

  /*
   * Read IP Discovery Table from VRAM to get register base addresses.
   * Phoenix / Hawk Point all support IP Discovery; if it fails we
   * cannot fall back since there are no hardcoded reg base tables.
   */
  ret = amdgpu_discovery_reg_base_init(adev);
  if (ret < 0)
  {
    vr_err(dev, "amdgpu_discovery_reg_base_init failed, Phoenix requires IP Discovery\n");
    goto free_adev;
  }

  if (!amdgpu_get_bios(adev))
  {
    vr_err(dev, "amdgpu_get_bios failed\n");
    ret = -ENOTSUPP;
    goto free_adev;
  }

  ret = atom_bios_init(adev);
  if (ret)
  {
    vr_err(dev, "atom_bios_init failed: %d\n", ret);
    goto free_adev;
  }

  /* Wait for SOC to be ready (SOL register) */
  for (timeout = 100000; timeout; --timeout)
  {
    sol = RREG32_SOC15(MP0, 0, regMP0_SMN_C2PMSG_81);
    if (sol != 0xFFFFFFFF && sol != 0)
      break;
    udelay(1);
  }

  if (sol == ~1L)
  {
    vr_warn(dev, "Timed out waiting for SOL to be valid\n");
    /* continue anyway - sometimes reset still works */
  }

  vr_info(dev, "bus reset disabled? %s\n",
          (dev->pdev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET) ? "yes" : "no");

  /*
   * Check if the GPU is already in a reset-friendly state.
   * SOL=0, MP1 interrupts disabled, PSP bootloader ready => already reset.
   */
  {
    u32 smu_resp = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90);
    u32 mp1_intr = (RREG32_PCIE(MP1_Public |
                                (smnMP1_FIRMWARE_FLAGS & 0xffffffff)) &
                    MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK) >>
                   MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED__SHIFT;
    u32 psp_bl_ready = !!(RREG32_SOC15(MP0, 0, regMP0_SMN_C2PMSG_35) & 0x80000000L);

    vr_info(dev, "SMU resp: %x, SOL: %x, MP1 intr: %s, BL ready: %s\n",
            smu_resp, sol, mp1_intr ? "yes" : "no",
            psp_bl_ready ? "yes" : "no");

    if (sol == 0x0 && !mp1_intr && psp_bl_ready)
      goto free_adev;
  }

  /* Tell driver that NVRAM is lost - everything needs reset */
  vr_info(dev, "Clearing scratch regs 6 and 7\n");
  WREG32(adev->bios_scratch_reg_offset + 6, 0);
  WREG32(adev->bios_scratch_reg_offset + 7, 0);

  /* Mark engine as hung for ATOM BIOS */
  amdgpu_atombios_scratch_regs_engine_hung(adev, true);

  /*
   * Save PCI state so we can restore after reset
   * The PSP reset will clear PCI config space
   */
  pci_save_state(dev->pdev);

  /* Check PSP readiness before sending reset command */
  offset = SOC15_REG_OFFSET(MP0, 0, regMP0_SMN_C2PMSG_64);
  tmp = psp_wait_for(adev, offset, 0x80000000, 0x8000FFFF, false);
  if (tmp)
    vr_warn(dev, "timed out waiting for PSP ready state, continuing anyway\n");

  /*
   * Issue PSP Mode 1 Reset command
   * This triggers a full GPU reset through the PSP firmware,
   * which is the standard reset method for RDNA 3 / SOC21 ASICs
   * that don't support SMU-assisted reset.
   */
  vr_info(dev, "Issuing PSP Mode 1 Reset\n");
  WREG32_SOC15(MP0, 0, regMP0_SMN_C2PMSG_64, GFX_CTRL_CMD_ID_MODE1_RST);
  msleep(500);

  /* Wait for PSP to acknowledge the reset command */
  offset = SOC15_REG_OFFSET(MP0, 0, regMP0_SMN_C2PMSG_33);
  tmp = psp_wait_for(adev, offset, 0x80000000, 0x80000000, false);
  if (tmp)
  {
    vr_warn(dev, "PSP did not acknowledge reset\n");
    ret = -EINVAL;
    goto out;
  }

  vr_info(dev, "PSP Mode 1 Reset acknowledged\n");

  pci_restore_state(dev->pdev);

  /* Wait for PCI config to become readable again */
  for (timeout = 100000; timeout; --timeout)
  {
    tmp = RREG32_SOC15(NBIO, 0, regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE);
    if (tmp != 0xffffffff)
      break;
    udelay(1);
  }

  /* Wait for PSP bootloader to come back */
  for (timeout = 100; timeout; --timeout)
  {
    if (RREG32_SOC15(MP0, 0, regMP0_SMN_C2PMSG_35) & 0x80000000L)
      break;
    msleep(100);
  }

  if (!timeout &&
      !(RREG32_SOC15(MP0, 0, regMP0_SMN_C2PMSG_35) & 0x80000000L))
  {
    vr_warn(dev, "timed out waiting for PSP bootloader after reset\n");
    ret = -ETIME;
  }
  else
    vr_info(dev, "PSP Mode 1 Reset successful\n");

out:
  pci_restore_state(dev->pdev);
  amdgpu_atombios_scratch_regs_engine_hung(adev, false);

free_adev:
  amd_fake_dev_fini(adev);

  return ret;
}

const struct vendor_reset_ops amd_phoenix_ops =
{
  .version = {1, 0},
  .probe = amd_common_probe,
  .pre_reset = amd_common_pre_reset,
  .reset = amd_phoenix_reset,
  .post_reset = amd_common_post_reset,
};
