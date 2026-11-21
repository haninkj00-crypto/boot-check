# boot-check

Pre-boot hardware and firmware diagnostics for x86 PCs.

The project has two independent boot paths:

- **Legacy BIOS/MBR:** a 16-bit boot path that starts `START.COM` and writes its report to `RESULT.TXT`.
- **UEFI:** a separate GNU-EFI application. The x64 removable-media fallback path is `\EFI\BOOT\BOOTX64.EFI`; it writes `EFILOG.TXT` to the root of the volume from which the EFI application was loaded.

The UEFI application does not execute the DOS program, and the legacy MBR path does not execute the EFI application. Both boot files can coexist on the same FAT32 partition.

## Source layout

| Path | Purpose |
|---|---|
| `boot.asm`, `c0boot.asm`, `start.c`, `test/`, `*.h` | Legacy BIOS/DOS implementation |
| `build.bat` | MASM / Turbo C++ / Turbo Link build |
| `BOOTX64.C`, `efitest/`, `efi*.h` | UEFI implementation |
| `Makefile` | GNU-EFI build, FAT32 EFI injection, optional QEMU/OVMF launch |
| `make-image.sh` | Copies the DOS program, resets `RESULT.TXT`, builds and injects EFI |
| `process-sector.py` | Installs the custom MBR and FAT32 VBR code in the raw disk image |

## Build requirements

### Legacy BIOS/DOS build

The legacy build expects the tools named in `build.bat`:

- MASM 6.11 (`ml`)
- Turbo C++ 3 (`tcc`)
- Turbo Link 5 (`tlink`)

Run `build.bat` in the DOS build environment. It produces `START.COM` and `BOOT.EXE`.

The legacy source files may need the encoding and line endings expected by the DOS toolchain (the working source set uses CP949/EUC-KR-compatible text and CRLF). Do not bulk-convert the UEFI source tree as part of this step; the EFI source files use UTF-8 with LF line endings.

### UEFI build and image tools

The UEFI host build uses GCC, GNU binutils, GNU-EFI, GNU Make and `mtools`. Image preparation also uses `dosfstools`, `sfdisk`, `losetup` and Python 3.

On Ubuntu, the relevant packages are commonly available with:

```sh
sudo apt install build-essential binutils make gnu-efi mtools dosfstools fdisk
```

The exact GNU-EFI library paths vary by distribution. Override the corresponding variables in the Makefile when necessary.

Build the default x64 EFI application:

```sh
make
```

Optional IA32 build (requires 32-bit compiler support and the IA32 GNU-EFI files):

```sh
make ia32
```

Build both architectures:

```sh
make all
```

`make inject` builds `BOOTX64.EFI` if needed and copies it to the target FAT32 volume. It also copies `BOOTIA32.EFI` when that file already exists; it does not build IA32 automatically.

## Disk image layout

The legacy and UEFI boot files are stored on the same FAT32 partition in the raw hard-disk image.

The current scripts use this layout:

| Item | Value |
|---|---:|
| Whole raw image | 256 MiB |
| Partition table | DOS/MBR |
| Partition 1 | Active, type `0x0C` (FAT32 LBA) |
| Partition start | LBA 2048 (1 MiB byte offset) |
| Partition size | 131072 sectors (64 MiB) |
| Filesystem | FAT32 |

The remaining image space is outside the FAT32 partition. The default target image is `$HOME/START_MBR.img` and the default partition offset used by `Makefile` and `make-image.sh` is `1048576` bytes.

### Create a new target image (first-time setup only)

**This creates or truncates the target image. It destroys any existing contents at that path.** Do not run it on an image containing results you need.

```sh
IMG="$HOME/START_MBR.img"

truncate -s 256M "$IMG"
printf 'label: dos\nunit: sectors\n\nstart=2048, size=131072, type=0c, bootable\n' |
    sfdisk "$IMG"

LOOP=$(sudo losetup --find --show \
    --offset $((2048 * 512)) \
    --sizelimit $((131072 * 512)) \
    "$IMG")
sudo mkfs.fat -F 32 "$LOOP"
sudo losetup -d "$LOOP"
```

This creates the MBR partition table and FAT32 filesystem only. The custom MBR/VBR boot code is installed separately by `process-sector.py`.

## Prepare the images

Two images have different roles:

- **Staging floppy image** (`boot-check.img` by default): a FAT12 image containing `START.COM`. It is only a source from which `make-image.sh` copies the DOS program. It is not the 256 MiB target disk.
- **Target hard-disk image** (`$HOME/START_MBR.img` by default): the raw MBR/FAT32 disk image that receives `START.COM`, `RESULT.TXT`, the custom boot code and the EFI application.

If the 1.44 MiB FAT12 staging image needs to be created:

```sh
truncate -s 1474560 boot-check.img
mkfs.fat -F 12 boot-check.img
mcopy -o -i boot-check.img START.COM ::START.COM
```

If it already exists, verify that the file is present:

```sh
mdir -i boot-check.img ::START.COM
```

### Install the custom MBR and FAT32 VBR

After the DOS build, ensure `BOOT.EXE` is available. Run this from the project directory:

```sh
python3 process-sector.py
```

Defaults:

- Input executable: `./BOOT.EXE`
- Extracted boot-sector file: `./BOOT.BIN`
- Target image: `$HOME/START_MBR.img`
- Target partition: active MBR partition 1
- VBR template: the `BOOT.BIN` just extracted from `BOOT.EXE`

The script reads the final 512 bytes of `BOOT.EXE`, verifies the expected jump/NOP and `0x55AA` signature, and writes those bytes to `BOOT.BIN`. It reads the target partition start LBA from the MBR partition table instead of assuming LBA 2048. It then:

1. Installs the custom chainloader in the first 440 bytes of LBA 0.
2. Preserves the MBR disk signature, partition table and `0x55AA` signature.
3. Installs the VBR boot code while preserving the existing FAT32 BPB bytes 3 through 89.
4. Sets `BPB_HiddSec` to the detected partition start LBA.

The script validates the target partition type (`0x0B` or `0x0C`), bounds, active flag and FAT32 VBR marker before modifying either boot sector. The selected partition must be the only active MBR partition because the installed MBR chainloader searches the partition table for an active entry and follows it.

Use a different target image or executable with explicit options:

```sh
python3 process-sector.py \
    --image "$HOME/START_MBR_TEST.img" \
    --exe ./BOOT.EXE
```

Available options:

```text
--image PATH          Raw target disk image (default: $IMG or ~/START_MBR.img)
--exe PATH            BOOT.EXE input (default: ./BOOT.EXE)
--boot-bin PATH       Extracted 512-byte output (default: ./BOOT.BIN)
--vbr-template PATH   Optional external 512-byte VBR template
--partition N         MBR partition number 1..4 (default: 1)
```

An external VBR template is normally unnecessary. To use one intentionally:

```sh
python3 process-sector.py \
    --image "$HOME/START_MBR.img" \
    --exe ./BOOT.EXE \
    --vbr-template /path/to/BOOT.BIN
```

The old fixed dependency on `/mnt/floppy/boot-check/BOOT.BIN` has been removed. By default, the extracted local `BOOT.BIN` is used directly.

### Populate the FAT32 volume and add UEFI

Run from the project directory after the target image and its FAT32 partition exist, and after installing the MBR/VBR code:

```sh
./make-image.sh
```

The script:

1. Reads `START.COM` from `boot-check.img` (or `SRC_IMG`).
2. Checks that the target FAT32 partition can be opened by `mtools`.
3. Copies `START.COM` to the FAT32 root.
4. Recreates `RESULT.TXT` as a 60 KiB zero-filled file.
5. Runs `make inject`, which builds `BOOTX64.EFI` if needed and copies it to `EFI/BOOT/BOOTX64.EFI`. If `BOOTIA32.EFI` already exists, it is copied to `EFI/BOOT/BOOTIA32.EFI` too.

The script does **not** create or format the 256 MiB target image, and it does not install the MBR/VBR boot code. Those are separate steps.

Override the staging image, target image and partition byte offset when needed:

```sh
SRC_IMG="$PWD/boot-check.img" \
IMG="$HOME/START_MBR.img" \
PART_OFFSET=1048576 \
./make-image.sh
```

For an EFI-only rebuild and reinjection:

```sh
make
make inject IMG="$HOME/START_MBR.img" PART_OFFSET=1048576
```

Inspect the installed files:

```sh
PART="$HOME/START_MBR.img@@1048576"
mdir -i "$PART" ::
mdir -i "$PART" ::EFI/BOOT
```

**Important:** running `make-image.sh` again resets `RESULT.TXT`. Back it up first if you need to retain legacy test results. It does not intentionally reset `EFILOG.TXT`; the EFI application opens that file in append mode.

## Boot and test

### VMware

The raw `START_MBR.img` can be attached as a virtual hard disk. A VMDK descriptor may reference the raw image without converting its contents. Keep the VMDK descriptor and raw image together, and do not move or rename the raw image while the descriptor is in use.

Select UEFI firmware in the virtual machine settings. Secure Boot may need to be disabled because the EFI application is unsigned.

### Physical USB

Write the **whole raw disk image** to the USB device, not to an individual partition. This erases the USB device.

Identify the device carefully:

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,TYPE,MOUNTPOINTS
```

Unmount every mounted partition on the chosen USB device, then write to the whole-disk device. Replace `/dev/sdX` with the verified device name:

```sh
sudo dd if="$HOME/START_MBR.img" of=/dev/sdX \
    bs=4M status=progress conv=fsync
sync
```

Do not use a partition path such as `/dev/sdX1`. Verify the written image if needed:

```sh
sudo cmp -n "$(stat -c '%s' "$HOME/START_MBR.img")" \
    "$HOME/START_MBR.img" /dev/sdX
```

In the firmware boot menu, select the UEFI entry for the USB device to start the EFI application. A Legacy/CSM entry starts the MBR path instead. Firmware behavior varies: this image currently uses MBR partition type `0x0C`, while the conventional MBR type for an EFI System Partition is `0xEF`. Some firmware may still discover the fallback EFI file on a FAT volume, but this is not guaranteed. If a firmware does not expose the USB as a UEFI boot option, do not alter the working image blindly; test a separate ESP-marked image or USB first.

### Optional QEMU test

The Makefile retains `make run-x64` for OVMF/QEMU testing. QEMU is optional; it is not required to build EFI, prepare images, use VMware or test on physical hardware.

## Output files

| Boot path | Output | Location |
|---|---|---|
| Legacy BIOS/MBR | `RESULT.TXT` | FAT32 volume root |
| UEFI | `EFILOG.TXT` | Root of the volume from which the EFI application was loaded |

The two logs are separate. The UEFI application uses firmware protocols and does not call BIOS interrupts or DOS services.

## Safety notes

- Back up an image before running boot-sector installation or writing it to a physical USB device.
- `process-sector.py` modifies LBA 0 and the selected FAT32 partition's VBR in place. It preserves the MBR partition table and FAT32 BPB, but it still changes boot code.
- `make-image.sh` overwrites `START.COM` and resets `RESULT.TXT` in the target volume.
- Writing the raw image to a physical disk overwrites that entire disk.
- Use a disposable image or USB for initial hardware tests.
- Device enumeration and protocol availability do not prove that every device function is fully operational.
