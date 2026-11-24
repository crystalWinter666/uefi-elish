# elish firmware guide

## Responsibility

This repository owns the Xiaomi Pad 5 Pro Wi-Fi (`elish`, M2105K81AC,
SM8250-AC) Mu-Silicium UEFI platform package and firmware inputs. The base
source revision is `UniXNU/uefi-sheng@66421cb7bb5694843aec8de1ca3d1de4c20cca38`.
That checkout supplies repository structure and shared firmware sources. Its
SM8550 board values and sheng-specific policy do not apply to elish.

## Required reading

1. Read `../UniXNU-elish/AGENTS.md`, this README, `Building.md`, and the
   elish platform's `PORTING.md` before changing firmware code.
2. Read the UniXNU firmware integration, porting, device-repository,
   component-catalog, device-tree, bring-up, payload, building, and porting
   record guides referenced by the device repository.
3. Inspect the selected SoC and component interfaces before adding platform
   code.

## Implementation

- Keep shared SM8250-AC operations in the selected SoC repository.
- Source elish memory ranges, reservations, UEFI FD placement, stacks,
  interrupts, and peripheral wiring from elish firmware, DTS, or measured
  device output. Do not copy enuma, alioth, sheng, or other board values.
- Include the required UniXNU, UniXNUDxe, DarwinBoot, UniXNURamDiskDxe,
  DiskIoDxe, and FAT modules in DSC/FDF when using the FDT initrd payload.
- Keep proprietary firmware in `.cache/firmware/`. Do not commit vendor
  binaries or generated firmware and boot images.
- Update the firmware area in `../UniXNU-elish/porting.toml` with each
  implementation or evidence change.

## Verification

Build with the pinned UniXNU and Mu_Basecore revisions from the device
repository's `board.toml`. Report commands, revisions, hashes, output paths,
results, and unresolved inputs. Hardware boot and peripheral results require
device evidence.
