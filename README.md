# apple_set_os loader + T2 gmux iGPU + dGPU power off

UEFI loader that:
1. Calls AppleSetOs (unlock iGPU for non-macOS)
2. Switches gmux panel route to iGPU (`0x10`)
3. Powers **OFF** the dGPU rail via gmux port `0x50` (same sequence as Linux)
4. Writes NVRAM `gpu-power-prefs=01 00 00 00` (firmware boot GPU = iGPU)

Runs in Boot Services before Windows. Based on 0xbb / aa15032261 apple_set_os loader.

## WARNING

- After dGPU rail OFF from this loader, **do not** power the Radeon back on from Windows
  (`gmux power on` / enable after hard power-cut). That path hung MacBookPro16,1 (CATERR).
- Recovery = boot with **D** (writes `gpu-power-prefs=dGPU`) or NVRAM reset, then normal dGPU boot.
- First test with RDP/SSH available.

## Keys during countdown

| Key | Action |
|-----|--------|
| **Z** | Skip AppleSetOs |
| **X** | Skip gmux + dGPU power off + NVRAM |
| **R** | Panel -> iGPU only (Radeon stays powered, no NVRAM) |
| **D** | **Recovery**: write `gpu-power-prefs=dGPU`, skip gmux this boot |
| other | Continue with full path (default) |

## Default path (no key)

1. AppleSetOs
2. gmux panel -> iGPU
3. gmux discrete power -> OFF
4. NVRAM gpu-power-prefs -> iGPU
5. chainload `bootx64_original.efi`

## Install

1. Secure Boot = No Security
2. Mount EFI
3. Rename `/EFI/Boot/bootx64.efi` -> `bootx64_original.efi`
4. Copy built `bootx64.efi` to `/EFI/Boot/`

## Build

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

GitHub Actions builds artifact on push (see `.github/workflows/build.yml`).
