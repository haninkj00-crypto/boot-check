typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);
extern void log_str(const char *s);
extern uint8_t inb(uint16_t port);

static volatile uint32_t far *kbd_ticks =
    (volatile uint32_t far *)MK_FP(0x0040, 0x006C);
static volatile uint8_t far *bda_flag1 =
    (volatile uint8_t far *)MK_FP(0x0040, 0x0017);
static volatile uint8_t far *bda_flag2 =
    (volatile uint8_t far *)MK_FP(0x0040, 0x0018);
static volatile uint8_t far *bda_flag3 =
    (volatile uint8_t far *)MK_FP(0x0040, 0x0096);
static volatile uint8_t far *prtsc_stat =
    (volatile uint8_t far *)MK_FP(0x0050, 0x0000);

/* ---------------------------------------------------------
 * IRQ 1 (INT 09h) 하드웨어 스캔코드 도달 카운터 (비파괴 체인)
 * --------------------------------------------------------- */
static volatile uint16_t g_irq1_hits = 0;
static void (interrupt far *g_old_irq1)(void);

static void interrupt far irq1_spy(void)
{
    g_irq1_hits++;
    g_old_irq1();
}

static void irq1_hook(uint8_t enable)
{
    void (interrupt far * far *ivt09)(void) =
        (void (interrupt far * far *)(void))MK_FP(0x0000, 0x0024);

    uint16_t far *ivt_words = (uint16_t far *)ivt09;
    uint16_t handler_cs;

    __emit__(0xFA); /* CLI */
    handler_cs = _CS;

    if (enable) {
        g_old_irq1 = *ivt09;

        /* IVT[09h] = CS:OFFSET(irq1_spy) */
        ivt_words[0] = (uint16_t)(void near *)irq1_spy;
        ivt_words[1] = handler_cs;

    } else if (g_old_irq1 != 0) {
        *ivt09 = g_old_irq1;
        g_old_irq1 = 0;
    }

    __emit__(0xFB); /* STI */
}

/* ---------------------------------------------------------
- INT 1Ch (사용자 타이머 틱) 훅: BIOS Pause 루프 자동 탈출용
- --------------------------------------------------------- */
static volatile uint16_t g_pause_hits = 0;
static void (interrupt far *g_old_int1c)(void);

static void interrupt far int1c_unpause(void)
{
    if ((*bda_flag2 & 0x08) != 0) {
        *bda_flag2 &= (uint8_t)~0x08;
        g_pause_hits++;
    }
    g_old_int1c();
}

static void int1c_hook(uint8_t enable)
{
    void (interrupt far * far *ivt1c)(void) =
        (void (interrupt far * far *)(void))MK_FP(0x0000, 0x0070);

    uint16_t far *ivt_words = (uint16_t far *)ivt1c;
    uint16_t handler_cs;

    __emit__(0xFA); /* CLI */
    handler_cs = _CS;

    if (enable) {
        g_old_int1c = *ivt1c;
        ivt_words[0] = (uint16_t)(void near *)int1c_unpause;
        ivt_words[1] = handler_cs;
    } else if (g_old_int1c != 0) {
        *ivt1c = g_old_int1c;
        g_old_int1c = 0;
    }

    __emit__(0xFB); /* STI */
}

/* ---------------------------------------------------------
 * 타임아웃 시 비대상 입력 수집기 (고유 6개 추적 -> 파일에만 최대 5개 기록)
 * --------------------------------------------------------- */
#define UNEXP_CAP 6

struct unexp_log {
    uint8_t src[UNEXP_CAP];     /* 1=BIOS(AL,AH), 2=Port60(sc) */
    uint8_t v1[UNEXP_CAP];
    uint8_t v2[UNEXP_CAP];
    uint8_t count;
    uint8_t suppress_sc;
};

static void unexp_add(struct unexp_log *u, uint8_t src, uint8_t a, uint8_t b)
{
    uint8_t i;
    if (u->count >= UNEXP_CAP) {
        return;
    }
    for (i = 0; i < u->count; i++) {
        if (u->src[i] == src && u->v1[i] == a && u->v2[i] == b) {
            return;
        }
    }
    u->src[u->count] = src;
    u->v1[u->count]  = a;
    u->v2[u->count]  = b;
    u->count++;
}

static void log_hex8(uint8_t val)
{
    const char hex[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(val >> 4) & 0x0F];
    buf[1] = hex[val & 0x0F];
    buf[2] = '\0';
    log_str(buf);
}

static void unexp_flush_file(const struct unexp_log *u)
{
    uint8_t i, lim = (u->count >= 6) ? 5 : u->count;
    if (lim == 0) {
        return;
    }
    log_str("    [UNEXPECTED] ");
    for (i = 0; i < lim; i++) {
        if (i > 0) {
            log_str(", ");
        }
        if (u->src[i] == 1) {
            log_str("INT16("); log_hex8(u->v1[i]); log_str(","); log_hex8(u->v2[i]); log_str(")");
        } else {
            log_str("P60("); log_hex8(u->v1[i]); log_str(")");
        }
    }
    log_str("\n");
}

static void track_p60_unexp(struct unexp_log *u, uint8_t sc, uint8_t *last_sc)
{
    if (sc == *last_sc) {
        return;
    }
    *last_sc = sc;
    if (sc == 0x00 || sc == 0xFF || sc == 0xE0 || sc == 0xE1) {
        return;
    }
    if (u->suppress_sc && (sc == u->suppress_sc || sc == (uint8_t)(u->suppress_sc | 0x80))) {
        return;
    }
    if ((sc & 0x80) && sc != 0xF1 && sc != 0xF2) {
        return;
    }
    unexp_add(u, 2, sc, 0);
}

/* ---------------------------------------------------------
 * 공통 BIOS 키보드 / 상태 조회 헬퍼
 * --------------------------------------------------------- */
static uint8_t kbd_poll(uint8_t *al, uint8_t *ah)
{
    _AH = 0x11;
    __emit__(0xCD, 0x16);
    if ((_FLAGS & 0x0040) != 0) {
        return 0;
    }
    _AH = 0x10;
    __emit__(0xCD, 0x16);
    *al = _AL;
    *ah = _AH;
    return 1;
}

static void kbd_drain(void)
{
    uint8_t a, s, g = 0;
    while (g++ < 64 && kbd_poll(&a, &s)) {
    }
}

/* INT 16h AH=12h 확장 시프트 상태 + BDA 홀드 플래그 통합 조회 */
static uint8_t read_hold_flag(uint8_t grp, uint8_t mask)
{
    uint8_t f_al, f_ah;
    _AH = 0x12;
    __emit__(0xCD, 0x16);
    f_al = _AL;
    f_ah = _AH;

    if (grp == 1) return (uint8_t)((f_al & mask) != 0);         /* Shift/Ctrl/Alt */
    /* if (grp == 2) return (uint8_t)((f_ah & mask) != 0);         Left Ctrl/Alt, SysRq, Locks */
    if (grp == 2) {                                             /* Left Ctrl/Alt, SysRq, Locks */
        if (mask == 0x80) {
            return (uint8_t)(((f_ah & 0x80) != 0) || ((*bda_flag2 & 0x04) != 0));
        }
        return (uint8_t)((f_ah & mask) != 0);
    }
    if (grp == 3) return (uint8_t)((*bda_flag3 & mask) != 0);   /* Right Ctrl(0x04)/Right Alt(0x08) */
    if (grp == 4) return (uint8_t)((*bda_flag2 & 0x08) != 0);   /* Pause latch */
    if (grp == 5) return (uint8_t)(*prtsc_stat != 0);           /* PrintScreen status */
    return 0;
}

/* ---------------------------------------------------------
 * 통합 테스트 엔진 1: 일반 단일 키 / BDA 토글 키 (10초 타임아웃)
 * --------------------------------------------------------- */
static uint8_t run_single_key(const char *label, uint8_t mode, uint8_t p1, uint8_t p2, uint8_t *out_sc)
{
    struct unexp_log u;
    uint32_t t0 = *kbd_ticks;
    uint8_t al, ah, sc, last_sc, step = 0;

    u.count = 0;
    u.suppress_sc = 0;
    out_str(label);
    kbd_drain();
    last_sc = inb(0x60);

    while ((*kbd_ticks - t0) < 182UL) {
        sc = inb(0x60);
        if (mode == 1 && (*bda_flag1 & 0x20) == p1) {       /* NumLock ON/OFF */
            out_str("PASS\n");
            return 1;
        }
        if (kbd_poll(&al, &ah)) {
            if (mode == 2) {                                /* 'a' then Enter */
                if (step == 0 && (al == 'a' || al == 'A')) {
                    step = 1;
                    continue;
                }
                if (step == 1 && al == 0x0D) {
                    out_str("PASS\n");
                    return 1;
                }
            } else if (mode == 3) {                         /* 단일 BIOS 키 */
                uint8_t ok_al = (uint8_t)(p1 == 0xFF || al == p1);
                uint8_t ok_ah = (uint8_t)(p2 == 0xFF || ah == p2 ||
                                (p2 == 0xFE && (ah == 0x86 || ah == 0x85 || ah == 0x44)));
                if (ok_al && ok_ah) {
                    if (out_sc) *out_sc = ah;
                    out_str("PASS (scan=0x"); out_hex16(ah); out_str(")\n");
                    return 1;
                }
            }
            u.suppress_sc = ah;
            unexp_add(&u, 1, al, ah);
        }
        track_p60_unexp(&u, sc, &last_sc);
    }

    out_str("TIMEOUT\n");
    unexp_flush_file(&u);
    return 0;
}

/* ---------------------------------------------------------
 * 통합 테스트 엔진 2: 특수 키 1초 이상 홀드 감지 (인터럽트 + 메이크 코드 분리)
 * --------------------------------------------------------- */
static void run_hold_key(const char *label, uint8_t m1, uint8_t m2, uint8_t grp, uint8_t mask, uint8_t is_tog)
{
    struct unexp_log u;
    uint32_t t0, t_hold;
    uint8_t  al, ah, sc, last_sc, triggered = 0;
    uint16_t make_hits = 0, int_hits = 0, flag_ticks = 0, tog_cnt = 0;
    uint16_t prev_pause = 0;
    uint8_t  early_rel = 0, prev_irq = 0, prev_tog = 0;
    uint32_t prev_tick = 0;

    u.count = 0;
    u.suppress_sc = 0;
    out_str(label);
    kbd_drain();
    if (grp == 5) {
        *prtsc_stat = 0;
    }
    if (grp == 4) {
        g_pause_hits = 0;
        *bda_flag2 &= (uint8_t)~0x08;
        int1c_hook(1);
    }

    irq1_hook(1);
    g_irq1_hits = 0;
    last_sc = inb(0x60);
    t0 = *kbd_ticks;

    /* 1단계: 최대 10초(182틱) 동안 최초 입력 대기 */
    while ((*kbd_ticks - t0) < 182UL) {
        sc = inb(0x60);
        if (grp == 4) {
            if (g_pause_hits > 0 || (*bda_flag2 & 0x08)) {
                *bda_flag2 &= (uint8_t)~0x08;
                triggered = 1;
                break;
            }
        } else if ((sc != last_sc && ((m1 && sc == m1) || (m2 && sc == m2))) ||
                   (mask && read_hold_flag(grp, mask))) {
            triggered = 1;
            break;
        }
        if (kbd_poll(&al, &ah)) {
            if (grp != 4 && ((m1 && ah == m1) || (m2 && ah == m2))) {
                triggered = 1;
                int_hits = 1;
                break;
            }
            u.suppress_sc = ah;
            unexp_add(&u, 1, al, ah);
        }
        track_p60_unexp(&u, sc, &last_sc);
    }

    if (!triggered) {
        irq1_hook(0);
        if (grp == 4) {
            int1c_hook(0);
            *bda_flag2 &= (uint8_t)~0x08;
        }
        out_str("TIMEOUT (Not working)\n");
        unexp_flush_file(&u);
        return;
    }

    /* 2단계: 1.05초(19틱) 동안 메이크 코드 반복과 INT 16h 상태를 분리 계측 */
    make_hits  = 1;
    prev_irq   = (uint8_t)g_irq1_hits;
    prev_pause = g_pause_hits;
    prev_tog   = (uint8_t)(*bda_flag1 & mask);
    prev_tick  = *kbd_ticks;
    t_hold     = prev_tick;

    while ((*kbd_ticks - t_hold) < 19UL) {
        uint8_t cur_irq = (uint8_t)g_irq1_hits;
        sc = inb(0x60);

        if (grp == 4) {
            /* Pause: BIOS 래치 재발동 여부로 반복 계측 (0xC5 오판 방지) */
            uint16_t cur_pause = g_pause_hits;
            if (cur_pause != prev_pause) {
                make_hits += (uint16_t)(cur_pause - prev_pause);
                prev_pause = cur_pause;
            }
        } else if (cur_irq != prev_irq) {
            prev_irq = cur_irq;
            if ((m1 && sc == m1) || (m2 && sc == m2)) {
                make_hits++;
            } else if ((m1 && sc == (uint8_t)(m1 | 0x80)) ||
                       (m2 && sc == (uint8_t)(m2 | 0x80))) {
                if ((*kbd_ticks - t_hold) < 16UL) {
                    early_rel = 1;
                }
            }
        }

        if (kbd_poll(&al, &ah)) {
            int_hits++;
        }

        if (*kbd_ticks != prev_tick) {
            prev_tick = *kbd_ticks;
            if (grp != 4 && mask && read_hold_flag(grp, mask)) {
                flag_ticks++;
            }
            if (is_tog && ((*bda_flag1 & mask) != prev_tog)) {
                prev_tog = (uint8_t)(*bda_flag1 & mask);
                tog_cnt++;
            }
        }
    }
    irq1_hook(0);
    if (grp == 4) {
        int1c_hook(0);
        *bda_flag2 &= (uint8_t)~0x08;
    }

    /* 3단계: 결과 출력 */
    out_str("Make=");
    if (make_hits == 0)      out_str("NONE");
    else if (make_hits == 1) out_str("1-SHOT");
    else { out_str("REPEAT("); out_ulong(make_hits); out_str("x)"); }

    out_str(" | INT16(buf="); out_ulong(int_hits);
    if (is_tog && tog_cnt > 1) {
        out_str(",tog="); out_ulong(tog_cnt);
    }
    out_str(",flag=");
    if (grp == 4) out_str("LATCH");
    else          out_str((flag_ticks >= 15) ? "YES" : "NO");
    out_str(") -> ");

    if (flag_ticks >= 15 && (make_hits > 1 || int_hits > 1 || tog_cnt > 1)) {
        out_str("HOLD FLAG + REPEAT");
    } else if (flag_ticks >= 15) {
        out_str("HOLD FLAG");
    } else if (make_hits > 1 || int_hits > 1) {
        out_str("TYPEMATIC BURST");
    } else {
        out_str("1-SHOT ONLY");
    }
    if (early_rel) {
        out_str(" (released <1s)");
    }
    out_str("\n");
}

extern void outb(uint16_t port, uint8_t val);

static uint16_t read_pit0(void)
{
    uint16_t v;
    outb(0x43, 0x00);
    v  = (uint16_t)inb(0x40);
    v |= ((uint16_t)inb(0x40) << 8);
    return v;
}

static uint8_t ps2_wait_ibf_clear(void)
{
    uint16_t g = 0;
    while (g++ < 30000U) {
        if ((inb(0x64) & 0x02) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 8042 출력 버퍼에서 1바이트 읽기 (timeout_ticks = PIT 1.193MHz 틱 단위) */
static uint8_t ps2_read_byte(uint32_t timeout_ticks, uint8_t *out_val, uint32_t *elapsed_ticks)
{
    uint16_t prev = read_pit0();
    uint32_t el = 0;
    uint32_t g = 0;

    while (el < timeout_ticks && g++ < 200000UL) {
        uint16_t cur = read_pit0();
        el += (uint16_t)(prev - cur);
        prev = cur;

        if ((inb(0x64) & 0x01) != 0) {
            *out_val = inb(0x60);
            if (elapsed_ticks) {
                *elapsed_ticks = el;
            }
            return 1;
        }
    }
    if (elapsed_ticks) {
        *elapsed_ticks = el;
    }
    return 0;
}

static void detect_kbd_interface(void)
{
    uint8_t  st, echo_ret = 0, ack = 0, id1 = 0, id2 = 0;
    uint8_t  got_echo = 0, got_id = 0, g = 0;
    uint32_t lat_ticks = 0;

    out_str("  [1.0] Keyboard Interface Probe   : ");

    st = inb(0x64);
    if (st == 0xFF) {
        out_str("NO 8042 CONTROLLER (Pure BIOS INT 16h)\n");
        return;
    }

    __emit__(0xFA); /* CLI: IRQ1 간섭 차단 */

    /* 잔류 버퍼 비우기 */
    while (g++ < 32 && (inb(0x64) & 0x01) != 0) {
        (void)inb(0x60);
    }

    /* 1) PS/2 Echo(0xEE) 명령으로 왕복 물리 지연(PIT 틱) 계측 */
    if (ps2_wait_ibf_clear()) {
        outb(0x60, 0xEE);
        if (ps2_read_byte(12000UL, &echo_ret, &lat_ticks) && echo_ret == 0xEE) {
            got_echo = 1;
        }
    }

    /* 2) PS/2 Identify(0xF2) 명령으로 디바이스 ID 확인 */
    if (ps2_wait_ibf_clear()) {
        outb(0x60, 0xF2);
        if (ps2_read_byte(12000UL, &ack, 0) && ack == 0xFA) {
            if (ps2_read_byte(12000UL, &id1, 0)) {
                (void)ps2_read_byte(6000UL, &id2, 0);
                got_id = 1;
            }
        }
    }

    g = 0;
    while (g++ < 32 && (inb(0x64) & 0x01) != 0) {
        (void)inb(0x60);
    }

    __emit__(0xFB); /* STI */

    /* 판정 및 결과 기록 */
    if (!got_echo && !got_id && ack != 0xFA) {
        out_str("BIOS EMULATION ONLY (8042 unresponsive, st=0x");
        out_hex16(st);
        out_str(")\n");
    } else if (got_echo && lat_ticks < 450UL) {
        out_str("USB LEGACY SMM EMULATION (trap latency=");
        out_ulong(lat_ticks);
        out_str(" PIT ticks)\n");
    } else if (got_id && id1 == 0xAB) {
        out_str("NATIVE PS/2 MF2 (ID=0xAB");
        out_hex16(id2);
        out_str(", lat=");
        out_ulong(lat_ticks);
        out_str(" ticks)\n");
    } else {
        out_str("EMULATED/COMPAT 8042 (echo=");
        out_hex16(echo_ret);
        out_str(", ack=");
        out_hex16(ack);
        out_str(", lat=");
        out_ulong(lat_ticks);
        out_str(" ticks)\n");
    }
}

/* ---------------------------------------------------------
 * Step 1 메인 목차
 * --------------------------------------------------------- */
void run_kbd_test(void)
{
    struct unexp_log u;
    uint32_t t0;
    uint8_t al, ah, sc, last_sc, step;

    out_str("=== [Step 1] Keyboard & Special Key Hold Test ===\n");
    out_str("10s timeout per item. Hold special keys for 1+ second.\n\n");

    detect_kbd_interface();

    /* 1. 기본 키 및 넘패드/펑션키 */
    run_single_key("  [1.1] Type 'a' then PRESS ENTER!   : ", 2, 0,    0,    0);
    run_single_key("  [1.2] Turn NumLock ON              : ", 1, 0x20, 0,    0);
    run_single_key("  [1.3] Press Numpad 0               : ", 3, '0',  0x52, 0);
    run_single_key("  [1.4] Press Main area 0            : ", 3, '0',  0x0B, 0);
    run_single_key("  [1.5] Press Numpad Enter           : ", 3, 0x0D, 0xFF, 0);
    run_single_key("  [1.6] Turn off NumLock Now         : ", 1, 0x00, 0,    0);
    run_single_key("  [1.7] Biggest F-key (F12/F11/F10)  : ", 3, 0xFF, 0xFE, 0);

    /* 2. 특수 키 1초 이상 홀드 진단 (메이크 코드 + 인터럽트 분리 계측) */
    run_hold_key("  [1.8]  Hold Left Shift (1s+)       : ", 0x2A, 0,    1, 0x02, 0);
    run_hold_key("  [1.9]  Hold Right Shift (1s+)      : ", 0x36, 0,    1, 0x01, 0);
    run_hold_key("  [1.10] Hold Right Ctrl (1s+)       : ", 0x1D, 0,    3, 0x04, 0);
    run_hold_key("  [1.11] Hold Right Alt (1s+)        : ", 0x38, 0,    3, 0x08, 0);
    run_hold_key("  [1.12] Hold Scroll Lock (1s+)      : ", 0x46, 0,    2, 0x10, 1);
    run_hold_key("  [1.13] Hold Menu Key (1s+)         : ", 0x5D, 0,    0, 0x00, 0);
    run_hold_key("  [1.14] Hold Pause/Break (1s+)      : ", 0x45, 0,    4, 0x08, 0);
    run_hold_key("  [1.15] Hold Print Screen (1s+)     : ", 0x37, 0,    5, 0x01, 0);
    run_hold_key("  [1.16] Hold SysRq (1s+)            : ", 0x54, 0,    2, 0x80, 0);
    run_hold_key("  [1.17] Hold Windows/Super (1s+)    : ", 0x5B, 0x5C, 0, 0x00, 0);

    /* 3. 국가별 언어 전환 키 구분 (KR / JP / Intl) */
    out_str("  [1.18] Language Key (KR/JP/Intl)   : ");
    u.count = 0; u.suppress_sc = 0;
    kbd_drain();
    last_sc = inb(0x60);
    t0 = *kbd_ticks;
    step = 0;
    while ((*kbd_ticks - t0) < 182UL) {
        sc = inb(0x60);
        if (sc != last_sc) {
            if      (sc == 0xF2 || sc == 0x72) { out_str("PASS (KR Hangul 0xF2)\n"); step = 1; break; }
            else if (sc == 0xF1 || sc == 0x71) { out_str("PASS (KR Hanja 0xF1)\n");  step = 1; break; }
            else if (sc == 0x79)               { out_str("PASS (JP Henkan 0x79)\n"); step = 1; break; }
            else if (sc == 0x7B)               { out_str("PASS (JP Muhenkan 0x7B)\n"); step = 1; break; }
            else if (sc == 0x70)               { out_str("PASS (JP Kana 0x70)\n");   step = 1; break; }
            else if (sc == 0x29)               { out_str("PASS (JP Zenkaku/Grave 0x29)\n"); step = 1; break; }
        }
        if (kbd_poll(&al, &ah)) {
            u.suppress_sc = ah;
            unexp_add(&u, 1, al, ah);
        }
        track_p60_unexp(&u, sc, &last_sc);
    }
    if (!step) {
        out_str("TIMEOUT\n");
        unexp_flush_file(&u);
    }

    /* 4. 전용 방향키 4방향 (E0 프리픽스만 허용) */
    out_str("  [1.19] Dedicated Arrows (U/D/L/R)  : ");
    {
        const uint8_t d_sc[4] = { 0x48, 0x50, 0x4B, 0x4D };
        const char   *d_nm[4] = { "U ", "D ", "L ", "R " };
        u.count = 0; u.suppress_sc = 0;
        kbd_drain();
        last_sc = inb(0x60);
        step = 0;
        t0 = *kbd_ticks;
        while (step < 4 && (*kbd_ticks - t0) < 182UL) {
            sc = inb(0x60);
            if (kbd_poll(&al, &ah)) {
                if (al == 0xE0 && ah == d_sc[step]) {
                    out_str(d_nm[step++]);
                } else {
                    u.suppress_sc = ah;
                    unexp_add(&u, 1, al, ah);
                }
            }
            track_p60_unexp(&u, sc, &last_sc);
        }
        if (step == 4) {
            out_str("-> PASS\n\n");
        } else {
            out_str("-> TIMEOUT\n\n");
            unexp_flush_file(&u);
        }
    }
}
