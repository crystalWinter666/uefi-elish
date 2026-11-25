/**
  Copyright (c) 2011-2017, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/RamLogLib.h>
#include <Library/PrePiLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/HobLib.h>

#if defined (ELISH_SEC_SMMU_PROBE)
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#endif

#include "Sec.h"

// First argument (x0) of the previous stage, saved by ModuleEntryPoint.S
extern UINT64  gSecEntryArg0;

// The library constructors ProcessLibraryConstructorList () runs for this module, in its generated order, called one by one so the screen
// shows which one a stall is in (an entry here that the generated list lacks fails to link, which is the check).
// Helper.S: clean and invalidate every data cache level by set/way (see there).
extern VOID SecCleanInvalidateDataCacheAll (VOID);

extern RETURN_STATUS EFIAPI BaseDebugLibSerialPortConstructor (VOID);
extern RETURN_STATUS EFIAPI ExtractGuidedSectionLibConstructor (VOID);
extern RETURN_STATUS EFIAPI ArmMmuBaseLibConstructor (VOID);
extern RETURN_STATUS EFIAPI LzmaDecompressLibConstructor (VOID);

// Walks the HOB list with a bound, so a zero-length or runaway entry is reported instead of looping forever in the constructors and in
// the FV decompression that follow (both search the list).
STATIC
VOID
CheckHobList (VOID)
{
  EFI_PEI_HOB_POINTERS  Hob;
  UINTN                 Count = 0;

  Hob.Raw = GetHobList ();
  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobLength < sizeof (EFI_HOB_GENERIC_HEADER) || Count >= 16384) {
      DEBUG ((EFI_D_ERROR, "SEC: HOB list broken at entry %u: type 0x%x length %u\n", (UINT32)Count, (UINT32)Hob.Header->HobType, (UINT32)Hob.Header->HobLength));
      return;
    }

    Count++;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  DEBUG ((EFI_D_WARN, "SEC: HOB list ok, %u entries\n", (UINT32)Count));
}

#ifndef MDEPKG_NDEBUG
STATIC
VOID
ClearFrameBuffer (VOID)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion;

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);

  // Clear Memory Region
  if (FrameBufferRegion.Address != 0 && FrameBufferRegion.Length != 0) {
    ZeroMem ((VOID *)FrameBufferRegion.Address, FrameBufferRegion.Length);
  }
}

STATIC
VOID
PrintFirmwareVersion (VOID)
{
  // Clear Frame Buffer
  ClearFrameBuffer ();

  // Print UEFI Version Message
  DEBUG ((EFI_D_WARN, "\n"));
  DEBUG ((EFI_D_WARN, "Project Silicium for %a %a\n",            FixedPcdGetPtr (PcdSmbiosSystemManufacturer), FixedPcdGetPtr (PcdSmbiosSystemModel)));
  DEBUG ((EFI_D_WARN, "Firmware Version %s Build at %a on %a\n", PcdGetPtr (PcdFirmwareVersionString), __TIME__, __DATE__));
  DEBUG ((EFI_D_WARN, "\n"));
}
#endif

STATIC
EFI_STATUS
InitializeMemory (VOID)
{
  EFI_STATUS                    Status;
  EFI_MEMORY_REGION_DESCRIPTOR  UefiMemoryRegion;
  EFI_HOB_HANDOFF_INFO_TABLE   *HobList;

  // Locate "DXE Heap" Memory Region
  LocateMemoryRegionByName ("DXE Heap", &UefiMemoryRegion);
  LocateMemoryRegionByName ("DXE_Heap", &UefiMemoryRegion);

  // Save UEFI Memory Details
  UINTN UefiMemoryBase = UefiMemoryRegion.Address;
  UINTN UefiMemorySize = UefiMemoryRegion.Length;

  // Verify UEFI Memory Base
  if (UefiMemoryBase == 0 && UefiMemorySize == 0) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"DXE Heap\" Memory Region!\n"));
    return EFI_NOT_FOUND;
  }

  // Initialize HOB List
  HobList = HobConstructor ((VOID *)UefiMemoryBase, UefiMemorySize, (VOID *)UefiMemoryBase, (VOID *)(UefiMemoryBase + UefiMemorySize));
  PrePeiSetHobList (HobList);

  // Initialize MMU & Memory HOBs
  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Build Stack HOB
  BuildStackHob (FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));

  return EFI_SUCCESS;
}

#if defined (ELISH_SEC_SMMU_PROBE)
STATIC
VOID
ElishSecSmmuLog (IN CONST CHAR8 *Format, ...)
{
  CHAR8    Line[192];
  VA_LIST  Args;
  UINTN    Length;

  VA_START (Args, Format);
  Length = AsciiVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);
  RamLogWrite ((CONST UINT8 *)Line, Length);
}

STATIC
BOOLEAN
ElishSecSmmuSnapshot (VOID)
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;
  UINTN                         Base;
  UINTN                         PageShift;
  UINTN                         GlobalPages;
  UINT32                        Control;
  UINT32                        Id0;
  UINT32                        Id1;
  UINT32                        Id2;
  UINT32                        Valid = 0;
  UINT32                        Seen = 0;
  UINT32                        Qualified = 0;
  BOOLEAN                       Conflict = FALSE;

  // Use the same GR0 aperture as Kona's SEC detach, before that code runs.
  if (EFI_ERROR (LocateMemoryRegionByName ("SMMU", &Region)) ||
      Region.Address != 0x15000000 || Region.Length < SIZE_4KB ||
      Region.ResourceType != MMAP_IO || Region.MemoryType != MmIO ||
      Region.ArmAttributes != NS_DEVICE) {
    ElishSecSmmuLog ("ELISH_SEC_SMMU: unexpected MMIO descriptor\n");
    return FALSE;
  }

  Base    = (UINTN)Region.Address;
  Control = MmioRead32 (Base);
  Id0     = MmioRead32 (Base + 0x20);
  Id1     = MmioRead32 (Base + 0x24);
  Id2     = MmioRead32 (Base + 0x28);
  ElishSecSmmuLog ("ELISH_SEC_SMMU: sCR0=0x%08x ID0=0x%08x ID1=0x%08x ID2=0x%08x\n",
                  Control, Id0, Id1, Id2);
  if (Control == MAX_UINT32 || Id0 == MAX_UINT32 || Id1 == MAX_UINT32 ||
      Id2 == MAX_UINT32 || (Control & BIT3) != 0 || (Id0 & BIT27) == 0 || (Id0 & 0xff) == 0) {
    ElishSecSmmuLog ("ELISH_SEC_SMMU: unsupported stream-match layout\n");
    return FALSE;
  }

  PageShift = (Id1 & BIT31) != 0 ? 16 : 12;
  GlobalPages = 1U << (((Id1 >> 28) & 7U) + 1);
  for (UINT32 Group = 0; Group < (Id0 & 0xff); Group++) {
    UINT32 Smr = MmioRead32 (Base + 0x800 + Group * sizeof (UINT32));
    if ((Smr & BIT31) == 0) {
      continue;
    }

    UINT32 S2cr = MmioRead32 (Base + 0xc00 + Group * sizeof (UINT32));
    UINT32 Bank = S2cr & 0xff;
    UINT32 Type = (S2cr >> 16) & 3U;
    UINTN Gr1 = (1U << PageShift) + Bank * sizeof (UINT32);
    UINTN Context = (GlobalPages + Bank) << PageShift;
    if (Smr == MAX_UINT32 || S2cr == MAX_UINT32) {
      ElishSecSmmuLog ("ELISH_SEC_SMMU: all-ones stream register at group=%u\n", Group);
      return FALSE;
    }
    Valid++;
    ElishSecSmmuLog ("ELISH_SEC_SMMU: group=%u SMR=0x%08x SID=0x%x mask=0x%x S2CR=0x%08x type=%u cb=%u\n",
                    Group, Smr, Smr & 0x7fff, (Smr >> 16) & 0x7fff, S2cr, Type, Bank);
    UINT32 Sid = Smr & 0x7fff;
    UINT32 Mask = (Smr >> 16) & 0x7fff;
    BOOLEAN MatchesArid0 = ((Sid ^ 0x4e0) & ~Mask) == 0;
    BOOLEAN MatchesArid10 = ((Sid ^ 0xe0) & ~Mask) == 0;
    UINT32 Route = 0;
    if (MatchesArid0 && Mask == 0 && S2cr == 2) {
      Route = BIT0;
    } else if (MatchesArid10 && Mask == 0 && S2cr == 3) {
      Route = BIT1;
    } else if (MatchesArid0 || MatchesArid10 || (Type == 0 && (Bank == 2 || Bank == 3))) {
      Conflict = TRUE;
    }
    if ((Seen & Route) != 0) {
      Conflict = TRUE;
    }
    Seen |= Route;
    if (Type == 0 && Bank < (Id1 & 0xff) &&
        Gr1 + 0x800 + sizeof (UINT32) <= Region.Length &&
        Context + 0x30 + sizeof (UINT32) <= Region.Length) {
      UINT32 Cbar = MmioRead32 (Base + Gr1);
      UINT32 Cba2r = MmioRead32 (Base + Gr1 + 0x800);
      UINT32 Sctlr = MmioRead32 (Base + Context);
      UINT64 Ttbr0 = MmioRead64 (Base + Context + 0x20);
      UINT32 Tcr = MmioRead32 (Base + Context + 0x30);
      ElishSecSmmuLog ("ELISH_SEC_SMMU: cb=%u CBAR=0x%08x CBA2R=0x%08x SCTLR=0x%08x TTBR0=0x%016lx TCR=0x%08x\n",
                      Bank, Cbar, Cba2r, Sctlr, Ttbr0, Tcr);
      if (Cbar == 0x0001f000 && Cba2r == 1 && Sctlr == 0xe0 && Ttbr0 == 0 && Tcr == 0) {
        Qualified |= Route;
      }
    }
  }

  ElishSecSmmuLog ("ELISH_SEC_SMMU: pre-detach snapshot complete, valid groups=%u\n", Valid);
  return !Conflict && Qualified == (BIT0 | BIT1) && (Control & (BIT0 | BIT10)) == BIT10 &&
         (Id2 & 0xfff) == 0x111;
}
#endif

STATIC
VOID
SecMain (VOID)
{
  EFI_STATUS Status;

#if defined (ELISH_SEC_SMMU_PROBE)
  RamLogReset ();
  ElishSecSmmuLog ("ELISH_SEC_SMMU: entering before PlatformInitialize\n");
#if defined (ELISH_UFS_HANDOFF_PROBE)
  if (!ElishSecSmmuSnapshot ()) {
    ElishSecSmmuLog ("ELISH_SEC_SMMU: inherited UFS routes failed qualification\n");
    RamLogRestartIntoFastboot ();
    CpuDeadLoop ();
  }
  ElishSecSmmuLog ("ELISH_SEC_SMMU: retaining qualified UFS routes through SEC\n");
#else
  ElishSecSmmuSnapshot ();
  RamLogRestartIntoFastboot ();
  CpuDeadLoop ();
#endif
#endif

  // Do Platform Specific Initialization
  PlatformInitialize ();

#ifndef MDEPKG_NDEBUG
#if !defined (ELISH_SEC_SMMU_PROBE)
  RamLogReset ();
#endif

  // Initialize Serial Port
  Status = SerialPortInitialize ();
  if (EFI_ERROR (Status)) {
    return;
  }

  // Print Firmware Version
  PrintFirmwareVersion ();

  // Initialize Debug Agent
  InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  SaveAndSetDebugTimerInterrupt (TRUE);
#endif

  // Initialize Memory
  Status = InitializeMemory ();
  if (EFI_ERROR (Status)) {
    return;
  }

  DEBUG ((EFI_D_WARN, "SEC: memory HOBs done\n"));

  // Build CPU HOB
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Set the Boot Mode
  SetBootMode (BOOT_WITH_DEFAULT_SETTINGS);

  // Initialize Platform HOBs
  Status = PlatformPeim ();
  DEBUG ((EFI_D_WARN, "SEC: platform HOBs done (%r)\n", Status));
  if (EFI_ERROR (Status)) {
    return;
  }

  // Pass on what the previous stage handed over in x0 (a DT address for a
  // Linux-style boot loader), so a DXE driver can use it.
  {
    UINT64  EntryArg = gSecEntryArg0;

    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));
  }

  CheckHobList ();

  // Run Library Cconstructors
  DEBUG ((EFI_D_WARN, "SEC: constructor debug\n"));
  BaseDebugLibSerialPortConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructor guided-section\n"));
  ExtractGuidedSectionLibConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructor mmu\n"));
  ArmMmuBaseLibConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructor lzma\n"));
  LzmaDecompressLibConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructors done\n"));
  CheckHobList ();

  // Decompress all FVs
  Status = DecompressFirstFv ();
  DEBUG ((EFI_D_WARN, "SEC: FV decompressed (%r)\n", Status));
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Decompress FV! Status = %r\n", Status));
    return;
  }

  // Load DXE Core
  DEBUG ((EFI_D_WARN, "SEC: loading DXE core\n"));
  Status = LoadDxeCoreFromFv (NULL, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Load DXE Core! Status = %r\n", Status));
  }
}

VOID
SecEntry (VOID)
{
  // The previous stage ran with its caches on. Clean and invalidate them before anything else: the UEFI writes its HOB tables and heap with
  // the MMU and caches off, and lines the previous stage left would hide those writes (or be written back over them) once the caches are on.
  SecCleanInvalidateDataCacheAll ();

  // Disable Data Cache
  ArmDisableDataCache ();

  // Invalidate Instruction Cache
  ArmInvalidateInstructionCache ();

  // Enable Instruction Cache
  ArmEnableInstructionCache ();

  // Write back and invalidate Stack & UEFI FD D-Cache. A plain invalidate would discard what the entry code just stored there
  // (live stack frames, gSecEntryArg0) if the previous stage left the data cache on.
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdFdBaseAddress),     FixedPcdGet32 (PcdFdSize));

  // Verify Exception Vector Table
  ASSERT (((UINTN)SecVectorTable & ARM_VECTOR_TABLE_ALIGNMENT) == 0);

  // Enable new Exception Vector Table
  ArmWriteVBar ((UINTN)SecVectorTable);

  // Enter Main SEC Function
  SecMain ();
}
