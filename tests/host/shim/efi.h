// Host-test shim: just enough of gnu-efi for lib/int_vbtpatch.c and lib/int_edp.c
#ifndef shim_efi_h
#define shim_efi_h
#include <stdint.h>
#include <stddef.h>
typedef int32_t INT32; typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef uintptr_t UINTN; typedef uint8_t CHAR8; typedef uint16_t CHAR16; typedef int BOOLEAN;
typedef void VOID; typedef UINTN EFI_STATUS; typedef void* EFI_HANDLE;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_ERROR(s) ((s) != 0)
#define EFI_NOT_FOUND 14
#define EFI_NOT_READY 6
#define EFI_TIMEOUT 18
#define EFI_DEVICE_ERROR 7
#define EFI_COMPROMISED_DATA 33
typedef struct EFI_BOOT_SERVICES { EFI_STATUS (*Stall)(UINTN us); } EFI_BOOT_SERVICES;
#endif