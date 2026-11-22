#include "../efi_spkio.h"
#include "efilog.h"
#include "efitxt.h"

static BOOLEAN g_efi_spk_latch_ok           = FALSE;
static BOOLEAN g_efi_spk_audible_or_partial = FALSE;

UINT8 spk_io_in8(UINT16 port)
{
    UINT8 val;

    __asm__ __volatile__ ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

VOID spk_io_out8(UINT16 port, UINT8 val)
{
    __asm__ __volatile__ ("outb %0, %1" : : "a"(val), "Nd"(port));
}

VOID spk_stall_us(EFI_BOOT_SERVICES *bs, UINTN us)
{
    if (bs != NULL && bs->Stall != NULL) {
        uefi_call_wrapper(bs->Stall, 1, us);
    }
}

VOID spk_rest(EFI_BOOT_SERVICES *bs, UINT16 duration_ms)
{
    spk_stall_us(bs, (UINTN)duration_ms * 1000U);
}

VOID spk_tone(EFI_BOOT_SERVICES *bs, UINT16 freq_hz, UINT16 duration_ms)
{
    UINT16 divisor;
    UINT8  base_p61, rb;

    if (freq_hz < 20U || duration_ms == 0U) {
        return;
    }

    divisor = (UINT16)(1193182U / (UINT32)freq_hz);
    spk_io_out8(0x0043U, 0xB6U); /* PIT Ch2, Lobyte/Hibyte, Mode 3 Square Wave */
    spk_io_out8(0x0042U, (UINT8)(divisor & 0xFFU));
    spk_io_out8(0x0042U, (UINT8)(divisor >> 8));

    base_p61 = (UINT8)(spk_io_in8(0x0061U) & ~0x03U);
    spk_io_out8(0x0061U, (UINT8)(base_p61 | 0x03U));

    rb = spk_io_in8(0x0061U);
    if (rb != 0xFFU && (rb & 0x03U) == 0x03U) {
        g_efi_spk_latch_ok = TRUE;
    }

    spk_stall_us(bs, (UINTN)duration_ms * 1000U);
    spk_io_out8(0x0061U, base_p61);
}

VOID spk_tone_vol(EFI_BOOT_SERVICES *bs, UINT16 freq_hz,
                  UINT16 duration_ms, UINT8 vol_pct)
{
    UINT16 divisor;
    UINT8  base_p61;
    UINTN  on_us, off_us, cycles, c;

    if (freq_hz < 20U || duration_ms == 0U) {
        return;
    }
    if (vol_pct == 0U) {
        spk_rest(bs, duration_ms);
        return;
    }
    if (vol_pct >= 100U) {
        spk_tone(bs, freq_hz, duration_ms);
        return;
    }

    divisor = (UINT16)(1193182U / (UINT32)freq_hz);
    spk_io_out8(0x0043U, 0xB6U);
    spk_io_out8(0x0042U, (UINT8)(divisor & 0xFFU));
    spk_io_out8(0x0042U, (UINT8)(divisor >> 8));

    base_p61 = (UINT8)(spk_io_in8(0x0061U) & ~0x03U);

    /* 20 kHz(50us 주기) 초음파 PWM: Gate(Bit0) 유지, SpkData(Bit1) 듀티 제어 */
    on_us = ((UINTN)vol_pct * 50U) / 100U;
    if (on_us == 0U)  on_us = 1U;
    if (on_us >= 50U) on_us = 49U;
    off_us = 50U - on_us;
    cycles = (UINTN)duration_ms * 20U;

    for (c = 0U; c < cycles; c++) {
        spk_io_out8(0x0061U, (UINT8)(base_p61 | 0x03U));
        spk_stall_us(bs, on_us);
        spk_io_out8(0x0061U, (UINT8)(base_p61 | 0x01U));
        spk_stall_us(bs, off_us);
    }

    spk_io_out8(0x0061U, base_p61);
}

/* -------------------------------------------------------------------------
 * 초기 화면(ConOut) 부재 시 1회 경고 톤 (A4 -> E5 짧은 2음)
 * ------------------------------------------------------------------------- */
VOID efi_speaker_no_display_alert(EFI_BOOT_SERVICES *bs)
{
    UINT8 orig_p61;

    if (bs == NULL) {
        return;
    }
    orig_p61 = spk_io_in8(0x0061U);
    if (orig_p61 == 0xFFU) {
        return;
    }

    spk_tone(bs, 440U, 140U);
    spk_rest(bs, 60U);
    spk_tone(bs, 659U, 220U);
    spk_io_out8(0x0061U, (UINT8)(orig_p61 & ~0x03U));
}

/* -------------------------------------------------------------------------
 * Step 7-2: PC Speaker 멜로디 · 35% PWM 에코 · 10초 [Y]/[N] 청음 확인
 * ------------------------------------------------------------------------- */
VOID efi_run_step7_speaker(efi_diag_ctx_t *ctx)
{
    /* 반짝반짝 작은별 / ABCD송 앞 7음절: 도 도 솔 솔 라 라 솔 */
    static const UINT16 kNotes[7] = { 523U, 523U, 784U, 784U, 880U, 880U, 784U };
    static const UINT16 kDurs[7]  = { 260U, 260U, 260U, 260U, 260U, 260U, 520U };

    EFI_BOOT_SERVICES *bs;
    UINT8   orig_p61, prev_out2;
    UINTN   i, k, edges = 0U;
    char    ans = 0;

    g_efi_spk_latch_ok           = FALSE;
    g_efi_spk_audible_or_partial = FALSE;

    efi_out_str(ctx, "=== [Step 7-2b] PC Speaker Melody & Audible Check ===\n");
    efilog_flush(ctx);

    if (ctx == NULL || ctx->st == NULL || ctx->st->BootServices == NULL) {
        efi_out_str(ctx, "  BootServices UNAVAILABLE\n\n");
        return;
    }
    bs = ctx->st->BootServices;

    orig_p61 = spk_io_in8(0x0061U);
    if (orig_p61 == 0xFFU) {
        efi_out_str(ctx, "  Port 0x61 Latch Status : NO LATCH (0xFF/Static)\n");
        efi_out_str(ctx, "  Melody Playback        : SKIPPED (No Port 0x61 hardware)\n\n");
        efilog_flush(ctx);
        return;
    }

    /* PIT Ch2 OUT2(Port 0x61 Bit 5) 칩셋 발진 보조 계측 (스피커 출력 OFF) */
    spk_io_out8(0x0043U, 0xB6U);
    spk_io_out8(0x0042U, 0xA9U);
    spk_io_out8(0x0042U, 0x04U);
    spk_io_out8(0x0061U, (UINT8)((orig_p61 & ~0x03U) | 0x01U));

    prev_out2 = (UINT8)(spk_io_in8(0x0061U) & 0x20U);
    for (k = 0U; k < 200U; k++) {
        UINT8 cur = (UINT8)(spk_io_in8(0x0061U) & 0x20U);
        if (cur != prev_out2) {
            edges++;
            prev_out2 = cur;
        }
        spk_stall_us(bs, 50U);
    }
    spk_io_out8(0x0061U, (UINT8)(orig_p61 & ~0x03U));

    efi_out_str(ctx, "  Playing first 7 notes of Twinkle Star / ABCD (C-C-G-G-A-A-G)...\n");
    efilog_flush(ctx);

    for (i = 0U; i < 7U; i++) {
        spk_tone(bs, kNotes[i], kDurs[i]);
        spk_rest(bs, 55U);
    }

    /* PWM 볼륨 감쇠 확인용 짧은 에코 1음 (솔 35% 볼륨) */
    spk_tone_vol(bs, 784U, 220U, 35U);
    spk_io_out8(0x0061U, (UINT8)(orig_p61 & ~0x03U));

    efi_out_str(ctx, "  Port 0x61 Latch Status : ");
    efi_out_str(ctx, g_efi_spk_latch_ok ? "LATCH DETECTED\n" : "NO LATCH (0xFF/Static)\n");
    efi_out_str(ctx, "  PIT Ch2 OUT2 Feedback  : ");
    if (edges >= 4U) {
        efi_out_str(ctx, "OSCILLATING (");
        efi_out_u64(ctx, (UINT64)edges);
        efi_out_str(ctx, " edges/10ms)\n");
    } else {
        efi_out_str(ctx, "STATIC (");
        efi_out_u64(ctx, (UINT64)edges);
        efi_out_str(ctx, " edges/10ms)\n");
    }

    efi_out_str(ctx, "  Did you hear the 7-note melody? Press [Y] or [N] (10s timeout): ");
    efilog_flush(ctx);

    if (ctx->st->ConIn != NULL && bs->CreateEvent != NULL &&
        bs->SetTimer != NULL && bs->WaitForEvent != NULL) {
        EFI_EVENT     ev_timer = NULL;
        EFI_EVENT     ev_list[2];
        EFI_INPUT_KEY key;
        UINTN         ev_idx = 0U;
        UINTN         guard  = 0U;

        uefi_call_wrapper(ctx->st->ConIn->Reset, 2, ctx->st->ConIn, FALSE);
        while (guard++ < 64U &&
               !EFI_ERROR(uefi_call_wrapper(ctx->st->ConIn->ReadKeyStroke, 2,
                                            ctx->st->ConIn, &key))) {
            /* drain */
        }

        if (!EFI_ERROR(uefi_call_wrapper(bs->CreateEvent, 5,
                                         EVT_TIMER, TPL_CALLBACK, NULL, NULL, &ev_timer))) {
            uefi_call_wrapper(bs->SetTimer, 3, ev_timer, TimerRelative, 100000000ULL);
            ev_list[0] = ctx->st->ConIn->WaitForKey;
            ev_list[1] = ev_timer;

            guard = 0U;
            while (guard++ < 256U) {
                EFI_STATUS wst = uefi_call_wrapper(bs->WaitForEvent, 3, 2, ev_list, &ev_idx);
                if (EFI_ERROR(wst) || ev_idx == 1U) {
                    break;
                }
                if (!EFI_ERROR(uefi_call_wrapper(ctx->st->ConIn->ReadKeyStroke, 2,
                                                 ctx->st->ConIn, &key))) {
                    if (key.UnicodeChar == L'y' || key.UnicodeChar == L'Y') {
                        ans = 'Y';
                        break;
                    }
                    if (key.UnicodeChar == L'n' || key.UnicodeChar == L'N') {
                        ans = 'N';
                        break;
                    }
                }
            }
            uefi_call_wrapper(bs->CloseEvent, 1, ev_timer);
        }
    }

    if (ans == 'Y') {
        g_efi_spk_audible_or_partial = TRUE;
        efi_out_str(ctx, "YES (Audible Confirmed -> Radio Time Signal Armed)\n\n");
    } else if (ans == 'N') {
        g_efi_spk_audible_or_partial = g_efi_spk_latch_ok;
        if (g_efi_spk_audible_or_partial) {
            efi_out_str(ctx, "NO (Muted, but 0x61 Latch OK -> Partial Pass)\n\n");
        } else {
            efi_out_str(ctx, "NO (Muted / Piezo Absent)\n\n");
        }
    } else {
        g_efi_spk_audible_or_partial = g_efi_spk_latch_ok;
        if (g_efi_spk_audible_or_partial) {
            efi_out_str(ctx, "TIMEOUT (Port 0x61 Latch OK -> Partial Pass)\n\n");
        } else {
            efi_out_str(ctx, "TIMEOUT (No response & No latch)\n\n");
        }
    }

    ctx->spk_audible_or_partial = g_efi_spk_audible_or_partial;
    spk_io_out8(0x0061U, (UINT8)(orig_p61 & ~0x03U));
    efilog_flush(ctx);
}

/* -------------------------------------------------------------------------
 * 진단 종료 후 실시간 시계 화면 및 매시 정각 시보
 * MBR test/rtc.c의 대기 루프를 UEFI RuntimeServices / BootServices로 대체
 * ------------------------------------------------------------------------- */
#ifndef EFI_UNSPECIFIED_TIMEZONE
#define EFI_UNSPECIFIED_TIMEZONE 0x07FF
#endif

#define LIVE_CLOCK_POLL_100NS 1000000ULL  /* 100 ms */
#define LIVE_CLOCK_FALLBACK_US 100000U     /* 타이머 이벤트 사용 불가 시 */

static const char *efi_live_weekday(const EFI_TIME *t)
{
    static const UINT8 month_offset[12] = {
        0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U
    };
    static const char *names[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    UINT32 year;
    UINT32 weekday;

    if (t == NULL || t->Month < 1U || t->Month > 12U ||
        t->Day < 1U || t->Day > 31U) {
        return "---";
    }

    year = (UINT32)t->Year;
    if (t->Month < 3U) {
        year--;
    }
    weekday = (year + year / 4U - year / 100U + year / 400U +
               month_offset[t->Month - 1U] + (UINT32)t->Day) % 7U;
    return names[weekday];
}

static BOOLEAN efi_live_time_valid(const EFI_TIME *t)
{
    UINT8 days_in_month;
    BOOLEAN leap_year;

    if (t == NULL || t->Year < 1U || t->Year > 9999U ||
        t->Month < 1U || t->Month > 12U ||
        t->Hour > 23U || t->Minute > 59U || t->Second > 59U) {
        return FALSE;
    }

    leap_year = ((t->Year % 4U) == 0U &&
                 ((t->Year % 100U) != 0U || (t->Year % 400U) == 0U))
                ? TRUE : FALSE;
    days_in_month = 31U;
    if (t->Month == 4U || t->Month == 6U ||
        t->Month == 9U || t->Month == 11U) {
        days_in_month = 30U;
    } else if (t->Month == 2U) {
        days_in_month = leap_year ? 29U : 28U;
    }
    return (t->Day >= 1U && t->Day <= days_in_month) ? TRUE : FALSE;
}

static VOID efi_live_clock_line(efi_diag_ctx_t *ctx,
                                const EFI_TIME *t,
                                BOOLEAN time_valid,
                                UINTN row)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *con;
    INT32 tz_minutes;

    if (ctx == NULL || ctx->st == NULL || ctx->st->ConOut == NULL) {
        return;
    }
    con = ctx->st->ConOut;

    /* MBR의 '\r  [LIVE]'처럼 동일 행을 덮어쓴다. */
    if (con->SetCursorPosition == NULL ||
        EFI_ERROR(uefi_call_wrapper(con->SetCursorPosition, 3,
                                    con, 0U, row))) {
        return;
    }

    efi_out_str(ctx, "  [LIVE] ");
    if (!time_valid) {
        efi_out_str(ctx, "RTC UNAVAILABLE");
    } else {
        efi_out_str(ctx, efi_live_weekday(t));
        efi_out_str(ctx, " ");
        efi_out_u64(ctx, (UINT64)t->Year);
        efi_out_str(ctx, "-");
        efi_out_u64_pad2(ctx, (UINT64)t->Month);
        efi_out_str(ctx, "-");
        efi_out_u64_pad2(ctx, (UINT64)t->Day);
        efi_out_str(ctx, " ");
        efi_out_u64_pad2(ctx, (UINT64)t->Hour);
        efi_out_str(ctx, ":");
        efi_out_u64_pad2(ctx, (UINT64)t->Minute);
        efi_out_str(ctx, ":");
        efi_out_u64_pad2(ctx, (UINT64)t->Second);
        efi_out_str(ctx, " RTC TZ=");

        if (t->TimeZone == (INT16)EFI_UNSPECIFIED_TIMEZONE) {
            efi_out_str(ctx, "UNSPEC");
        } else if (t->TimeZone >= -1440 && t->TimeZone <= 1440) {
            tz_minutes = -(INT32)t->TimeZone;
            efi_out_str(ctx, (tz_minutes < 0) ? "-" : "+");
            if (tz_minutes < 0) {
                tz_minutes = -tz_minutes;
            }
            efi_out_u64_pad2(ctx, (UINT64)(tz_minutes / 60));
            efi_out_str(ctx, ":");
            efi_out_u64_pad2(ctx, (UINT64)(tz_minutes % 60));
        } else {
            efi_out_str(ctx, "INVALID");
        }
    }

    /* TZ가 UNSPEC/INVALID에서 숫자 오프셋으로 바뀌어도 잔여 문자를 지운다. */
    efi_out_str(ctx, "                ");
}

static BOOLEAN efi_live_time_changed(const EFI_TIME *a, const EFI_TIME *b)
{
    return (a->Year   != b->Year   || a->Month  != b->Month ||
            a->Day    != b->Day    || a->Hour   != b->Hour  ||
            a->Minute != b->Minute || a->Second != b->Second ||
            a->TimeZone != b->TimeZone) ? TRUE : FALSE;
}

static VOID efi_live_drain_keys(efi_diag_ctx_t *ctx)
{
    EFI_INPUT_KEY key;

    if (ctx == NULL || ctx->st == NULL || ctx->st->ConIn == NULL ||
        ctx->st->ConIn->ReadKeyStroke == NULL) {
        return;
    }
    while (!EFI_ERROR(uefi_call_wrapper(ctx->st->ConIn->ReadKeyStroke, 2,
                                        ctx->st->ConIn, &key))) {
        /* MBR처럼 입력을 소비하되 대기 루프는 종료하지 않는다. */
    }
}

VOID efi_run_radio_time_signal(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES    *bs;
    EFI_RUNTIME_SERVICES *rt;
    SIMPLE_TEXT_OUTPUT_INTERFACE *con;
    EFI_TIME              now;
    EFI_TIME              previous;
    EFI_EVENT             timer_event = NULL;
    EFI_EVENT             events[2];
    UINTN                 event_count;
    UINTN                 event_index;
    UINTN                 live_row = 0U;
    BOOLEAN               have_previous = FALSE;
    BOOLEAN               timer_ready = FALSE;
    BOOLEAN               have_console;
    BOOLEAN               chime_armed = FALSE;
    BOOLEAN               speaker_ready;
    BOOLEAN               unavailable_displayed = FALSE;

    if (ctx == NULL || ctx->st == NULL || ctx->st->BootServices == NULL) {
        return;
    }

    bs = ctx->st->BootServices;
    rt = ctx->st->RuntimeServices;
    con = ctx->st->ConOut;
    have_console = (con != NULL) ? TRUE : FALSE;
    speaker_ready = (ctx->spk_audible_or_partial ||
                     g_efi_spk_audible_or_partial) ? TRUE : FALSE;

    efi_out_str(ctx, "All scheduled diagnostics finished. Entering live clock monitor...\n");
    if (have_console && con->Mode != NULL) {
        live_row = (UINTN)con->Mode->CursorRow;
    }

    /* 100ms 주기 타이머. 실패하면 BootServices->Stall()로 폴링한다. */
    if (bs->CreateEvent != NULL && bs->SetTimer != NULL &&
        bs->WaitForEvent != NULL && bs->CloseEvent != NULL) {
        EFI_STATUS st;
        st = uefi_call_wrapper(bs->CreateEvent, 5,
                               EVT_TIMER, TPL_CALLBACK, NULL, NULL, &timer_event);
        if (!EFI_ERROR(st) && timer_event != NULL) {
            st = uefi_call_wrapper(bs->SetTimer, 3, timer_event,
                                   TimerPeriodic, LIVE_CLOCK_POLL_100NS);
            if (!EFI_ERROR(st)) {
                timer_ready = TRUE;
            } else {
                uefi_call_wrapper(bs->CloseEvent, 1, timer_event);
                timer_event = NULL;
            }
        }
    }

    for (;;) {
        EFI_STATUS gst = EFI_UNSUPPORTED;
        BOOLEAN time_valid = FALSE;
        BOOLEAN time_changed = FALSE;
        BOOLEAN second_changed = FALSE;

        if (timer_ready) {
            event_count = 0U;
            if (ctx->st->ConIn != NULL && ctx->st->ConIn->WaitForKey != NULL) {
                events[event_count++] = ctx->st->ConIn->WaitForKey;
            }
            events[event_count++] = timer_event;
            gst = uefi_call_wrapper(bs->WaitForEvent, 3,
                                    event_count, events, &event_index);
            if (EFI_ERROR(gst)) {
                uefi_call_wrapper(bs->CloseEvent, 1, timer_event);
                timer_event = NULL;
                timer_ready = FALSE;
            }
        }

        if (!timer_ready) {
            spk_stall_us(bs, LIVE_CLOCK_FALLBACK_US);
        }
        efi_live_drain_keys(ctx);

        if (rt != NULL && rt->GetTime != NULL) {
            gst = uefi_call_wrapper(rt->GetTime, 2, &now, NULL);
            if (!EFI_ERROR(gst) && efi_live_time_valid(&now)) {
                time_valid = TRUE;
            }
        }

        if (time_valid) {
            time_changed = (!have_previous ||
                            efi_live_time_changed(&now, &previous)) ? TRUE : FALSE;
            second_changed = (!have_previous || now.Second != previous.Second) ? TRUE : FALSE;

            if (time_changed) {
                efi_live_clock_line(ctx, &now, TRUE, live_row);
                unavailable_displayed = FALSE;
            }

            /* MBR test/rtc.c와 같은 시보: 59:57~59에 440Hz, 00:00에 880Hz */
            if (second_changed && speaker_ready) {
                if (now.Minute == 59U &&
                    (now.Second == 57U || now.Second == 58U || now.Second == 59U)) {
                    chime_armed = TRUE;
                    spk_tone(bs, 440U, 100U);
                } else if (now.Minute == 0U && now.Second == 0U && chime_armed) {
                    chime_armed = FALSE;
                    spk_tone(bs, 880U, 1500U);
                } else if (now.Minute != 59U) {
                    chime_armed = FALSE;
                }
            } else if (second_changed && !speaker_ready) {
                chime_armed = FALSE;
            }

            previous = now;
            have_previous = TRUE;
        } else {
            if (have_previous || !unavailable_displayed) {
                efi_live_clock_line(ctx, &now, FALSE, live_row);
                unavailable_displayed = TRUE;
            }
            have_previous = FALSE;
            chime_armed = FALSE;
        }
    }
}