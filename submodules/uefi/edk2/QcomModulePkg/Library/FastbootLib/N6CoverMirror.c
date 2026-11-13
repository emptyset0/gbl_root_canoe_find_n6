/** @file
  Find N6 frame mirror and optional EFI application service.
  Keep the device-specific DisplayDxe checks and blit in one provider so
  standalone tools need only request a complete frame mirror.
**/
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/N6CoverMirror.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct {
  UINT32 Width;
  UINT32 Height;
  UINT64 Unknown0;
  UINT64 Base;
  UINT64 Unknown1;
  UINT32 Stride;
  UINT32 Unknown2;
} N6_DSI_SURFACE;

typedef EFI_STATUS (EFIAPI *N6_DISPLAY_BLT) (
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINT32 DisplayId,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixel, EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,
    UINTN SourceX, UINTN SourceY, UINTN DestinationX, UINTN DestinationY,
    UINTN Width, UINTN Height, UINTN Delta);

STATIC EFI_GUID mN6DisplayUtils = {
  0xc0dd69ac, 0x76ba, 0x11e6, {0xab, 0x24, 0x1f, 0xc7, 0xf5, 0x57, 0x5f, 0x19}
};
STATIC CONST UINT8 mN6BltCode[] = { 0xff, 0xc3, 0x02, 0xd1, 0xfd, 0x7b, 0x05, 0xa9, 0xfc, 0x6f, 0x06, 0xa9, 0xfa, 0x67, 0x07, 0xa9, 0xf8, 0x5f, 0x08, 0xa9, 0xf6, 0x57, 0x09, 0xa9, 0xf4, 0x4f, 0x0a, 0xa9, 0xfd, 0x43, 0x01, 0x91, 0x58, 0x00, 0x80, 0xd2, 0x18, 0x00, 0xf0, 0xf2, 0xc0, 0x25, 0x00, 0xb4, 0xc8, 0x02, 0x00, 0xd0, 0x08, 0xa1, 0x5d, 0x39, 0xa8, 0x07, 0x00, 0x34, 0xa8, 0x3b, 0x40, 0xf9, 0xfa, 0x03, 0x01, 0x2a, 0xb3, 0x6f, 0x46, 0xa9, 0xe0, 0xa3, 0x00, 0x91, 0x01, 0x05, 0x80, 0x52, 0xf4, 0x03, 0x07, 0xaa, 0xf6, 0x03, 0x05, 0xaa, 0xf5, 0x03, 0x04, 0xaa, 0xf9, 0x03, 0x03, 0x2a, 0xe2, 0x13, 0x00, 0xf9, 0xf7, 0x03, 0x06, 0xaa, 0xe8, 0x0f, 0x00, 0xf9, 0x1c, 0x05, 0x80, 0x52, 0x7b, 0x84, 0x00, 0x94, 0xc8, 0x02, 0x00, 0xf0, 0x08, 0x41, 0x05, 0x91, 0x49, 0x23, 0xbc, 0x9b, 0x5f, 0x07, 0x00, 0x71, 0xe0, 0xa3, 0x00, 0x91, 0x02, 0x05, 0x80, 0x52, 0x01, 0x81, 0x89, 0x9a, 0xab, 0x83, 0x00, 0x94, 0x3f, 0x07, 0x00, 0x71, 0xec, 0x04, 0x00, 0x54, 0x39, 0x11, 0x00, 0x34 };
STATIC CONST UINT8 mN6LogoCode[] = { 0xa0, 0x03, 0x5f, 0xf8, 0xe8, 0x03, 0x14, 0x2a, 0xe3, 0x03, 0x1f, 0x2a, 0xe4, 0x03, 0x1f, 0xaa, 0xe5, 0x03, 0x1f, 0xaa, 0xe6, 0x03, 0x1f, 0xaa, 0xe7, 0x03, 0x1f, 0xaa, 0xa2, 0x53, 0x00, 0xd1, 0xe1, 0x03, 0x19, 0x2a, 0xe8, 0xff, 0x00, 0xa9, 0xed, 0x03, 0x00, 0xf9, 0xf3, 0x03, 0x0d, 0xaa, 0x49, 0x6c, 0xff, 0x97, 0xa0, 0x00, 0x00, 0xb4, 0xf8, 0x03, 0x00, 0xaa, 0xe1, 0x00, 0x00, 0xf0, 0x21, 0x50, 0x02, 0x91, 0x14, 0x00, 0x00, 0x14, 0xe8, 0x7a, 0x1e, 0x12, 0xa0, 0x03, 0x5f, 0xf8, 0xa7, 0x9b, 0x7c, 0x29, 0x1f, 0x05, 0x00, 0x71, 0xe4, 0x03, 0x1f, 0xaa, 0x95, 0x02, 0x93, 0x1a, 0x73, 0x02, 0x94, 0x1a, 0xe5, 0x03, 0x1f, 0xaa, 0xe1, 0x03, 0x19, 0x2a, 0xe2, 0x03, 0x16, 0xaa, 0x43, 0x00, 0x80, 0x52, 0xa8, 0x7e, 0x7e, 0xd3, 0xf5, 0x03, 0x00, 0xf9, 0xf3, 0xa3, 0x00, 0xa9, 0x35, 0x6c, 0xff, 0x97 };
STATIC UINT32 *mN6Snapshot;
STATIC UINT32 *mN6Canvas;
STATIC UINTN mN6CanvasPixels;
STATIC UINTN mN6LastX;
STATIC UINTN mN6LastY;
STATIC UINTN mN6LastW;
STATIC UINTN mN6LastH;


STATIC BOOLEAN
N6HasCode (CONST UINT8 *Code, CONST UINT8 *Expected, UINTN Size)
{
  UINTN I;
  for (I = 0; I < Size; I++) {
    if (Code[I] != Expected[I]) {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC BOOLEAN
N6ImagePresent (UINT8 *Base)
{
  EFI_HANDLE *Handles = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Image = NULL;
  UINTN Count = 0;
  UINTN Index;
  BOOLEAN Found = FALSE;
  EFI_STATUS Status = gBS->LocateHandleBuffer (
      ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  for (Index = 0; Index < Count; Index++) {
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid,
                                        (VOID **)&Image)) &&
        Image && Image->ImageBase == Base && Image->ImageSize >= 0x5e1a0 &&
        Image->ImageCodeType == EfiBootServicesCode) {
      Found = TRUE;
      break;
    }
  }
  gBS->FreePool (Handles);
  return Found;
}

STATIC VOID
N6MirrorSurface (VOID)
{
  UINT64 *Utils = NULL;
  UINT8 *Base;
  CONST N6_DSI_SURFACE *Inner;
  CONST N6_DSI_SURFACE *Cover;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_STATUS Status;
  N6_DISPLAY_BLT Blt;
  UINT64 InnerEnd;
  UINT64 CoverEnd;
  UINTN X;
  UINTN Y;


  if (EFI_ERROR (gBS->LocateProtocol (&mN6DisplayUtils, NULL, (VOID **)&Utils)) ||
      !Utils || Utils[0] != 0x20004 || (UINTN)Utils < 0x59c68) {
    return;
  }
  Base = (UINT8 *)Utils - 0x59c68;
  if ((UINTN)Base & 0xfff || Utils[1] != (UINTN)(Base + 0x25930) ||
      Utils[2] != (UINTN)(Base + 0x25c78) || Utils[3] != (UINTN)(Base + 0x26240) ||
      !N6ImagePresent (Base)) {
    return;
  }
  if (!N6HasCode (Base + 0x3018, mN6BltCode, sizeof (mN6BltCode)) ||
      !N6HasCode (Base + 0x27ec4, mN6LogoCode, sizeof (mN6LogoCode))) {
    return;
  }
  Inner = (CONST N6_DSI_SURFACE *)(Base + 0x5e150);
  Cover = (CONST N6_DSI_SURFACE *)(Base + 0x5e178);
  InnerEnd = Inner->Base + (UINT64)Inner->Stride * Inner->Height;
  CoverEnd = Cover->Base + (UINT64)Cover->Stride * Cover->Height;
  // The live DXE allocates these two surfaces back-to-back, not on page boundaries.
  if (Base[0x5d768] == 0 || Inner->Width != 2480 || Inner->Height != 2248 ||
      Inner->Stride != 9920 || Inner->Base == 0 ||
      Cover->Width != 1140 || Cover->Height != 2616 || Cover->Stride != 4560 ||
      CoverEnd <= Cover->Base || InnerEnd <= Inner->Base ||
      Cover->Base != InnerEnd || (Cover->Base & 0x3f)) {
    return;
  }
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) ||
      !Gop || !Gop->Mode || Gop->Mode->FrameBufferBase != Inner->Base) {
    return;
  }

  Blt = (N6_DISPLAY_BLT)(Base + 0x3018);
  {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = {0, 0, 0, 0};
    UINTN MinX = Inner->Width;
    UINTN MaxX = 0;
    UINTN MinY = Inner->Height;
    UINTN MaxY = 0;
    UINTN SrcW;
    UINTN SrcH;
    UINTN DstW;
    UINTN DstH;
    UINTN Ratio;
    UINTN DstX;
    UINTN DstY;
    UINTN Left;
    UINTN Top;
    UINTN Right;
    UINTN Bottom;
    UINTN DirtyW;
    UINTN DirtyH;
    UINT32 *RowOut;
    if (!mN6Snapshot) {
      mN6Snapshot = AllocatePool (Inner->Stride * Inner->Height);
      if (!mN6Snapshot) return;
    }
    Status = Blt (Gop, 0, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mN6Snapshot,
                  EfiBltVideoToBltBuffer, 0, 0, 0, 0, Inner->Width, Inner->Height, Inner->Stride);
    if (EFI_ERROR (Status)) {
      return;
    }

    for (Y = 0; Y < Inner->Height; Y++) {
      for (X = 0; X < Inner->Width; X++) {
        if ((mN6Snapshot[Y * Inner->Width + X] & 0xffffff) != 0) {
          if (X < MinX) MinX = X;
          if (Y < MinY) MinY = Y;
          if (X > MaxX) MaxX = X;
          if (Y > MaxY) MaxY = Y;
        }
      }
    }
    if (MaxX < MinX || MaxY < MinY) {
      return;
    }
    SrcW = MaxX - MinX + 1;
    SrcH = MaxY - MinY + 1;
    Ratio = (Cover->Width - 80) * 256 / SrcW;
    if (Ratio > (Cover->Height - 100) * 256 / SrcH) {
      Ratio = (Cover->Height - 100) * 256 / SrcH;
    }
    if (Ratio > 512) Ratio = 512;
    DstW = SrcW * Ratio / 256;
    DstH = SrcH * Ratio / 256;
    if (!DstW || !DstH) return;
    DstX = (Cover->Width - DstW) / 2;
    DstY = (Cover->Height - DstH) / 4;
    Left = DstX;
    Top = DstY;
    Right = DstX + DstW;
    Bottom = DstY + DstH;
    if (mN6LastW != 0) {
      if (mN6LastX < Left) Left = mN6LastX;
      if (mN6LastY < Top) Top = mN6LastY;
      if (mN6LastX + mN6LastW > Right) Right = mN6LastX + mN6LastW;
      if (mN6LastY + mN6LastH > Bottom) Bottom = mN6LastY + mN6LastH;
    }
    DirtyW = Right - Left;
    DirtyH = Bottom - Top;
    if (DirtyW * DirtyH > mN6CanvasPixels) {
      if (mN6Canvas) FreePool (mN6Canvas);
      mN6Canvas = AllocatePool (DirtyW * DirtyH * sizeof (UINT32));
      mN6CanvasPixels = mN6Canvas ? DirtyW * DirtyH : 0;
      if (!mN6Canvas) return;
    }
    ZeroMem (mN6Canvas, DirtyW * DirtyH * sizeof (UINT32));
    for (Y = 0; Y < DstH; Y++) {
      UINTN Row = (MinY + Y * SrcH / DstH) * Inner->Width;
      RowOut = mN6Canvas + (DstY - Top + Y) * DirtyW + DstX - Left;
      for (X = 0; X < DstW; X++) {
        RowOut[X] = mN6Snapshot[Row + MinX + X * SrcW / DstW];
      }
    }
    if (mN6LastW == 0) {
      Status = Blt (Gop, 1, &Black, EfiBltVideoFill, 0, 0, 0, 0, Cover->Width, Cover->Height, 0);
    }
    if (!EFI_ERROR (Status)) {
      Status = Blt (Gop, 1, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mN6Canvas,
                    EfiBltBufferToVideo, 0, 0, Left, Top, DirtyW, DirtyH, DirtyW * 4);
    }
    if (!EFI_ERROR (Status)) {
      mN6LastX = DstX;
      mN6LastY = DstY;
      mN6LastW = DstW;
      mN6LastH = DstH;
    }
  }
}

VOID
N6MirrorConsole (VOID)
{
  N6MirrorSurface ();
}

STATIC EFI_HANDLE mN6MirrorHandle = NULL;

STATIC EFI_STATUS EFIAPI
N6MirrorFrame (IN N6_COVER_MIRROR_PROTOCOL *This)
{
  (VOID)This;
  N6MirrorConsole ();
  return EFI_SUCCESS;
}

STATIC N6_COVER_MIRROR_PROTOCOL mN6MirrorProtocol = { N6MirrorFrame };

EFI_STATUS
N6InstallCoverMirror (VOID)
{
  if (mN6MirrorHandle != NULL) {
    return EFI_SUCCESS;
  }
  return gBS->InstallProtocolInterface (&mN6MirrorHandle,
                                        &gN6CoverMirrorProtocolGuid,
                                        EFI_NATIVE_INTERFACE,
                                        &mN6MirrorProtocol);
}
