#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#define TIMER_100NS_PER_MS    10000ULL
#define STALL_CAL_US          50000U   /* 50 ms 기준 캘리브레이션 */
#define REL_TARGET_MS         50ULL    /* 단발 타이머 목표: 50 ms */
#define PER_TARGET_MS         20ULL    /* 주기 타이머 목표: 20 ms */
#define PER_SAMPLE_COUNT      10U      /* 주기 타이머 측정 횟수 (총 ~200 ms) */

#ifndef EFI_UNSPECIFIED_TIMEZONE
#define EFI_UNSPECIFIED_TIMEZONE 0x07FF
#endif

/* -------------------------------------------------------------------------
 * 내부 헬퍼: IA32 -ffreestanding 안전 64비트 나눗셈 및 CPUID/RDTSC
 * ------------------------------------------------------------------------- */
static UINT64 u64_div(UINT64 num, UINT64 den)
{
    UINT64 q = 0;
    UINT64 r = 0;
    UINTN  i;

    if (den == 0) {
        return 0;
    }
    for (i = 0; i < 64; i++) {
        r = (r << 1) | ((num >> 63) & 1ULL);
        num <<= 1;
        q <<= 1;
        if (r >= den) {
            r -= den;
            q |= 1ULL;
        }
    }
    return q;
}

static BOOLEAN cpu_has_tsc(void)
{
    UINT32 eax = 1U, ebx = 0U, ecx = 0U, edx = 0U;

#if EFI_IS_X64
    __asm__ __volatile__ (
        "cpuid"
        : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
    );
#else
    /* IA32 -fpic 환경에서 EBX(PIC 레지스터) 보존 */
    __asm__ __volatile__ (
        "xchgl %%ebx, %1\n\t"
        "cpuid\n\t"
        "xchgl %%ebx, %1"
        : "+a"(eax), "=&r"(ebx), "=c"(ecx), "=d"(edx)
    );
#endif
    (void)eax;
    (void)ebx;
    (void)ecx;
    return ((edx & (1U << 4)) != 0U) ? TRUE : FALSE;
}

static UINT64 read_rdtsc(void)
{
    UINT32 lo = 0, hi = 0;

    __asm__ __volatile__ (
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );
    return ((UINT64)hi << 32) | (UINT64)lo;
}

static VOID out_u64_pad3(efi_diag_ctx_t *ctx, UINT64 val)
{
    if (val < 10ULL) {
        efi_out_str(ctx, "00");
    } else if (val < 100ULL) {
        efi_out_str(ctx, "0");
    }
    efi_out_u64(ctx, val);
}

static VOID out_us_as_ms(efi_diag_ctx_t *ctx, UINT64 us)
{
    UINT64 ms_int  = u64_div(us, 1000ULL);
    UINT64 ms_frac = us - (ms_int * 1000ULL);

    efi_out_u64(ctx, ms_int);
    efi_out_str(ctx, ".");
    out_u64_pad3(ctx, ms_frac);
    efi_out_str(ctx, " ms");
}

static UINT64 tsc_delta_to_us(UINT64 delta_tsc, UINT64 tsc_per_ms)
{
    if (tsc_per_ms == 0) {
        return 0;
    }
    if (delta_tsc > 0x0010000000000000ULL) {
        return u64_div(delta_tsc, tsc_per_ms) * 1000ULL;
    }
    return u64_div(delta_tsc * 1000ULL, tsc_per_ms);
}

/* -------------------------------------------------------------------------
 * [1] RuntimeServices->GetTime() 및 FADT CMOS RTC 플래그 대조
 * ------------------------------------------------------------------------- */
static VOID check_runtime_rtc(efi_diag_ctx_t *ctx)
{
    EFI_RUNTIME_SERVICES  *rt = ctx->st->RuntimeServices;
    EFI_TIME               t;
    EFI_TIME_CAPABILITIES  caps;
    BOOLEAN                caps_ok = FALSE;
    EFI_STATUS             st;

    SetMem(&t, sizeof(t), 0);
    SetMem(&caps, sizeof(caps), 0);

    efi_out_str(ctx, "  [1] Runtime Services RTC (GetTime)\n");

    if (rt == NULL || rt->GetTime == NULL) {
        efi_out_str(ctx, "    GetTime()      : NOT EXPOSED (No RuntimeServices)\n");
        return;
    }

    st = uefi_call_wrapper(rt->GetTime, 2, &t, &caps);
    if (!EFI_ERROR(st)) {
        caps_ok = TRUE;
    } else {
        /* 일부 펌웨어는 Capabilities 포인터가 NULL일 때만 성공함 */
        st = uefi_call_wrapper(rt->GetTime, 2, &t, NULL);
    }

    if (EFI_ERROR(st)) {
        efi_out_str(ctx, "    GetTime()      : FAILED - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
        return;
    }

    efi_out_str(ctx, "    Current Time   : ");
    efi_out_u64(ctx, (UINT64)t.Year);
    efi_out_str(ctx, "-");
    efi_out_u64_pad2(ctx, (UINT64)t.Month);
    efi_out_str(ctx, "-");
    efi_out_u64_pad2(ctx, (UINT64)t.Day);
    efi_out_str(ctx, " ");
    efi_out_u64_pad2(ctx, (UINT64)t.Hour);
    efi_out_str(ctx, ":");
    efi_out_u64_pad2(ctx, (UINT64)t.Minute);
    efi_out_str(ctx, ":");
    efi_out_u64_pad2(ctx, (UINT64)t.Second);
    if (t.Nanosecond > 0) {
        efi_out_str(ctx, ".");
        out_u64_pad3(ctx, u64_div((UINT64)t.Nanosecond, 1000000ULL));
    }
    efi_out_str(ctx, "\n");

    /* TimeZone (UEFI 규격: LocalTime = UTC - TimeZone) */
    efi_out_str(ctx, "    TimeZone       : ");
    if (t.TimeZone == (INT16)EFI_UNSPECIFIED_TIMEZONE) {
        efi_out_str(ctx, "UNSPECIFIED (0x07FF)\n");
    } else if (t.TimeZone >= -1440 && t.TimeZone <= 1440) {
        INT32 utc_off = -(INT32)t.TimeZone;
        UINT64 abs_off = (utc_off < 0) ? (UINT64)(-utc_off) : (UINT64)utc_off;
        UINT64 hrs = u64_div(abs_off, 60ULL);
        UINT64 min = abs_off - (hrs * 60ULL);

        if (t.TimeZone < 0) {
            efi_out_str(ctx, "-");
            efi_out_u64(ctx, (UINT64)(-(INT32)t.TimeZone));
        } else {
            efi_out_u64(ctx, (UINT64)t.TimeZone);
        }
        efi_out_str(ctx, " min (UTC");
        efi_out_str(ctx, (utc_off < 0) ? "-" : "+");
        efi_out_u64_pad2(ctx, hrs);
        efi_out_str(ctx, ":");
        efi_out_u64_pad2(ctx, min);
        efi_out_str(ctx, ")\n");
    } else {
        efi_out_str(ctx, "OUT OF RANGE (0x");
        efi_out_hex16(ctx, (UINT16)t.TimeZone);
        efi_out_str(ctx, ")\n");
    }

    efi_out_str(ctx, "    Daylight Flags : 0x");
    efi_out_hex8(ctx, t.Daylight);
    efi_out_str(ctx, " [Adjust=");
    efi_out_str(ctx, (t.Daylight & 0x01U) ? "YES" : "NO");
    efi_out_str(ctx, ", InDST=");
    efi_out_str(ctx, (t.Daylight & 0x02U) ? "YES" : "NO");
    efi_out_str(ctx, "]\n");

    efi_out_str(ctx, "    RTC Caps       : ");
    if (caps_ok) {
        efi_out_str(ctx, "Resolution=");
        efi_out_u64(ctx, (UINT64)caps.Resolution);
        efi_out_str(ctx, " Hz, Accuracy=");
        efi_out_u64(ctx, (UINT64)caps.Accuracy);
        efi_out_str(ctx, " (1e-6 ppm), SetsToZero=");
        efi_out_str(ctx, caps.SetsToZero ? "YES\n" : "NO\n");
    } else {
        efi_out_str(ctx, "NOT EXPOSED (GetTime succeeded only with NULL caps)\n");
    }

    efi_out_str(ctx, "    FADT CMOS RTC  : ");
    if (!ctx->fadt_valid) {
        efi_out_str(ctx, "UNKNOWN (FADT IAPC_BOOT_ARCH unavailable)\n");
    } else if (ctx->iapc_boot_arch & 0x0020U) {
        efi_out_str(ctx, "CMOS_RTC_NOT_PRESENT=1 (Legacy CMOS RTC absent; firmware/virtual RTC)\n");
    } else {
        efi_out_str(ctx, "CMOS_RTC_NOT_PRESENT=0 (Legacy CMOS RTC declared present)\n");
    }
}

/* -------------------------------------------------------------------------
 * [2] UEFI Boot Services Timer Event 지연 및 주기 편차 측정
 * ------------------------------------------------------------------------- */
static VOID check_uefi_timer_events(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    EFI_EVENT          ev = NULL;
    UINTN              ev_idx = 0;
    EFI_STATUS         st;
    BOOLEAN            tsc_ok = FALSE;
    UINT64             tsc_per_ms = 0;
    UINT64             t0, t1, dt;

    efi_out_str(ctx, "  [2] UEFI Timer Services & Event Jitter\n");

    if (bs == NULL || bs->CreateEvent == NULL || bs->SetTimer == NULL ||
        bs->WaitForEvent == NULL || bs->CloseEvent == NULL) {
        efi_out_str(ctx, "    Timer Services : NOT EXPOSED\n");
        return;
    }

    /* 타이머 대기 진입 전 로그 플러시 */
    efilog_flush(ctx);

    /* (a) BS->Stall(50ms) 기준 RDTSC 눈금 산출 */
    if (cpu_has_tsc() && bs->Stall != NULL) {
        t0 = read_rdtsc();
        st = uefi_call_wrapper(bs->Stall, 1, (UINTN)STALL_CAL_US);
        t1 = read_rdtsc();
        if (!EFI_ERROR(st) && t1 > t0) {
            dt = t1 - t0;
            tsc_per_ms = u64_div(dt, (UINT64)(STALL_CAL_US / 1000U));
            if (tsc_per_ms > 0) {
                tsc_ok = TRUE;
            }
        }
    }

    efi_out_str(ctx, "    TSC Reference  : ");
    if (tsc_ok) {
        efi_out_u64(ctx, tsc_per_ms);
        efi_out_str(ctx, " ticks/ms (~");
        efi_out_u64(ctx, u64_div(tsc_per_ms, 1000ULL));
        efi_out_str(ctx, " MHz via 50ms BS->Stall)\n");
    } else {
        efi_out_str(ctx, "UNAVAILABLE (Event pass/fail only)\n");
    }

    /* (b) Timer Event 생성 */
    st = uefi_call_wrapper(bs->CreateEvent, 5,
                           EVT_TIMER, TPL_CALLBACK, NULL, NULL, &ev);
    if (EFI_ERROR(st) || ev == NULL) {
        efi_out_str(ctx, "    CreateEvent()  : FAILED - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
        return;
    }

    /* (c) 단발 TimerRelative (50 ms) */
    efi_out_str(ctx, "    One-Shot (50ms): ");
    t0 = tsc_ok ? read_rdtsc() : 0ULL;
    st = uefi_call_wrapper(bs->SetTimer, 3,
                           ev, TimerRelative, REL_TARGET_MS * TIMER_100NS_PER_MS);
    if (EFI_ERROR(st)) {
        efi_out_str(ctx, "SetTimer FAILED - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
    } else {
        st = uefi_call_wrapper(bs->WaitForEvent, 3, 1, &ev, &ev_idx);
        t1 = tsc_ok ? read_rdtsc() : 0ULL;
        if (EFI_ERROR(st)) {
            efi_out_str(ctx, "WaitForEvent FAILED - ");
            efi_out_status(ctx, st);
            efi_out_str(ctx, "\n");
        } else {
            efi_out_str(ctx, "PASS");
            if (tsc_ok && t1 > t0) {
                efi_out_str(ctx, " (actual=");
                out_us_as_ms(ctx, tsc_delta_to_us(t1 - t0, tsc_per_ms));
                efi_out_str(ctx, ")");
            }
            efi_out_str(ctx, "\n");
        }
    }

    /* (d) 주기 TimerPeriodic (20 ms x 10회) */
    efi_out_str(ctx, "    Periodic (20ms): ");
    st = uefi_call_wrapper(bs->SetTimer, 3,
                           ev, TimerPeriodic, PER_TARGET_MS * TIMER_100NS_PER_MS);
    if (EFI_ERROR(st)) {
        efi_out_str(ctx, "SetTimer FAILED - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
    } else {
        UINTN  k;
        UINT64 prev_tsc = tsc_ok ? read_rdtsc() : 0ULL;
        UINT64 min_dt = 0xFFFFFFFFFFFFFFFFULL;
        UINT64 max_dt = 0ULL;
        UINT64 sum_dt = 0ULL;
        UINTN  ok_cnt = 0;

        for (k = 0; k < PER_SAMPLE_COUNT; k++) {
            st = uefi_call_wrapper(bs->WaitForEvent, 3, 1, &ev, &ev_idx);
            if (EFI_ERROR(st)) {
                break;
            }
            if (tsc_ok) {
                UINT64 now_tsc = read_rdtsc();
                UINT64 step_dt = (now_tsc > prev_tsc) ? (now_tsc - prev_tsc) : 0ULL;
                prev_tsc = now_tsc;
                if (step_dt < min_dt) min_dt = step_dt;
                if (step_dt > max_dt) max_dt = step_dt;
                sum_dt += step_dt;
            }
            ok_cnt++;
        }

        uefi_call_wrapper(bs->SetTimer, 3, ev, TimerCancel, 0ULL);

        if (ok_cnt == PER_SAMPLE_COUNT) {
            efi_out_str(ctx, "PASS (10/10 ticks");
            if (tsc_ok && sum_dt > 0) {
                UINT64 avg_dt = u64_div(sum_dt, (UINT64)PER_SAMPLE_COUNT);
                efi_out_str(ctx, ", min=");
                out_us_as_ms(ctx, tsc_delta_to_us(min_dt, tsc_per_ms));
                efi_out_str(ctx, ", avg=");
                out_us_as_ms(ctx, tsc_delta_to_us(avg_dt, tsc_per_ms));
                efi_out_str(ctx, ", max=");
                out_us_as_ms(ctx, tsc_delta_to_us(max_dt, tsc_per_ms));
            }
            efi_out_str(ctx, ")\n");
        } else {
            efi_out_str(ctx, "FAILED at tick ");
            efi_out_u64(ctx, (UINT64)ok_cnt);
            efi_out_str(ctx, " - ");
            efi_out_status(ctx, st);
            efi_out_str(ctx, "\n");
        }
    }

    uefi_call_wrapper(bs->CloseEvent, 1, ev);
}

/* -------------------------------------------------------------------------
 * Step 2 메인 엔트리
 * ------------------------------------------------------------------------- */
void efi_run_step2_rtc_and_timer(efi_diag_ctx_t *ctx)
{
    efi_out_str(ctx, "=== [Step 2] Runtime RTC & UEFI Timer Services ===\n");

    check_runtime_rtc(ctx);
    check_uefi_timer_events(ctx);

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}