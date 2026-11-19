#include "txtio.h"
#include "logio.h"
#include "spkio.h"

void probe_firmware_and_boot_path(void);
void run_kbd_test(void);
void run_speaker_test(void);
void run_timer_test(void);
void run_mem_test(void);
void run_disk_test(void);
void run_mouse_test(void);
void run_port_test(void);
void run_gfx_test(void);
void run_cpu_test(void);
void run_rtc_and_live_clock(void);

/* ---------------------------------------------------------
 * Step 0 결과 요약: 비디오 · 콘솔 · 로그 대상 출력
 * --------------------------------------------------------- */
void print_io_summary(void)
{
    out_str("=== [Step 0] Basic I/O & Console Check ===\n");

    out_str("  1. Direct VRAM (0x");
    out_hex16(g_io.vram_seg);
    out_str(", CRTC 0x");
    out_hex16(g_io.crtc_port);
    out_str(") : ");
    out_str(g_io.vram_ok ? "SUPPORTED\n" : "NOT AVAILABLE\n");

    out_str("  2. BIOS Video (INT 10h)         : ");
    out_str(g_io.bios_ok ? "SUPPORTED\n" : "NOT AVAILABLE\n");

    out_str("  3. BIOS Kbd   (INT 16h)         : ");
    out_str(g_io.kbd_bios_ok ? "SUPPORTED\n" : "NOT AVAILABLE\n");

    out_str("  4. Selected put_ch driver       : ");
    out_str(g_io.active_out == OUT_VRAM ? "DIRECT VRAM (1 Beep emitted)\n"
                                        : "BIOS INT 10h (1 Beep emitted)\n");

    out_str("  5. Real-time Disk Log Target    : ");
    out_str(g_log_is_file ? "RESULT.TXT (FAT32 Root)\n\n"
                          : "Reserved Sector 7 (RESULT.TXT not found)\n\n");
}

/* ---------------------------------------------------------
 * 메인 목차 (Diagnostics Table of Contents)
 * --------------------------------------------------------- */
void main(void)
{
    /* 최우선: 디스크 실시간 로거 초기화 (화면 먹통 대비) */
    log_init();
    log_str("[BOOT] START.COM entered at 07F0:0100.\n");
    log_str("[SESSION START] BDA_Tick=");
    log_ulong_only(*(volatile uint32_t far *)MK_FP(0x0040, 0x006C));
    log_str("\n");

    /* [0] 비디오 콘솔 검증 및 put_ch 바인딩 */
    probe_and_bind_console();

    /* 출력 불가: 2회 비프 + 디스크 즉시 기록 후 정지 */
    if (g_io.active_out == OUT_NONE) {
        log_str("[Step 0] Console output unavailable (OUT_NONE). Emitting 2 beeps and halting.\n");
        beep_code(2);
        return;
    }

    /* 출력 가능: 정보성 1회 비프 + 요약 출력/기록 */
    beep_code(1);

    /* [Step 0] 결과 요약 화면 출력 + 디스크 실시간 기록 */
    print_io_summary();

    /* [Step 0.5] 부팅 경로(Legacy vs UEFI CSM) 및 펌웨어 지원 옵션 기록 */
    probe_firmware_and_boot_path();

    /* [1] 키보드 테스트 */
    run_kbd_test();

    /* [2] PC 스피커 멜로디 테스트 */
    run_speaker_test();

    /* [3] 타이머 및 DIV 0 호환성 테스트 */
    run_timer_test();

    /* [4] 메모리 맵 · EMS/XMS 가능성 · 충돌 주소 진단 */
    run_mem_test();

    /* [5] 부팅 미디어 에뮬레이션 판별 및 디스크 목록 */
    run_disk_test();

    /* [6] 마우스 인터페이스 수집 · 버튼(더블클릭 바운스 필터) · 이동 진단 */
    run_mouse_test();

    /* [7] 시리얼(COM) · 패러렐(LPT) 및 확장카드 포트 진단 */
    run_port_test();

    /* [8] 그래픽 어댑터 · VESA LFB · 모드 전환 진단 */
    run_gfx_test();

    /* [9] 세분화 CPU 명령어 셋 · 보호모드 · 페이징 선택 진단 */
    run_cpu_test();

    /* [10] CMOS/BIOS 시계 진단 기록 및 timedatectl 스타일 실시간 시계 갱신 */
    run_rtc_and_live_clock();

    /* out_str("All scheduled diagnostics finished. Halted.\n"); */
}