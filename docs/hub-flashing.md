# Flashing the hub from a CI build

The Firmware workflow builds the hub on every run and keeps the four files that the chip needs for 14 days, as the artifact `hub-flash` of that run. This page says how to put those files on a hub board without installing PlatformIO. It needs Python with `esptool` 4.6 or newer, and the GitHub CLI with a login (`gh auth login`) to download the files.

Use only the artifact of a run on `main`, or of a pull request that you have read. The artifact is a program that runs on your board, and a run from an unreviewed branch can hold anything.

## The files

| File | Offset |
| :--- | :--- |
| `bootloader.bin` | `0x1000` |
| `partitions.bin` | `0x8000` |
| `boot_app0.bin` | `0xe000` |
| `firmware.bin` | `0x10000` |

The offsets follow `gateway/partitions.csv`. The table has the NVS at `0x9000` with a size of `0x5000`, the OTA data at `0xe000`, two app slots of `0x1F0000` at `0x10000` and `0x200000`, and a core dump area of `0x10000` at `0x3F0000`. The hub needs a flash chip of 4 MB.

## Steps

1. Find the run of the commit or the branch that you want, and download its artifact. The list shows the commit of every run. Replace `<run id>` with the number of the run.

   ```sh
   gh run list --repo aethera-deeptech/firmware --workflow firmware.yaml --limit 5 --json databaseId,headSha,headBranch,displayTitle
   gh run download <run id> --repo aethera-deeptech/firmware --name hub-flash --dir hub-flash
   ```

2. Optional but advised when the board runs other firmware. Save the whole flash first, which takes a few minutes. Pick a new folder name for each board and after each failed try, for example `hub-backup-1`, and use the same name in both commands below. The command stops when the folder exists, and that keeps one backup from replacing another. The backup holds the NVS of the board and any crash dump, so it can hold a Wi-Fi password and keys, and it must stay private. Write it into a folder that only you can open, and treat the file like a password.

   ```sh
   mkdir -m 700 <backup folder> && python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 read_flash 0 ALL <backup folder>/hub-backup.bin
   ```

   Step 3 changes folder, so go back to the folder that holds the backup folder to put the backup back.

   ```sh
   python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0 <backup folder>/hub-backup.bin
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

- It replaces the bootloader, the partition table, the OTA data and the app. It does not erase the rest of the chip. The NVS partition of this build starts at `0x9000` and ends at `0xe000`, where the write of `boot_app0.bin` begins. Saved settings of an earlier build survive only when its NVS partition was not larger than that. An earlier partition that reached past `0xe000` loses its last pages, and the settings in them may be lost. The hub then shows that no Wi-Fi is saved, and the setup portal enters it again.
- Other data in the second half of the flash, for example the file system of another firmware, is not erased by this command. The new table lists the start of that area as the second app slot and the last 64 KB as the core dump area. A later over the air update or a full erase destroys that data. A crash dump overwrites only the start of the core dump area at `0x3F0000`, and no other part of the flash. Copy files off the board first. A crash dump is plain flash and holds the stacks of the tasks, so it can hold key material for a moment. A later crash erases only the sectors that it needs, so parts of an old dump can stay in the flash until the area is erased on purpose.
- The first boot line starts with `WATTLINK hub` and the version, and it shows that the new build runs. The lines after it report the saved nodes, the saved settings and the crypto self test. A line about the Wi-Fi follows only when a network is saved, so a new board prints none. None of them shows a password or a key. The settings line does show the name of the saved Wi-Fi network, so look at the log before you paste it in a public place.
