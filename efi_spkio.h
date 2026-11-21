#ifndef BOOT_CHECK_SPKIO_H
#define BOOT_CHECK_SPKIO_H

#include "efictx.h"

/* -------------------------------------------------------------------------
 * 저수준 x86 포트 I/O (PC Speaker / PIT Ch2)
 * ------------------------------------------------------------------------- */
UINT8 spk_io_in8(UINT16 port);
VOID  spk_io_out8(UINT16 port, UINT8 val);
VOID  spk_stall_us(EFI_BOOT_SERVICES *bs, UINTN us);

/* -------------------------------------------------------------------------
 * 기본 톤 / 볼륨 PWM / 휴지 프리미티브
 * ------------------------------------------------------------------------- */
VOID  spk_tone(EFI_BOOT_SERVICES *bs, UINT16 freq_hz, UINT16 duration_ms);
VOID  spk_tone_vol(EFI_BOOT_SERVICES *bs, UINT16 freq_hz,
                   UINT16 duration_ms, UINT8 vol_pct);
VOID  spk_rest(EFI_BOOT_SERVICES *bs, UINT16 duration_ms);

/* -------------------------------------------------------------------------
 * 상위 진단 / 알림 루틴
 * ------------------------------------------------------------------------- */
/* console.c: 초기 콘솔(ConOut) 부재/출력 불가 시 짧은 경고 톤 출력 */
VOID  efi_speaker_no_display_alert(EFI_BOOT_SERVICES *bs);

/* Step 7-2: Port 0x61 래치 · PIT OUT2 · 7음절 멜로디 · 35% PWM · 10초 Y/N */
VOID  efi_run_step7_speaker(efi_diag_ctx_t *ctx);

/* 진단 종료 후: 실시간 RTC 화면 및 매시 정각 시보를 무한 대기 루프로 수행 */
VOID  efi_run_radio_time_signal(efi_diag_ctx_t *ctx);

#endif /* BOOT_CHECK_SPKIO_H */