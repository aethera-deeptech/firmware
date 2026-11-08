# Flashing the hub from a CI build

The Firmware workflow builds the hub on every run and keeps the four files that the chip needs for 14 days, as the artifact `hub-flash` of that run. This page says how to put those files on a hub board without installing PlatformIO. It needs Python with `esptool`, and the GitHub CLI with a login (`gh auth login`) to download the files.

Use only the artifact of a run on `main`, or of a pull request that you have read. The artifact is a program that runs on your board, and a run from an unreviewed branch can hold anything.

## The files

| File | Offset |
| :--- | :--- |
| `bootloader.bin` | `0x1000` |
| `partitions.bin` | `0x8000` |
| `boot_app0.bin` | `0xe000` |
| `firmware.bin` | `0x10000` |

The offsets follow `gateway/partitions.csv`. The table has the NVS at `0x9000` with a size of `0x5000`, the OTA data at `0xe000`, and two app slots of `0x1F0000` at `0x10000` and `0x200000`. The hub needs a flash chip of 4 MB.

## Steps

1. Find the run of the commit or the branch that you want, and download its artifact. The list shows the commit of every run. Replace `<run id>` with the number of the run.

   ```sh
   gh run list --repo aethera-deeptech/firmware --workflow firmware.yaml --limit 5 --json databaseId,headSha,headBranch,displayTitle
   gh run download <run id> --repo aethera-deeptech/firmware --name hub-flash --dir hub-flash
   ```

2. Optional but advised when the board runs other firmware. Save the whole flash of 4 MB first, which takes a few minutes. The backup holds the NVS of the board, so it can hold a Wi-Fi password and keys, and it must stay private. The backup stays in the folder where you ran the command, and step 3 changes folder, so go back to it to restore.

   ```sh
   python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 read_flash 0 0x400000 hub-backup.bin
   ```

   To put the backup back, run this in the same folder.

   ```sh
   python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0 hub-backup.bin
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

## When the port does not work

- On Linux the port belongs to a group, often `dialout` or `uucp`. A permission error means that your user is not in it.
- The port can be `ttyACM0` or a COM name instead of `ttyUSB0`.
- Some boards have no automatic reset. When `esptool` stays at `Connecting....`, hold the BOOT button until the write starts. This also reaches a board that restarts in a loop, because `esptool` talks to the loader in the chip and not to the firmware.
- `esptool` can erase the whole flash with `erase_flash`. That gives a clean state, and it deletes the saved settings and every pairing.

## What the write changes

- It replaces the bootloader, the partition table, the OTA data and the app. It does not erase the rest of the chip, so the NVS pages of earlier builds stay and the saved settings survive when the NVS partition starts at `0x9000`.
- Other data in the second half of the flash, for example the file system of another firmware, is not erased by this command. The new table lists that area as the second app slot, so a later over the air update or a full erase destroys it. Copy files off the board first.
- The first boot line starts with `WATTLINK hub` and the version, and it shows that the new build runs. The lines after it report the saved nodes, the saved settings and the crypto self test. A line about the Wi-Fi follows only when a network is saved, so a new board prints none. None of them shows a password or a key. The settings line does show the name of the saved Wi-Fi network, so look at the log before you paste it in a public place.
