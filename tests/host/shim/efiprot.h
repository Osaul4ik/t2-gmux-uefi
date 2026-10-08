#pragma once
#include <efi.h>
typedef enum { EfiPciIoWidthUint8, EfiPciIoWidthUint16, EfiPciIoWidthUint32 } EFI_PCI_IO_PROTOCOL_WIDTH;
typedef enum { EfiPciIoAttributeOperationEnable } EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION;
#define EFI_PCI_IO_ATTRIBUTE_MEMORY 2
typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
struct EFI_PCI_IO_PROTOCOL {
    struct { EFI_STATUS (*Read)(EFI_PCI_IO_PROTOCOL*, int, UINT32, UINTN, void*); } Pci;
    struct { EFI_STATUS (*Read)(EFI_PCI_IO_PROTOCOL*, int, UINT8, UINT64, UINTN, void*);
             EFI_STATUS (*Write)(EFI_PCI_IO_PROTOCOL*, int, UINT8, UINT64, UINTN, void*); } Mem;
    EFI_STATUS (*Attributes)(EFI_PCI_IO_PROTOCOL*, int, UINT64, UINT64*);
};
