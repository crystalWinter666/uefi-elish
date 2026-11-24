/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#include <Guid/Fdt.h>
#include <Protocol/AdvancedLogger.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/RamLogLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIRamPartition.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/UniXNUMenuPlugin.h>

#define ELISH_BANK0_BASE  0x80000000ULL
#define ELISH_BANK0_END   0xBBB00000ULL
#define ELISH_BANK1_BASE  0xC0000000ULL
#define ELISH_BANK1_END   0x280000000ULL
#define MAP_BUFFER_EXTRA_DESCRIPTORS  8
#define MAP_TEXT_BUFFER_SIZE  0x10000
#define FDT_BUFFER_EXTRA      (MAP_TEXT_BUFFER_SIZE + 0x1000)
#define MAP_TEXT_PROPERTY     "unixnu,uefi-memory-map"

#ifndef ELISH_MAP_CAPTURE_TO_RAMOOPS
#define ELISH_MAP_CAPTURE_TO_RAMOOPS  0
#endif

STATIC
BOOLEAN
OverlapsElishDram (
  IN EFI_MEMORY_DESCRIPTOR  *Descriptor
  );

STATIC
EFI_STATUS
ExportProbeLog (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UNIXNU_MENU_PLUGIN_PROTOCOL   *Plugin;
  UNIXNU_PLUGIN_RECT            Content;

  Status = gBS->LocateProtocol (&gUniXNUMenuPluginProtocolGuid, NULL, (VOID **)&Plugin);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE log plugin unavailable: %r\n", Status));
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE GOP unavailable: %r\n", Status));
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }

  Content.X      = 0;
  Content.Y      = 0;
  Content.Width  = Gop->Mode->Info->HorizontalResolution;
  Content.Height = Gop->Mode->Info->VerticalResolution;
  DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE starting QR log export\n"));
  return Plugin->Run (Plugin, Gop, &Content, NULL);
}

STATIC
BOOLEAN
AppendMapText (
  IN OUT CHAR8        *Buffer,
  IN     UINTN         BufferSize,
  IN OUT UINTN        *Offset,
  IN     CONST CHAR8  *Format,
  ...
  )
{
  CHAR8   Line[256];
  VA_LIST Args;
  UINTN   Length;

  if (*Offset >= BufferSize) {
    return FALSE;
  }

  VA_START (Args, Format);
  Length = AsciiVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);

  if ((Length >= sizeof (Line) - 1) || (Length >= BufferSize - *Offset)) {
    return FALSE;
  }

  CopyMem (Buffer + *Offset, Line, Length + 1);
  *Offset += Length;
  return TRUE;
}

STATIC
EFI_STATUS
PrepareWritableFdt (
  OUT VOID  **Fdt,
  OUT INT32  *ChosenNode
  )
{
  EFI_STATUS  Status;
  VOID        *SourceFdt;
  UINTN       Index;
  UINT32      SourceSize;
  UINTN       BufferSize;
  INT32       Node;

  *Fdt        = NULL;
  *ChosenNode = -1;
  SourceFdt   = NULL;

  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gFdtTableGuid)) {
      SourceFdt = gST->ConfigurationTable[Index].VendorTable;
      break;
    }
  }

  if (SourceFdt == NULL) {
    return EFI_NOT_FOUND;
  }

  if (FdtCheckHeader (SourceFdt) != 0) {
    return EFI_COMPROMISED_DATA;
  }

  SourceSize = FdtTotalSize (SourceFdt);
  if ((UINT64)SourceSize + FDT_BUFFER_EXTRA > 0x7FFFFFFF) {
    return EFI_BAD_BUFFER_SIZE;
  }

  BufferSize = SourceSize + FDT_BUFFER_EXTRA;
  *Fdt = AllocatePool (BufferSize);
  if (*Fdt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (SourceFdt, *Fdt, (INT32)BufferSize) != 0) {
    FreePool (*Fdt);
    *Fdt = NULL;
    return EFI_COMPROMISED_DATA;
  }

  Node = FdtPathOffset (*Fdt, "/chosen");
  if (Node < 0) {
    Node = FdtAddSubnode (*Fdt, 0, "chosen");
  }

  if (Node < 0) {
    FreePool (*Fdt);
    *Fdt = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, *Fdt);
  if (EFI_ERROR (Status)) {
    FreePool (*Fdt);
    *Fdt = NULL;
    return Status;
  }

  *ChosenNode = Node;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
BuildMemoryMapText (
  IN  EFI_STATUS              PartitionStatus,
  IN  RamPartitionEntry      *Partitions,
  IN  UINT32                  PartitionCount,
  IN  EFI_STATUS              MapStatus,
  IN  EFI_MEMORY_DESCRIPTOR  *Map,
  IN  UINTN                   MapSize,
  IN  UINTN                   DescriptorSize,
  IN  UINT32                  DescriptorVersion,
  OUT CHAR8                  *Text,
  IN  UINTN                   TextCapacity,
  OUT UINTN                  *TextLength
  )
{
  EFI_MEMORY_DESCRIPTOR  *Descriptor;
  UINTN                  DescriptorCount;
  UINTN                  RelevantDescriptorCount;
  UINTN                  Index;
  UINTN                  Offset;

  Offset      = 0;
  *TextLength = 0;
  Text[0]     = '\0';

  if (!AppendMapText (Text, TextCapacity, &Offset,
                      "ELISH UEFI PRE-BDS MEMORY MAP v1\n"
                      "stage=DXE-entry-before-BDS\n"
                      "ram-partitions-status=0x%016lx count=%u\n",
                      (UINT64)PartitionStatus,
                      PartitionCount))
  {
    return FALSE;
  }

  if (!EFI_ERROR (PartitionStatus)) {
    for (Index = 0; Index < PartitionCount; Index++) {
      if (!AppendMapText (Text, TextCapacity, &Offset,
                          "ram-partition[%u] base=0x%016lx length=0x%016lx\n",
                          (UINT32)Index,
                          Partitions[Index].Base,
                          Partitions[Index].AvailableLength))
      {
        return FALSE;
      }
    }
  }

  if (!AppendMapText (Text, TextCapacity, &Offset,
                      "memory-map-status=0x%016lx size=0x%lx descriptor-size=0x%lx version=%u\n",
                      (UINT64)MapStatus,
                      (UINT64)MapSize,
                      (UINT64)DescriptorSize,
                      DescriptorVersion))
  {
    return FALSE;
  }

  if (EFI_ERROR (MapStatus)) {
    *TextLength = Offset;
    return TRUE;
  }

  if ((DescriptorSize == 0) ||
      (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
      ((MapSize % DescriptorSize) != 0) ||
      ((Map == NULL) && (MapSize != 0)))
  {
    if (!AppendMapText (Text, TextCapacity, &Offset,
                        "memory-map-error=invalid-descriptor-layout\n"))
    {
      return FALSE;
    }

    *TextLength = Offset;
    return TRUE;
  }

  DescriptorCount = MapSize / DescriptorSize;
  RelevantDescriptorCount = 0;
  for (Index = 0; Index < DescriptorCount; Index++) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (Index * DescriptorSize));
    if (OverlapsElishDram (Descriptor)) {
      RelevantDescriptorCount++;
    }
  }

  if (!AppendMapText (Text, TextCapacity, &Offset,
                      "descriptor-count=%lu\n",
                      (UINT64)RelevantDescriptorCount))
  {
    return FALSE;
  }

  for (Index = 0; Index < DescriptorCount; Index++) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (Index * DescriptorSize));
    if (!OverlapsElishDram (Descriptor)) {
      continue;
    }

    if (!AppendMapText (Text, TextCapacity, &Offset,
                        "descriptor[%u] type=%u base=0x%016lx pages=0x%016lx virtual=0x%016lx attr=0x%016lx\n",
                        (UINT32)Index,
                        Descriptor->Type,
                        Descriptor->PhysicalStart,
                        Descriptor->NumberOfPages,
                        Descriptor->VirtualStart,
                        Descriptor->Attribute))
    {
      return FALSE;
    }
  }

  *TextLength = Offset;
  return TRUE;
}

STATIC
EFI_STATUS
WriteProbeTextToLogger (
  IN CONST CHAR8  *Text,
  IN UINTN        TextLength
  )
{
  EFI_STATUS                Status;
  ADVANCED_LOGGER_PROTOCOL  *Logger;
  UINTN                     Offset;
  UINTN                     LineLength;

  Status = gBS->LocateProtocol (&gAdvancedLoggerProtocolGuid, NULL, (VOID **)&Logger);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Logger->Signature != ADVANCED_LOGGER_PROTOCOL_SIGNATURE) ||
      (Logger->Version != ADVANCED_LOGGER_PROTOCOL_VERSION) ||
      (Logger->AdvancedLoggerWriteProtocol == NULL))
  {
    return EFI_COMPROMISED_DATA;
  }

  Offset = 0;
  while (Offset < TextLength) {
    LineLength = 0;
    while (((Offset + LineLength) < TextLength) && (Text[Offset + LineLength] != '\n')) {
      LineLength++;
    }

    if ((Offset + LineLength) < TextLength) {
      LineLength++;
    }

    Logger->AdvancedLoggerWriteProtocol (Logger, DEBUG_ERROR, Text + Offset, LineLength);
    Offset += LineLength;
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
OverlapsElishDram (
  IN EFI_MEMORY_DESCRIPTOR *Descriptor
  )
{
  EFI_PHYSICAL_ADDRESS Base;
  EFI_PHYSICAL_ADDRESS End;

  Base = Descriptor->PhysicalStart;
  End  = Base + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);

  return ((Base < ELISH_BANK0_END) && (End > ELISH_BANK0_BASE)) ||
         ((Base < ELISH_BANK1_END) && (End > ELISH_BANK1_BASE));
}

STATIC
EFI_STATUS
GetRamPartitionTable (
  OUT EFI_RAMPARTITION_PROTOCOL **Protocol,
  OUT RamPartitionEntry         **Entries,
  OUT UINT32                     *Count
  )
{
  EFI_STATUS Status;

  *Protocol = NULL;
  *Entries  = NULL;
  *Count    = 0;

  Status = gBS->LocateProtocol (
                  &gEfiRamPartitionProtocolGuid,
                  NULL,
                  (VOID **)Protocol
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = (*Protocol)->GetRamPartitions (*Protocol, NULL, Count);
  if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
    return Status;
  }

  if (*Count == 0) {
    return EFI_SUCCESS;
  }

  *Entries = AllocatePool (sizeof (**Entries) * *Count);
  if (*Entries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = (*Protocol)->GetRamPartitions (*Protocol, *Entries, Count);
  if (EFI_ERROR (Status)) {
    FreePool (*Entries);
    *Entries = NULL;
  }

  return Status;
}

STATIC
EFI_STATUS
GetMemoryMapSnapshot (
  OUT EFI_MEMORY_DESCRIPTOR **Map,
  OUT UINTN                  *MapSize,
  OUT UINTN                  *DescriptorSize,
  OUT UINT32                 *DescriptorVersion
  )
{
  EFI_STATUS Status;
  UINTN      MapKey;
  UINTN      Capacity;
  UINTN      Attempt;

  *Map             = NULL;
  *MapSize         = 0;
  *DescriptorSize  = 0;
  *DescriptorVersion = 0;

  Status = gBS->GetMemoryMap (
                  MapSize,
                  NULL,
                  &MapKey,
                  DescriptorSize,
                  DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  if (*DescriptorSize == 0) {
    *DescriptorSize = sizeof (EFI_MEMORY_DESCRIPTOR);
  }

  Capacity = *MapSize + (*DescriptorSize * MAP_BUFFER_EXTRA_DESCRIPTORS);
  for (Attempt = 0; Attempt < 3; Attempt++) {
    *Map = AllocatePool (Capacity);
    if (*Map == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    *MapSize = Capacity;
    Status = gBS->GetMemoryMap (
                    MapSize,
                    *Map,
                    &MapKey,
                    DescriptorSize,
                    DescriptorVersion
                    );
    if (Status != EFI_BUFFER_TOO_SMALL) {
      return Status;
    }

    FreePool (*Map);
    *Map = NULL;
    Capacity = *MapSize + (*DescriptorSize * MAP_BUFFER_EXTRA_DESCRIPTORS);
  }

  return EFI_BUFFER_TOO_SMALL;
}

STATIC
VOID
EFIAPI
DisplayElishMemoryMap (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_RAMPARTITION_PROTOCOL *Protocol;
  RamPartitionEntry         *Partitions;
  UINT32                     PartitionCount;
  EFI_MEMORY_DESCRIPTOR    *Map;
  UINTN                      MapSize;
  UINTN                      DescriptorSize;
  UINT32                     DescriptorVersion;
  UINTN                      DescriptorCount;
  UINTN                      Index;
  EFI_STATUS                 PartitionStatus;
  EFI_STATUS                 MapStatus;
  EFI_STATUS                 LoggerStatus;
  EFI_STATUS                 FdtPrepareStatus;
  VOID                      *Fdt;
  INT32                      ChosenNode;
  INT32                      FdtStatus;
  CHAR8                     *MapText;
  UINTN                      MapTextLength;

  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }

  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"ELISH MEMORY MAP PROBE v3\r\n");
  Print (L"Watchdog disable status: %r\r\n\r\n", Status);
  DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE v2\n"));
  DEBUG ((DEBUG_ERROR, "WATCHDOG_DISABLE status=%r\n", Status));

  PartitionStatus = GetRamPartitionTable (&Protocol, &Partitions, &PartitionCount);
  Print (L"RAM partitions: status=%r count=%u\r\n", PartitionStatus, PartitionCount);
  DEBUG ((DEBUG_ERROR, "RAM_PARTITIONS status=%r count=%u\n", PartitionStatus, PartitionCount));
  if (!EFI_ERROR (PartitionStatus)) {
    for (Index = 0; Index < PartitionCount; Index++) {
      Print (L"RAM[%u] base=0x%lx length=0x%lx\r\n",
             (UINT32)Index,
             Partitions[Index].Base,
             Partitions[Index].AvailableLength);
      DEBUG ((DEBUG_ERROR, "RAM_PARTITION[%u] base=0x%lx length=0x%lx\n",
              (UINT32)Index,
              Partitions[Index].Base,
              Partitions[Index].AvailableLength));
    }
  }

  MapText = AllocateZeroPool (MAP_TEXT_BUFFER_SIZE);
  if (MapText == NULL) {
    Print (L"Cannot allocate memory-map output buffer\r\n");
    DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE output buffer allocation failed\n"));
    return;
  }

  FdtPrepareStatus = PrepareWritableFdt (&Fdt, &ChosenNode);
  Print (L"FDT handoff preparation: %r\r\n", FdtPrepareStatus);
  DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE FDT preparation status=%r\n", FdtPrepareStatus));

  // Keep every diagnostic allocation live after the snapshot is captured.
  MapStatus = GetMemoryMapSnapshot (&Map, &MapSize, &DescriptorSize, &DescriptorVersion);
  if (!EFI_ERROR (MapStatus) &&
      ((DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
       ((MapSize % DescriptorSize) != 0)))
  {
    MapStatus = EFI_COMPROMISED_DATA;
  }

  if (BuildMemoryMapText (
        PartitionStatus,
        Partitions,
        PartitionCount,
        MapStatus,
        Map,
        MapSize,
        DescriptorSize,
        DescriptorVersion,
        MapText,
        MAP_TEXT_BUFFER_SIZE,
        &MapTextLength
        ))
  {
    LoggerStatus = WriteProbeTextToLogger (MapText, MapTextLength);
    Print (L"Advanced Logger capture status: %r\r\n", LoggerStatus);
    DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE logger capture status=%r bytes=%lu\n",
            LoggerStatus,
            (UINT64)MapTextLength));

    if ((Fdt != NULL) && (ChosenNode >= 0)) {
      FdtStatus = FdtSetProp (
                    Fdt,
                    ChosenNode,
                    MAP_TEXT_PROPERTY,
                    MapText,
                    (UINT32)(MapTextLength + 1)
                    );
      Print (L"FDT memory-map property status: %d\r\n", FdtStatus);
      DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE FDT property status=%d bytes=%lu\n",
              FdtStatus,
              (UINT64)(MapTextLength + 1)));
    } else {
      Print (L"FDT memory-map property unavailable\r\n");
      DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE FDT property unavailable\n"));
    }
  } else {
    Print (L"Memory-map text exceeded its output buffer\r\n");
    DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE text buffer overflow\n"));
  }

  if (EFI_ERROR (MapStatus)) {
    Print (L"UEFI memory map failed: %r\r\n", MapStatus);
    DEBUG ((DEBUG_ERROR, "UEFI_MEMORY_MAP status=%r\n", MapStatus));
  } else {
    DescriptorCount = MapSize / DescriptorSize;
    Print (L"\r\nUEFI memory map count=%lu stride=%lu version=%u\r\n",
           (UINT64)DescriptorCount,
           (UINT64)DescriptorSize,
           DescriptorVersion);
    DEBUG ((DEBUG_ERROR, "UEFI_MEMORY_MAP count=%lu stride=%lu version=%u\n",
            (UINT64)DescriptorCount,
            (UINT64)DescriptorSize,
            DescriptorVersion));

    for (Index = 0; Index < DescriptorCount; Index++) {
      EFI_MEMORY_DESCRIPTOR *Descriptor;

      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (Index * DescriptorSize));
      if (!OverlapsElishDram (Descriptor)) {
        continue;
      }

      Print (L"MAP[%u] type=%u base=0x%lx pages=0x%lx attr=0x%lx\r\n",
             (UINT32)Index,
             Descriptor->Type,
             Descriptor->PhysicalStart,
             Descriptor->NumberOfPages,
             Descriptor->Attribute);
      DEBUG ((DEBUG_ERROR, "UEFI_MAP[%u] type=%u base=0x%lx pages=0x%lx attr=0x%lx\n",
              (UINT32)Index,
              Descriptor->Type,
              Descriptor->PhysicalStart,
              Descriptor->NumberOfPages,
              Descriptor->Attribute));
    }
  }

  Print (L"\r\nProbe complete. Continuing boot.\r\n");
  DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE complete\n"));
  Status = ExportProbeLog ();
  DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE QR export returned: %r\n", Status));
}

EFI_STATUS
EFIAPI
ElishMemoryMapProbeDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  if (ELISH_MAP_CAPTURE_TO_RAMOOPS) {
    EFI_RAMPARTITION_PROTOCOL  *Protocol;
    RamPartitionEntry          *Partitions;
    UINT32                      PartitionCount;
    EFI_MEMORY_DESCRIPTOR      *Map;
    UINTN                       MapSize;
    UINTN                       DescriptorSize;
    UINT32                      DescriptorVersion;
    EFI_STATUS                  PartitionStatus;
    EFI_STATUS                  MapStatus;
    CHAR8                      *Text;
    UINTN                       Length;

    DEBUG ((DEBUG_ERROR, "ELISH: collecting RAM partitions and UEFI map for pstore\n"));
    Text = AllocatePool (MAP_TEXT_BUFFER_SIZE);
    if (Text == NULL) {
      DEBUG ((DEBUG_ERROR, "ELISH: could not allocate map capture buffer\n"));
      RamLogRestartIntoFastboot ();
      return EFI_OUT_OF_RESOURCES;
    }

    PartitionStatus = GetRamPartitionTable (&Protocol, &Partitions, &PartitionCount);
    MapStatus = GetMemoryMapSnapshot (&Map, &MapSize, &DescriptorSize, &DescriptorVersion);
    if (BuildMemoryMapText (PartitionStatus, Partitions, PartitionCount, MapStatus,
                            Map, MapSize, DescriptorSize, DescriptorVersion,
                            Text, MAP_TEXT_BUFFER_SIZE, &Length)) {
      RamLogWrite ((CONST UINT8 *)Text, Length);
    } else {
      DEBUG ((DEBUG_ERROR, "ELISH: memory map exceeds capture buffer\n"));
    }

    if (Partitions != NULL) {
      FreePool (Partitions);
    }

    if (Map != NULL) {
      FreePool (Map);
    }

    FreePool (Text);
    RamLogRestartIntoFastboot ();
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "ELISH_MEMORY_MAP_PROBE driver started\n"));
  Print (L"ELISH MEMORY MAP PROBE ENTRY\r\n");
  gBS->Stall (15000000);
  DisplayElishMemoryMap ();
  Print (L"Holding memory-map probe output for 120 seconds.\r\n");
  gBS->Stall (120000000);
  return EFI_SUCCESS;
}
