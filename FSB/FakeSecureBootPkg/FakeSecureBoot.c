/*
 * FakeSecureBoot (runtime build).
 *
 * Based on https://github.com/Shmurkio/FakeSecureBoot: hooks gRT->GetVariable and answers
 * "SecureBoot" (EFI global variable) with 1.
 *
 * Differences to the upstream driver:
 *  - MODULE_TYPE = DXE_RUNTIME_DRIVER: the image is loaded into EfiRuntimeServicesCode/Data, so the hook
 *    stays valid after ExitBootServices (the upstream UEFI_DRIVER build lives in boot-services memory,
 *    which Windows reclaims -> BSOD at the first runtime GetVariable call).
 *  - gOrigGetVariable points outside this image (into the firmware), so it is converted in the
 *    SetVirtualAddressMap notification. The image itself (code, strings, GUID) is relocated by the firmware.
 *  - the hook checks Data / *DataSize, fills Attributes, and gRT's header CRC32 is recalculated.
 *  - nothing in the hook touches boot services, the console or any other not converted pointer.
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/GlobalVariable.h>

STATIC EFI_GET_VARIABLE  mOrigGetVariable = NULL;
STATIC EFI_EVENT         mVirtualAddressChangeEvent = NULL;

STATIC
EFI_STATUS
EFIAPI
GetVariableHook (
  IN     CHAR16    *VariableName,
  IN     EFI_GUID  *VendorGuid,
  OUT    UINT32    *Attributes OPTIONAL,
  IN OUT UINTN     *DataSize,
  OUT    VOID      *Data OPTIONAL
  )
{
  //
  // Always report Secure Boot enabled when the SecureBoot variable is read
  //
  if ((VariableName != NULL) && (VendorGuid != NULL) &&
      (StrCmp (VariableName, L"SecureBoot") == 0) &&
      CompareGuid (VendorGuid, &gEfiGlobalVariableGuid))
  {
    if (DataSize == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    if (Attributes != NULL) {
      *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    }

    // size probe or too small buffer: report the size like a real variable
    if (*DataSize < sizeof (UINT8)) {
      *DataSize = sizeof (UINT8);
      return EFI_BUFFER_TOO_SMALL;
    }

    if (Data == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    *((UINT8 *)Data) = 1;       // 1 = enabled
    *DataSize        = sizeof (UINT8);
    return EFI_SUCCESS;
  }

  //
  // Every other variable: the real GetVariable
  //
  return mOrigGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
}

//
// SetVirtualAddressMap: the pointer to the firmware's own GetVariable must become a virtual address,
// otherwise the pass-through above jumps to a physical address once the OS has switched mappings.
//
STATIC
VOID
EFIAPI
VirtualAddressChangeNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gRT->ConvertPointer (0, (VOID **)&mOrigGetVariable);
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  //
  // The event first: without it the hook must not be installed
  //
  Status = gBS->CreateEvent (
                  EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE,
                  TPL_NOTIFY,
                  VirtualAddressChangeNotify,
                  NULL,
                  &mVirtualAddressChangeEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mOrigGetVariable = gRT->GetVariable;
  gRT->GetVariable = GetVariableHook;

  gRT->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 ((VOID *)&gRT->Hdr, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);

  //
  // EFI_SUCCESS keeps the (runtime) image resident
  //
  return EFI_SUCCESS;
}