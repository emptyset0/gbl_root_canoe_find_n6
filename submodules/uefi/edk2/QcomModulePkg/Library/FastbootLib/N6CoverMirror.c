/** @file
  Find N6 frame mirror and optional EFI application service.
  Keep the device-specific GOP extension in one provider so
  standalone tools need only request a complete frame mirror.
**/
#include <Uefi.h>
#include <IndustryStandard/PeImage.h>
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

// N6 DisplayDxe overloads GOP.Blt SourceX == 1 to select display 1,
// passing 0 as the actual source column. This is not standard GOP behavior.
STATIC CONST UINT32 mN6GopCoverThunk[] = {
  0xd100c3ff, 0xa9027bfd, 0x910083fd, 0x2a0203e9,
  0xf100047f, 0xa9412faa, 0xaa0703e8, 0xaa0603e7,
  0xaa0503e6, 0xaa0403e5, 0xaa0103e2, 0x1a9f17e1,
  0x9a8303e4, 0x2a0903e3, 0xa900afea, 0xf90003e8,
  0, // BL imm26: decode the target, don't match its address.
  0xa9427bfd, 0x9100c3ff, 0xd65f03c0
};
STATIC EFI_GUID mN6DisplayUtils = {
  0xc0dd69ac, 0x76ba, 0x11e6, {0xab, 0x24, 0x1f, 0xc7, 0xf5, 0x57, 0x5f, 0x19}
};
typedef struct {
  CONST UINT8 *Base;
  UINTN Size;
  CONST EFI_IMAGE_SECTION_HEADER *Sections;
  UINTN SectionCount;
} N6_IMAGE;

#define N6_COVER_SOURCE_X 1
#define N6_THUNK_CALL 16

STATIC UINT32 *mN6Snapshot;
STATIC UINT32 *mN6Canvas;
STATIC UINTN mN6CanvasPixels;
STATIC UINTN mN6LastX;
STATIC UINTN mN6LastY;
STATIC UINTN mN6LastW;
STATIC UINTN mN6LastH;


STATIC BOOLEAN
N6Fits (UINTN Offset, UINTN Length, UINTN Size)
{
  return Offset <= Size && Length <= Size - Offset;
}

STATIC BOOLEAN
N6SectionContains (CONST N6_IMAGE *Image, UINTN Offset, UINTN Length,
                   UINT32 Required, UINT32 Forbidden)
{
  UINTN I;
  if (!N6Fits (Offset, Length, Image->Size)) {
    return FALSE;
  }
  for (I = 0; I < Image->SectionCount; I++) {
    CONST EFI_IMAGE_SECTION_HEADER *S = &Image->Sections[I];
    if ((S->Characteristics & Required) == Required &&
        (S->Characteristics & Forbidden) == 0 && Offset >= S->VirtualAddress &&
        N6Fits (S->VirtualAddress, S->Misc.VirtualSize, Image->Size) &&
        N6Fits (Offset - S->VirtualAddress, Length, S->Misc.VirtualSize)) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN
N6GetImage (CONST EFI_LOADED_IMAGE_PROTOCOL *Loaded, N6_IMAGE *Image)
{
  EFI_IMAGE_DOS_HEADER Dos;
  EFI_IMAGE_NT_HEADERS64 Nt;
  UINTN SectionOffset;
  if (!Loaded->ImageBase || Loaded->ImageCodeType != EfiBootServicesCode ||
      Loaded->ImageSize < sizeof (Dos) || Loaded->ImageSize > MAX_UINT32) {
    return FALSE;
  }
  Image->Base = Loaded->ImageBase;
  Image->Size = (UINTN)Loaded->ImageSize;
  CopyMem (&Dos, Image->Base, sizeof (Dos));
  if (Dos.e_magic != EFI_IMAGE_DOS_SIGNATURE || (UINTN)Dos.e_lfanew < sizeof (Dos) ||
      !N6Fits (Dos.e_lfanew, sizeof (Nt), Image->Size)) {
    return FALSE;
  }
  CopyMem (&Nt, Image->Base + Dos.e_lfanew, sizeof (Nt));
  if (Nt.Signature != EFI_IMAGE_NT_SIGNATURE || Nt.FileHeader.Machine != EFI_IMAGE_MACHINE_AARCH64 ||
      Nt.OptionalHeader.Magic != EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      Nt.OptionalHeader.Subsystem != EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER ||
      Nt.FileHeader.SizeOfOptionalHeader < sizeof (Nt.OptionalHeader) ||
      Nt.OptionalHeader.SizeOfImage > Image->Size ||
      Nt.OptionalHeader.SizeOfHeaders > Nt.OptionalHeader.SizeOfImage) {
    return FALSE;
  }
  Image->Size = Nt.OptionalHeader.SizeOfImage;
  SectionOffset = (UINTN)Dos.e_lfanew + sizeof (Nt.Signature) + sizeof (Nt.FileHeader) +
                  Nt.FileHeader.SizeOfOptionalHeader;
  Image->SectionCount = Nt.FileHeader.NumberOfSections;
  if (SectionOffset % sizeof (UINT32) != 0 ||
      !N6Fits (SectionOffset, Image->SectionCount * sizeof (EFI_IMAGE_SECTION_HEADER),
               Nt.OptionalHeader.SizeOfHeaders)) {
    return FALSE;
  }
  Image->Sections = (CONST EFI_IMAGE_SECTION_HEADER *)(Image->Base + SectionOffset);
  return TRUE;
}

STATIC INT64
N6SignExtend (UINT32 Value, UINT32 Bits)
{
  return (INT64)Value - ((Value & (1U << (Bits - 1))) ? (1LL << Bits) : 0);
}

// Decode the ADRP at Pc and its following LDRB or ADD. These pairs identify
// the enable byte and the surface array that the actual GOP implementation uses.
STATIC INT64
N6DataRva (UINTN Pc, CONST UINT32 *Pair, UINT32 Form)
{
  UINT32 Adrp = Pair[0];
  UINT32 Imm;
  if ((Adrp & 0x9f00001f) != 0x90000008 || (Pair[1] & 0xffc003ff) != Form) {
    return -1;
  }
  Imm = ((Adrp >> 3) & 0x1ffffc) | ((Adrp >> 29) & 3);
  return (INT64)(Pc & ~(UINTN)0xfff) + N6SignExtend (Imm, 21) * 4096 +
         ((Pair[1] >> 10) & 0xfff);
}

STATIC EFI_STATUS
N6ResolveSurfaces (CONST N6_IMAGE *Image, UINTN ThunkRva, UINTN UtilsRva,
                   CONST N6_DSI_SURFACE **Inner, CONST N6_DSI_SURFACE **Cover)
{
  CONST UINT32 *Thunk;
  CONST UINT32 *Worker;
  UINTN I;
  INT64 WorkerRva;
  INT64 EnableRva;
  INT64 SurfaceRva;
  UINT32 Code = EFI_IMAGE_SCN_MEM_READ | EFI_IMAGE_SCN_MEM_EXECUTE;
  UINT32 Data = EFI_IMAGE_SCN_MEM_READ | EFI_IMAGE_SCN_MEM_WRITE;

  if ((ThunkRva & 3) || (UtilsRva & 7) ||
      !N6SectionContains (Image, ThunkRva, sizeof (mN6GopCoverThunk), Code, EFI_IMAGE_SCN_MEM_WRITE) ||
      !N6SectionContains (Image, UtilsRva, sizeof (UINT64), Data, EFI_IMAGE_SCN_MEM_EXECUTE)) {
    return EFI_UNSUPPORTED;
  }
  Thunk = (CONST UINT32 *)(Image->Base + ThunkRva);
  for (I = 0; I < sizeof (mN6GopCoverThunk) / sizeof (mN6GopCoverThunk[0]); I++) {
    if (I != N6_THUNK_CALL && Thunk[I] != mN6GopCoverThunk[I]) {
      return EFI_UNSUPPORTED;
    }
  }
  if ((Thunk[N6_THUNK_CALL] & 0xfc000000) != 0x94000000) {
    return EFI_UNSUPPORTED;
  }
  WorkerRva = (INT64)ThunkRva + N6_THUNK_CALL * 4 +
              N6SignExtend (Thunk[N6_THUNK_CALL] & 0x03ffffff, 26) * 4;
  if (WorkerRva < 0 ||
      !N6SectionContains (Image, (UINTN)WorkerRva, 36 * sizeof (UINT32), Code, EFI_IMAGE_SCN_MEM_WRITE)) {
    return EFI_UNSUPPORTED;
  }
  Worker = (CONST UINT32 *)(Image->Base + WorkerRva);
  // Check the argument and array selection, not unrelated instructions or
  // branch/relocation immediates. The worker copies surface[displayId] (40 bytes).
  STATIC_ASSERT (sizeof (N6_DSI_SURFACE) == 40, "N6 surface descriptor layout");
  if ((Worker[13] & 0xff00001f) != 0x34000008 || // CBZ W8 (enable)
      Worker[15] != 0x2a0103fa ||                  // MOV W26, W1 (DisplayId)
      Worker[26] != 0x5280051c ||                  // MOV W28, #40
      Worker[30] != 0x9bbc2349 ||                  // UMADDL X9, W26, W28, X8
      Worker[31] != 0x7100075f ||                  // CMP W26, #1
      Worker[33] != 0x52800502 ||                  // MOV W2, #40
      Worker[34] != 0x9a898101 ||                  // CSEL X1, X8, X9, HI
      (Worker[35] & 0xfc000000) != 0x94000000) {   // BL copy
    return EFI_UNSUPPORTED;
  }
  EnableRva = N6DataRva ((UINTN)WorkerRva + 11 * 4, Worker + 11, 0x39400108);
  SurfaceRva = N6DataRva ((UINTN)WorkerRva + 28 * 4, Worker + 28, 0x91000108);
  if (EnableRva < 0 || SurfaceRva < 0 || (SurfaceRva & 7) ||
      !N6SectionContains (Image, (UINTN)EnableRva, 1, Data, EFI_IMAGE_SCN_MEM_EXECUTE) ||
      !N6SectionContains (Image, (UINTN)SurfaceRva, 2 * sizeof (**Inner), Data, EFI_IMAGE_SCN_MEM_EXECUTE)) {
    return EFI_UNSUPPORTED;
  }
  if (Image->Base[EnableRva] == 0) {
    return EFI_NOT_READY;
  }
  *Inner = (CONST N6_DSI_SURFACE *)(Image->Base + SurfaceRva);
  *Cover = *Inner + 1;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
N6FindSurfaces (EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                CONST N6_DSI_SURFACE **Inner, CONST N6_DSI_SURFACE **Cover)
{
  VOID *Utils = NULL;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  UINTN I;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&mN6DisplayUtils, NULL, &Utils);
  if (EFI_ERROR (Status) || !Utils) {
    return EFI_UNSUPPORTED;
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = EFI_UNSUPPORTED;
  for (I = 0; I < Count; I++) {
    EFI_LOADED_IMAGE_PROTOCOL *Loaded = NULL;
    N6_IMAGE Image;
    UINTN Base;
    UINTN Thunk;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) ||
        !Loaded || !Loaded->ImageBase) {
      continue;
    }
    Base = (UINTN)Loaded->ImageBase;
    Thunk = (UINTN)Gop->Blt;
    // Both published interfaces must belong to the same loaded driver.
    if (Thunk < Base || (UINTN)Utils < Base || Thunk - Base >= Loaded->ImageSize ||
        (UINTN)Utils - Base >= Loaded->ImageSize || !N6GetImage (Loaded, &Image)) {
      continue;
    }
    Status = N6ResolveSurfaces (&Image, Thunk - Base, (UINTN)Utils - Base, Inner, Cover);
    break;
  }
  gBS->FreePool (Handles);
  return Status;
}

STATIC EFI_STATUS
N6ValidateSurfaces (EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, CONST N6_DSI_SURFACE *Inner,
                    CONST N6_DSI_SURFACE *Cover)
{
  UINT64 InnerEnd;
  UINT64 CoverEnd;
  if (Inner->Width != 2480 || Inner->Height != 2248 || Inner->Stride != 9920 ||
      Cover->Width != 1140 || Cover->Height != 2616 || Cover->Stride != 4560 ||
      Inner->Base == 0 || Gop->Mode->FrameBufferBase != Inner->Base ||
      Cover->Base == 0 || (Cover->Base & 0x3f)) {
    return EFI_UNSUPPORTED;
  }
  InnerEnd = Inner->Base + (UINT64)Inner->Stride * Inner->Height;
  CoverEnd = Cover->Base + (UINT64)Cover->Stride * Cover->Height;
  if (InnerEnd <= Inner->Base || CoverEnd <= Cover->Base ||
      (InnerEnd > Cover->Base && CoverEnd > Inner->Base)) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
N6DrawSurface (EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, CONST N6_DSI_SURFACE *Inner,
               CONST N6_DSI_SURFACE *Cover)
{
  EFI_STATUS Status;
  UINTN X;
  UINTN Y;
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
    if (!mN6Snapshot) return EFI_OUT_OF_RESOURCES;
  }
  Status = Gop->Blt (Gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mN6Snapshot,
                    EfiBltVideoToBltBuffer, 0, 0, 0, 0, Inner->Width, Inner->Height, Inner->Stride);
  if (EFI_ERROR (Status)) {
    return Status;
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
    return EFI_NOT_READY;
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
  if (!DstW || !DstH) return EFI_NOT_READY;
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
    if (!mN6Canvas) return EFI_OUT_OF_RESOURCES;
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
    Status = Gop->Blt (Gop, &Black, EfiBltVideoFill, N6_COVER_SOURCE_X, 0,
                      0, 0, Cover->Width, Cover->Height, 0);
  }
  if (!EFI_ERROR (Status)) {
    Status = Gop->Blt (Gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mN6Canvas,
                      EfiBltBufferToVideo, N6_COVER_SOURCE_X, 0, Left, Top, DirtyW, DirtyH, DirtyW * 4);
  }
  if (!EFI_ERROR (Status)) {
    mN6LastX = DstX;
    mN6LastY = DstY;
    mN6LastW = DstW;
    mN6LastH = DstH;
  }
  return Status;
}

STATIC EFI_STATUS
N6MirrorSurface (VOID)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  CONST N6_DSI_SURFACE *Inner;
  CONST N6_DSI_SURFACE *Cover;
  EFI_STATUS Status;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (!Gop || !Gop->Blt || !Gop->Mode) {
    return EFI_NOT_READY;
  }
  Status = N6FindSurfaces (Gop, &Inner, &Cover);
  if (!EFI_ERROR (Status)) {
    Status = N6ValidateSurfaces (Gop, Inner, Cover);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return N6DrawSurface (Gop, Inner, Cover);
}

VOID
N6MirrorConsole (VOID)
{
  (VOID)N6MirrorSurface ();
}

STATIC EFI_HANDLE mN6MirrorHandle = NULL;

STATIC EFI_STATUS EFIAPI
N6MirrorFrame (IN N6_COVER_MIRROR_PROTOCOL *This)
{
  (VOID)This;
  return N6MirrorSurface ();
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
