[Defines]
  PLATFORM_NAME           = FakeSecureBoot
  PLATFORM_GUID           = 6DFCAB8D-78FF-475F-BDBF-CC41BD1B48D0
  PLATFORM_VERSION        = 1.1
  DSC_SPECIFICATION       = 0x00010005
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS           = DEBUG|RELEASE
  OUTPUT_DIRECTORY        = Build/FakeSecureBoot

[LibraryClasses]
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  # no console in the debug library: this driver runs after ExitBootServices
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  StackCheckFailureHookLib|MdePkg/Library/StackCheckFailureHookLibNull/StackCheckFailureHookLibNull.inf

[BuildOptions]
  # 4 KiB section alignment, as the OS expects for runtime images
  GCC:*_*_X64_DLINK_FLAGS = -z common-page-size=0x1000

[Components]
  FakeSecureBootPkg/FakeSecureBoot.inf
