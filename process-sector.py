from pathlib import Path
import struct

raw = Path("BOOT.EXE").read_bytes()
boot = raw[-512:]

assert len(boot) == 512
assert boot[0] == 0xEB and boot[2] == 0x90
assert boot[510:512] == b"\x55\xaa"

Path("BOOT.BIN").write_bytes(boot)
print("New BOOT.BIN extracted:", len(boot), "bytes")

img_path = Path.home() / "START_MBR.img"
vbr_boot = Path("/mnt/floppy/boot-check/BOOT.BIN").read_bytes()

assert len(vbr_boot) == 512
assert vbr_boot[510:512] == b"\x55\xaa"

# 표준 16비트 체인로드 MBR 부트 코드 (LBA 0 -> 0x0600 재배치 후 활성 파티션 VBR을 0x7C00에 로드)
# CLI; XOR AX,AX; MOV DS,AX; MOV ES,AX; MOV SS,AX; MOV SP,7C00h; STI
# MOV SI,7C00h; MOV DI,0600h; MOV CX,0100h; CLD; REP MOVSW; JMP FAR 0000:061Dh
mbr_code = bytes.fromhex(
    "FA31C08ED88EC08ED0BC007CFB"
    "BE007CBF0006B90001FCF3A5"
    "EA1D060000"
    "BEBE07B90400803C80740B83C610E2F6CD18F4EBFD"
    "88164007668B440831DB5353665068007C534353B11051"
    "89E6B4428A164007CD1372E683C4108A164007EA007C0000"
)
assert len(mbr_code) <= 440

with img_path.open("r+b") as f:
    # 1) LBA 0: 기존 sfdisk 파티션 테이블(440..512)은 유지하고 앞 440바이트만 MBR 코드 기록
    mbr = bytearray(f.read(512))
    assert mbr[510:512] == b"\x55\xaa", "MBR signature missing"
    mbr[:len(mbr_code)] = mbr_code
    f.seek(0)
    f.write(mbr)

    # 2) LBA 2048 (바이트 1048576): VBR 읽기 -> BPB 보존 + BPB_HiddSec(2048) 기록 + BOOT.BIN 주입
    part_lba = 2048
    part_off = part_lba * 512
    f.seek(part_off)
    old_vbr = bytearray(f.read(512))
    assert old_vbr[82:90] == b"FAT32   ", "Partition 1 is not FAT32"

    # BPB_HiddSec (오프셋 0x1C, 4바이트)에 2048 명시적 기록
    struct.pack_into("<I", old_vbr, 0x1C, part_lba)

    new_vbr = vbr_boot[:3] + bytes(old_vbr[3:90]) + vbr_boot[90:512]
    f.seek(part_off)
    f.write(new_vbr)

print("MBR (LBA 0) and FAT32 VBR (LBA 2048, HiddSec=2048) installed.")
