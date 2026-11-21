# Makefile - boot-check UEFI (GNU-EFI) Build System
# 사용법:
#   make               -> BOOTX64.EFI 빌드 (기본)
#   make ia32          -> BOOTIA32.EFI 빌드 (gcc-multilib + gnu-efi:i386 필요)
#   make all           -> X64 + IA32 모두 빌드
#   make inject        -> START_MBR.img FAT32 파티션에 주입
#   make run-x64       -> 주입 후 QEMU OVMF(UEFI) 실행
#   make clean         -> EFI 빌드 산출물 정리

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy

EFI_MAIN  := BOOTX64.C
EFI_SRCS  := $(sort $(wildcard efitest/*.c))
EFI_HDRS  := $(wildcard efi*.h)
BUILD_DIR := build_efi

# 이미지 및 QEMU 테스트 기본 경로 (명령줄에서 IMG=... 등으로 덮어쓰기 가능)
IMG         ?= $(HOME)/START_MBR.img
PART_OFFSET ?= 1048576
PART        := $(IMG)@@$(PART_OFFSET)
OVMF_CODE   ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS   ?= /usr/share/OVMF/OVMF_VARS_4M.fd

# GNU-EFI 경로 (Ubuntu 24.04 기본값)
EFI_INC      ?= /usr/include/efi
EFI_LIB_X64  ?= /usr/lib
EFI_CRT_X64  ?= $(EFI_LIB_X64)/crt0-efi-x86_64.o
EFI_LDS_X64  ?= $(EFI_LIB_X64)/elf_x86_64_efi.lds

EFI_LIB_IA32 ?= /usr/lib/i386-linux-gnu
EFI_CRT_IA32 ?= $(EFI_LIB_IA32)/crt0-efi-ia32.o
EFI_LDS_IA32 ?= $(EFI_LIB_IA32)/elf_ia32_efi.lds

CFLAGS_COMMON := -I. -I$(EFI_INC) \
                 -fpic -ffreestanding -fno-stack-protector -fno-stack-check \
                 -fshort-wchar -maccumulate-outgoing-args -Wall -Wextra

CFLAGS_X64  := $(CFLAGS_COMMON) -I$(EFI_INC)/x86_64 -mno-red-zone -DEFI_FUNCTION_WRAPPER
CFLAGS_IA32 := $(CFLAGS_COMMON) -I$(EFI_INC)/ia32 -m32

OBJCOPY_SECTIONS := -j .text -j .sdata -j .data -j .rodata \
                    -j .dynamic -j .dynsym -j .rel -j .rela \
                    -j .rel.* -j .rela.* -j .reloc

# 아키텍처별 오브젝트 목록 분리
OBJS_X64  := $(BUILD_DIR)/x64/BOOTX64.o \
             $(patsubst efitest/%.c,$(BUILD_DIR)/x64/%.o,$(EFI_SRCS))

OBJS_IA32 := $(BUILD_DIR)/ia32/BOOTX64.o \
             $(patsubst efitest/%.c,$(BUILD_DIR)/ia32/%.o,$(EFI_SRCS))

.PHONY: x64 ia32 all inject run-x64 clean

x64: BOOTX64.EFI

ia32: BOOTIA32.EFI

all: x64 ia32

# --- X64 빌드 규칙 ---
$(BUILD_DIR)/x64/BOOTX64.o: $(EFI_MAIN) $(EFI_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_X64) -x c -c $< -o $@

$(BUILD_DIR)/x64/%.o: efitest/%.c $(EFI_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_X64) -c $< -o $@

$(BUILD_DIR)/x64/BOOTX64.so: $(OBJS_X64)
	$(LD) -shared -Bsymbolic -L$(EFI_LIB_X64) -T$(EFI_LDS_X64) \
		$(EFI_CRT_X64) $(OBJS_X64) -o $@ -lgnuefi -lefi

BOOTX64.EFI: $(BUILD_DIR)/x64/BOOTX64.so
	$(OBJCOPY) $(OBJCOPY_SECTIONS) -O efi-app-x86_64 --subsystem=10 $< $@

# --- IA32 빌드 규칙 ---
$(BUILD_DIR)/ia32/BOOTX64.o: $(EFI_MAIN) $(EFI_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_IA32) -x c -c $< -o $@

$(BUILD_DIR)/ia32/%.o: efitest/%.c $(EFI_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_IA32) -c $< -o $@

$(BUILD_DIR)/ia32/BOOTIA32.so: $(OBJS_IA32)
	$(LD) -m elf_i386 -shared -Bsymbolic -L$(EFI_LIB_IA32) -T$(EFI_LDS_IA32) \
		$(EFI_CRT_IA32) $(OBJS_IA32) -o $@ -lgnuefi -lefi

BOOTIA32.EFI: $(BUILD_DIR)/ia32/BOOTIA32.so
	$(OBJCOPY) $(OBJCOPY_SECTIONS) -O efi-app-ia32 --subsystem=10 $< $@

# --- 기존 MBR FAT32 이미지 주입 및 QEMU 실행 ---
inject: BOOTX64.EFI
	mmd -D s -i "$(PART)" ::EFI ::EFI/BOOT </dev/null || true
	mcopy -o -i "$(PART)" BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI
	@if [ -f BOOTIA32.EFI ]; then \
		mcopy -o -i "$(PART)" BOOTIA32.EFI ::EFI/BOOT/BOOTIA32.EFI; \
	fi
	mdir -i "$(PART)" ::EFI/BOOT

run-x64: inject
	@mkdir -p $(BUILD_DIR)
	@if [ ! -f "$(BUILD_DIR)/OVMF_VARS_4M.fd" ]; then \
		cp "$(OVMF_VARS)" "$(BUILD_DIR)/OVMF_VARS_4M.fd"; \
	fi
	qemu-system-x86_64 \
		-machine q35,accel=kvm:tcg \
		-m 512 \
		-vga std \
		-drive if=pflash,format=raw,unit=0,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,unit=1,file="$(BUILD_DIR)/OVMF_VARS_4M.fd" \
		-drive file="$(IMG)",format=raw,if=ide,index=0

clean:
	rm -rf $(BUILD_DIR) BOOTX64.EFI BOOTIA32.EFI