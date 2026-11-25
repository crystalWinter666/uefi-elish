# SPDX-License-Identifier: BSD-2-Clause-Patent

[Defines]
  PLATFORM_NAME           = elish
  PLATFORM_GUID           = E4983C12-98D8-47B8-BB09-2D52F04BF89D
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/elishPkg
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS           = RELEASE|DEBUG
  SKUID_IDENTIFIER        = DEFAULT
  FLASH_DEFINITION        = elishPkg/elish.fdf
  USE_CUSTOM_DISPLAY_DRIVER = 0
  SOC_TYPE                = 2
  DEFINE ELISH_RAMOOPS_LOG = FALSE
  DEFINE ELISH_LOADER_PROBE = FALSE
  DEFINE ELISH_LOADER_PROBE_RUN_KERNEL = FALSE
  DEFINE ELISH_LOADER_PROBE_SMMU = FALSE
  DEFINE ELISH_LOADER_PROBE_PMSG = FALSE

!include KonaPkg/KonaPkg.dsc.inc

[PcdsFixedAtBuild]
  # Live FDT DRAM base, and SEC stack from the installed elish XBL.
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Xiaomi Pad 5 Pro Wi-Fi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"elish"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"M2105K81AC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"K81"
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel|"elish"

  # SimpleFbDxe uses the active panel dimensions and assumes a packed 32-bit
  # scanout. ABL's framebuffer format and pitch require runtime verification.
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1600
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2560
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"
  # EnvDxe requires the Info Blk HOB from the elish XBL memory map.
  gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|TRUE
!if $(TARGET) == DEBUG && $(ELISH_RAMOOPS_LOG) == TRUE
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|0
!elseif $(TARGET) == DEBUG
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000000
!endif

[LibraryClasses]
  MemoryMapLib|elishPkg/Library/MemoryMapLib/MemoryMapLib.inf
  ConfigurationMapLib|elishPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  AdvancedLoggerAccessLib|AdvLoggerPkg/Library/AdvancedLoggerAccessLib/AdvancedLoggerAccessLib.inf
!if $(TARGET) == DEBUG && $(ELISH_RAMOOPS_LOG) == TRUE
  RamLogLib|elishPkg/Library/RamoopsLogLib/RamoopsLogLib.inf
  AdvancedLoggerHdwPortLib|AdvLoggerPkg/Library/AdvancedLoggerHdwPortLib/AdvancedLoggerHdwPortLib.inf
!else
  AdvancedLoggerHdwPortLib|AdvLoggerPkg/Library/AdvancedLoggerHdwPortLibNull/AdvancedLoggerHdwPortLibNull.inf
!endif
  AssertLib|AdvLoggerPkg/Library/AssertLib/AssertLib.inf
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf

[LibraryClasses.common.DXE_CORE]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/DxeCore/AdvancedLoggerLib.inf
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf

[LibraryClasses.common.DXE_DRIVER]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Dxe/AdvancedLoggerLib.inf
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf

[LibraryClasses.common.UEFI_APPLICATION]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Dxe/AdvancedLoggerLib.inf
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf

[Components]
  Binaries/pipa/QcomPkg/Drivers/EnvDxe/EnvDxe.inf
  Binaries/pipa/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  SiliciumPkg/Drivers/RamManagerDxe/RamManagerDxe.inf
!if $(ELISH_MEMORY_MAP_PROBE) == TRUE
!if $(TARGET) == DEBUG && $(ELISH_RAMOOPS_LOG) == TRUE
  elishPkg/Drivers/ElishMemoryMapProbeDxe/ElishMemoryMapProbeDxe.inf {
    <BuildOptions>
      *_*_*_CC_FLAGS = -DELISH_MAP_CAPTURE_TO_RAMOOPS=1
  }
!else
  elishPkg/Drivers/ElishMemoryMapProbeDxe/ElishMemoryMapProbeDxe.inf
!endif
!endif
  UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
!if $(TARGET) == DEBUG && $(ELISH_RAMOOPS_LOG) == TRUE && $(ELISH_LOADER_PROBE) == TRUE
  elishPkg/Drivers/ElishLoaderProbeDxe/ElishLoaderProbeDxe.inf {
!if $(ELISH_LOADER_PROBE_SMMU) == TRUE
    <BuildOptions>
      *_*_*_CC_FLAGS = -DELISH_LOADER_PROBE_SMMU=1
!elseif $(ELISH_LOADER_PROBE_RUN_KERNEL) == TRUE
    <BuildOptions>
      *_*_*_CC_FLAGS = -DELISH_LOADER_PROBE_RUN_KERNEL=1
!elseif $(ELISH_LOADER_PROBE_PMSG) == TRUE
    <BuildOptions>
      *_*_*_CC_FLAGS = -DELISH_LOADER_PROBE_PMSG=1
!endif
  }
!endif
