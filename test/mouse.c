typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void delay_ms_pit(uint16_t ms);

static volatile uint32_t far *ms_ticks =
    (volatile uint32_t far *)MK_FP(0x0040, 0x006C);

/* ---------------------------------------------------------
 * 마우스 백엔드 유형 정의
 * --------------------------------------------------------- */
#define M_NONE      0
#define M_BIOS_PS2  1   /* BIOS INT 15h AH=C2h 콜백 방식 */
#define M_RAW_8042  2   /* 8042 보조 포트(0x64/0x60) 직접 폴링 방식 */
#define M_SERIAL    3   /* RS-232 COM 포트 Microsoft/Logitech 프로토콜 */
#define M_INT33     4   /* DOS INT 33h 마우스 드라이버 방식 */

static uint8_t  g_ms_mode = M_NONE;
static uint8_t  g_ms_dev_id = 0xFF;
static uint8_t  g_ms_res = 0;
static uint8_t  g_ms_rate = 0;
static uint8_t  g_int33_vec = 0;        /* 0=없음/IRET, 1=스텁, 2=실제 드라이버(0xFFFF) */
static uint16_t g_int33_btns = 0;
static uint8_t  g_bda_ps2_bit = 0;

/* 8042 / Serial 백업용 상태 */
static uint8_t  g_orig_8042_cmd = 0;
static uint8_t  g_has_8042_cmd = 0;
static uint16_t g_ser_base = 0;
static char     g_ser_id[4];

/* 실시간 패킷 집계 변수 (BIOS 콜백 및 폴링 공용) */
static volatile uint8_t  g_btn_state = 0;   /* Bit0=Left, Bit1=Right, Bit2=Middle */
static volatile long     g_sum_dx = 0;
static volatile long     g_sum_dy = 0;
static volatile uint32_t g_abs_move = 0;
static volatile uint32_t g_pkt_count = 0;

/* 8042 / Serial 3바이트 패킷 조립 버퍼 */
static uint8_t  g_pkt_buf[3];
static uint8_t  g_pkt_idx = 0;

static void out_long(long val)
{
    if (val < 0L) {
        out_str("-");
        out_ulong((uint32_t)(-val));
    } else {
        out_ulong((uint32_t)val);
    }
}

/* ---------------------------------------------------------
 * 1. BIOS INT 15h AH=C2h Far 콜백 핸들러 (cdecl -> RETF, 호출자 스택 정리)
 *    BIOS는 status, X, Y, Z 순으로 푸시 -> cdecl은 역순(z, y, x, st)으로 수신
 * --------------------------------------------------------- */
void far cdecl bios_ps2_cb(uint16_t z_raw, uint16_t y_raw, uint16_t x_raw, uint16_t st)
{
    uint8_t s_byte;
    int     dx, dy;

    /* IRQ12 문맥 보호: AX/BX/CX/DX/ES/DS 전부 보존 후 DS=CS(.COM 데이터 세그먼트) 설정 */
    __emit__(0x50, 0x53, 0x51, 0x52, 0x06, 0x1E, 0x0E, 0x1F);

    (void)z_raw;
    s_byte = (uint8_t)(st & 0xFFU);
    dx = (int)(signed char)(x_raw & 0xFFU);
    dy = (int)(signed char)(y_raw & 0xFFU);

    if ((s_byte & 0x08U) != 0) {
        g_btn_state = (uint8_t)(s_byte & 0x07U);
        g_sum_dx   += (long)dx;
        g_sum_dy   += (long)dy;
        g_abs_move += (uint32_t)((dx < 0) ? -dx : dx) +
                      (uint32_t)((dy < 0) ? -dy : dy);
        g_pkt_count++;
    }

    /* 보존한 DS/ES/DX/CX/BX/AX 역순 복원 */
    __emit__(0x1F, 0x07, 0x5A, 0x59, 0x5B, 0x58);
}

/* ---------------------------------------------------------
 * 2. INT 15h AH=C2h / INT 33h 원자적 레지스터 호출 헬퍼
 * --------------------------------------------------------- */
struct int_regs_es {
    uint16_t ax;        /* +0 */
    uint16_t bx;        /* +2 */
    uint16_t cx;        /* +4 */
    uint16_t dx;        /* +6 */
    uint16_t es;        /* +8 */
    uint8_t  cf;        /* +10 */
};

static void call_int15_c2(struct int_regs_es *r)
{
    _SI = (uint16_t)r;
    __emit__(
        0x55,                   /* PUSH BP */
        0x56,                   /* PUSH SI */
        0x57,                   /* PUSH DI */
        0x1E,                   /* PUSH DS */
        0x06,                   /* PUSH ES */
        0x8E, 0x44, 0x08,       /* MOV ES, [SI+8] */
        0x8B, 0x04,             /* MOV AX, [SI] */
        0x8B, 0x5C, 0x02,       /* MOV BX, [SI+2] */
        0x8B, 0x4C, 0x04,       /* MOV CX, [SI+4] */
        0x8B, 0x54, 0x06,       /* MOV DX, [SI+6] */
        0xCD, 0x15,             /* INT 15h */
        0x07,                   /* POP ES */
        0x1F,                   /* POP DS */
        0x5F,                   /* POP DI */
        0x5E,                   /* POP SI */
        0x5D,                   /* POP BP */
        0x89, 0x04,             /* MOV [SI], AX */
        0x89, 0x5C, 0x02,       /* MOV [SI+2], BX */
        0x89, 0x4C, 0x04,       /* MOV [SI+4], CX */
        0x89, 0x54, 0x06,       /* MOV [SI+6], DX */
        0x18, 0xC0,             /* SBB AL, AL */
        0x88, 0x44, 0x0A        /* MOV [SI+10], AL */
    );
}

static void call_int33(struct int_regs_es *r)
{
    _SI = (uint16_t)r;
    __emit__(
        0x55, 0x56, 0x57, 0x1E, 0x06,
        0x8B, 0x04,
        0x8B, 0x5C, 0x02,
        0x8B, 0x4C, 0x04,
        0x8B, 0x54, 0x06,
        0xCD, 0x33,             /* INT 33h */
        0x07, 0x1F, 0x5F, 0x5E, 0x5D,
        0x89, 0x04,
        0x89, 0x5C, 0x02,
        0x89, 0x4C, 0x04,
        0x89, 0x54, 0x06,
        0x18, 0xC0,
        0x88, 0x44, 0x0A
    );
}

static uint8_t init_int33_mouse(void)
{
    struct int_regs_es r;

    if (g_int33_vec == 0) {
        return 0;
    }

    r.ax = 0x0000U; r.bx = 0; r.cx = 0; r.dx = 0; r.es = 0; r.cf = 0;
    call_int33(&r);
    if (r.ax != 0xFFFFU) {
        return 0;
    }

    g_int33_vec  = 2;
    g_int33_btns = (r.bx == 0xFFFFU) ? 2U : r.bx;

    /* 초기 누적 이동 카운터 비우기 (AX=000Bh) */
    r.ax = 0x000BU; r.bx = 0; r.cx = 0; r.dx = 0;
    call_int33(&r);
    return 1;
}

static uint8_t init_bios_ps2(void)
{
    struct int_regs_es r;

    /* C205h: Pointing Device 초기화 (패킷 크기 3바이트) */
    r.ax = 0xC205U; r.bx = 0x0300U; r.cx = 0; r.dx = 0; r.es = _DS; r.cf = 0;
    call_int15_c2(&r);
    if (r.cf != 0 || (r.ax & 0xFF00U) != 0) {
        return 0;
    }

    /* C201h: 마우스 리셋 및 장치 ID 확인 */
    r.ax = 0xC201U; r.bx = 0; r.cx = 0; r.dx = 0; r.es = _DS; r.cf = 0;
    call_int15_c2(&r);
    if (r.cf != 0 || ((r.bx >> 8) & 0xFFU) != 0xAAU) {
        return 0;
    }
    g_ms_dev_id = (uint8_t)(r.bx & 0xFFU);

    /* C206h: 현재 해상도 및 샘플링 레이트 조회 */
    r.ax = 0xC206U; r.bx = 0x0000U; r.cx = 0; r.dx = 0; r.es = _DS; r.cf = 0;
    call_int15_c2(&r);
    if (r.cf == 0) {
        g_ms_res  = (uint8_t)(r.cx & 0xFFU);
        g_ms_rate = (uint8_t)(r.dx & 0xFFU);
    }

    /* C207h: Far 콜백 주소 등록 (ES:BX) */
    r.ax = 0xC207U;
    r.bx = (uint16_t)(void near *)bios_ps2_cb;
    r.cx = 0; r.dx = 0;
    r.es = _CS;
    r.cf = 0;
    call_int15_c2(&r);
    if (r.cf != 0) {
        return 0;
    }

    /* C200h: 포인팅 디바이스 활성화 (BH=01h) */
    r.ax = 0xC200U; r.bx = 0x0100U; r.cx = 0; r.dx = 0; r.es = _DS; r.cf = 0;
    call_int15_c2(&r);
    if (r.cf != 0) {
        r.ax = 0xC207U; r.bx = 0; r.cx = 0; r.dx = 0; r.es = 0; r.cf = 0;
        call_int15_c2(&r);
        return 0;
    }

    return 1;
}

/* ---------------------------------------------------------
 * 3. Direct 8042 보조 포트 초기화 및 폴링 (BIOS C2h 미지원 시 백업)
 * --------------------------------------------------------- */
static uint8_t kbc_wait_write(void)
{
    uint16_t g = 0;
    while (g++ < 30000U) {
        if ((inb(0x64) & 0x02U) == 0) {
            return 1;
        }
    }
    return 0;
}

static uint8_t kbc_wait_read(uint8_t *val)
{
    uint16_t g = 0;
    while (g++ < 30000U) {
        if ((inb(0x64) & 0x01U) != 0) {
            *val = inb(0x60);
            return 1;
        }
    }
    return 0;
}

static uint8_t aux_send_cmd(uint8_t cmd, uint8_t *ack_out)
{
    if (!kbc_wait_write()) return 0;
    outb(0x64, 0xD4);
    if (!kbc_wait_write()) return 0;
    outb(0x60, cmd);
    return kbc_wait_read(ack_out);
}

static void restore_8042_cmd(void)
{
    if (g_has_8042_cmd && kbc_wait_write()) {
        outb(0x64, 0x60);
        if (kbc_wait_write()) {
            outb(0x60, g_orig_8042_cmd);
        }
    }
}

static uint8_t init_raw_8042_ps2(void)
{
    uint8_t cmd_byte, ack, id_byte, test_res;
    uint8_t g = 0;

    g_has_8042_cmd = 0;
    if (inb(0x64) == 0xFF) {
        return 0;
    }

    __emit__(0xFA); /* CLI */
    while (g++ < 32 && (inb(0x64) & 0x01U) != 0) {
        (void)inb(0x60);
    }

    /* 현재 8042 커맨드 바이트 백업 후 IRQ12(Bit1) 끄고 AUX 클록(Bit5=0) 허용 */
    if (kbc_wait_write()) {
        outb(0x64, 0x20);
        if (kbc_wait_read(&cmd_byte)) {
            g_orig_8042_cmd = cmd_byte;
            g_has_8042_cmd  = 1;
            cmd_byte = (uint8_t)((cmd_byte | 0x01U) & ~0x22U);
            if (kbc_wait_write()) {
                outb(0x64, 0x60);
                if (kbc_wait_write()) {
                    outb(0x60, cmd_byte);
                }
            }
        }
    }

    /* AUX 포트 활성화(0xA8) 및 인터페이스 테스트(0xA9) */
    if (!kbc_wait_write()) {
        restore_8042_cmd();
        __emit__(0xFB);
        return 0;
    }
    outb(0x64, 0xA8);

    if (kbc_wait_write()) {
        outb(0x64, 0xA9);
        if (!kbc_wait_read(&test_res) || test_res != 0x00) {
            restore_8042_cmd();
            __emit__(0xFB);
            return 0;
        }
    }

    /* 장치 ID 조회(0xF2) 및 데이터 보고 활성화(0xF4) */
    if (aux_send_cmd(0xF2, &ack) && ack == 0xFA) {
        if (kbc_wait_read(&id_byte)) {
            g_ms_dev_id = id_byte;
        }
    }
    if (!aux_send_cmd(0xF4, &ack) || ack != 0xFA) {
        restore_8042_cmd();
        __emit__(0xFB);
        return 0;
    }

    g_pkt_idx = 0;
    __emit__(0xFB); /* STI */
    return 1;
}

/* ---------------------------------------------------------
 * 4. 시리얼 마우스(COM1~COM4) 탐지 및 3바이트 패킷 디코더
 * --------------------------------------------------------- */
static uint16_t probe_serial_mouse(char *id_out)
{
    uint16_t far *bda_com = (uint16_t far *)MK_FP(0x0040, 0x0000);
    uint8_t  port_idx;

    id_out[0] = '\0';

    for (port_idx = 0; port_idx < 4; port_idx++) {
        uint16_t base = bda_com[port_idx];
        uint8_t  orig_ier, orig_lcr, orig_mcr, dll, dlh;
        uint8_t  i, got = 0;

        if (base == 0 || inb(base + 5U) == 0xFF) {
            continue;
        }

        orig_ier = inb(base + 1U);
        orig_lcr = inb(base + 3U);
        orig_mcr = inb(base + 4U);

        outb(base + 1U, 0x00);      /* UART 인터럽트 끔 */
        outb(base + 3U, 0x80);      /* DLAB=1 */
        dll = inb(base + 0U);
        dlh = inb(base + 1U);

        /* Microsoft 시리얼 마우스 표준: 1200 bps (divisor=96), 7N1 (0x02) */
        outb(base + 0U, 0x60);
        outb(base + 1U, 0x00);
        outb(base + 3U, 0x02);

        /* DTR/RTS 내렸다가 올려서 PnP ID('M' / 'M3' / 'MZ') 유도 */
        outb(base + 4U, 0x00);
        delay_ms_pit(20);
        while ((inb(base + 5U) & 0x01U) != 0) {
            (void)inb(base + 0U);
        }
        outb(base + 4U, 0x03);      /* DTR=1, RTS=1 */

        for (i = 0; i < 12 && got < 3; i++) {
            delay_ms_pit(8);
            while ((inb(base + 5U) & 0x01U) != 0 && got < 3) {
                uint8_t ch = (uint8_t)(inb(base + 0U) & 0x7FU);
                if (ch >= 0x20 && ch <= 0x7E) {
                    id_out[got++] = (char)ch;
                }
            }
        }
        id_out[got] = '\0';

        if (got > 0 && id_out[0] == 'M') {
            g_pkt_idx = 0;
            return base;
        }

        /* 미감지 포트는 원래 레지스터 복원 */
        outb(base + 3U, 0x80);
        outb(base + 0U, dll);
        outb(base + 1U, dlh);
        outb(base + 3U, orig_lcr);
        outb(base + 4U, orig_mcr);
        outb(base + 1U, orig_ier);
    }
    return 0;
}

/* ---------------------------------------------------------
 * 폴링 기반 패킷 처리기 (M_RAW_8042 / M_SERIAL / M_INT33 공용)
 * --------------------------------------------------------- */
static void pump_mouse_packets(void)
{
    if (g_ms_mode == M_RAW_8042) {
        uint8_t guard = 0;
        while (guard++ < 32) {
            uint8_t st = inb(0x64);
            uint8_t b;
            if ((st & 0x21U) != 0x21U) {
                break;
            }
            b = inb(0x60);
            if (g_pkt_idx == 0 && (b & 0x08U) == 0) {
                continue;           /* 동기 비트(Bit 3) 재정렬 */
            }
            g_pkt_buf[g_pkt_idx++] = b;
            if (g_pkt_idx == 3) {
                int dx = (int)(signed char)g_pkt_buf[1];
                int dy = (int)(signed char)g_pkt_buf[2];
                g_btn_state = (uint8_t)(g_pkt_buf[0] & 0x07U);
                g_sum_dx   += (long)dx;
                g_sum_dy   += (long)dy;
                g_abs_move += (uint32_t)((dx < 0) ? -dx : dx) +
                              (uint32_t)((dy < 0) ? -dy : dy);
                g_pkt_count++;
                g_pkt_idx = 0;
            }
        }
    } else if (g_ms_mode == M_SERIAL && g_ser_base != 0) {
        uint8_t guard = 0;
        while (guard++ < 32 && (inb(g_ser_base + 5U) & 0x01U) != 0) {
            uint8_t b = (uint8_t)(inb(g_ser_base + 0U) & 0x7FU);
            if ((b & 0x40U) != 0) {
                g_pkt_buf[0] = b;   /* Microsoft 3바이트 패킷 동기 바이트 */
                g_pkt_idx = 1;
            } else if (g_pkt_idx > 0 && g_pkt_idx < 3) {
                g_pkt_buf[g_pkt_idx++] = b;
                if (g_pkt_idx == 3) {
                    int dx = (int)(signed char)(((g_pkt_buf[0] & 0x03U) << 6) | (g_pkt_buf[1] & 0x3FU));
                    int dy = (int)(signed char)(((g_pkt_buf[0] & 0x0CU) << 4) | (g_pkt_buf[2] & 0x3FU));
                    uint8_t btn = 0;
                    if (g_pkt_buf[0] & 0x20U) btn |= 0x01U; /* Left */
                    if (g_pkt_buf[0] & 0x10U) btn |= 0x02U; /* Right */
                    g_btn_state = btn;
                    g_sum_dx   += (long)dx;
                    g_sum_dy   -= (long)dy;
                    g_abs_move += (uint32_t)((dx < 0) ? -dx : dx) +
                                  (uint32_t)((dy < 0) ? -dy : dy);
                    g_pkt_count++;
                    g_pkt_idx = 0;
                }
            }
        }
    } else if (g_ms_mode == M_INT33) {
        struct int_regs_es r;
        int dx, dy;

        r.ax = 0x0003U; r.bx = 0; r.cx = 0; r.dx = 0;
        call_int33(&r);
        g_btn_state = (uint8_t)(r.bx & 0x07U);

        r.ax = 0x000BU; r.bx = 0; r.cx = 0; r.dx = 0;
        call_int33(&r);
        dx = (int)r.cx;
        dy = (int)r.dx;
        if (dx != 0 || dy != 0) {
            g_sum_dx   += (long)dx;
            g_sum_dy   += (long)dy;
            g_abs_move += (uint32_t)((dx < 0) ? -dx : dx) +
                          (uint32_t)((dy < 0) ? -dy : dy);
            g_pkt_count++;
        }
    }
}

/* ---------------------------------------------------------
 * 테스트 종료 시 마우스 하드웨어/콜백 안전 정리
 * --------------------------------------------------------- */
static void cleanup_mouse(void)
{
    uint8_t ack;

    if (g_ms_mode == M_BIOS_PS2) {
        struct int_regs_es r;
        r.ax = 0xC200U; r.bx = 0x0000U; r.cx = 0; r.dx = 0; r.es = _DS; r.cf = 0;
        call_int15_c2(&r);          /* Disable pointing device */
        r.ax = 0xC207U; r.bx = 0x0000U; r.cx = 0; r.dx = 0; r.es = 0; r.cf = 0;
        call_int15_c2(&r);          /* Clear far callback */
    } else if (g_ms_mode == M_RAW_8042) {
        (void)aux_send_cmd(0xF5, &ack);
        restore_8042_cmd();
    } else if (g_ms_mode == M_SERIAL && g_ser_base != 0) {
        outb(g_ser_base + 4U, 0x00);
    }
}

/* ---------------------------------------------------------
 * 버튼 클릭 + 노후 마우스 더블클릭(접점 바운스) 디바운싱 진단
 * --------------------------------------------------------- */
static void test_mouse_button(const char *label, uint8_t btn_mask)
{
    uint32_t t0, t_press, t_rel_start;
    uint8_t  pressed = 0, prev_down = 0, edges = 0, wrong_mask = 0;

    out_str(label);

    /* 이전 단계의 잔류 누름 상태가 풀릴 때까지 최대 2초 대기 */
    t0 = *ms_ticks;
    while ((*ms_ticks - t0) < 36UL) {
        pump_mouse_packets();
        if (g_btn_state == 0) {
            break;
        }
    }

    /* 1단계: 최대 10초(182틱) 동안 대상 버튼 최초 누름 대기 */
    t0 = *ms_ticks;
    while ((*ms_ticks - t0) < 182UL) {
        uint8_t cur;
        pump_mouse_packets();
        cur = g_btn_state;

        if ((cur & btn_mask) != 0) {
            pressed   = 1;
            prev_down = 1;
            edges     = 1;
            break;
        }
        if (cur != 0) {
            wrong_mask |= cur;
        }
    }

    if (!pressed) {
        if (wrong_mask != 0) {
            out_str("TIMEOUT (Other button mask=0x");
            out_hex16(wrong_mask);
            out_str(")\n");
        } else {
            out_str("TIMEOUT\n");
        }
        return;
    }

    /* 2단계: 떼어진 상태가 4틱(~220ms) 연속 유지될 때까지 추적해 더블클릭 바운스 흡수 및 계측 */
    t_press     = *ms_ticks;
    t_rel_start = 0;

    while ((*ms_ticks - t_press) < 91UL) {
        uint8_t down;
        pump_mouse_packets();
        down = (uint8_t)((g_btn_state & btn_mask) != 0);

        if (down != prev_down) {
            prev_down = down;
            if (down) {
                edges++;            /* 접점 바운스 / 더블클릭 재트리거 감지 */
                t_rel_start = 0;
            } else {
                t_rel_start = *ms_ticks;
            }
        } else if (!down) {
            if (t_rel_start == 0) {
                t_rel_start = *ms_ticks;
            } else if ((*ms_ticks - t_rel_start) >= 4UL) {
                break;              /* 안정적 해제 완료 */
            }
        }
    }

    if (edges > 1) {
        out_str("PASS (bounce/double-click filtered: ");
        out_ulong(edges);
        out_str("x edges)\n");
    } else {
        out_str("PASS\n");
    }
}

/* ---------------------------------------------------------
 * 간단한 이동 테스트 (누적 이동량 40 Mickeys 이상 시 즉시 통과)
 * --------------------------------------------------------- */
static void test_mouse_movement(void)
{
    uint32_t t0;
    uint8_t  moved = 0;

    out_str("  [2.4] Move Mouse (any direction, 10s)   : ");

    t0 = *ms_ticks;
    while ((*ms_ticks - t0) < 36UL) {
        pump_mouse_packets();
        if (g_btn_state == 0) {
            break;
        }
    }

    g_sum_dx   = 0;
    g_sum_dy   = 0;
    g_abs_move = 0;

    t0 = *ms_ticks;
    while ((*ms_ticks - t0) < 182UL) {
        pump_mouse_packets();
        if (g_abs_move >= 40UL) {
            moved = 1;
            break;
        }
    }

    if (moved) {
        out_str("PASS (net dX=");
        out_long(g_sum_dx);
        out_str(", dY=");
        out_long(g_sum_dy);
        out_str(", dist=");
        out_ulong(g_abs_move);
        out_str(")\n\n");
    } else if (g_abs_move > 0) {
        out_str("TIMEOUT (Weak move: dist=");
        out_ulong(g_abs_move);
        out_str(" < 40)\n\n");
    } else {
        out_str("TIMEOUT (No movement signal)\n\n");
    }
}

/* ---------------------------------------------------------
 * Step 6 메인 엔트리: 마우스 인터페이스 수집 · 버튼 · 이동 진단
 * --------------------------------------------------------- */
void run_mouse_test(void)
{
    uint16_t far *ivt33 = (uint16_t far *)MK_FP(0x0000, 0x0033U * 4U);
    uint16_t far *equip = (uint16_t far *)MK_FP(0x0040, 0x0010);
    uint8_t  far *entry33;

    g_ms_mode    = M_NONE;
    g_ms_dev_id  = 0xFF;
    g_ms_res     = 0;
    g_ms_rate    = 0;
    g_int33_vec  = 0;
    g_int33_btns = 0;
    g_ser_base   = 0;
    g_ser_id[0]  = '\0';
    g_btn_state  = 0;
    g_pkt_count  = 0;

    /* 1. 펌웨어 / 벡터 / 포트 인터페이스 수집 */
    g_bda_ps2_bit = (uint8_t)((*equip & 0x0004U) != 0);
    if (ivt33[0] != 0 || ivt33[1] != 0) {
        entry33 = (uint8_t far *)MK_FP(ivt33[1], ivt33[0]);
        if (*entry33 != 0xCF) {
            g_int33_vec = 1;
        }
    }

    if (init_int33_mouse()) {
        g_ms_mode = M_INT33;
    } else {
        g_ser_base = probe_serial_mouse(g_ser_id);
        if (init_bios_ps2()) {
            g_ms_mode = M_BIOS_PS2;
        } else if (init_raw_8042_ps2()) {
            g_ms_mode = M_RAW_8042;
        } else if (g_ser_base != 0) {
            g_ms_mode = M_SERIAL;
        }
    }

    /* 2. 인터페이스 수집 결과 출력 및 기록 */
    out_str("=== [Step 6] Mouse Interface & Input Check ===\n");
    out_str("  BDA PS/2 Flag   : "); out_str(g_bda_ps2_bit ? "INSTALLED\n" : "CLEAR\n");
    out_str("  INT 33h Driver  : ");
    if (g_int33_vec == 2)      out_str("ACTIVE (AX=FFFFh)\n");
    else if (g_int33_vec == 1) out_str("STUB ONLY (AX!=FFFFh)\n");
    else                       out_str("NONE (Bare-metal)\n");

    out_str("  Serial Mouse    : ");
    if (g_ser_base != 0) {
        out_str("DETECTED at 0x"); out_hex16(g_ser_base);
        out_str(" (ID=\""); out_str(g_ser_id); out_str("\")\n");
    } else {
        out_str("NONE\n");
    }

    out_str("  Active Backend  : ");
    if (g_ms_mode == M_INT33) {
        out_str("DOS INT 33h DRIVER (Buttons=");
        out_ulong(g_int33_btns);
        out_str(")\n\n");
    } else if (g_ms_mode == M_BIOS_PS2) {
        out_str("BIOS PS/2 INT 15h C2h (DevID=0x");
        out_hex16(g_ms_dev_id);
        out_str(", Res="); out_ulong(g_ms_res);
        out_str(", Rate="); out_ulong(g_ms_rate);
        out_str("Hz)\n\n");
    } else if (g_ms_mode == M_RAW_8042) {
        out_str("DIRECT 8042 AUX PORT (DevID=0x");
        out_hex16(g_ms_dev_id);
        out_str(")\n\n");
    } else if (g_ms_mode == M_SERIAL) {
        out_str("RS-232 SERIAL MOUSE (0x");
        out_hex16(g_ser_base);
        out_str(")\n\n");
    } else {
        out_str("NONE (No pointing device detected)\n\n");
        return;
    }

    /* 3. 주요 버튼 클릭 및 이동 테스트 실행 */
    out_str("Click each button once (10s timeout; switch bounce auto-filtered).\n");
    test_mouse_button("  [2.1] Click Left Button                 : ", 0x01);
    test_mouse_button("  [2.2] Click Right Button                : ", 0x02);
    test_mouse_button("  [2.3] Click Middle Button (if present)  : ", 0x04);
    test_mouse_movement();

    cleanup_mouse();
}