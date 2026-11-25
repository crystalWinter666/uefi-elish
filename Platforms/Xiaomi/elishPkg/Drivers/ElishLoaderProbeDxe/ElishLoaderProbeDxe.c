/** @file
  Diagnostic-only DarwinBoot request and ExitBootServices capture for elish.

  By default this image stops after a successful ExitBootServices and restarts
  into fastboot. An explicit diagnostic build may continue to a terminal XNU
  checkpoint payload. The normal elish firmware excludes this driver.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PrintLib.h>
#include <Library/RamLogLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <IndustryStandard/IoRemappingTable.h>
#include <IndustryStandard/Scsi.h>
#include <IndustryStandard/UfsHci.h>
#include <Protocol/UniXNULoader.h>
#include <Uefi/UefiGpt.h>

#define PROBE_TIMEOUT_SECONDS  20
#define PROBE_MAP_BYTES        SIZE_64KB
#define ELISH_SMMU_GR0_ID0     0x20
#define ELISH_SMMU_GR0_SCR0     0x0
#define ELISH_SMMU_SCR0_CLIENTPD BIT0
#define ELISH_SMMU_SCR0_USFCFG  BIT10
#define ELISH_SMMU_GR0_ID1     0x24
#define ELISH_SMMU_GR0_ID2     0x28
#define ELISH_SMMU_GR0_SMR     0x800
#define ELISH_SMMU_GR0_S2CR    0xC00
#define ELISH_SMMU_GR1_PAGE    1
#define ELISH_SMMU_GR1_CBAR    0x0
#define ELISH_SMMU_CB_SCTLR    0x0
#define ELISH_SMMU_CB_TTBR0    0x20
#define ELISH_SMMU_CB_TCR      0x30
#define ELISH_SMMU_CB_FSR      0x58
#define ELISH_SMMU_NUMSMRG_MASK 0xFF
#define ELISH_SMMU_NUMCB_MASK  0xFF
#define ELISH_SMMU_NUMPAGENDXB_SHIFT 28
#define ELISH_SMMU_NUMPAGENDXB_MASK  0x7
#define ELISH_SMMU_ID1_PAGESIZE_64K BIT31
#define ELISH_SMMU_SMR_VALID   BIT31
#define ELISH_SMMU_SMR_ID_MASK 0xFFFF
#define ELISH_SMMU_SMR_MASK_MASK 0x7FFF
#define ELISH_SMMU_SMR_MASK_SHIFT 16
#define ELISH_SMMU_S2CR_TYPE_SHIFT 16
#define ELISH_SMMU_S2CR_TYPE_MASK 0x3
#define ELISH_SMMU_S2CR_CBNDX_MASK 0xFF
#define ELISH_SMMU_MAX_GROUPS  256

#ifndef ELISH_LOADER_PROBE_RUN_KERNEL
#define ELISH_LOADER_PROBE_RUN_KERNEL  0
#endif

#ifndef ELISH_LOADER_PROBE_SMMU
#define ELISH_LOADER_PROBE_SMMU  0
#endif

#ifndef ELISH_LOADER_PROBE_PMSG
#define ELISH_LOADER_PROBE_PMSG  0
#endif

#ifndef ELISH_LOADER_PROBE_UFS_CLOCKS
#define ELISH_LOADER_PROBE_UFS_CLOCKS 0
#endif

#ifndef ELISH_UFS_HANDOFF_PROBE
#define ELISH_UFS_HANDOFF_PROBE 0
#endif

#ifndef ELISH_UFS_NOP_PROBE
#define ELISH_UFS_NOP_PROBE 0
#endif

#ifndef ELISH_UFS_GPT_PROBE
#define ELISH_UFS_GPT_PROBE 0
#endif

#define ELISH_PMSG_BASE                    0xB0200000
#define ELISH_PMSG_BYTES                   0x00200000
#define ELISH_PMSG_RETURN_MARKER_ADDRESS   0xB03FF000
#define ELISH_PMSG_RETURN_MARKER           0x4D564C45

STATIC UNIXNU_LOADER_BOOT       mBoot;
STATIC EFI_EXIT_BOOT_SERVICES   mExitBootServices;
STATIC BOOLEAN                 mInsideLoader;
STATIC UINT64                  mMapStorage[PROBE_MAP_BYTES / sizeof (UINT64)];
STATIC CONST EFI_GUID           mEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

STATIC
VOID
Trace (
  IN CONST CHAR8  *Format,
  ...
  );

#if ELISH_LOADER_PROBE_SMMU
STATIC
VOID
TraceIortState (
  VOID
  )
{
  EFI_ACPI_COMMON_HEADER                      *CommonTable;
  EFI_ACPI_6_0_IO_REMAPPING_TABLE             *Iort;
  EFI_ACPI_6_0_IO_REMAPPING_NODE              *Node;
  EFI_ACPI_6_0_IO_REMAPPING_NAMED_COMP_NODE   *NamedNode;
  EFI_ACPI_6_0_IO_REMAPPING_ID_TABLE          *Mapping;
  CONST UINT8                                  *TableBytes;
  CONST UINT8                                  *NodeBytes;
  CONST UINT8                                  *NameBytes;
  UINTN                                        TableLength;
  UINTN                                        Offset;
  UINTN                                        NameLimit;
  UINTN                                        NameLength;
  UINTN                                        Character;
  UINTN                                        MapOffset;
  UINT32                                       Index;
  UINT32                                       MapIndex;
  UINT8                                        Checksum;
  CHAR8                                        DeviceName[64];
  BOOLEAN                                      NameTerminated;
  BOOLEAN                                      IsUfs;
  BOOLEAN                                      FoundUfs;

  CommonTable = EfiLocateFirstAcpiTable (EFI_ACPI_6_0_IO_REMAPPING_TABLE_SIGNATURE);
  if (CommonTable == NULL) {
    Trace ("ELISH_IORT_PROBE: IORT table not present in ACPI configuration\n");
    return;
  }

  Iort = (EFI_ACPI_6_0_IO_REMAPPING_TABLE *)CommonTable;
  TableLength = Iort->Header.Length;
  Trace ("ELISH_IORT_PROBE: table=%p revision=%u length=0x%lx nodes=%u node-offset=0x%x\n",
         Iort, Iort->Header.Revision, (UINT64)TableLength, Iort->NumNodes, Iort->NodeOffset);
  if ((TableLength < sizeof (*Iort)) || (TableLength > 0x00100000) ||
      (Iort->NodeOffset < sizeof (*Iort)) || (Iort->NodeOffset > TableLength) ||
      (Iort->NumNodes > 256)) {
    Trace ("ELISH_IORT_PROBE: invalid table geometry\n");
    return;
  }

  Checksum = 0;
  TableBytes = (CONST UINT8 *)Iort;
  for (Offset = 0; Offset < TableLength; Offset++) {
    Checksum = (UINT8)(Checksum + TableBytes[Offset]);
  }
  Trace ("ELISH_IORT_PROBE: checksum-valid=%u\n", Checksum == 0);
  if (Checksum != 0) {
    return;
  }

  FoundUfs = FALSE;
  Offset = Iort->NodeOffset;
  for (Index = 0; Index < Iort->NumNodes; Index++) {
    if ((Offset > TableLength) ||
        (sizeof (*Node) > TableLength - Offset)) {
      Trace ("ELISH_IORT_PROBE: node %u header outside table\n", Index);
      return;
    }

    NodeBytes = TableBytes + Offset;
    Node = (EFI_ACPI_6_0_IO_REMAPPING_NODE *)NodeBytes;
    if ((Node->Length < sizeof (*Node)) || (Node->Length > TableLength - Offset)) {
      Trace ("ELISH_IORT_PROBE: node %u invalid length=0x%x\n", Index, Node->Length);
      return;
    }

    Trace ("ELISH_IORT_PROBE: node=%u offset=0x%lx type=%u identifier=0x%x length=0x%x mappings=%u\n",
           Index, (UINT64)Offset, Node->Type, ReadUnaligned32 (&Node->Identifier),
           Node->Length, Node->NumIdMappings);

    if ((Node->NumIdMappings != 0) &&
        ((Node->IdReference < sizeof (*Node)) || (Node->IdReference > Node->Length) ||
         (Node->NumIdMappings >
          (Node->Length - Node->IdReference) / sizeof (*Mapping)))) {
      Trace ("ELISH_IORT_PROBE: node %u invalid mapping array offset=0x%x count=%u\n",
             Index, Node->IdReference, Node->NumIdMappings);
      return;
    }

    if (Node->Type == EFI_ACPI_IORT_TYPE_NAMED_COMP) {
      NamedNode = (EFI_ACPI_6_0_IO_REMAPPING_NAMED_COMP_NODE *)Node;
      if (Node->Length < sizeof (*NamedNode)) {
        Trace ("ELISH_IORT_PROBE: named component %u is truncated\n", Index);
        return;
      }

      NameBytes = NodeBytes + sizeof (*NamedNode);
      NameLimit = (Node->NumIdMappings == 0) ? Node->Length : Node->IdReference;
      if (NameLimit < sizeof (*NamedNode)) {
        Trace ("ELISH_IORT_PROBE: named component %u has invalid name range\n", Index);
        return;
      }

      NameLimit -= sizeof (*NamedNode);
      NameLength = 0;
      NameTerminated = FALSE;
      while ((NameLength < NameLimit) && (NameLength < sizeof (DeviceName) - 1)) {
        if (NameBytes[NameLength] == '\0') {
          NameTerminated = TRUE;
          break;
        }
        NameLength++;
      }

      if (!NameTerminated) {
        Trace ("ELISH_IORT_PROBE: named component %u has unterminated/long name\n", Index);
        return;
      }

      for (Character = 0; Character < NameLength; Character++) {
        DeviceName[Character] = (CHAR8)NameBytes[Character];
      }
      DeviceName[NameLength] = '\0';
      IsUfs = FALSE;
      for (Character = 0; Character + 4 <= NameLength; Character++) {
        if (CompareMem (DeviceName + Character, "UFS0", 4) == 0) {
          IsUfs = TRUE;
          break;
        }
      }

      if (IsUfs) {
        FoundUfs = TRUE;
        Trace ("ELISH_IORT_PROBE: UFS0 node=%u name=%a id-count=%u id-offset=0x%x\n",
               Index, DeviceName, Node->NumIdMappings, Node->IdReference);
        for (MapIndex = 0; MapIndex < Node->NumIdMappings; MapIndex++) {
          MapOffset = Offset + Node->IdReference + (MapIndex * sizeof (*Mapping));
          Mapping = (EFI_ACPI_6_0_IO_REMAPPING_ID_TABLE *)(TableBytes + MapOffset);
          Trace ("ELISH_IORT_PROBE: UFS0 map=%u input=0x%x count=0x%x output=0x%x reference=0x%x flags=0x%x\n",
                 MapIndex, ReadUnaligned32 (&Mapping->InputBase),
                 ReadUnaligned32 (&Mapping->NumIds), ReadUnaligned32 (&Mapping->OutputBase),
                 ReadUnaligned32 (&Mapping->OutputReference), ReadUnaligned32 (&Mapping->Flags));
        }
      }
    }

    Offset += Node->Length;
  }

  Trace ("ELISH_IORT_PROBE: UFS0 %a\n", FoundUfs ? "found" : "not found");
}

STATIC
VOID
TraceSmmuState (
  VOID
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;
  EFI_STATUS                    Status;
  UINT32                        Id0;
  UINT32                        Id1;
  UINT32                        SmmuCr0;
  UINT32                        Id2;
  UINT32                        GroupCount;
  UINT32                        Smr;
  UINT32                        S2cr;
  UINT32                        StreamId;
  UINT32                        StreamMask;
  UINT32                        ContextBank;
  UINT32                        ContextCount;
  UINT32                        PageShift;
  UINT32                        NumPage;
  UINT32                        Cbar;
  UINT32                        Sctlr;
  UINT32                        Tcr;
  UINT32                        Fsr;
  UINT64                        Ttbr0;
  UINTN                         Gr1BaseOffset;
  UINTN                         ContextBaseOffset;
  UINTN                         Index;
  UINTN                         SmrOffset;
  UINTN                         S2crOffset;

  Status = LocateMemoryRegionByName ("SMMU", &Region);
  if (EFI_ERROR (Status)) {
    Trace ("ELISH_SMMU_PROBE: region lookup failed %r\n", Status);
    return;
  }

  Trace ("ELISH_SMMU_PROBE: region base=0x%lx bytes=0x%lx type=%u\n",
         Region.Address, Region.Length, Region.ResourceType);
  if ((Region.ResourceType != MMAP_IO) || (Region.Length < 0x1000) ||
      ((Region.Address & 0xFFF) != 0)) {
    Trace ("ELISH_SMMU_PROBE: refusing unexpected MMIO region\n");
    return;
  }

  SmmuCr0 = MmioRead32 ((UINTN)(Region.Address + ELISH_SMMU_GR0_SCR0));
  if (SmmuCr0 == MAX_UINT32) {
    Trace ("ELISH_SMMU_PROBE: refusing all-ones control register\n");
    return;
  }

  Trace ("ELISH_SMMU_PROBE: sCR0=0x%08x clientpd=%u unmatched-fault-config=%u\n",
         SmmuCr0, (SmmuCr0 & ELISH_SMMU_SCR0_CLIENTPD) != 0,
         (SmmuCr0 & ELISH_SMMU_SCR0_USFCFG) != 0);

  Id0 = MmioRead32 ((UINTN)(Region.Address + ELISH_SMMU_GR0_ID0));
  Id1 = MmioRead32 ((UINTN)(Region.Address + ELISH_SMMU_GR0_ID1));
  Trace ("ELISH_SMMU_PROBE: ID0=0x%08x ID1=0x%08x\n", Id0, Id1);
  Id2 = MmioRead32 ((UINTN)(Region.Address + ELISH_SMMU_GR0_ID2));
  Trace ("ELISH_SMMU_PROBE: ID2=0x%08x\n", Id2);
  if ((Id0 == MAX_UINT32) || (Id1 == MAX_UINT32)) {
    Trace ("ELISH_SMMU_PROBE: refusing all-ones identification registers\n");
    return;
  }

  GroupCount = Id0 & ELISH_SMMU_NUMSMRG_MASK;
  if (GroupCount > ELISH_SMMU_MAX_GROUPS) {
    Trace ("ELISH_SMMU_PROBE: invalid stream group count=%u\n", GroupCount);
    return;
  }

  Trace ("ELISH_SMMU_PROBE: stream groups=%u; read-only scan begins\n", GroupCount);
  for (Index = 0; Index < GroupCount; Index++) {
    SmrOffset  = ELISH_SMMU_GR0_SMR + (Index * sizeof (UINT32));
    S2crOffset = ELISH_SMMU_GR0_S2CR + (Index * sizeof (UINT32));
    if ((SmrOffset + sizeof (UINT32) > Region.Length) ||
        (S2crOffset + sizeof (UINT32) > Region.Length)) {
      Trace ("ELISH_SMMU_PROBE: group %u outside mapped region; stop\n", (UINT32)Index);
      break;
    }

    Trace ("ELISH_SMMU_PROBE: reading group %u\n", (UINT32)Index);
    Smr = MmioRead32 ((UINTN)(Region.Address + SmrOffset));
    if ((Smr & ELISH_SMMU_SMR_VALID) == 0) {
      continue;
    }

    S2cr      = MmioRead32 ((UINTN)(Region.Address + S2crOffset));
    StreamId  = Smr & ELISH_SMMU_SMR_ID_MASK;
    StreamMask = (Smr >> ELISH_SMMU_SMR_MASK_SHIFT) & ELISH_SMMU_SMR_MASK_MASK;
    Trace ("ELISH_SMMU_PROBE: group=%u SMR=0x%08x SID=0x%x mask=0x%x S2CR=0x%08x type=%u cb=%u\n",
           (UINT32)Index, Smr, StreamId, StreamMask, S2cr,
           (S2cr >> ELISH_SMMU_S2CR_TYPE_SHIFT) & ELISH_SMMU_S2CR_TYPE_MASK,
           S2cr & ELISH_SMMU_S2CR_CBNDX_MASK);

    if (((S2cr >> ELISH_SMMU_S2CR_TYPE_SHIFT) & ELISH_SMMU_S2CR_TYPE_MASK) != 0) {
      continue;
    }

    ContextBank = S2cr & ELISH_SMMU_S2CR_CBNDX_MASK;
    ContextCount = Id1 & ELISH_SMMU_NUMCB_MASK;
    PageShift = ((Id1 & ELISH_SMMU_ID1_PAGESIZE_64K) != 0) ? 16 : 12;
    NumPage = 1U << ((((Id1 >> ELISH_SMMU_NUMPAGENDXB_SHIFT) &
                       ELISH_SMMU_NUMPAGENDXB_MASK) + 1));
    Gr1BaseOffset = (UINTN)ELISH_SMMU_GR1_PAGE << PageShift;
    ContextBaseOffset = (UINTN)(NumPage + ContextBank) << PageShift;
    if ((ContextBank >= ContextCount) ||
        (Gr1BaseOffset > Region.Length) ||
        ((ContextBank * sizeof (UINT32) + sizeof (UINT32)) > Region.Length - Gr1BaseOffset) ||
        (ContextBaseOffset > Region.Length) ||
        (ELISH_SMMU_CB_FSR + sizeof (UINT32) > Region.Length - ContextBaseOffset)) {
      Trace ("ELISH_SMMU_PROBE: context bank %u outside advertised MMIO range; skip\n",
             ContextBank);
      continue;
    }

    Cbar = MmioRead32 ((UINTN)(Region.Address + Gr1BaseOffset +
                               (ContextBank * sizeof (UINT32)) + ELISH_SMMU_GR1_CBAR));
    Sctlr = MmioRead32 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_SCTLR));
    Ttbr0 = MmioRead64 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_TTBR0));
    Tcr = MmioRead32 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_TCR));
    Fsr = MmioRead32 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_FSR));
    Trace ("ELISH_SMMU_PROBE: cb=%u cbar=0x%08x sctlr=0x%08x ttbr0=0x%016lx tcr=0x%08x fsr=0x%08x\n",
           ContextBank, Cbar, Sctlr, Ttbr0, Tcr, Fsr);
  }

  ContextCount = Id1 & ELISH_SMMU_NUMCB_MASK;
  PageShift = ((Id1 & ELISH_SMMU_ID1_PAGESIZE_64K) != 0) ? 16 : 12;
  NumPage = 1U << ((((Id1 >> ELISH_SMMU_NUMPAGENDXB_SHIFT) &
                     ELISH_SMMU_NUMPAGENDXB_MASK) + 1));
  Gr1BaseOffset = (UINTN)ELISH_SMMU_GR1_PAGE << PageShift;
  Trace ("ELISH_SMMU_PROBE: context banks=%u; read-only state scan begins\n", ContextCount);
  for (Index = 0; Index < ContextCount; Index++) {
    ContextBaseOffset = (UINTN)(NumPage + Index) << PageShift;
    if ((Gr1BaseOffset > Region.Length) ||
        ((Index * sizeof (UINT32) + sizeof (UINT32)) > Region.Length - Gr1BaseOffset) ||
        (ContextBaseOffset > Region.Length) ||
        (ELISH_SMMU_CB_TTBR0 + sizeof (UINT64) > Region.Length - ContextBaseOffset) ||
        (ELISH_SMMU_CB_TCR + sizeof (UINT32) > Region.Length - ContextBaseOffset)) {
      Trace ("ELISH_SMMU_PROBE: context bank %u outside advertised MMIO range; stop\n",
             (UINT32)Index);
      break;
    }

    Cbar = MmioRead32 ((UINTN)(Region.Address + Gr1BaseOffset +
                               (Index * sizeof (UINT32)) + ELISH_SMMU_GR1_CBAR));
    Sctlr = MmioRead32 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_SCTLR));
    Ttbr0 = MmioRead64 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_TTBR0));
    Tcr = MmioRead32 ((UINTN)(Region.Address + ContextBaseOffset + ELISH_SMMU_CB_TCR));
    if ((Cbar != 0) || (Sctlr != 0) || (Ttbr0 != 0) || (Tcr != 0)) {
      Trace ("ELISH_SMMU_PROBE: context-state cb=%u cbar=0x%08x sctlr=0x%08x ttbr0=0x%016lx tcr=0x%08x\n",
             (UINT32)Index, Cbar, Sctlr, Ttbr0, Tcr);
    }
  }

  Trace ("ELISH_SMMU_PROBE: read-only scan complete\n");
}
#endif

#if ELISH_LOADER_PROBE_UFS_CLOCKS
#define ELISH_GCC_UFS_BASE       0x00100000
#define ELISH_GCC_UFS_BYTES      0x00078000
#define ELISH_UFS_HOST_BASE      0x01D84000
#define ELISH_UFS_HOST_BYTES     0x00003000
#define ELISH_UFS_PHY_BASE       0x01D87000
#define ELISH_UFS_PHY_BYTES      0x00000E00
#define ELISH_UFS_ICE_BASE       0x01D90000
#define ELISH_UFS_ICE_BYTES      0x00008000
#define ELISH_GCC_UFS_CORE_BCR   0x77000
#define ELISH_GCC_UFS_CORE_RESET BIT0

STATIC
BOOLEAN
ValidateUfsMmioWindow (
  IN CHAR8                         *Name,
  IN EFI_PHYSICAL_ADDRESS          ExpectedAddress,
  IN UINT64                        MinimumLength,
  OUT EFI_MEMORY_REGION_DESCRIPTOR *Region
  )
{
  EFI_STATUS Status;

  Status = LocateMemoryRegionByName (Name, Region);
  if (EFI_ERROR (Status)) {
    Trace ("ELISH_UFS_CLOCK_PROBE: %a map lookup failed %r\n", Name, Status);
    return FALSE;
  }

  if ((Region->Address != ExpectedAddress) || (Region->Length < MinimumLength) ||
      (Region->ResourceType != MMAP_IO) || (Region->MemoryType != MmIO) ||
      (Region->ArmAttributes != NS_DEVICE)) {
    Trace ("ELISH_UFS_CLOCK_PROBE: %a map mismatch base=0x%lx bytes=0x%lx type=%u mem=%u attr=0x%lx\n",
           Name, Region->Address, Region->Length, Region->ResourceType,
           Region->MemoryType, (UINT64)Region->ArmAttributes);
    return FALSE;
  }

  Trace ("ELISH_UFS_CLOCK_PROBE: %a mapped base=0x%lx bytes=0x%lx type=%u\n",
         Name, Region->Address, Region->Length, Region->ResourceType);
  return TRUE;
}

STATIC
BOOLEAN
TraceUfsClockState (
  VOID
  )
{
  typedef struct {
    CHAR8   *Name;
    UINT32  Offset;
    UINT32  EnableMask;
    UINT32  HwCgMask;
  } UFS_CLOCK_BRANCH;
  typedef struct {
    CHAR8   *Name;
    UINT32  Offset;
  } UFS_CLOCK_RCG;
  STATIC CONST UFS_CLOCK_BRANCH Branches[] = {
    {"core_clk",            0x77010, BIT0, BIT1},
    {"iface_clk",           0x77018, BIT0, BIT1},
    {"tx_lane0_sync_clk",   0x7701C, BIT0, 0},
    {"rx_lane0_sync_clk",   0x77020, BIT0, 0},
    {"core_clk_unipro",     0x7705C, BIT0, BIT1},
    {"core_clk_ice",        0x77064, BIT0, BIT1},
    {"ref_aux_clk",         0x7709C, BIT0, BIT1},
    {"rx_lane1_sync_clk",   0x770B8, BIT0, 0},
    {"bus_aggr_clk",        0x770CC, BIT0, BIT1}
  };
  STATIC CONST UFS_CLOCK_RCG Rcgs[] = {
    {"core_clk",        0x77024},
    {"core_clk_ice",    0x7706C},
    {"core_clk_unipro", 0x77084},
    {"ref_aux_clk",     0x770A0}
  };
  EFI_MEMORY_REGION_DESCRIPTOR GccRegion;
  EFI_MEMORY_REGION_DESCRIPTOR HostRegion;
  EFI_MEMORY_REGION_DESCRIPTOR PhyRegion;
  EFI_MEMORY_REGION_DESCRIPTOR IceRegion;
  UINTN Index;
  UINT32 Value;
  BOOLEAN CoreClockEnabled;
  BOOLEAN InterfaceClockEnabled;
  CoreClockEnabled = FALSE;
  InterfaceClockEnabled = FALSE;

  if (!ValidateUfsMmioWindow ("GCC UFS clocks", ELISH_GCC_UFS_BASE,
                              ELISH_GCC_UFS_BYTES, &GccRegion) ||
      !ValidateUfsMmioWindow ("UFS Host", ELISH_UFS_HOST_BASE,
                              ELISH_UFS_HOST_BYTES, &HostRegion) ||
      !ValidateUfsMmioWindow ("UFS PHY", ELISH_UFS_PHY_BASE,
                              ELISH_UFS_PHY_BYTES, &PhyRegion) ||
      !ValidateUfsMmioWindow ("UFS ICE", ELISH_UFS_ICE_BASE,
                              ELISH_UFS_ICE_BYTES, &IceRegion)) {
    Trace ("ELISH_UFS_CLOCK_PROBE: refusing register reads; required windows are not mapped as expected\n");
    return FALSE;
  }

  Trace ("ELISH_UFS_CLOCK_PROBE: GCC snapshot; UFS HCI reads require the UFS GDSC and core/interface clocks\n");
  for (Index = 0; Index < ARRAY_SIZE (Branches); Index++) {
    if (Branches[Index].Offset + sizeof (UINT32) > GccRegion.Length) {
      Trace ("ELISH_UFS_CLOCK_PROBE: branch %a outside GCC window; stop\n", Branches[Index].Name);
      return FALSE;
    }

    Value = MmioRead32 ((UINTN)(GccRegion.Address + Branches[Index].Offset));
    Trace ("ELISH_UFS_CLOCK_PROBE: branch %a offset=0x%x value=0x%08x enable=%u hwcg=%u\n",
           Branches[Index].Name, Branches[Index].Offset, Value,
           (Value & Branches[Index].EnableMask) != 0,
           (Branches[Index].HwCgMask != 0) && ((Value & Branches[Index].HwCgMask) != 0));
    if (Branches[Index].Offset == 0x77010) {
      CoreClockEnabled = (Value & BIT0) != 0;
    }
    if (Branches[Index].Offset == 0x77018) {
      InterfaceClockEnabled = (Value & BIT0) != 0;
    }
    if (Value == MAX_UINT32) {
      Trace ("ELISH_UFS_CLOCK_PROBE: refusing further reads after all-ones GCC value\n");
      return FALSE;
    }
  }

  for (Index = 0; Index < ARRAY_SIZE (Rcgs); Index++) {
    if (Rcgs[Index].Offset + sizeof (UINT32) > GccRegion.Length) {
      Trace ("ELISH_UFS_CLOCK_PROBE: RCG %a outside GCC window; stop\n", Rcgs[Index].Name);
      return FALSE;
    }

    Value = MmioRead32 ((UINTN)(GccRegion.Address + Rcgs[Index].Offset));
    Trace ("ELISH_UFS_CLOCK_PROBE: RCG %a command-register offset=0x%x value=0x%08x\n",
           Rcgs[Index].Name, Rcgs[Index].Offset, Value);
    if (Value == MAX_UINT32) {
      Trace ("ELISH_UFS_CLOCK_PROBE: refusing further reads after all-ones GCC value\n");
      return FALSE;
    }
  }

  if ((ELISH_GCC_UFS_CORE_BCR + sizeof (UINT32)) > GccRegion.Length) {
    Trace ("ELISH_UFS_CLOCK_PROBE: UFS core-reset register outside GCC window; stop\n");
    return FALSE;
  }

  Value = MmioRead32 ((UINTN)(GccRegion.Address + ELISH_GCC_UFS_CORE_BCR));
  Trace ("ELISH_UFS_CLOCK_PROBE: UFS core-reset register offset=0x%x raw=0x%08x\n",
         ELISH_GCC_UFS_CORE_BCR, Value);
  if ((Value & ELISH_GCC_UFS_CORE_RESET) != 0) {
    Trace ("ELISH_UFS_CLOCK_PROBE: core reset asserted or reset read invalid; skip UFS HCI reads\n");
    return FALSE;
  }

  if ((0x77004 + sizeof (UINT32)) > GccRegion.Length) {
    Trace ("ELISH_UFS_CLOCK_PROBE: UFS PHY GDSC status outside GCC window; stop\n");
    return FALSE;
  }

  Value = MmioRead32 ((UINTN)(GccRegion.Address + 0x77004));
  if (Value == MAX_UINT32) {
    Trace ("ELISH_UFS_CLOCK_PROBE: refusing host access after all-ones GDSC value\n");
    return FALSE;
  }
  Trace ("ELISH_UFS_CLOCK_PROBE: UFS PHY GDSC offset=0x77004 raw=0x%08x powered=%u\n",
         Value, (Value & 0x80000000U) != 0);
  if ((Value & 0x80000000U) == 0) {
    Trace ("ELISH_UFS_CLOCK_PROBE: UFS PHY GDSC is off; skip UFS HCI reads\n");
    return FALSE;
  }
  if (!CoreClockEnabled || !InterfaceClockEnabled) {
    Trace ("ELISH_UFS_CLOCK_PROBE: core/interface clock gate missing; skip UFS HCI reads core=%u iface=%u\n",
           CoreClockEnabled, InterfaceClockEnabled);
    return FALSE;
  }

  Trace ("ELISH_UFS_CLOCK_PROBE: core reset deasserted, GDSC and HCI clock gates on\n");
  return TRUE;
}
STATIC
VOID
TraceUfsHostState (
  VOID
  )
{
  typedef struct {
    CHAR8   *Name;
    UINT32  Offset;
  } UFS_HCI_STATUS_REGISTER;
  STATIC CONST UFS_HCI_STATUS_REGISTER Registers[] = {
    {"CAP", 0x00},
    {"VER", 0x08},
    {"IS",  0x20},
    {"HCS", 0x30},
    {"HCE",     0x34},
    {"UTRLBA",  0x50},
    {"UTRLBAU", 0x54},
    {"UTRLDBR", 0x58},
    {"UTRLRSR", 0x60},
    {"UTMRLBA",  0x70},
    {"UTMRLBAU", 0x74},
    {"UTMRLDBR", 0x78},
    {"UTMRLRSR", 0x80}
  };
  EFI_MEMORY_REGION_DESCRIPTOR HostRegion;
  UINTN Index;
  UINT32 Value;

  if (!ValidateUfsMmioWindow ("UFS Host", ELISH_UFS_HOST_BASE,
                              ELISH_UFS_HOST_BYTES, &HostRegion)) {
    Trace ("ELISH_UFS_HOST_PROBE: refusing status reads; host window is not mapped as expected\n");
    return;
  }

  Trace ("ELISH_UFS_HOST_PROBE: read-only UFSHCI status and queue registers; no list memory access or command submission\n");
  for (Index = 0; Index < ARRAY_SIZE (Registers); Index++) {
    if ((Registers[Index].Offset > HostRegion.Length) ||
        (sizeof (UINT32) > HostRegion.Length - Registers[Index].Offset)) {
      Trace ("ELISH_UFS_HOST_PROBE: %a outside host window; stop\n", Registers[Index].Name);
      return;
    }

    Trace ("ELISH_UFS_HOST_PROBE: reading %a offset=0x%x\n",
           Registers[Index].Name, Registers[Index].Offset);
    Value = MmioRead32 ((UINTN)(HostRegion.Address + Registers[Index].Offset));
    Trace ("ELISH_UFS_HOST_PROBE: %a value=0x%08x\n",
           Registers[Index].Name, Value);
    if (Value == MAX_UINT32) {
      Trace ("ELISH_UFS_HOST_PROBE: stop after all-ones register value\n");
      return;
    }
  }

  Trace ("ELISH_UFS_HOST_PROBE: status read complete; no UFS command, DMA or write issued\n");
}

#if ELISH_UFS_NOP_PROBE
typedef struct {
  UINTN Base;
  UINTN Context[2];
} UFS_DMA_ROUTES;

STATIC
BOOLEAN
QualifyUfsDmaRoutes (OUT UFS_DMA_ROUTES *Routes)
{
  EFI_MEMORY_REGION_DESCRIPTOR Region;
  UINT32 Seen = 0;

  if (!ValidateUfsMmioWindow ("SMMU", 0x15000000, 0xD0000, &Region)) {
    return FALSE;
  }

  UINTN Base = (UINTN)Region.Address;
  // Require the run38 stream-match format, 4 KiB pages and 36-bit address geometry.
  if (MmioRead32 (Base + ELISH_SMMU_GR0_SCR0) != 0x00270406 ||
      MmioRead32 (Base + ELISH_SMMU_GR0_ID0) != 0x4C017E57 ||
      MmioRead32 (Base + ELISH_SMMU_GR0_ID1) != 0x6000004A ||
      MmioRead32 (Base + ELISH_SMMU_GR0_ID2) != 0x00005111) {
    return FALSE;
  }

  for (UINTN Group = 0; Group < 87; Group++) {
    UINT32 Smr = MmioRead32 (Base + ELISH_SMMU_GR0_SMR + Group * sizeof (UINT32));
    if ((Smr & BIT31) == 0) {
      continue;
    }

    UINT32 S2cr = MmioRead32 (Base + ELISH_SMMU_GR0_S2CR + Group * sizeof (UINT32));
    UINT32 Sid = Smr & 0x7FFF;
    UINT32 Mask = (Smr >> 16) & 0x7FFF;
    UINT32 Bank = S2cr & 0xFF;
    if (Smr == MAX_UINT32 || S2cr == MAX_UINT32) {
      return FALSE;
    }
    if (Mask == 0 && ((Sid == 0x4E0 && S2cr == 2) || (Sid == 0xE0 && S2cr == 3))) {
      if ((Seen & (1U << Bank)) != 0) {
        return FALSE;
      }
      Seen |= 1U << Bank;
    } else if (((Sid ^ 0x4E0) & ~Mask) == 0 || ((Sid ^ 0xE0) & ~Mask) == 0 ||
               (((S2cr >> 16) & 3) == 0 && (Bank == 2 || Bank == 3))) {
      return FALSE;
    }
  }

  if (Seen != (BIT2 | BIT3)) {
    return FALSE;
  }

  Routes->Base = Base;
  for (UINTN Bank = 2; Bank <= 3; Bank++) {
    UINTN Context = Base + (128 + Bank) * SIZE_4KB;
    UINTN Gr1 = Base + SIZE_4KB + Bank * sizeof (UINT32);
    if (MmioRead32 (Gr1) != 0x0001F000 || MmioRead32 (Gr1 + 0x800) != 1 ||
        MmioRead32 (Context + ELISH_SMMU_CB_SCTLR) != 0xE0 ||
        MmioRead64 (Context + ELISH_SMMU_CB_TTBR0) != 0 ||
        MmioRead32 (Context + ELISH_SMMU_CB_TCR) != 0) {
      return FALSE;
    }
    Routes->Context[Bank - 2] = Context;
  }
  return TRUE;
}

STATIC
BOOLEAN
WaitUfsRegister (IN UINTN Address, IN UINT32 Expected)
{
  for (UINTN Attempt = 0; Attempt < 1000; Attempt++) {
    if (MmioRead32 (Address) == Expected) {
      return TRUE;
    }
    MicroSecondDelay (1000);
  }
  return MmioRead32 (Address) == Expected;
}

#define ELISH_GPT_BLOCKS  5U

#if ELISH_UFS_GPT_PROBE
#define ELISH_LUN0_LAST   61880319U

STATIC
BOOLEAN
ReadUfsLun0 (IN UINTN Host, IN UINTN Base, IN UINT32 Lba, IN UINT16 Blocks)
{
  UINTN Ucd = Base + EFI_PAGE_SIZE;
  UINT8 *Data = (UINT8 *)(Ucd + EFI_PAGE_SIZE);
  UTP_TRD *Trd = (UTP_TRD *)Base;
  UTP_COMMAND_UPIU *Command = (UTP_COMMAND_UPIU *)Ucd;
  UTP_RESPONSE_UPIU *Response = (UTP_RESPONSE_UPIU *)(Ucd + 512);
  UTP_TR_PRD *Prd = (UTP_TR_PRD *)(Ucd + 1024);
  UINT32 Bytes = Blocks == 0 ? 8 : EFI_PAGES_TO_SIZE (Blocks);
  UINT32 Is;
  CONST UINT8 Tag = 0x38;

  if (Blocks > ELISH_GPT_BLOCKS || Lba > ELISH_LUN0_LAST ||
      (Blocks && Blocks - 1U > ELISH_LUN0_LAST - Lba) ||
      MmioRead32 (Host + UFS_HC_UTRLDBR_OFFSET) != 0) {
    return FALSE;
  }

  ZeroMem (Trd, sizeof (*Trd));
  ZeroMem (Command, sizeof (*Command));
  ZeroMem (Response, sizeof (*Response));
  ZeroMem (Prd, sizeof (*Prd));
  Command->TransCode = 1;
  Command->Flags = 0x40;
  Command->TaskTag = Tag;
  Command->ExpDataTranLen = SwapBytes32 (Bytes);
  if (Blocks == 0) {
    Command->Cdb[0] = EFI_SCSI_OP_READ_CAPACITY;
  } else {
    Command->Cdb[0] = EFI_SCSI_OP_READ10;
    WriteUnaligned32 ((UINT32 *)(Command->Cdb + 2), SwapBytes32 (Lba));
    WriteUnaligned16 ((UINT16 *)(Command->Cdb + 7), SwapBytes16 (Blocks));
  }
  Trd->Int = 1;
  Trd->Dd = UfsDataIn;
  Trd->Ct = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa = (UINT32)(Ucd >> 7);
  Trd->RuO = 512 / sizeof (UINT32);
  Trd->RuL = 512 / sizeof (UINT32);
  Trd->PrdtO = 1024 / sizeof (UINT32);
  Trd->PrdtL = 1;
  Prd->DbAddr = (UINT32)((UINTN)Data >> 2);
  Prd->DbCount = Bytes - 1;

  WriteBackInvalidateDataCacheRange (Trd, sizeof (*Trd));
  WriteBackInvalidateDataCacheRange (Command, 1024 + sizeof (*Prd));
  // Reserve the entire first cache line even when READ CAPACITY transfers eight bytes.
  WriteBackInvalidateDataCacheRange (Data, Blocks == 0 ? EFI_PAGE_SIZE : Bytes);
  MmioWrite32 (Host + UFS_HC_IS_OFFSET, BIT0);
  MmioWrite32 (Host + UFS_HC_UTRLDBR_OFFSET, BIT0);
  if (!WaitUfsRegister (Host + UFS_HC_UTRLDBR_OFFSET, 0)) {
    Trace ("ELISH_UFS_GPT: opcode=%x lba=%u blocks=%u timed out, IS=%x HCS=%x\n",
           Command->Cdb[0], Lba, Blocks, MmioRead32 (Host + UFS_HC_IS_OFFSET),
           MmioRead32 (Host + UFS_HC_STATUS_OFFSET));
    return FALSE;
  }

  InvalidateDataCacheRange (Trd, sizeof (*Trd));
  InvalidateDataCacheRange (Response, sizeof (*Response));
  InvalidateDataCacheRange (Data, Blocks == 0 ? EFI_PAGE_SIZE : Bytes);
  Is = MmioRead32 (Host + UFS_HC_IS_OFFSET);
  if (Trd->Ocs != 0 || Response->TransCode != 0x21 || Response->TaskTag != Tag ||
      Response->Lun != 0 || Response->Response != 0 || Response->Status != 0 ||
      (Response->Flags & 0x60) != 0 || Response->ResTranCount != 0 || (Is & BIT0) == 0) {
    Trace ("ELISH_UFS_GPT: opcode=%x lba=%u blocks=%u OCS=%x type=%x tag=%x response=%x status=%x residual=%x IS=%x sense=%02x/%02x/%02x\n",
           Command->Cdb[0], Lba, Blocks, Trd->Ocs, Response->TransCode, Response->TaskTag,
           Response->Response, Response->Status, SwapBytes32 (Response->ResTranCount), Is,
           Response->SenseData[2], Response->SenseData[12], Response->SenseData[13]);
    return FALSE;
  }
  return TRUE;
}

STATIC
BOOLEAN
CheckElishGpt (IN OUT UINT8 *Data, IN BOOLEAN Backup)
{
  UINT32 CaptureCrc = CalculateCrc32 (Data, EFI_PAGES_TO_SIZE (ELISH_GPT_BLOCKS));
  EFI_PARTITION_TABLE_HEADER *Header = (EFI_PARTITION_TABLE_HEADER *)(Data + (Backup ? 4 * EFI_PAGE_SIZE : 0));
  EFI_PARTITION_ENTRY *Entries = (EFI_PARTITION_ENTRY *)(Data + (Backup ? 0 : EFI_PAGE_SIZE));
  EFI_PARTITION_ENTRY *Userdata = &Entries[33];
  UINT32 HeaderCrc = Header->Header.CRC32;
  UINT32 EntriesCrc = CalculateCrc32 (Entries, 4 * EFI_PAGE_SIZE);
  UINT32 ComputedHeaderCrc;
  STATIC CONST CHAR16 UserdataName[] = L"userdata";

  Header->Header.CRC32 = 0;
  ComputedHeaderCrc = CalculateCrc32 (Header, sizeof (*Header));
  Header->Header.CRC32 = HeaderCrc;
  BOOLEAN Valid = CaptureCrc == (Backup ? 0x294CAD87U : 0x4F1522CEU) &&
                  Header->Header.Signature == EFI_PTAB_HEADER_ID && Header->Header.Revision == 0x10000 &&
                  Header->Header.HeaderSize == sizeof (*Header) && Header->Header.Reserved == 0 &&
                  HeaderCrc == ComputedHeaderCrc && EntriesCrc == Header->PartitionEntryArrayCRC32 &&
                  Header->MyLBA == (Backup ? ELISH_LUN0_LAST : 1) &&
                  Header->AlternateLBA == (Backup ? 1 : ELISH_LUN0_LAST) &&
                  Header->PartitionEntryLBA == (Backup ? ELISH_LUN0_LAST - 4 : 2) &&
                  Header->FirstUsableLBA == 6 && Header->LastUsableLBA == ELISH_LUN0_LAST - 5 &&
                  Header->NumberOfPartitionEntries == 128 && Header->SizeOfPartitionEntry == sizeof (*Entries) &&
                  Userdata->StartingLBA == 6619136 && Userdata->EndingLBA == 61870314 &&
                  CompareMem (Userdata->PartitionName, UserdataName, sizeof (UserdataName)) == 0;
  Trace ("ELISH_UFS_GPT: %a %a capture-crc=%08x header-crc=%08x/%08x entries-crc=%08x/%08x userdata=%lu-%lu\n",
         Backup ? "backup" : "primary", Valid ? "PASS" : "FAIL", CaptureCrc, HeaderCrc,
         ComputedHeaderCrc, Header->PartitionEntryArrayCRC32, EntriesCrc,
         Userdata->StartingLBA, Userdata->EndingLBA);
  return Valid;
}

STATIC
BOOLEAN
TraceUfsGpt (IN UINTN Host, IN UINTN Base)
{
  UINT8 *Data = (UINT8 *)(Base + 2 * EFI_PAGE_SIZE);
  UINT32 LastLba;
  UINT32 BlockBytes;
  UINT32 KnownCrc;

  Trace ("ELISH_UFS_GPT: LUN 0 data DMA base=0x%lx bytes=0x%lx\n",
         (UINT64)(UINTN)Data, (UINT64)EFI_PAGES_TO_SIZE (ELISH_GPT_BLOCKS));
  if (!ReadUfsLun0 (Host, Base, 0, 0)) {
    return FALSE;
  }
  LastLba = SwapBytes32 (ReadUnaligned32 ((UINT32 *)Data));
  BlockBytes = SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Data + 4)));
  Trace ("ELISH_UFS_GPT: READ CAPACITY(10) last-lba=%u logical-block-bytes=%u\n", LastLba, BlockBytes);
  if (LastLba != ELISH_LUN0_LAST || BlockBytes != EFI_PAGE_SIZE || !ReadUfsLun0 (Host, Base, 1, 1)) {
    return FALSE;
  }
  KnownCrc = CalculateCrc32 (Data, EFI_PAGE_SIZE);
  Trace ("ELISH_UFS_GPT: known LBA 1 %a crc=%08x\n", KnownCrc == 0xFA250023U ? "PASS" : "FAIL", KnownCrc);
  if (KnownCrc != 0xFA250023U ||
      !ReadUfsLun0 (Host, Base, 1, ELISH_GPT_BLOCKS) || !CheckElishGpt (Data, FALSE) ||
      !ReadUfsLun0 (Host, Base, ELISH_LUN0_LAST - 4, ELISH_GPT_BLOCKS) || !CheckElishGpt (Data, TRUE)) {
    return FALSE;
  }
  return TRUE;
}
#endif

STATIC
VOID
TraceUfsNop (VOID)
{
  UFS_DMA_ROUTES               Routes;
  EFI_MEMORY_REGION_DESCRIPTOR HostRegion;
  EFI_PHYSICAL_ADDRESS         Base = MAX_UINT32;
  CONST UINTN                  Pages = 2 + ELISH_UFS_GPT_PROBE * ELISH_GPT_BLOCKS;
  CONST UINT8                  Tag = 0x37;
  UINT32                       FaultBefore[3];
  UINT32                       FaultAfter[3];
#if ELISH_UFS_GPT_PROBE
  BOOLEAN                      GptPassed = FALSE;
#endif

  if (!ValidateUfsMmioWindow ("UFS Host", ELISH_UFS_HOST_BASE,
                              ELISH_UFS_HOST_BYTES, &HostRegion) ||
      !QualifyUfsDmaRoutes (&Routes)) {
    Trace ("ELISH_UFS_NOP: refusing DMA, host window or stream routes differ from run38\n");
    return;
  }

  UINTN Host = (UINTN)HostRegion.Address;
  if (MmioRead32 (Host + UFS_HC_CAP_OFFSET) != 0x1587031F ||
      MmioRead32 (Host + UFS_HC_VER_OFFSET) != 0x300 ||
      MmioRead32 (Host + UFS_HC_ENABLE_OFFSET) != 1 ||
      MmioRead32 (Host + UFS_HC_STATUS_OFFSET) != 0x10F ||
      MmioRead32 (Host + UFS_HC_UTRLRSR_OFFSET) != 1 ||
      MmioRead32 (Host + UFS_HC_UTRLDBR_OFFSET) != 0 ||
      MmioRead32 (Host + UFS_HC_UTMRLRSR_OFFSET) != 0 ||
      MmioRead32 (Host + UFS_HC_UTMRLDBR_OFFSET) != 0) {
    Trace ("ELISH_UFS_NOP: refusing queue takeover, inherited HCI state differs from run38\n");
    return;
  }

  EFI_STATUS Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Base);
  if (EFI_ERROR (Status)) {
    Trace ("ELISH_UFS_NOP: low DMA arena allocation failed %r\n", Status);
    return;
  }
  if ((Base & EFI_PAGE_MASK) != 0 || Base > MAX_UINT32 - (EFI_PAGES_TO_SIZE (Pages) - 1)) {
    Trace ("ELISH_UFS_NOP: invalid DMA arena base=0x%lx\n", Base);
    gBS->FreePages (Base, Pages);
    return;
  }

  UINTN UcdAddress = (UINTN)Base + EFI_PAGE_SIZE;
  UTP_TRD *Trd = (UTP_TRD *)(UINTN)Base;
  UTP_NOP_OUT_UPIU *Request = (UTP_NOP_OUT_UPIU *)UcdAddress;
  UTP_NOP_IN_UPIU *Response = (UTP_NOP_IN_UPIU *)(Request + 1);
  ZeroMem (Trd, 2 * EFI_PAGE_SIZE);
  Request->TaskTag = Tag;
  Trd->Int   = 1;
  Trd->Ct    = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs   = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa = (UINT32)(UcdAddress >> 7);
  Trd->RuO   = sizeof (*Request) / sizeof (UINT32);
  Trd->RuL   = sizeof (*Response) / sizeof (UINT32);
  WriteBackInvalidateDataCacheRange (Trd, 2 * EFI_PAGE_SIZE);

  // The terminal probe owns the queue until reset. Keep its pages reserved,
  // including on timeout, in case a request remains in flight.
  MmioWrite32 (Host + UFS_HC_IE_OFFSET, 0);
  MmioWrite32 (Host + UFS_HC_UTRLRSR_OFFSET, 0);
  if (!WaitUfsRegister (Host + UFS_HC_UTRLRSR_OFFSET, 0) ||
      MmioRead32 (Host + UFS_HC_UTRLDBR_OFFSET) != 0) {
    Trace ("ELISH_UFS_NOP: inherited queue did not stop idle\n");
    return;
  }

  MmioWrite32 (Host + UFS_HC_UTRLBA_OFFSET, (UINT32)Base);
  MmioWrite32 (Host + UFS_HC_UTRLBAU_OFFSET, 0);
  if (MmioRead32 (Host + UFS_HC_UTRLBA_OFFSET) != Base ||
      MmioRead32 (Host + UFS_HC_UTRLBAU_OFFSET) != 0) {
    Trace ("ELISH_UFS_NOP: queue base readback mismatch\n");
    return;
  }
  MmioWrite32 (Host + UFS_HC_UTRLRSR_OFFSET, 1);
  if (!WaitUfsRegister (Host + UFS_HC_UTRLRSR_OFFSET, 1)) {
    Trace ("ELISH_UFS_NOP: diagnostic queue did not start\n");
    return;
  }

  FaultBefore[0] = MmioRead32 (Routes.Base + 0x48);
  FaultBefore[1] = MmioRead32 (Routes.Context[0] + ELISH_SMMU_CB_FSR);
  FaultBefore[2] = MmioRead32 (Routes.Context[1] + ELISH_SMMU_CB_FSR);
  Trace ("ELISH_UFS_NOP: submitting one slot-0 no-data NOP, UTRL=0x%lx UCD=0x%lx GFSR/FSR2/FSR3=%x/%x/%x\n",
         Base, (UINT64)UcdAddress, FaultBefore[0], FaultBefore[1], FaultBefore[2]);
  MmioWrite32 (Host + UFS_HC_UTRLDBR_OFFSET, BIT0);
  BOOLEAN Completed = WaitUfsRegister (Host + UFS_HC_UTRLDBR_OFFSET, 0);
  if (Completed) {
    InvalidateDataCacheRange (Trd, 2 * EFI_PAGE_SIZE);
    FaultAfter[0] = MmioRead32 (Routes.Base + 0x48);
    FaultAfter[1] = MmioRead32 (Routes.Context[0] + ELISH_SMMU_CB_FSR);
    FaultAfter[2] = MmioRead32 (Routes.Context[1] + ELISH_SMMU_CB_FSR);
    BOOLEAN Passed = Trd->Ocs == 0 && Response->TransCode == 0x20 &&
                     Response->TaskTag == Tag && Response->Resp == 0 &&
                     CompareMem (FaultBefore, FaultAfter, sizeof (FaultBefore)) == 0;
    Trace ("ELISH_UFS_NOP: %a OCS=%x type=%x tag=%x response=%x\n",
           Passed ? "PASS" : "FAIL", Trd->Ocs, Response->TransCode, Response->TaskTag, Response->Resp);
#if ELISH_UFS_GPT_PROBE
    if (Passed) {
      GptPassed = TraceUfsGpt (Host, (UINTN)Base);
    }
#endif
  }

  MmioWrite32 (Host + UFS_HC_UTRLRSR_OFFSET, 0);
  BOOLEAN Stopped = WaitUfsRegister (Host + UFS_HC_UTRLRSR_OFFSET, 0);
  UINT32 Doorbell = MmioRead32 (Host + UFS_HC_UTRLDBR_OFFSET);
  FaultAfter[0] = MmioRead32 (Routes.Base + 0x48);
  FaultAfter[1] = MmioRead32 (Routes.Context[0] + ELISH_SMMU_CB_FSR);
  FaultAfter[2] = MmioRead32 (Routes.Context[1] + ELISH_SMMU_CB_FSR);
  Trace ("ELISH_UFS_NOP: completed=%u stopped=%u DBR=%x IS=%x HCS=%x GFSR/FSR2/FSR3=%x/%x/%x\n",
         Completed, Stopped, Doorbell, MmioRead32 (Host + UFS_HC_IS_OFFSET),
         MmioRead32 (Host + UFS_HC_STATUS_OFFSET), FaultAfter[0], FaultAfter[1], FaultAfter[2]);
#if ELISH_UFS_GPT_PROBE
  if (GptPassed && Stopped && Doorbell == 0 && CompareMem (FaultBefore, FaultAfter, sizeof (FaultBefore)) == 0) {
    Trace ("ELISH_UFS_GPT: PASS capacity, known block and both GPT copies\n");
  }
#endif
}
#endif

#if ELISH_UFS_HANDOFF_PROBE && !ELISH_UFS_NOP_PROBE && !ELISH_LOADER_PROBE_RUN_KERNEL
STATIC
VOID
TraceUfsDmaArena (VOID)
{
  EFI_PHYSICAL_ADDRESS  Base = MAX_UINT32;
  EFI_STATUS            Status;
  CONST UINTN           Pages = 3;

  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Base);
  if (EFI_ERROR (Status)) {
    Trace ("ELISH_UFS_HANDOFF: low DMA arena allocation failed %r\n", Status);
    return;
  }
  Trace ("ELISH_UFS_HANDOFF: reserved arena [0x%lx,0x%lx), address cap 0x100000000, DMA not started\n",
         Base, Base + EFI_PAGES_TO_SIZE (Pages));
  gBS->FreePages (Base, Pages);
}
#endif
#endif

STATIC
UINT64
ReadFdtCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
VOID
TraceInitrdLayout (
  VOID
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  CONST VOID         *Fdt;
  CONST VOID         *StartProp;
  CONST VOID         *EndProp;
  CONST UINT8        *Initrd;
  CONST UINT8        FatBootSector[] = { 0xEB, 0x58, 0x90, 'M', 'T', 'O', 'O', '4', '0', '4', '8' };
  CHAR8              Head[(sizeof (FatBootSector) * 3) + 1];
  INT32              Chosen;
  INT32              StartLength;
  INT32              EndLength;
  UINT64             Start;
  UINT64             End;
  UINT64             Size;
  UINTN              Offset;
  BOOLEAN            Found;

  Hob = GetFirstGuidHob (&mEntryArgHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64))) {
    Trace ("ELISH_LOADER_PROBE: initrd layout unavailable; no entry-argument HOB\n");
    return;
  }

  Fdt = (CONST VOID *)(UINTN)ReadUnaligned64 ((CONST UINT64 *)GET_GUID_HOB_DATA (Hob));
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    Trace ("ELISH_LOADER_PROBE: initrd layout unavailable; invalid FDT\n");
    return;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  StartProp = Chosen < 0 ? NULL : FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = Chosen < 0 ? NULL : FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      (((StartLength != 4) && (StartLength != 8))) || (((EndLength != 4) && (EndLength != 8)))) {
    Trace ("ELISH_LOADER_PROBE: initrd layout unavailable; missing FDT range\n");
    return;
  }

  Start = ReadFdtCells (StartProp, StartLength / 4);
  End   = ReadFdtCells (EndProp, EndLength / 4);
  if ((End <= Start) || (End - Start < sizeof (FatBootSector)) || (End - Start > MAX_UINTN)) {
    Trace ("ELISH_LOADER_PROBE: invalid initrd range 0x%lx-0x%lx\n", Start, End);
    return;
  }

  Initrd = (CONST UINT8 *)(UINTN)Start;
  Size   = End - Start;
  AsciiSPrint (Head, sizeof (Head), "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
               (UINTN)Initrd[0], (UINTN)Initrd[1], (UINTN)Initrd[2], (UINTN)Initrd[3],
               (UINTN)Initrd[4], (UINTN)Initrd[5], (UINTN)Initrd[6], (UINTN)Initrd[7],
               (UINTN)Initrd[8], (UINTN)Initrd[9], (UINTN)Initrd[10]);
  Trace ("ELISH_LOADER_PROBE: initrd start=0x%lx bytes=0x%lx head=%a\n", Start, Size, Head);

  Found = FALSE;
  for (Offset = 0; Offset <= (UINTN)Size - sizeof (FatBootSector); Offset++) {
    if ((Initrd[Offset] == FatBootSector[0]) &&
        (CompareMem (Initrd + Offset, FatBootSector, sizeof (FatBootSector)) == 0)) {
      Trace ("ELISH_LOADER_PROBE: FAT boot sector offset=0x%lx aligned512=%u\n",
             (UINT64)Offset, (Offset % 512) == 0);
      Found = TRUE;
      break;
    }
  }

  if (!Found) {
    Trace ("ELISH_LOADER_PROBE: FAT boot sector signature not found in initrd\n");
  }
}

// No DEBUG, console, pool allocation or boot service is used by this writer.
// In particular it remains usable immediately after ExitBootServices succeeds.
STATIC
VOID
Trace (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Line[384];
  VA_LIST  Marker;
  UINTN    Length;
  BOOLEAN  Interrupts;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Line, sizeof (Line), Format, Marker);
  VA_END (Marker);
  Interrupts = SaveAndDisableInterrupts ();
  RamLogWrite ((CONST UINT8 *)Line, Length);
  SetInterruptState (Interrupts);
}

#if ELISH_LOADER_PROBE_PMSG
STATIC
VOID
TracePmsgState (
  VOID
  )
{
  CONST volatile UINT8   *Pmsg;
  CONST volatile UINT32  *Marker;
  UINTN                  Offset;

  Pmsg = (CONST volatile UINT8 *)(UINTN)ELISH_PMSG_BASE;
  Marker = (CONST volatile UINT32 *)(UINTN)ELISH_PMSG_RETURN_MARKER_ADDRESS;
  Trace ("ELISH_PMSG_READBACK: region=0x%lx+0x%lx signature=0x%08x start=0x%08x size=0x%08x\n",
         (UINT64)ELISH_PMSG_BASE, (UINT64)ELISH_PMSG_BYTES,
         ReadUnaligned32 ((CONST VOID *)(UINTN)(Pmsg + 0)),
         ReadUnaligned32 ((CONST VOID *)(UINTN)(Pmsg + 4)),
         ReadUnaligned32 ((CONST VOID *)(UINTN)(Pmsg + 8)));
  for (Offset = 0; Offset < 48; Offset += 16) {
    Trace ("ELISH_PMSG_READBACK: +0x%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           Offset,
           (UINTN)Pmsg[Offset + 0], (UINTN)Pmsg[Offset + 1],
           (UINTN)Pmsg[Offset + 2], (UINTN)Pmsg[Offset + 3],
           (UINTN)Pmsg[Offset + 4], (UINTN)Pmsg[Offset + 5],
           (UINTN)Pmsg[Offset + 6], (UINTN)Pmsg[Offset + 7],
           (UINTN)Pmsg[Offset + 8], (UINTN)Pmsg[Offset + 9],
           (UINTN)Pmsg[Offset + 10], (UINTN)Pmsg[Offset + 11],
           (UINTN)Pmsg[Offset + 12], (UINTN)Pmsg[Offset + 13],
           (UINTN)Pmsg[Offset + 14], (UINTN)Pmsg[Offset + 15]);
  }
  Trace ("ELISH_PMSG_READBACK: return-marker address=0x%lx magic=0x%08x complement=0x%08x valid=%u\n",
         (UINT64)ELISH_PMSG_RETURN_MARKER_ADDRESS,
         Marker[0], Marker[1],
         Marker[0] == ELISH_PMSG_RETURN_MARKER &&
         Marker[1] == ~ELISH_PMSG_RETURN_MARKER);
}
#endif

STATIC
VOID
EFIAPI
Timeout (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  Trace ("ELISH_LOADER_PROBE: timeout inside-loader=%u; warm restart\n", mInsideLoader);
  RamLogRestartIntoFastboot ();
}

STATIC
EFI_STATUS
EFIAPI
ProbeExitBootServices (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS             Status;
  EFI_STATUS             MapStatus;
  EFI_MEMORY_DESCRIPTOR  *Descriptor;
  UINTN                  Bytes;
  UINTN                  Key;
  UINTN                  Stride;
  UINTN                  Offset;
  UINT32                 Version;

  if (!mInsideLoader) {
    return mExitBootServices (ImageHandle, MapKey);
  }

  Bytes   = sizeof (mMapStorage);
  Key     = 0;
  Stride  = 0;
  Version = 0;
  MapStatus = gBS->GetMemoryMap (&Bytes, (EFI_MEMORY_DESCRIPTOR *)mMapStorage, &Key, &Stride, &Version);
  Trace ("ELISH_LOADER_PROBE: EBS call image=%p requested-key=0x%lx snapshot=%r key=0x%lx bytes=0x%lx stride=0x%lx version=%u\n",
         ImageHandle, (UINT64)MapKey, MapStatus, (UINT64)Key, (UINT64)Bytes, (UINT64)Stride, Version);

  // The snapshot uses a fixed buffer. Only direct ramoops stores occur between
  // GetMemoryMap and the original service. A matching key and EFI_SUCCESS are
  // required before calling this the accepted map. EBS notifications can make
  // the first attempt fail; DarwinBoot keeps its original retry behavior.
  Status = mExitBootServices (ImageHandle, MapKey);
  Trace ("ELISH_LOADER_PROBE: EBS returned %r accepted-snapshot=%u\n", Status,
         !EFI_ERROR (Status) && !EFI_ERROR (MapStatus) && Key == MapKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Boot services are gone: no gBS/gST, console, events or allocations below.
  if (!EFI_ERROR (MapStatus) && Key == MapKey &&
      Stride >= sizeof (EFI_MEMORY_DESCRIPTOR) && Bytes <= sizeof (mMapStorage) && Bytes % Stride == 0) {
    for (Offset = 0; Offset < Bytes; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mMapStorage + Offset);
      Trace ("ELISH_LOADER_PROBE: map %u type=%u pa=0x%lx pages=0x%lx attr=0x%lx\n",
             (UINT32)(Offset / Stride), Descriptor->Type, Descriptor->PhysicalStart,
             Descriptor->NumberOfPages, Descriptor->Attribute);
    }
  }
#if ELISH_LOADER_PROBE_RUN_KERNEL
  Trace ("ELISH_LOADER_PROBE: capture complete after EBS; continue to diagnostic XNU\n");
  return EFI_SUCCESS;
#else
  Trace ("ELISH_LOADER_PROBE: capture complete after EBS; XNU not entered\n");
  RamLogRestartIntoFastboot ();
  CpuDeadLoop ();
  return EFI_ABORTED;
#endif
}

STATIC
EFI_STATUS
EFIAPI
ProbeBoot (
  IN UNIXNU_LOADER_PROTOCOL            *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST  *Request
  )
{
  EFI_STATUS  Status;

  Trace ("ELISH_LOADER_PROBE: DarwinBoot request=%p\n", Request);
  if (Request != NULL) {
    Trace ("ELISH_LOADER_PROBE: volume=%p kernel=%s args=%s\n", Request->Volume,
           Request->KernelPath != NULL ? Request->KernelPath : L"(null)",
           Request->BootArgs != NULL ? Request->BootArgs : L"(null)");
    Trace ("ELISH_LOADER_PROBE: initrd=%p bytes=0x%lx fdt=%p bytes=0x%lx kexts=%lu\n",
           Request->Ramdisk, (UINT64)Request->RamdiskSize, Request->DeviceTree,
           (UINT64)Request->DeviceTreeSize, (UINT64)Request->KextCount);
  }
  mInsideLoader = TRUE;
#if ELISH_LOADER_PROBE_UFS_CLOCKS
  TraceIortState ();
  TraceSmmuState ();
  if (TraceUfsClockState ()) {
    TraceUfsHostState ();
#if ELISH_UFS_NOP_PROBE
    TraceUfsNop ();
#endif
  }
#if !ELISH_LOADER_PROBE_RUN_KERNEL
#if ELISH_UFS_HANDOFF_PROBE && !ELISH_UFS_NOP_PROBE
  TraceUfsDmaArena ();
#endif
  Trace ("ELISH_UFS_CLOCK_PROBE: warm restart before DarwinBoot\n");
  RamLogRestartIntoFastboot ();
  CpuDeadLoop ();
#endif
#elif ELISH_LOADER_PROBE_SMMU
  TraceIortState ();
  TraceSmmuState ();
#endif
  Status = mBoot (This, Request);
  mInsideLoader = FALSE;
  Trace ("ELISH_LOADER_PROBE: DarwinBoot returned %r; warm restart\n", Status);
  RamLogRestartIntoFastboot ();
  CpuDeadLoop ();
  return Status;
}

EFI_STATUS
EFIAPI
ElishLoaderProbeDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  UNIXNU_LOADER_PROTOCOL  *Loader;
  EFI_EVENT               Timer;
  EFI_TPL                 OldTpl;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, (VOID **)&Loader);
  if (EFI_ERROR (Status)) { return Status; }
  if (Loader->Revision != UNIXNU_LOADER_REVISION || Loader->Boot == NULL) { return EFI_UNSUPPORTED; }

#if ELISH_LOADER_PROBE_PMSG
  TracePmsgState ();
#endif

  TraceInitrdLayout ();

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Timeout, NULL, &Timer);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->SetTimer (Timer, TimerRelative, PROBE_TIMEOUT_SECONDS * 10000000ULL);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Timer);
    return Status;
  }

  // Interpose only in this separate diagnostic image. Keep the original loader
  // and service implementation, and maintain the boot-services table CRC.
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  mBoot = Loader->Boot;
  mExitBootServices = gBS->ExitBootServices;
  Loader->Boot = ProbeBoot;
  gBS->ExitBootServices = ProbeExitBootServices;
  gBS->Hdr.CRC32 = 0;
  gBS->Hdr.CRC32 = CalculateCrc32 (gBS, gBS->Hdr.HeaderSize);
  gBS->RestoreTPL (OldTpl);
  Trace ("ELISH_LOADER_PROBE: armed %u seconds, loader revision=0x%x; run-kernel=%u\n",
         PROBE_TIMEOUT_SECONDS, Loader->Revision, ELISH_LOADER_PROBE_RUN_KERNEL);
  return EFI_SUCCESS;
}
