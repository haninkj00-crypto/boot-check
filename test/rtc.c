typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void (*put_ch)(char c);
extern void put_str(const char *s);
extern void put_ulong(uint32_t val);
extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);

/* speaker.c 및 spkio.h 연동 (라디오 시보 이스터에그용) */
extern uint8_t g_spk_audible_or_partial;
extern void spk_tone(uint16_t freq_hz, uint16_t duration_ms);

static volatile uint32_t far *bda_ticks =
    (volatile uint32_t far *)MK_FP(0x0040, 0x006C);
static volatile uint8_t far *bda_rollover =
    (volatile uint8_t far *)MK_FP(0x0040, 0x0070);

struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
    uint8_t  reg_b;
    uint8_t  reg_d;
    uint8_t  cmos_ok;
    uint8_t  bios_time_ok;
    uint8_t  bios_date_ok;
};

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, (uint8_t)(reg & 0x7FU));
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

static void read_rtc_snapshot(struct rtc_time *t)
{
    uint8_t s1, m1, h1, d1, mo1, y1, c1;
    uint8_t s2, m2, h2, d2, mo2, y2, c2;
    uint8_t reg_b, reg_d, tries = 0;
    uint16_t wait_uip;
    uint8_t is_bcd, is_24h, pm_bit;

    reg_d = cmos_read(0x0D);
    reg_b = cmos_read(0x0B);
    t->reg_b = reg_b;
    t->reg_d = reg_d;

    if (reg_b == 0xFF && reg_d == 0xFF) {
        t->cmos_ok = 0;
        return;
    }
    t->cmos_ok = 1;

    do {
        wait_uip = 0;
        while ((cmos_read(0x0A) & 0x80U) != 0 && wait_uip++ < 10000U) {
        }
        s1  = cmos_read(0x00);
        m1  = cmos_read(0x02);
        h1  = cmos_read(0x04);
        d1  = cmos_read(0x07);
        mo1 = cmos_read(0x08);
        y1  = cmos_read(0x09);
        c1  = cmos_read(0x32);

        s2  = cmos_read(0x00);
        m2  = cmos_read(0x02);
        h2  = cmos_read(0x04);
        d2  = cmos_read(0x07);
        mo2 = cmos_read(0x08);
        y2  = cmos_read(0x09);
        c2  = cmos_read(0x32);
        tries++;
    } while ((s1 != s2 || m1 != m2 || h1 != h2 || d1 != d2||mo1 != mo2 || y1 != y2 || c1 != c2) && tries < 5);

    is_bcd = (uint8_t)((reg_b & 0x04U) == 0);
    is_24h = (uint8_t)((reg_b & 0x02U) != 0);
    pm_bit = (uint8_t)(h2 & 0x80U);
    h2    &= 0x7FU;

    if (is_bcd) {
        s2  = bcd_to_bin(s2);
        m2  = bcd_to_bin(m2);
        h2  = bcd_to_bin(h2);
        d2  = bcd_to_bin(d2);
        mo2 = bcd_to_bin(mo2);
        y2  = bcd_to_bin(y2);
        if (c2 != 0x00 && c2 != 0xFF) {
            c2 = bcd_to_bin(c2);
        } else {
            c2 = 0;
        }
    }

    if (!is_24h) {
        if (h2 == 12U) {
            h2 = 0;
        }
        if (pm_bit) {
            h2 = (uint8_t)(h2 + 12U);
        }
    }

    _AH = 0x02;
    __emit__(0x55, 0xCD, 0x1A, 0x5D);
    t->bios_time_ok = (uint8_t)((_FLAGS & 0x0001U) == 0);

    _AH = 0x04;
    __emit__(0x55, 0xCD, 0x1A, 0x5D);
    if ((_FLAGS & 0x0001U) == 0) {
        uint8_t bios_cent = _CH;
        t->bios_date_ok = 1;
        if (c2 < 19U || c2 > 21U) {
            c2 = bcd_to_bin(bios_cent);
        }
    } else {
        t->bios_date_ok = 0;
    }

    if (c2 < 19U || c2 > 21U) {
        c2 = (y2 >= 80U) ? 19U : 20U;
    }

    t->year  = (uint16_t)c2 * 100U + (uint16_t)y2;
    t->month = mo2;
    t->day   = d2;
    t->hour  = h2;
    t->min   = m2;
    t->sec   = s2;
}

static const char *get_weekday_name(uint16_t y, uint8_t m, uint8_t d)
{
    static const uint8_t t_off[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    static const char *names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    uint16_t yr = y;
    uint16_t w;

    if (m < 1U || m > 12U || d < 1U || d > 31U) {
        return "---";
    }
    if (m < 3U) {
        yr--;
    }
    w = (yr + yr / 4U - yr / 100U + yr / 400U + t_off[m - 1U] + d) % 7U;
    return names[w];
}

static void emit_2d(uint8_t v, uint8_t log_also)
{
    char buf[3];
    buf[0] = (char)('0' + ((v / 10U) % 10U));
    buf[1] = (char)('0' + (v % 10U));
    buf[2] = '\0';
    if (log_also) out_str(buf);
    else          put_str(buf);
}

static void emit_4d(uint16_t v, uint8_t log_also)
{
    emit_2d((uint8_t)(v / 100U), log_also);
    emit_2d((uint8_t)(v % 100U), log_also);
}

static void emit_datetime(const struct rtc_time *t, uint8_t log_also)
{
    const char *wday = get_weekday_name(t->year, t->month, t->day);

    if (log_also) { out_str(wday); out_str(" "); }
    else          { put_str(wday); put_str(" "); }

    emit_4d(t->year, log_also);
    if (log_also) out_str("-"); else put_str("-");
    emit_2d(t->month, log_also);
    if (log_also) out_str("-"); else put_str("-");
    emit_2d(t->day, log_also);
    if (log_also) out_str(" "); else put_str(" ");

    emit_2d(t->hour, log_also);
    if (log_also) out_str(":"); else put_str(":");
    emit_2d(t->min, log_also);
    if (log_also) out_str(":"); else put_str(":");
    emit_2d(t->sec, log_also);
    if (log_also) out_str(" RTC"); else put_str(" RTC");
}

static void emit_bda_uptime(uint32_t ticks, uint8_t log_also)
{
    uint32_t total_sec = (ticks * 10UL) / 182UL;
    uint8_t  hh = (uint8_t)((total_sec / 3600UL) % 24UL);
    uint8_t  mm = (uint8_t)((total_sec / 60UL) % 60UL);
    uint8_t  ss = (uint8_t)(total_sec % 60UL);

    emit_2d(hh, log_also);
    if (log_also) out_str(":"); else put_str(":");
    emit_2d(mm, log_also);
    if (log_also) out_str(":"); else put_str(":");
    emit_2d(ss, log_also);
}

/* ---------------------------------------------------------
 * Step 10: 시계 초기 기록 후 timedatectl 스타일 실시간 루프 + 정각 라디오 시보
 * --------------------------------------------------------- */
void run_rtc_and_live_clock(void)
{
    struct rtc_time rt;
    uint32_t ticks_now;
    uint8_t  last_sec = 0xFF;
    uint8_t  chime_armed = 0;       /* 59분 57~59초 경과 확인 플래그 */
    uint32_t last_tick_div = 0xFFFFFFFFUL;

    read_rtc_snapshot(&rt);
    ticks_now = *bda_ticks;

    /* 1. 초기 진단 스냅샷을 화면과 RESULT.TXT에 기록 */
    out_str("=== [Step 10] System Clock & RTC Status (timedatectl style) ===\n");
    out_str("               Local time: ");
    if (rt.cmos_ok) {
        emit_datetime(&rt, 1);
        out_str("\n");
    } else {
        out_str("N/A (CMOS Port 0x70/0x71 unresponsive)\n");
    }

    out_str("                 RTC time: ");
    if (rt.cmos_ok) {
        emit_datetime(&rt, 1);
        out_str("\n");
    } else {
        out_str("N/A\n");
    }

    out_str("            BDA Tick Time: ");
    emit_bda_uptime(ticks_now, 1);
    out_str(" (ticks=");
    out_ulong(ticks_now);
    out_str(", rollover=");
    out_ulong(*bda_rollover);
    out_str(")\n");

    out_str("           CMOS VRT Power: ");
    if (!rt.cmos_ok) {
        out_str("UNKNOWN\n");
    } else if ((rt.reg_d & 0x80U) != 0) {
        out_str("yes (Battery OK, RegD=0x"); out_hex16(rt.reg_d); out_str(")\n");
    } else {
        out_str("NO (CMOS Battery Dead / Checksum Risk! RegD=0x"); out_hex16(rt.reg_d); out_str(")\n");
    }

    out_str("           RTC Data Mode : ");
    if (rt.cmos_ok) {
        out_str(((rt.reg_b & 0x04U) != 0) ? "Binary" : "BCD");
        out_str(", ");
        out_str(((rt.reg_b & 0x02U) != 0) ? "24-hour" : "12-hour");
        out_str(" (RegB=0x"); out_hex16(rt.reg_b); out_str(")\n");
    } else {
        out_str("N/A\n");
    }

    out_str("         BIOS INT 1Ah API: Time=");
    out_str(rt.bios_time_ok ? "OK" : "FAIL");
    out_str(", Date=");
    out_str(rt.bios_date_ok ? "OK\n\n" : "FAIL\n\n");

    out_str("All scheduled diagnostics finished. Entering live clock monitor...\n");

    /* 2. 실시간 시계 갱신 및 매시 59:57~00:00 라디오 시보 루프 */
    while (1) {
        uint32_t cur_ticks = *bda_ticks;
        uint32_t tick_sec_slot = cur_ticks / 9UL;
        uint8_t  sec_changed;

        read_rtc_snapshot(&rt);

        /* CMOS가 없는 시스템에서는 BDA 환산 시/분/초로 대체 */
        if (!rt.cmos_ok) {
            uint32_t tot_s = (cur_ticks * 10UL) / 182UL;
            rt.hour = (uint8_t)((tot_s / 3600UL) % 24UL);
            rt.min  = (uint8_t)((tot_s / 60UL) % 60UL);
            rt.sec  = (uint8_t)(tot_s % 60UL);
        }

        sec_changed = (uint8_t)(rt.sec != last_sec);

        if (sec_changed || tick_sec_slot != last_tick_div) {
            last_sec      = rt.sec;
            last_tick_div = tick_sec_slot;

            /* 화면을 먼저 갱신해야 정각 00:00 "땡-" 울리는 동안 00:00이 즉시 표시됨 */
            put_str("\r  [LIVE] ");
            if (rt.cmos_ok) {
                emit_datetime(&rt, 0);
            } else {
                put_str("CMOS N/A");
            }
            put_str(" | BDA: ");
            emit_bda_uptime(cur_ticks, 0);
            put_str(" (Ticks: ");
            put_ulong(cur_ticks);
            put_str(")   ");

            /* 이스터에그: 스피커 테스트 성공(일부 성공 포함) 시 매시 정각 4초 시보 */
            if (sec_changed && g_spk_audible_or_partial) {
                if (rt.min == 59U && (rt.sec == 57U || rt.sec == 58U || rt.sec == 59U)) {
                    /* 59분 57초, 58초, 59초: "뚜" (440Hz, 100ms) */
                    chime_armed = 1;
                    spk_tone(440U, 100U);
                } else if (rt.min == 0U && rt.sec == 0U && chime_armed) {
                    /* 정각 00분 00초: "땡-" (880Hz, 1500ms) */
                    chime_armed = 0;
                    spk_tone(880U, 1500U);
                } else if (rt.min != 59U) {
                    chime_armed = 0;
                }
            }
        }

        /* 키 입력 버퍼 비우기 */
        _AH = 0x11;
        __emit__(0xCD, 0x16);
        if ((_FLAGS & 0x0040U) == 0) {
            _AH = 0x10;
            __emit__(0xCD, 0x16);
        }

        __emit__(0xFB, 0xF4);   /* STI; HLT */
    }
}