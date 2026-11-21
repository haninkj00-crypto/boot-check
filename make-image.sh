#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

# 입력: DOS 빌드 결과가 있는 플로피 이미지
SRC_IMG="${SRC_IMG:-$ROOT/boot-check.img}"

# 출력: 256MiB MBR/FAT32 하드디스크 이미지
IMG="${IMG:-$HOME/START_MBR.img}"
PART_OFFSET="${PART_OFFSET:-1048576}"
PART="${IMG}@@${PART_OFFSET}"

TMPDIR_WORK=$(mktemp -d)
trap 'rm -rf "$TMPDIR_WORK"' EXIT

START_COM="$TMPDIR_WORK/START.COM"
RESULT_TXT="$TMPDIR_WORK/RESULT.TXT"

# 입력 이미지와 출력 이미지 확인
test -f "$SRC_IMG" || {
    echo "DOS 소스 이미지 없음: $SRC_IMG"
    exit 1
}

test -f "$IMG" || {
    echo "대상 하드디스크 이미지 없음: $IMG"
    exit 1
}

# 원본 플로피의 START.COM 확인 및 추출
mdir -i "$SRC_IMG" ::START.COM >/dev/null
mcopy -o -i "$SRC_IMG" ::START.COM "$START_COM"

# FAT32 파티션이 정상적으로 열리는지 확인
mdir -i "$PART" :: >/dev/null

# START.COM 설치
mcopy -o -i "$PART" "$START_COM" ::START.COM

# RESULT.TXT 60KiB 초기화
dd if=/dev/zero of="$RESULT_TXT" \
   bs=61440 count=1 status=none

mcopy -o -i "$PART" "$RESULT_TXT" ::RESULT.TXT

# EFI 빌드 및 주입
make -C "$ROOT" inject \
    IMG="$IMG" \
    PART_OFFSET="$PART_OFFSET"

echo
echo "Image preparation completed."
echo "Image : $IMG"
echo "EFI   : EFI/BOOT/BOOTX64.EFI"
echo "Log   : RESULT.TXT / EFILOG.TXT"
