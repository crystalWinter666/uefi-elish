# uefi-elish

Mu-Silicium UEFI for the Xiaomi Pad 5 Pro Wi-Fi (`elish`, `M2105K81AC`, SM8250-AC). This firmware is part of the UniXNU port targeting iPadOS 17.7.11 (`21H461`). [UniXNU-elish device repository](https://github.com/crystalWinter666/UniXNU-elish)

## Hardware status

- RAM-only `fastboot boot` reached the UniXNU boot menu.
- The DEBUG ramoops probe captured two Qualcomm RAM Partition ranges and 47 UEFI DRAM descriptors before BDS. Recovery through LineageOS root ADB was verified. [Capture report](https://github.com/crystalWinter666/UniXNU-elish/blob/format2-device/reports/elish-uefi-pre-bds-memory-map-20261124.md)
- DarwinBoot's final memory map and handoff to XNU, UFS/APFS root mounting, and iPadOS startup remain to be verified.

## Build

Clone this checkout with its submodules next to `UniXNU-elish` and `UniXNU`. Follow the device repository's instructions for the UniXNU revision and build support patch. Run the firmware build from the UniXNU checkout. [Device build instructions](https://github.com/crystalWinter666/UniXNU-elish#build)

```sh
./unixnu build firmware -D ../UniXNU-elish \
  --uefi ../uefi-elish --edk2 ../uefi-elish/Mu_Basecore
```

The elish DSC and FDF are in `Platforms/Xiaomi/elishPkg/`. `Resources/Configs/elish.toml` selects the 3 MiB firmware volume and Android v3 boot image format. The DEBUG build writes to the Android ramoops console. The board's normal configuration leaves `ELISH_MEMORY_MAP_PROBE` disabled. [Platform notes](Platforms/Xiaomi/elishPkg/PORTING.md)

The port is based on the `uefi-sheng` branch at commit `66421cb7bb5694843aec8de1ca3d1de4c20cca38`. [Upstream](https://github.com/UniXNU/uefi-sheng/tree/66421cb7bb5694843aec8de1ca3d1de4c20cca38)
