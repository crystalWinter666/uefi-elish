// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Uefi/UefiInternalFormRepresentation.h>

#define CONSOLE_BASE  0xB0000000
#define CONSOLE_DATA  (0x200000 - 12)
#define CONSOLE_SIG   0x43474244
#define LINE_BYTES    192

UINT32
EFIAPI
ElishWarmFastbootPreserveConsole (VOID);

STATIC
CONST CHAR8 *
TouchLine (IN CONST CHAR8 *Line)
{
  // Skip any kernel log priority prefix, e.g. <5>.
  if (Line[0] == '<' && Line[1] >= '0' && Line[1] <= '9' && Line[2] == '>') {
    Line += 3;
  }
  // XNU's IOLog prefixes msgbuf lines with a timestamp in square brackets.
  if (Line[0] == '[') {
    CONST CHAR8 *End = AsciiStrStr (Line, "]: ");
    if (End != NULL) {
      Line = End + 3;
    }
  }
  if (AsciiStrnCmp (Line, "ELISH_TOUCH:", 12) == 0 ||
      AsciiStrnCmp (Line, "elish-touch-hid:", 16) == 0 ||
      AsciiStrnCmp (Line, "ELISH_XNU: RAM-only deadline", 28) == 0) {
    return Line;
  }
  return NULL;
}

STATIC
VOID
ShowLog (IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Text)
{
  CONST volatile UINT8 *Data = (CONST volatile UINT8 *)(CONSOLE_BASE + 12);
  UINT32 Signature = MmioRead32 (CONSOLE_BASE);
  UINT32 Cursor = MmioRead32 (CONSOLE_BASE + 4);
  UINT32 Size = MmioRead32 (CONSOLE_BASE + 8);
  UINTN Columns;
  UINTN Rows;
  UINTN UsedRows = 4;
  UINTN Matches = 0;
  UINTN Length = 0;
  CHAR8 Line[LINE_BYTES];
  CHAR16 Output[LINE_BYTES + 2];

  Text->SetAttribute (Text, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  Text->ClearScreen (Text);
  Text->EnableCursor (Text, FALSE);
  UnicodeSPrint (Output, sizeof (Output), L"ELISH touch ramoops\r\nSig %08x  Cursor %u  Bytes %u\r\n\r\n",
                 Signature, Cursor, Size);
  Text->OutputString (Text, Output);

  if (Signature != CONSOLE_SIG || Cursor >= CONSOLE_DATA || Size > CONSOLE_DATA ||
      (Size != CONSOLE_DATA && Cursor != Size)) {
    Text->OutputString (Text, L"No valid console ring.\r\n");
    return;
  }
  if (EFI_ERROR (Text->QueryMode (Text, Text->Mode->Mode, &Columns, &Rows)) || Columns < 40 || Rows < 10) {
    return;
  }

  UINT32 Offset = Size == CONSOLE_DATA ? Cursor : 0;
  for (UINT32 Remaining = Size; Remaining != 0; --Remaining) {
    UINT8 Byte = Data[Offset];
    if (++Offset == CONSOLE_DATA) {
      Offset = 0;
    }
    if (Byte != '\n') {
      if (Byte != '\r' && Length < sizeof (Line) - 1) {
        Line[Length++] = Byte >= ' ' && Byte <= '~' ? Byte : '?';
      }
      continue;
    }
    Line[Length] = '\0';
    CONST CHAR8 *Match = TouchLine (Line);
    if (Match != NULL) {
      ++Matches;
      UINTN MatchLength = Length - (Match - Line);
      UINTN LineRows = (MatchLength + Columns - 2) / (Columns - 1);
      if (UsedRows + LineRows < Rows - 3) {
        for (UINTN Start = 0; Start < MatchLength; Start += Columns - 1) {
          UINTN Count = MIN (MatchLength - Start, Columns - 1);
          UnicodeSPrint (Output, sizeof (Output), L"%.*a\r\n", Count, Match + Start);
          Text->OutputString (Text, Output);
        }
        UsedRows += LineRows;
      }
    }
    Length = 0;
  }
  UnicodeSPrint (Output, sizeof (Output), L"\r\n%u matching lines. Returning to fastboot.\r\n", Matches);
  Text->OutputString (Text, Output);
}

STATIC
VOID
MagnifyLog (IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Text)
{
  UINTN Columns, Rows;
  if (Text->Mode->Mode < 0 || Text->Mode->CursorRow < 0 ||
      EFI_ERROR (Text->QueryMode (Text, Text->Mode->Mode, &Columns, &Rows)) ||
      (UINTN)Text->Mode->CursorRow >= Rows) {
    return;
  }

  UINTN ScreenWidth = Gop->Mode->Info->HorizontalResolution;
  UINTN ScreenHeight = Gop->Mode->Info->VerticalResolution;
  UINTN Width = Columns * EFI_GLYPH_WIDTH;
  UINTN Height = ((UINTN)Text->Mode->CursorRow + 1) * EFI_GLYPH_HEIGHT;
  if (Width == 0 || Width > ScreenWidth / 2 || Height > ScreenHeight / 2 ||
      Rows > ScreenHeight / EFI_GLYPH_HEIGHT) {
    return;
  }

  UINTN Stride = 2 * Width;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels = AllocatePool (2 * Height * Stride * sizeof (*Pixels));
  if (Pixels == NULL) {
    return;
  }

  // GraphicsConsole centers the text grid. Expand from the end of the buffer
  // so that each original glyph pixel is read before its slot is reused.
  if (!EFI_ERROR (Gop->Blt (Gop, Pixels, EfiBltVideoToBltBuffer,
                            (ScreenWidth - Width) / 2, (ScreenHeight - Rows * EFI_GLYPH_HEIGHT) / 2,
                            0, 0, Width, Height, 0))) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = { 0, 0, 0, 0 };
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Ink = { 0x60, 0x60, 0x60, 0 };
    for (UINTN Y = Height; Y-- > 0;) {
      for (UINTN X = Width; X-- > 0;) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel = Pixels[Y * Width + X].Green ? Ink : Black;
        UINTN Dest = 2 * Y * Stride + 2 * X;
        Pixels[Dest] = Pixels[Dest + 1] = Pixels[Dest + Stride] = Pixels[Dest + Stride + 1] = Pixel;
      }
    }
    UINTN Top = MIN (ScreenHeight / 4, ScreenHeight - 2 * Height);
    Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, ScreenWidth, ScreenHeight, 0);
    Gop->Blt (Gop, Pixels, EfiBltBufferToVideo, 0, 0, (ScreenWidth - Stride) / 2, Top,
              Stride, 2 * Height, Stride * sizeof (*Pixels));
  }
  FreePool (Pixels);
}

EFI_STATUS
EFIAPI
ElishRamoopsViewerEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Text = NULL;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles))) {
    for (UINTN Index = 0; Index < Count; ++Index) {
      gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
      EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) &&
          !EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextOutProtocolGuid, (VOID **)&Text))) {
        ShowLog (Text);
        MagnifyLog (Gop, Text);
        break;
      }
    }
    FreePool (Handles);
  }
  // Stay in DXE here so BDS cannot try a persistent boot target.
  gBS->Stall (35000000);
  UINT32 Code = ElishWarmFastbootPreserveConsole ();
  if (Text != NULL) {
    CHAR16 Output[80];
    UnicodeSPrint (Output, sizeof (Output), L"Warm reset failed (%02x). Use the hardware keys for fastboot.\r\n", Code);
    Text->OutputString (Text, Output);
  }
  CpuDeadLoop ();
  return EFI_DEVICE_ERROR;
}
