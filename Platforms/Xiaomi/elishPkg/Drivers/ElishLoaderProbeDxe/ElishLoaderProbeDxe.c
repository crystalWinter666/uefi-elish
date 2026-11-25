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
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PrintLib.h>
#include <Library/RamLogLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <IndustryStandard/IoRemappingTable.h>
#include <Protocol/UniXNULoader.h>

#define PROBE_TIMEOUT_SECONDS  20
#define PROBE_MAP_BYTES        SIZE_64KB
#define ELISH_SMMU_GR0_ID0     0x20
#define ELISH_SMMU_GR0_ID1     0x24
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

  Id0 = MmioRead32 ((UINTN)(Region.Address + ELISH_SMMU_GR0_ID0));
  Id1 = MmioRead32 ((UINTN)(Region.Address + ELISH_SMMU_GR0_ID1));
  Trace ("ELISH_SMMU_PROBE: ID0=0x%08x ID1=0x%08x\n", Id0, Id1);
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

  Trace ("ELISH_SMMU_PROBE: read-only scan complete\n");
}
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
#if ELISH_LOADER_PROBE_SMMU
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
