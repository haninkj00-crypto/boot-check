/*
 * BOOTX64.C - boot-check UEFI Diagnostic Entry Point & Table of Contents
 *
 * Build Toolchain : GNU-EFI (GCC + ld + objcopy)
 * Target Arch     : X64 (BOOTX64.EFI) primary, IA32 (BOOTIA32.EFI) compatible 가능성 고려
 */

#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

/* -------------------------------------------------------------------------
 * efitest/ 하위 모듈 프로토타입
 * ------------------------------------------------------------------------- */
void efi_run_step0_console_and_log(efi_diag_ctx_t *ctx);   /* efitest/console.c  */
void efi_run_step0_5_firmware(efi_diag_ctx_t *ctx);        /* efitest/firmware.c */
void efi_run_step1_memory_map(efi_diag_ctx_t *ctx);        /* efitest/mem.c      */
void efi_run_step2_rtc_and_timer(efi_diag_ctx_t *ctx);     /* efitest/rtc.c      */
void efi_run_step3_pci_and_audio(efi_diag_ctx_t *ctx);     /* efitest/pci.c      */
void efi_run_step4_graphics_gop(efi_diag_ctx_t *ctx);      /* efitest/gfx.c      */
void efi_run_step5_usb_stack(efi_diag_ctx_t *ctx);         /* efitest/usb.c      */
void efi_run_step6_storage_fs(efi_diag_ctx_t *ctx);        /* efitest/disk.c     */
void efi_run_step7_network(efi_diag_ctx_t *ctx);           /* efitest/net.c      */
void efi_run_step7_port(efi_diag_ctx_t *ctx);              /* efitest/port.c     */
void efi_run_step7_speaker(efi_diag_ctx_t *ctx);           /* efitest/speaker.c  */
void efi_run_step7_serial_ports(efi_diag_ctx_t *ctx);      /* efitest/serial.c   */
void efi_run_step7_input(efi_diag_ctx_t *ctx);             /* efitest/input.c    */
void efi_run_step8_cpu_features(efi_diag_ctx_t *ctx);      /* efitest/cpu.c      */
void efi_run_radio_time_signal(efi_diag_ctx_t *ctx);       /* efitest/speaker.c  */

/* -------------------------------------------------------------------------
 * 미구현 단계 플레이스홀더 (단계별 구현 시 각 모듈 호출로 교체)
 * -------------------------------------------------------------------------
static VOID run_pending_steps_placeholder(efi_diag_ctx_t *ctx)
{


    efilog_flush(ctx);
} */

/* -------------------------------------------------------------------------
 * UEFI 진입점
 * ------------------------------------------------------------------------- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    efi_diag_ctx_t ctx;

    InitializeLib(ImageHandle, SystemTable);

    /* 5분 UEFI 워치독 타이머 해제 */
    uefi_call_wrapper(SystemTable->BootServices->SetWatchdogTimer, 4,
                      0, 0, 0, NULL);

    SetMem(&ctx, sizeof(ctx), 0);
    ctx.image_handle         = ImageHandle;
    ctx.st                   = SystemTable;
    ctx.allow_connect_rescan = FALSE;
    ctx.allow_gop_setmode    = FALSE;

    /* [Step 0] UEFI 환경 · 텍스트 콘솔 · EFILOG.TXT 초기화 */
    efi_run_step0_console_and_log(&ctx);

    /* [Step 0.5] 펌웨어 테이블 · Secure Boot · CSM · ACPI FADT · SMBIOS */
    efi_run_step0_5_firmware(&ctx);

    /* [Step 1] UEFI 메모리 맵 및 1MB 미만 영역 요약 */
    efi_run_step1_memory_map(&ctx);

    /* [Step 2] RTC 및 UEFI 타이머 서비스 */
    efi_run_step2_rtc_and_timer(&ctx);

    /* [Step 3] PCI/PCIe 전체 인벤토리 · BAR · 4GB MMIO · Audio/USB 요약 */
    efi_run_step3_pci_and_audio(&ctx);

    /* [Step 4] 그래픽: GOP 모드 열거 · 프레임버퍼 · EDID */
    efi_run_step4_graphics_gop(&ctx);

    /* [Step 5] USB 호스트 컨트롤러 · 루트 허브 · 장치 열거 */
    efi_run_step5_usb_stack(&ctx);

    /* [Step 6] Block I/O · 파티션 · 파일시스템 */
    efi_run_step6_storage_fs(&ctx);

    /* [Step 7-1] 네트워크 (PCI NIC vs SNP / PXE / IPv4 / IPv6) */
    efi_run_step7_network(&ctx);

    /* [Step 7-2-1] 시리얼(COM/모뎀) · 병렬(LPT/프린터) */
    efi_run_step7_port(&ctx);

    /* [Step 7-2-2] PC 스피커 */    
    efi_run_step7_speaker(&ctx);

    /* [Step 7-3] 입력 장치 (TextInEx · Pointer · AbsPointer · 8042) */
    efi_run_step7_input(&ctx);

    /* [Step 8] CPU 식별 및 안전 기능 검사 */
    efi_run_step8_cpu_features(&ctx);

    /* 키 대기 전 로그 최종 플러시 및 닫기 (강제 전원 차단 시에도 기록 보존) */
    efi_out_str(&ctx, "All active UEFI diagnostic steps finished. Closing EFILOG.TXT...\n");
    efilog_close(&ctx);

    efi_run_radio_time_signal(&ctx);
    /*
    efi_out_str(&ctx, "Press any key to exit to firmware...\n");
    efi_wait_any_key(&ctx);
    */

    return EFI_SUCCESS;
}