#!/usr/bin/env python3
"""Install the boot-check MBR/VBR code into a raw MBR/FAT32 disk image.

Defaults preserve the project's current workflow:
  - BOOT.EXE is read from the current working directory.
  - BOOT.BIN is written to the current working directory.
  - $HOME/START_MBR.img is modified in place.
  - Partition 1 is used unless --partition is specified.

The VBR template defaults to the BOOT.BIN extracted from BOOT.EXE.  An
external template can be selected with --vbr-template when needed.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
MBR_PARTITION_TABLE_OFFSET = 446
MBR_PARTITION_ENTRY_SIZE = 16
MBR_PARTITION_COUNT = 4
FAT32_TYPES = {0x0B, 0x0C, 0xEF}

# 16-bit chainloader: relocate LBA 0 to 0x0600, find the active MBR
# partition, load its VBR at 0000:7C00, then transfer control.
MBR_CODE = bytes.fromhex(
    "FA31C08ED88EC08ED0BC007CFB"
    "BE007CBF0006B90001FCF3A5"
    "EA1D060000"
    "BEBE07B90400803C80740B83C610E2F6CD18F4EBFD"
    "88164007668B440831DB5353665068007C534353B11051"
    "89E6B4428A164007CD1372E683C4108A164007EA007C0000"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Install boot-check MBR and FAT32 VBR code into a raw disk image."
    )
    parser.add_argument(
        "--image",
        type=Path,
        default=Path(os.environ.get("IMG", str(Path.home() / "START_MBR.img"))),
        help="raw disk image to modify (default: $IMG or ~/START_MBR.img)",
    )
    parser.add_argument(
        "--exe",
        type=Path,
        default=Path(os.environ.get("BOOT_EXE", "BOOT.EXE")),
        help="BOOT.EXE containing the 512-byte boot sector at EOF (default: ./BOOT.EXE)",
    )
    parser.add_argument(
        "--boot-bin",
        type=Path,
        default=Path("BOOT.BIN"),
        help="where to write the extracted 512-byte sector (default: ./BOOT.BIN)",
    )
    parser.add_argument(
        "--vbr-template",
        type=Path,
        default=(Path(os.environ["VBR_TEMPLATE"]) if os.environ.get("VBR_TEMPLATE") else None),
        help="optional external 512-byte VBR template; defaults to the extracted BOOT.BIN",
    )
    parser.add_argument(
        "--partition",
        type=int,
        choices=range(1, MBR_PARTITION_COUNT + 1),
        default=int(os.environ.get("PARTITION", "1")),
        help="1-based MBR partition number (default: 1)",
    )
    return parser.parse_args()


def read_partition(image_path: Path, partition_number: int) -> tuple[int, int, int, int]:
    """Return (type, start_lba, sector_count, boot_flag) for an MBR partition."""
    image_size = image_path.stat().st_size
    if image_size < SECTOR_SIZE:
        raise ValueError(f"Image is smaller than one sector: {image_size} bytes")

    with image_path.open("rb") as image:
        mbr = image.read(SECTOR_SIZE)

    if mbr[510:512] != b"\x55\xaa":
        raise ValueError("MBR signature 0x55AA is missing")

    active_partitions = []
    for i in range(MBR_PARTITION_COUNT):
        entry_off = MBR_PARTITION_TABLE_OFFSET + i * MBR_PARTITION_ENTRY_SIZE
        if mbr[entry_off] == 0x80:
            active_partitions.append(i + 1)

    index = partition_number - 1
    off = MBR_PARTITION_TABLE_OFFSET + index * MBR_PARTITION_ENTRY_SIZE
    entry = mbr[off : off + MBR_PARTITION_ENTRY_SIZE]
    boot_flag = entry[0]
    part_type = entry[4]
    start_lba, sector_count = struct.unpack_from("<II", entry, 8)

    if boot_flag not in (0x00, 0x80):
        raise ValueError(
            f"Partition {partition_number}: invalid boot flag 0x{boot_flag:02X}"
        )
    if part_type not in FAT32_TYPES:
        raise ValueError(
            f"Partition {partition_number}: type 0x{part_type:02X} is not a supported FAT32 MBR type (0x0B/0x0C)"
        )
    if start_lba == 0 or sector_count == 0:
        raise ValueError(f"Partition {partition_number} is empty or has an invalid start LBA")
    if active_partitions != [partition_number]:
        raise ValueError(
            "The selected partition must be the only active MBR partition; "
            f"active partition numbers found: {active_partitions or 'none'}"
        )

    end_byte = (start_lba + sector_count) * SECTOR_SIZE
    if end_byte > image_size:
        raise ValueError(
            f"Partition {partition_number} extends beyond image: end={end_byte}, image={image_size}"
        )

    return part_type, start_lba, sector_count, boot_flag


def read_boot_sector(path: Path, label: str) -> bytes:
    data = path.read_bytes()
    if len(data) != SECTOR_SIZE:
        raise ValueError(f"{label} must be exactly 512 bytes; got {len(data)}")
    if data[0] != 0xEB or data[2] != 0x90:
        raise ValueError(f"{label} does not begin with the expected short jump/NOP (EB ?? 90)")
    if data[510:512] != b"\x55\xaa":
        raise ValueError(f"{label} is missing the 0x55AA boot signature")
    return data


def main() -> int:
    args = parse_args()
    image_path = args.image.expanduser().resolve()
    exe_path = args.exe.expanduser().resolve()
    boot_bin_path = args.boot_bin.expanduser().resolve()

    try:
        if args.partition not in range(1, MBR_PARTITION_COUNT + 1):
            raise ValueError(f"Invalid partition number: {args.partition}")
        if not image_path.is_file():
            raise FileNotFoundError(f"Disk image not found: {image_path}")
        if not exe_path.is_file():
            raise FileNotFoundError(f"BOOT executable not found: {exe_path}")
        if len(MBR_CODE) > 440:
            raise ValueError(f"MBR code is {len(MBR_CODE)} bytes; maximum is 440")

        raw = exe_path.read_bytes()
        if len(raw) < SECTOR_SIZE:
            raise ValueError("BOOT.EXE is shorter than 512 bytes")

        # The project stores the boot-sector image in the final 512 bytes of BOOT.EXE.
        extracted_vbr = raw[-SECTOR_SIZE:]
        if extracted_vbr[0] != 0xEB or extracted_vbr[2] != 0x90:
            raise ValueError("The final 512 bytes of BOOT.EXE are not the expected boot sector")
        if extracted_vbr[510:512] != b"\x55\xaa":
            raise ValueError("The final 512 bytes of BOOT.EXE do not end with 0x55AA")

        boot_bin_path.parent.mkdir(parents=True, exist_ok=True)
        boot_bin_path.write_bytes(extracted_vbr)

        if args.vbr_template is not None:
            vbr_path = args.vbr_template.expanduser().resolve()
            vbr_boot = read_boot_sector(vbr_path, "VBR template")
        else:
            vbr_path = boot_bin_path
            vbr_boot = read_boot_sector(boot_bin_path, "Extracted BOOT.BIN")

        part_type, part_lba, part_sectors, boot_flag = read_partition(
            image_path, args.partition
        )
        part_offset = part_lba * SECTOR_SIZE

        with image_path.open("r+b") as image:
            # Validate both target sectors before modifying either one.
            image.seek(0)
            mbr = bytearray(image.read(SECTOR_SIZE))
            image.seek(part_offset)
            old_vbr = bytearray(image.read(SECTOR_SIZE))
            if len(old_vbr) != SECTOR_SIZE:
                raise ValueError("Could not read a complete FAT32 VBR")
            if old_vbr[82:90] != b"FAT32   ":
                raise ValueError(
                    f"Partition {args.partition} does not have the expected FAT32 label at VBR offset 82"
                )
            if boot_flag != 0x80:
                raise ValueError(
                    f"Partition {args.partition} is not active; the installed MBR chainloader selects the active partition"
                )

            # 1) Patch only the boot-code area of LBA 0.  Disk signature,
            #    partition table (bytes 446..509), and 0x55AA remain intact.
            mbr[: len(MBR_CODE)] = MBR_CODE

            # 2) Preserve the existing FAT32 BPB, but install boot code from
            #    the selected template and set BPB_HiddSec to the detected LBA.
            struct.pack_into("<I", old_vbr, 0x1C, part_lba)
            new_vbr = vbr_boot[:3] + bytes(old_vbr[3:90]) + vbr_boot[90:512]

            # Write only after validation has completed.
            image.seek(0)
            image.write(mbr)
            image.seek(part_offset)
            image.write(new_vbr)
            image.flush()
            os.fsync(image.fileno())

        active_text = "active" if boot_flag == 0x80 else "not active"
        print(f"BOOT.BIN extracted: {boot_bin_path} ({SECTOR_SIZE} bytes)")
        print(f"VBR template      : {vbr_path}")
        print(f"Target image      : {image_path}")
        print(
            f"Partition {args.partition}: type=0x{part_type:02X}, "
            f"start LBA={part_lba}, sectors={part_sectors}, {active_text}"
        )
        print(f"Partition offset  : {part_offset} bytes")
        print("MBR (LBA 0) and FAT32 VBR installed; partition table preserved.")
        return 0

    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
