# Flashing the hub from a CI build

The Firmware workflow builds the hub on every run and keeps the four files that the chip needs for 14 days, as the artifact `hub-flash-<commit sha>`. This page says how to put those files on a hub board without installing PlatformIO. It needs Python with `esptool` and, to download the files, the GitHub CLI.

## The files

| File | Offset |
| :--- | :--- |
| `bootloader.bin` | `0x1000` |
| `partitions.bin` | `0x8000` |
| `boot_app0.bin` | `0xe000` |
| `firmware.bin` | `0x10000` |

The offsets follow `gateway/partitions.csv`. The table has the NVS at `0x9000` with a size of `0x5000`, the OTA data at `0xe000`, and two app slots of `0x1F0000` at `0x10000` and `0x200000`. The hub needs a flash chip of 4 MB.

## Steps

1. Find the run of the commit or the branch that you want, and download its artifact. Replace `<run id>` and `<commit sha>`.

   ```sh
   gh run list --repo aethera-deeptech/firmware --workflow firmware.yaml --limit 5
   gh run download <run id> --repo aethera-deeptech/firmware --name hub-flash-<commit sha> --dir hub-flash
   ```

2. Optional but advised when the board runs other firmware. Save the whole flash of 4 MB first. `write_flash 0 hub-backup.bin` puts it back.

   ```sh
   python -m esptool --port /dev/ttyUSB0 read_flash 0 0x400000 hub-backup.bin
   ```

3. Write the four files.

   ```sh
   cd hub-flash
   python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash --flash_mode dio --flash_freq 40m --flash_size detect \
     0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
   ```

4. Read the boot lines. The board restarts when `esptool` ends, so open the monitor first and press the EN button on the board. If a monitor keeps the chip in reset, force both control lines off as below.

   ```sh
   python -m serial.tools.miniterm --dtr 0 --rts 0 /dev/ttyUSB0 115200
   ```

## What the write changes

- It replaces the bootloader, the partition table, the OTA data and the app. It does not erase the rest of the chip, so the NVS pages of earlier builds stay and the saved settings survive when the NVS partition starts at `0x9000`.
- Other data in the second half of the flash, for example the file system of another firmware, is not erased by this command. The new table lists that area as the second app slot, so a later over the air update or a full erase destroys it. Copy files off the board first.
- The boot lines show `Nodes:`, `Crypto self test:` and `WiFi:` lines. They never show a network name, a password or a key.
