/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/ArmSmmuDetachLib.h>

#include "PlatformRegisters.h"

VOID
DisableWatchDogTimer ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR ApssRegion;

  // Locate "APSS_HM" Memory Region
  Status = LocateMemoryRegionByName ("APSS_HM", &ApssRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Disable WatchDog Timer
  MmioWrite32 (ApssRegion.Address + APSS_WDT_OFFSET + APSS_WDT_ENABLE_OFFSET, 0x0);
}

VOID
PlatformInitialize ()
{
  // Disable WatchDog Timer
  DisableWatchDogTimer ();

  CONST UINT16 RetainedStreams[] = {
    0x820, 0x821, 0x822, 0xC20, 0xC21, 0xC22,
#if defined (ELISH_UFS_HANDOFF_PROBE)
    // SecMain has checked these inherited matches and their context state.
    0x4E0, 0xE0,
#endif
  };

  // Detach IOMMU Domains
  ArmSmmuDetach (RetainedStreams, ARRAY_SIZE (RetainedStreams));
}
