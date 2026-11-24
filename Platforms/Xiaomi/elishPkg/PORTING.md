# elish UEFI platform

The target is Xiaomi Pad 5 Pro Wi-Fi (`elish`, M2105K81AC, SM8250-AC). The
initial elish DSC/FDF, device configuration, XBL memory map and numeric
configuration map are implemented. The UEFI source checkout is at
`66421cb7bb5694843aec8de1ca3d1de4c20cca38`, and its Device-Binaries submodule
is at `a1501e0c2e642d6ac0d8f52c85af2a67b75a7799`. A host FD build is recorded
below. A DXE screen capture showed that both FV HOBs received FVB protocols,
but `FwVol` could not read either header because `EFI_FVB2_READ_STATUS` was
clear. The FDF now enables read capability and read status for both the inner
and compact firmware volumes. A corrected RAM-only image was accepted by
fastboot and the device returned to its installed LineageOS system. The
memory-map probe is compiled into a separate diagnostic image with synchronous
entry, DXE APRIORI ordering, direct console output and a 600-second hold. The
later user screenshots identify this elish Silicium firmware and show the
standard DXE Core HOB allocation dump. They do not show the custom probe marker,
RAM Partition output, or the UEFI `GetMemoryMap()` descriptor array. The screen
evidence and cross-check are recorded in the device repository's
`reports/elish-live-uefi-hob-observation-20261124.md`.

## Firmware and memory evidence

The supplied `xbl.img` has SHA-256
`9a3a3bb92e6cf478b227f70dd1bd140fde32982994a6c34a07bd314ea0acb72f`. A
read-only FV parse found an embedded `uefiplat.cfg`. Its version-3 `MemoryMap`
section declares up to 74 regions and an unusable prefix at
`0x80000000+0x600000`. The listed DDR entries include the following.

The connected device reports active slot `_b`. Read-only full-partition
hashes match between `xbl_a` and `xbl_b`
(`1f83e3e89a7b119702802bb78a113a53e02e81b76639b88839ab0007c3e01c28`) and
between `xbl_config_a` and `xbl_config_b`
(`3d470f4d5584dd7b3ced8d2b9b639e5d5bc10deca648a9d8c96c2bc14cdb87fa`). The
first 3,579,904 bytes of `xbl_b` and first 102,400 bytes of `xbl_config_b`
match the supplied ROM's `xbl.img` and `xbl_config.img` exactly. This ties the
ROM evidence to the device's installed boot firmware.

| Range | Label | BuildHob / resource type / memory type |
| --- | --- | --- |
| `0x805d0000+0x20000` | IPC SHM | AddMem / SYS_MEM / BsData |
| `0x80600000+0x100000` | MPSS EFS | AddMem / SYS_MEM / Reserv |
| `0x80700000+0x100000` | BOOT INFO | AddMem / SYS_MEM / BsData |
| `0x80860000+0x20000` | AOP CMD DB | AddMem / MEM_RES / Reserv |
| `0x80884000+0x10000` | XBL Log Buffer | AddMem / SYS_MEM / Reserv |
| `0x80900000+0x200000` | SMEM | AddMem / MEM_RES / Reserv |
| `0x82400000+0x3a00000` | TZApps Reserved | HobOnlyNoCacheSetting / MEM_RES / Reserv |
| `0x86000000+0xd200000` | PIL Reserved | AddMem / MEM_RES / Reserv |
| `0x98900000+0x3300000` | DXE Heap | AddMem / SYS_MEM / Conv |
| `0x9bc00000+0x400000` | Sched Heap | AddMem / SYS_MEM / BsData |
| `0x9c000000+0x2400000` | Display Reserved | AddMem / MEM_RES / Reserv |
| `0x9e400000+0xf00000` | DBI Dump | NoHob / MMAP_IO / Conv |
| `0x9f800000+0x200000` | FV Region | AddMem / SYS_MEM / BsData |
| `0x9fa00000+0x200000` | ABOOT FV | AddMem / SYS_MEM / Reserv |
| `0x9fc00000+0x300000` | UEFI FD | AddMem / SYS_MEM / BsData |
| `0x9ff00000+0x8c000` | SEC Heap | AddMem / SYS_MEM / BsData |
| `0x9ff8c000+0x1000` | CPU Vectors | AddMem / SYS_MEM / BsData |
| `0x9ff8d000+0x3000` | MMU PageTables | AddMem / SYS_MEM / BsData |
| `0x9ff90000+0x40000` | UEFI Stack | AddMem / SYS_MEM / BsData |
| `0x9fff7000+0x8000` | Log Buffer | AddMem / SYS_MEM / RtData |
| `0x9ffff000+0x1000` | Info Blk | AddMem / SYS_MEM / RtData |
| `0xa0000000+0x10000000` | Kernel | AddMem / SYS_MEM / Reserv |

The embedded `MemoryMap` text ends with the `0xa0000000+0x10000000` Kernel
reservation and contains no descriptor for the second DRAM bank. This is the
embedded configuration's declared map, not a captured UEFI `GetMemoryMap`.

Ghidra 12.2 analysis of the supplied XBL found that `FUN_9fc107e0` requests
`uefiplat.cfg` through `FUN_9fc12890` and parses its `Config`, `MemoryMap`,
`RegisterMap`, `ConfigParameters` and `ChipIDConfig` sections. The
`ConfigParameters` handler stores key/value pairs and the embedded configuration
sets `PlatConfigFileName` to `uefiplatLA.cfg` and `OsTypeString` to `LA`. The
analysis did not identify the routine that opens that named file. Its contents
and source remain unavailable in the supplied ROM images and checked-out UEFI
repository. Command details and hashes are recorded in
`reports/elish-xbl-config-lookup-20261124.md` in the device repository.

The installed `xbl_config_b` partition is 8,343,552 bytes and its SHA-256
matches the complete supplied `xbl_config.img`. The installed `xbl_b`
prefix containing the active firmware volume and configuration matches the
supplied `xbl.img`. The selected `uefiplatLA.cfg` is not present in either
image. The platform uses the elish-specific XBL DDR and register maps and does
not claim to reproduce that unavailable configuration file.

The initial elish memory map transcribes the XBL entries and leaves the
declared 6 MiB unusable prefix outside the memory descriptors. The framebuffer
reservation uses the full 36 MiB from the installed XBL config, compared with
35 MiB in the live Android FDT. The DBI Dump retains its XBL
`NoHob/MMAP_IO` attributes despite the later Linux System RAM view.

The stock XBL `EnvDxe` and `SmemDxe` PE32 sections are byte-identical to
the matching catalog binaries under `Binaries/pipa`. The platform includes
those modules and `RamManagerDxe` so usable ranges come from the Qualcomm RAM
Partition protocol. The photographed DXE Core HOB dump confirms that the
firmware built allocation HOBs and entered DXE. The returned RAM Partition
entries, full UEFI `GetMemoryMap()` array, and final map at `ExitBootServices`
remain to be observed on elish.

The board's 256 MiB DarwinBoot window is intended to land in the second
measured DRAM bank at `0xc0000000+0x1c0000000`. Each unreserved contiguous run
in the first bank described by the embedded XBL map is smaller than 256 MiB,
including the 187 MiB range after the XBL Kernel reservation. This lets the
iPadOS ADT describe that second bank as a contiguous range while leaving the
first bank outside the kernel-managed window. Confirm DarwinBoot's physical
base against the live UEFI map during bring-up.

Root ADB exported the live FDT with SHA-256
`90cf1d5333e758f001a8e21ed4e78fc000ebf3b5502338f010435a2da292a663`. It
reports DRAM banks `0x80000000+0x3bb00000` and
`0xc0000000+0x1c0000000`, with a `0x4500000` gap. The FDT has 22 fixed and
12 dynamic reserved-memory children. The Linux `/proc/iomem` capture does not
establish the UEFI memory descriptors or the RAM partition protocol's ranges.

The live FDT's display reservation is `0x9c000000+0x2300000`, which is
`0x100000` smaller than the embedded configuration's display reservation.
The embedded PIL range is `0x86000000+0xd200000`, while the live FDT lists PIL
and secure ranges from `0x86200000` through `0x92700000`. The embedded DBI
range is `0x9e400000+0xf00000` with `NoHob / MMAP_IO`, while the live Linux
`/proc/iomem` reports that address interval as System RAM. These differences
show that the embedded section cannot stand in for the selected UEFI map. The
stock XBL FD and stack addresses are known, while their owners after
`ExitBootServices` still need to be traced through the UEFI memory map and
DarwinBoot handoff.

## Driver inputs

The XBL FV's `SmemDxe` PE32 section has SHA-256
`4ad21b61ab64d1d91403403c8ce23b60648a2fb29c781fe4a7cb5a3996d727aa`. It is
byte-identical to the catalog binaries for alioth and pipa. This identifies a
same-SoC candidate.

The XBL `EnvDxe` PE32 section has SHA-256
`62e81b81dc507a5ecd2e9e2d732c3e642c7c107bcdd51a236bbc029800b645ed` and FFS
GUID `90a49afd-422f-08ae-9611-e788d3804845`. It is byte-identical to the
alioth and pipa `EnvDxe` catalog binaries. The XBL section contains the RAM
Partition protocol GUID and strings naming `RamPartitionTableLib.c` and an
SMEM memory-partition-table read failure. This identifies `EnvDxe` as the
protocol-provider candidate. Android does not expose the table through the
available debugfs or `/dev/mem` interfaces. Root ADB confirmed the Linux
`qcom-smem` driver is bound and its device-tree `memory-region` resolves to
no-map SMEM at `0x80900000+0x200000`. The driver exposes no SMEM nvmem device or
`/dev` node. Protocol installation, execution order, and the returned elish
ranges still need runtime confirmation.

The FV includes panel XML `Panel_K81_42_02_0a_dualdsi_cphy_lcd_vid.xml` under
FFS GUID `2962bfba-d7e7-4f37-a4c6-e28f7b5a685a`, matching the live
`42_02_0a` panel identification. Candidate `DisplayDxe` PE32 has SHA-256
`6c6351c3c770d4f698e5e22097021fe8d20d35a8d4181df05676a48c4347f425` and
candidate `UFSDxe` PE32 has SHA-256
`ffeaed467f8251b7e30a45075da84a1c563e2bac7dc4be0f79f0ed49a88b8dc9`.
The DisplayDxe DEPEX provider mapping remains unresolved. Catalog analysis
traces the UFSDxe dependency GUIDs to SmemDxe and SPMIDxe providers. The elish
stock SmemDxe and SPMI PE32 hashes match their catalog copies. Stock NpaDxe
also matches the catalog, and its NPA protocol installation matches the GUID
queried by UFSDxe. Runtime integration and the remaining UFS resources remain
unverified. No proprietary module has been copied into the elish firmware
cache.

A raw-byte search across the 28 supplied stock `.img` files did not locate
`uefiplatLA.cfg` contents. Among those ROM images, the filename occurs as the
reference in the XBL `uefiplat.cfg`. An exact filename search under
`/root/repo` and `/root/.cache` found no matching file, and the checked-out
UEFI Git history has no such path.

## Build and verification

On 2026-11-24, the local package built with Mu-Silicium revision
`66421cb7bb5694843aec8de1ca3d1de4c20cca38` and Mu_Basecore revision
`cfb93724a06ff86ff5ba971413368a7809ee42a5`. The 3 MiB FD is
`Build/elishPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`, SHA-256
`f8f662313c0ffee611a2e55f687486b1625c9e80a983d9280f26f46f5dbdbf74`.
The EDK build compiled UniXNU, UniXNUDxe, DarwinBoot, UniXNURamDiskDxe,
DiskIoDxe and FAT. It emitted a warning that `ArmPsciResetSystemLib.inf` does
not support `HwResetSystemLib`. The complete command and output record is in
`/root/repo/UniXNU-elish/reports/elish-uefi-build-20261124.md`.

The core boot-image builder also produced a firmware-only Android v3 probe
with an empty ramdisk. It is 618,496 bytes with SHA-256
`8b08c449b4af3a4ab13c07389a789c170a83d6c373b68dfe76dc959224fef349` and
passed the builder's unpack-and-compare check. It has not been booted and is
not an iPadOS image.

The diagnostic build enables `ElishMemoryMapProbeDxe` through
`ELISH_MEMORY_MAP_PROBE=TRUE`. At `ReadyToBoot`, it prints the Qualcomm RAM
Partition protocol entries and UEFI memory descriptors overlapping the two
measured DRAM banks through the framebuffer debug logger. It holds that screen
for 30 seconds in the first diagnostic image. This snapshot precedes UniXNU startup and is not the later
`ExitBootServices` map. The second DRAM bank endpoint is `0x280000000`, as
measured in the live FDT. After correcting an earlier endpoint calculation,
the FD remains 3 MiB with SHA-256
`8f797816d1b3ac59338c248a7ed17442d4f3878e5abc2fae80314811d6967548`. The
firmware-only Android v3 image is 622,592 bytes with SHA-256
`b2ccb5968d7cb30a462bf0f1c6deca84167ccb10c7f7a36782e07559c0fbf6d6`. The
round-trip boot-image checks pass. The image was sent using `fastboot boot`,
which returned `Sending` and `Booting` `OKAY`; the tablet then disappeared from
ADB, fastboot and USB enumeration. No framebuffer output was captured, so
callback execution and the displayed values remain unverified. No partition
write was run.

The display hold was extended to 120 seconds and the firmware rebuilt. The new
3 MiB FD SHA-256 is
`063614fdc3de0d2e7ab7a26bb7856c684fa4d4c45e95dfe83e8065fe31d1b2b1`. Its
622,592-byte Android v3 image SHA-256 is
`f312ddfa2adacd2a48ef3d09e313765b668b1affe7e710426b3e3411fcd1e650`. The
extended image has not yet been booted. Hashes from the earlier diagnostic
build are superseded only by the stated correction of its bank endpoint.

The host build establishes source and packaging consistency. Capture the
Qualcomm RAM Partition entries and final UEFI `GetMemoryMap` on elish before
validating the XNU window. The Android root shell lacks debugfs, `/dev/mem`,
and an exported UEFI memory map, so current RAM ownership remains unresolved.
Record hardware results and any revised image hashes in the device repository's
porting record.

## DXE volume handoff diagnosis and correction

The screen capture `/root/repo/946BF84D23D5AC424DD564A47EFE82EA.png` showed
two FV HOBs and successful FVB protocol installation. The `FwVol`
notification processed both handles, then `GetFwVolHeader` returned
`EFI_ACCESS_DENIED` for each. The DXE dispatcher returned without loading
drivers, and the architecture-service check asserted with `EFI_NOT_FOUND`.

The pinned Mu_Basecore `FwVolBlockReadBlock()` returns `EFI_ACCESS_DENIED`
when `EFI_FVB2_READ_STATUS` is clear. The generated outer and inner FV
headers had attributes `0x000C0C00` and `0x00030C00`. Both lacked the required
read-status bit. The elish FDF now sets `READ_ENABLED_CAP = TRUE` and
`READ_STATUS = TRUE` in `[FV.FVMAIN_COMPACT]` and `[FV.FvMain]`. The rebuilt
headers have attributes `0x000C0C06` and `0x00030C06`.

After the FDF correction, the RAM-only image
`elish-dxe-fv-readable.img` was accepted by `fastboot boot`. The user reported
that the device entered its system. ADB later identified
`product:lineage_elish`, model `M2105K81AC`, and device `elish`. That image
had an empty ramdisk and contained no iPadOS payload. No partition write was
run.

The first corrected-FV memory-map probe used `DEBUG_INFO`, while the elish
DEBUG firmware mask is `0x8007EE0F` and excludes `DEBUG_INFO` (`0x40`). Later
diagnostic builds run synchronously from the DXE entry point, enter the FDF DXE
APRIORI list after console, framebuffer and RAM manager drivers, print with
`UefiLib` `Print()` and `DEBUG_ERROR`, disable the UEFI watchdog and request a
600-second stall. The generated APRIORI data includes the probe FFS GUID. The
latest Android v3 image is
`/root/elish-build-20261123/UniXNU/out/elish/ipados-17.7.11-21H461/elish/boot/diagnostics/elish-dxe-memory-map-probe-apriori.img`,
SHA-256
`adacbcf850e5d49f6d2fb0671215a83e64f21e497f5cd70b0b7cc42bb7df6b89`.
Fastboot accepted it and the device returned to LineageOS. No probe text or
memory values were captured, so entry-point execution and the screen hold are
unverified. The routine `ELISH_MEMORY_MAP_PROBE` board setting remains
`FALSE`.

The clean routine FD is 3,145,728 bytes with SHA-256
`809131c7ffc4273e8e5e25deaa423499172113ba135366c06c144c50c7b9247f`. The
DXE cause and image hashes are also recorded in
`reports/elish-dxe-fv-read-status-20261124.md` in the device repository.

## UFS DXE dependency analysis

The `UFSDxe` PE32 extracted from the elish stock XBL is 118,784 bytes and has
SHA-256 `ffeaed467f8251b7e30a45075da84a1c563e2bac7dc4be0f79f0ed49a88b8dc9`.
It is byte-identical to the alioth and pipa catalog `UFSDxe.efi` files. The
catalog copies have the same DEPEX hash
`d24ba46b9b195a4affe0c6d783f99361fb08fa85894d0e9ec3a070a995ef5d44` and
require protocol GUIDs `f4e5c7d0-d239-47cb-aacd-7f66ef763238` and
`fa5f306b-f47d-4ac4-a47d-882f8204ec30`.

Ghidra headless analysis showed the alioth catalog `SmemDxe` installs the
first protocol GUID with `EFI_BOOT_SERVICES.InstallMultipleProtocolInterfaces`.
The elish stock `SmemDxe` PE32 hash matches the catalog module at
`4ad21b61ab64d1d91403403c8ce23b60648a2fb29c781fe4a7cb5a3996d727aa`.
The catalog `SPMIDxe` installs the second GUID with the same Boot Services
function. Its alioth and pipa PE32 hashes match at
`b062b30ffe231da3333c1a4a7ba57896f3e59e0d6d5478e8e89c259b8f63766f`.
`DALSYSDxe` also declares a DEPEX on the first GUID. The elish stock `SPMI` PE32 hash matches the alioth and pipa catalog
`SPMIDxe` files.

The UFS PE32 contains strings for dynamic `NpaProtocol` lookup, IOMMU domains
and ARIDs `0x0` and `0x10`, `UfsSmmuConfigForOtherBootDev`, `EnableUfsIOC`,
and nine GCC UFS/PHY clocks. Static analysis confirms the stock NpaDxe
provider, while elish values and runtime ownership for clocks, SMMU, reset,
RPMh, TCSR and QMP PHY remain unresolved. No proprietary module has been
added to the elish FD.

Reproduction details and the Ghidra project/script paths are recorded in
`reports/elish-offline-audit-20261124.md` in the device repository.

The Ghidra call sites resolve through `gBS`, which each module initializes
from `EFI_SYSTEM_TABLE.BootServices` at `gST + 0x60`. The `gBS + 0x148` slot
is `InstallMultipleProtocolInterfaces`, and `gBS + 0x140` is `LocateProtocol`
per the pinned Mu_Basecore `MdePkg/Include/Uefi/UefiSpec.h` table. This confirms
the protocol installation observed in both catalog provider modules.

The elish XBL's embedded gzip stream at offset `0xca830` decompresses to an FV
with 147 FFS files. Its `SPMI`, `NpaDxe`, `UFSDxe`, `ClockDxe`, `CmdDbDxe`,
`PwrUtilsDxe`, `RpmhDxe`, `HWIODxeDriver`, and `VcsDxe` PE32 hashes match the
alioth/pipa catalog copies. The extracted stock `DALSys` PE32 is a distinct
elish variant. The elish `UFSDxe` DEPEX is byte-identical to the catalog file
and requires the SMEM and SPMI protocol GUIDs. The stock `SmemDxe` and `SPMI`
modules provide those GUIDs. `DALSys` also waits for the SMEM GUID. `NpaDxe`,
`SPMI` and `SmemDxe` have unconditional DEPEX expressions.

Ghidra analysis of the extracted stock modules resolved the dynamic NPA
lookup. `UFSDxe` calls `LocateProtocol` with GUID
`79d6c870-725e-489e-a0a1-27e7a5d0cb35`. `NpaDxe` installs that same protocol
using `InstallMultipleProtocolInterfaces`. The complete module table, hashes,
Ghidra findings and extraction command are recorded in
`reports/elish-offline-audit-20261124.md` in the device repository.

The XBL `uefiplat.cfg` UFS section at file offset `0xca06d` sets
`EnableUfsIOC = 0` and `UfsSmmuConfigForOtherBootDev = 1`. The elish
`ConfigurationMapLib` uses the same values. The active UFS stream IDs, SMMU
context ownership, clock state and reset/PHY initialization remain unknown.

## 2026-11-24 memory-map probe retest

The diagnostic build compiled `ElishMemoryMapProbeDxe` into `FVMAIN`. A source
change added an unconditional 120-second `gBS->Stall()` after its display
routine returns. The rebuilt FD SHA-256 is
`98dc9b6ab41af1f80db8159c087d8aead4b3a315807f4b01f4bca6d714128699`; the
verified 634,880-byte firmware-only Android v3 image SHA-256 is
`03b08a9f39c09038d2c4f57c5b0df0fbb754f6e23964d7a9c40507e2b5027cd0`.

`fastboot getvar product` returned `elish`. Fastboot accepted the new image,
then Android ADB re-enumerated within 10 seconds. An earlier FDT-export probe
also returned to Android, whose `/proc/device-tree/chosen` did not contain
`unixnu,uefi-memory-map`. Neither test captured the custom probe marker or
memory descriptors. Driver dispatch and the platform Stall behavior remain
unverified. A third image added a visible entry marker and a 15-second stall
before the memory read, then retained the 120-second post-return stall. It was
also accepted by fastboot, and Android ADB re-enumerated within 10 seconds.
The current runs do not distinguish driver dispatch from timer behavior. No
partition was written. Full hashes and results are in
`/root/repo/UniXNU-elish/reports/elish-memory-map-probe-20261124.md`.
