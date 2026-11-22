#ifndef EFICTX_H
#define EFICTX_H

#include <efi.h>
#include <efilib.h>

/* -------------------------------------------------------------------------
 * 아키텍처 식별 매크로 (동일 소스로 BOOTX64.EFI / BOOTIA32.EFI 빌드)
 * ------------------------------------------------------------------------- */
#if defined(__x86_64__) || defined(_M_X64)
  #define EFI_ARCH_TAG        "X64"
  #define EFI_EXEC_MODE_STR   "64-bit Long Mode (UEFI X64)"
  #define EFI_IMAGE_NAME      "BOOTX64.EFI"
  #define EFI_IS_X64          1
#elif defined(__i386__) || defined(_M_IX86)
  #define EFI_ARCH_TAG        "IA32"
  #define EFI_EXEC_MODE_STR   "32-bit Protected Mode (UEFI IA32)"
  #define EFI_IMAGE_NAME      "BOOTIA32.EFI"
  #define EFI_IS_X64          0
#else
  #define EFI_ARCH_TAG        "UNKNOWN"
  #define EFI_EXEC_MODE_STR   "Unknown UEFI Architecture"
  #define EFI_IMAGE_NAME      "BOOT.EFI"
  #define EFI_IS_X64          0
#endif

/* -------------------------------------------------------------------------
 * 6단계 진단 상태 분류 체계 및 드라이버 연결 상태
 * ------------------------------------------------------------------------- */
typedef enum {
    DIAG_NOT_EXPOSED    = 0,
    DIAG_PCI_FOUND      = 1,
    DIAG_DRIVER_BOUND   = 2,
    DIAG_PROTO_AVAIL    = 3,
    DIAG_DEV_ENUM       = 4,
    DIAG_XFER_OK        = 5
} efi_diag_state_t;

typedef enum {
    CONN_BOUND_AT_ENTRY         = 0,
    CONN_UNCONNECTED_AT_ENTRY   = 1,
    CONN_CONNECTED_AFTER_RESCAN = 2
} efi_conn_state_t;

/* -------------------------------------------------------------------------
 * 단계 간 공유 진단 컨텍스트
 * ------------------------------------------------------------------------- */
typedef struct {
    EFI_HANDLE            image_handle;
    EFI_SYSTEM_TABLE     *st;

    /* LoadedImage 정보 (부팅 매체 보호 및 메모리 위치 기록용) */
    EFI_HANDLE            boot_dev_handle;
    EFI_PHYSICAL_ADDRESS  image_base;
    UINT64                image_size;

    /* EFILOG.TXT 핸들 및 상태 (레거시 RESULT.TXT와 완전 분리) */
    EFI_FILE_HANDLE       root_dir;
    EFI_FILE_HANDLE       log_file;
    BOOLEAN               log_ready;
    BOOLEAN               log_appended;
    UINT64                log_initial_size;
    EFI_STATUS            log_init_status;

    /* ACPI FADT IAPC_BOOT_ARCH (offset 109, 16-bit) */
    BOOLEAN               fadt_valid;
    UINT16                iapc_boot_arch;

    /* 펌웨어/옵션 제어 플래그 */
    BOOLEAN               csm_present;
    BOOLEAN               allow_connect_rescan;
    BOOLEAN               allow_gop_setmode;

    /* PC 스피커 */
    BOOLEAN spk_audible_or_partial;
} efi_diag_ctx_t;

static inline VOID bc_set_mem(VOID *dst, UINTN len, UINT8 val)
{
    volatile UINT8 *p = (volatile UINT8 *)dst;
    while (len-- > 0U) {
        *p++ = val;
    }
}

static inline VOID bc_zero_mem(VOID *dst, UINTN len)
{
    bc_set_mem(dst, len, 0U);
}

static inline VOID bc_copy_mem(VOID *dst, const VOID *src, UINTN len)
{
    volatile UINT8       *d = (volatile UINT8 *)dst;
    const volatile UINT8 *s = (const volatile UINT8 *)src;
    while (len-- > 0U) {
        *d++ = *s++;
    }
}

#ifdef SetMem
#undef SetMem
#endif
#define SetMem(buf, len, val) bc_set_mem((buf), (len), (UINT8)(val))

#endif /* EFICTX_H */