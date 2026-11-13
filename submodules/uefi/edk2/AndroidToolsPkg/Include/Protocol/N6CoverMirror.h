/** @file
  Optional boot-services frame mirror for the Find N6 cover display.

  The provider lives in the boot menu image. A launched EFI application may
  request a copy after it has finished drawing a complete console frame.
  Absence of this protocol means that the application keeps its normal display.
**/
#ifndef N6_COVER_MIRROR_PROTOCOL_H_
#define N6_COVER_MIRROR_PROTOCOL_H_

#include <Uefi.h>

#define N6_COVER_MIRROR_PROTOCOL_GUID \
  { 0x31a7c769, 0x68df, 0x4cf1, { 0xb4, 0xa0, 0x65, 0x4c, 0x29, 0xb8, 0x47, 0x13 } }

typedef struct _N6_COVER_MIRROR_PROTOCOL N6_COVER_MIRROR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *N6_COVER_MIRROR_FRAME)(
  IN N6_COVER_MIRROR_PROTOCOL *This
  );

struct _N6_COVER_MIRROR_PROTOCOL {
  N6_COVER_MIRROR_FRAME MirrorFrame;
};

extern EFI_GUID gN6CoverMirrorProtocolGuid;

#endif
