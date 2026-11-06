# ESP32-S31 CDC + Flash MSC (phase 2)

One TinyUSB instance exports CDC-ACM and MSC over Mosaico's USB-OTG Type-C.
CDC retains byte echo, printf and ESP_LOG output. No auto-download yet.

## Versions

ESP-IDF 6.2.0, local revision d445e947; esp_tinyusb 3.0.0 is recorded in
dependencies.lock. Implementation follows the installed component and local
examples/peripherals/usb/device/tusb_composite_msc_serialdevice.

## Flash layout and pre-flash requirement

The user reports 16 MiB of program SPI NOR flash. Configuration now uses
16 MiB; verify this is program NOR (not SPI NAND) before flashing. The runtime
check requires at least 16 MiB. ROM `esptool --chip esp32s31 -p COMxx flash-id`
can independently confirm the physical capacity.

| Region | Offset | Size | End (exclusive) |
| --- | --- | --- | --- |
| Partition table | 0x8000 | 0x1000 | 0x9000 |
| NVS | 0x9000 | 0x6000 | 0xf000 |
| PHY | 0xf000 | 0x1000 | 0x10000 |
| Factory app | 0x10000 | 0x100000 | 0x110000 |
| msc_fat | 0x110000 | 0xef0000 | 0x1000000 |

Original NVS/PHY/app offsets and sizes are preserved. No OTA partitions.
MSC uses the remaining 14.9375 MiB of the 16 MiB chip. Wear levelling metadata
and FAT structures reduce the capacity/free space visible to the host.

The firmware checks the named partition's offset/size and reported flash size
before initializing storage. These runtime checks do not replace the pre-flash
capacity check: an oversized flash header may prevent boot before app_main.

## Ownership and formatting

Only msc_fat is passed to wl_mount. Before USB starts, firmware temporarily
registers the wear-levelled disk with FatFs and checks the filesystem. A valid
FAT volume is preserved. Only FR_NO_FILESYSTEM plus a completely erased (all
0xff) logical disk permits automatic FAT creation. Read errors, internal FAT
errors and nonblank disks without a valid FAT fail without automatic formatting.
The filesystem and temporary diskio registration are removed before creating
MSC with owner USB, auto_mount_off=1 and do_not_format=true. Disconnect/eject
never mounts FAT in the application. No application file operations run while
USB owns storage. First initialization can delay USB enumeration.

Migration from the old 2 MiB partition is not an in-place filesystem resize.
Back up files before flashing the new partition table: wear-levelling metadata
locations depend on partition size, and old data may become unreadable. Old
metadata may also make the expanded disk nonblank, causing the safety check to
refuse initialization. In that case, after backup, erase only msc_fat through a
separate deliberate maintenance operation; do not erase the whole chip. No
migration erase is performed by this firmware. A blank partition initializes
on the next boot without a Windows format prompt. A power failure during first
format can similarly leave a nonblank incomplete filesystem requiring recovery.

Do not write a raw FAT image to msc_fat: the host sees the wear-levelled logical
disk, not the raw partition layout. Unexpected format prompts after files exist
are failures to investigate, not instructions to reformat.

## User build and flash

The agent did not build, link, generate images, flash, or commit this phase.

1. Confirm program NOR capacity as above; keep the ROM COM number.
2. Build with the existing IDF setup (`idf.py build` or extension).
3. Manually enter ROM with BOOT and power-on; close XCOM first.
4. Use a full project flash (`idf.py -p COMxx flash`), not app-only flash.
   The updated bootloader/header and partition table must also be written.
   Do not use erase-flash; it removes NVS and test files.
5. Release BOOT and return to normal application boot.

A new composite PID/interface layout may assign a different application COM
number. Product string: S31 USB CDC MSC Test. Automatic flash/reset through the
application COM is not implemented; retain manual BOOT recovery.

## Acceptance

- A blank MSC partition initializes FAT before USB enumeration, without a Windows format prompt.
- Same Type-C cable exposes a COM port and an approximately 14.9 MiB removable disk.
- Open the COM in XCOM: DTR on, hardware flow control off, 115200/8N1.
- Every second: [USB], [printf], [ESP_LOGI]. Sending hello echoes the bytes.
- The [USB] line reports speed, reported flash bytes and logical MSC bytes.
  Runtime flash size may reflect the configured addressable size, not the full
  physical capacity; flash-id is the pre-flash capacity evidence.
- While copying a small file to the disk, status and echo should keep working.
  NOR writes can add latency; HS bus rate does not equal flash write throughput.
- Record SHA256 of a source file (`Get-FileHash path -Algorithm SHA256`), copy
  it to the disk, safely eject, unplug/power-cycle, reconnect and compare hash
  of the stored copy. The hash must match and no format prompt should recur.
- Close/reopen COM and unplug/replug; logs and echo must resume.
- Confirm current negotiated speed with a host USB tree viewer: High-Speed,
  480 Mbps, not merely USB 2.0 capability/bcdUSB. Cross-check [USB] speed=HIGH.
  If FULL, record cable/hub/topology; enumeration alone is not HS evidence.

Safely eject before power-off or another flash. Use a small file below the
reported free space. Sudden power loss during writing is not guaranteed safe.

## Implementation

main/usb_storage.c owns wear levelling and MSC setup. Async MSC storage I/O
keeps flash work off the USB servicing task. Default FS/HS descriptors combine
CDC and MSC. No independent USB stack or runtime device-side FAT consumer is installed.

CDC uses tinyusb_console_init. RX has one reader in app_main, echo uses stdout,
and VFS TX is nonblocking: slow/absent hosts can lose logs. echo_tx_dropped only
counts short echo writes. Initialization/ROM/panic logs are not replayed later.

The HS USB shutdown workaround follows Mosaico factory usb_hs_console at
commit 2654a4a9, without importing its download/reset trigger.
