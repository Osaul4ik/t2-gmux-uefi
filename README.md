# apple_set_os loader + T2 gmux / dGPU / NVRAM

UEFI loader (Boot Services, before Windows).

## Keys during countdown

| Key | mux panel→iGPU | dGPU rail OFF | NVRAM |
|-----|----------------|---------------|-------|
| *(default)* | no | no | — |
| **Z** | — | — | skip AppleSetOs |
| **X** | yes | yes | — |
| **V** | yes | no | — |
| **C** | no | **yes** | — |
| **R** | no | no | **dGPU only** |
| **E** | no | no | **iGPU only** |

## WARNING

- After dGPU rail OFF (**X** or **C**), do **not** power Radeon back on from Windows.
- **C** (rail off without mux switch) will black the panel if firmware still routes display through dGPU — use only if you know why.
- Recovery NVRAM: press **R**, reboot.

## Install / build

1. Secure Boot = No Security  
2. Rename `/EFI/Boot/bootx64.efi` → `bootx64_original.efi`  
3. Copy built `bootx64.efi` to `/EFI/Boot/`  

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```
