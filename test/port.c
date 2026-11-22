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

/* ---------------------------------------------------------
 * 중복 검사 방지용 포트 기록 테이블
 * --------------------------------------------------------- */
#define MAX_TRACK_PORTS 16

static uint16_t g_tested_com[MAX_TRACK_PORTS];
static uint8_t  g_tested_com_cnt = 0;

static uint16_t g_tested_lpt[MAX_TRACK_PORTS];
static uint8_t  g_tested_lpt_cnt = 0;

static uint8_t is_port_tested(const uint16_t *arr, uint8_t cnt, uint16_t base)
{
    uint8_t i;
    for (i = 0; i < cnt; i++) {
        if (arr[i] == base) {
            return 1;
        }
    }
    return 0;
}

static void mark_port_tested(uint16_t *arr, uint8_t *cnt, uint16_t base)
{
    if (*cnt < MAX_TRACK_PORTS && !is_port_tested(arr, *cnt, base)) {
        arr[*cnt] = base;
        (*cnt)++;
    }
}

static void out_hex8(uint8_t val)
{
    const char hex[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(val >> 4) & 0x0F];
    buf[1] = hex[val & 0x0F];
    buf[2] = '\0';
    out_str(buf);
}

/* ---------------------------------------------------------
 * UART 송수신 헬퍼
 * --------------------------------------------------------- */
static void uart_set_baud(uint16_t base, uint16_t divisor)
{
    uint8_t lcr = inb(base + 3U);
    outb(base + 3U, (uint8_t)(lcr | 0x80U));
    outb(base + 0U, (uint8_t)(divisor & 0xFFU));
    outb(base + 1U, (uint8_t)(divisor >> 8));
    outb(base + 3U, (uint8_t)(lcr & 0x7FU));
}

static void uart_drain_rx(uint16_t base)
{
    uint8_t g = 0;
    while (g++ < 64 && (inb(base + 5U) & 0x01U) != 0) {
        (void)inb(base + 0U);
    }
}

static void uart_send_str(uint16_t base, const char *s)
{
    while (*s) {
        uint16_t g = 0;
        while (g++ < 10000U && (inb(base + 5U) & 0x20U) == 0) {
        }
        outb(base + 0U, (uint8_t)(*s++));
    }
}

static uint8_t uart_recv_buf(uint16_t base, uint8_t *buf, uint8_t max_len,
                             uint16_t wait_ms, uint8_t *lsr_err_out)
{
    uint8_t  cnt = 0;
    uint16_t elapsed = 0;
    uint8_t  err_acc = 0;

    while (elapsed < wait_ms && cnt < max_len) {
        uint8_t lsr = inb(base + 5U);
        err_acc |= (uint8_t)(lsr & 0x1EU);  /* OE(02), PE(04), FE(08), BI(10) */
        if ((lsr & 0x01U) != 0) {
            buf[cnt++] = inb(base + 0U);
        } else {
            delay_ms_pit(5);
            elapsed += 5U;
        }
    }
    if (lsr_err_out) {
        *lsr_err_out = err_acc;
    }
    return cnt;
}

static uint8_t buf_contains_ok(const uint8_t *buf, uint8_t len)
{
    uint8_t i;
    if (len < 2) {
        return 0;
    }
    for (i = 0; i + 1U < len; i++) {
        if ((buf[i] == 'O' || buf[i] == 'o') &&
            (buf[i + 1U] == 'K' || buf[i + 1U] == 'k')) {
            return 1;
        }
    }
    return 0;
}

static void print_clean_ascii(const uint8_t *buf, uint8_t len)
{
    uint8_t i, printed = 0;
    for (i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c >= 0x20 && c <= 0x7E) {
            char ch[2];
            ch[0] = (char)c;
            ch[1] = '\0';
            out_str(ch);
            printed = 1;
        } else if ((c == '\r' || c == '\n') && printed) {
            out_str(" ");
            printed = 0;
        }
    }
}

static void print_hex_dump(const uint8_t *buf, uint8_t len)
{
    uint8_t i, lim = (len > 16) ? 16 : len;
    for (i = 0; i < lim; i++) {
        if (i > 0) {
            out_str(" ");
        }
        out_hex8(buf[i]);
    }
    if (len > 16) {
        out_str(" ...");
    }
}

/* ---------------------------------------------------------
 * 1. COM 포트 진단 함수 (레거시 및 확장카드 공용 호출)
 * --------------------------------------------------------- */
void test_com_port(uint16_t base, const char *origin)
{
    uint8_t orig_ier, orig_lcr, orig_mcr, orig_scr, dll, dlh;
    uint8_t lsr, msr, iir, r1, r2;
    uint8_t rx[48];
    uint8_t rx_len, lsr_err, b_idx;
    const uint16_t divisors[3] = { 12U, 48U, 3U };      /* 9600, 2400, 38400 bps */
    const char    *baud_str[3] = { "9600", "2400", "38400" };
    uint8_t modem_found = 0, got_any_data = 0, last_err = 0;
    uint8_t last_rx[24];
    uint8_t last_rx_len = 0;

    if (base == 0 || is_port_tested(g_tested_com, g_tested_com_cnt, base)) {
        return;
    }
    mark_port_tested(g_tested_com, &g_tested_com_cnt, base);

    out_str("  - COM 0x"); out_hex16(base);
    out_str(" ["); out_str(origin); out_str("] : ");

    lsr = inb(base + 5U);
    msr = inb(base + 6U);
    iir = inb(base + 2U);
    if (lsr == 0xFF && msr == 0xFF && iir == 0xFF) {
        out_str("NOT PRESENT\n");
        return;
    }

    /* 기존 UART 설정 백업 */
    orig_ier = inb(base + 1U);
    orig_lcr = inb(base + 3U);
    orig_mcr = inb(base + 4U);
    orig_scr = inb(base + 7U);

    outb(base + 1U, 0x00);
    outb(base + 3U, (uint8_t)(orig_lcr | 0x80U));
    dll = inb(base + 0U);
    dlh = inb(base + 1U);
    outb(base + 3U, (uint8_t)(orig_lcr & 0x7FU));

    /* UART 칩 판별 (SCR 및 FIFO 테스트) */
    outb(base + 7U, 0x55);
    r1 = inb(base + 7U);
    outb(base + 7U, 0xAA);
    r2 = inb(base + 7U);
    outb(base + 7U, orig_scr);

    if (r1 != 0x55 || r2 != 0xAA) {
        if ((orig_ier & 0xF0U) != 0) {
            out_str("INVALID / NON-UART I/O\n");
            goto restore_uart;
        }
        out_str("UART 8250");
    } else {
        outb(base + 2U, 0x01);          /* FIFO Enable */
        iir = (uint8_t)(inb(base + 2U) & 0xC0U);
        outb(base + 2U, 0x00);          /* FIFO Off */
        if (iir == 0xC0U)      out_str("UART 16550A");
        else if (iir == 0x80U) out_str("UART 16550");
        else                   out_str("UART 16450");
    }

    /* 모뎀 제어선 상태(MSR) 표시 */
    msr = inb(base + 6U);
    out_str(" (MSR=0x"); out_hex8(msr);
    if (msr & 0x10U) out_str(" CTS");
    if (msr & 0x20U) out_str(" DSR");
    if (msr & 0x40U) out_str(" RI");
    if (msr & 0x80U) out_str(" DCD");
    out_str(")\n");

    /* 1) 무송신 패시브 스트림 확인 (GPS / 시리얼 터미널 등, 9600 8N1) */
    outb(base + 3U, 0x03);              /* 8N1 */
    outb(base + 4U, 0x03);              /* DTR=1, RTS=1 */
    uart_set_baud(base, 12U);
    rx_len = uart_recv_buf(base, rx, 32, 90, &lsr_err);
    if (rx_len > 0) {
        out_str("      -> Passive RX stream (9600bps): \"");
        print_clean_ascii(rx, rx_len);
        out_str("\" [");
        print_hex_dump(rx, rx_len);
        out_str("]\n");
    }

    /* 2) 비파괴 Hayes 모뎀 AT 질의 (AT -> ATI0 -> ATI3) */
    for (b_idx = 0; b_idx < 3; b_idx++) {
        uart_set_baud(base, divisors[b_idx]);
        uart_drain_rx(base);
        uart_send_str(base, "AT\r");
        rx_len = uart_recv_buf(base, rx, 40, 220, &lsr_err);

        if (rx_len > 0) {
            got_any_data = 1;
            last_err     = lsr_err;
            last_rx_len  = (rx_len > 24) ? 24 : rx_len;
            {
                uint8_t k;
                for (k = 0; k < last_rx_len; k++) {
                    last_rx[k] = rx[k];
                }
            }

            if (buf_contains_ok(rx, rx_len)) {
                modem_found = 1;
                out_str("      -> HAYES MODEM DETECTED @ ");
                out_str(baud_str[b_idx]);
                out_str(" bps! Querying ATI0/ATI3...\n");

                /* 제품 코드 조회 (ATI0) */
                uart_drain_rx(base);
                uart_send_str(base, "ATI0\r");
                rx_len = uart_recv_buf(base, rx, 47, 250, 0);
                if (rx_len > 0) {
                    out_str("         ATI0: ");
                    print_clean_ascii(rx, rx_len);
                    out_str("\n");
                }

                /* 펌웨어/모델 문자열 조회 (ATI3) */
                uart_drain_rx(base);
                uart_send_str(base, "ATI3\r");
                rx_len = uart_recv_buf(base, rx, 47, 250, 0);
                if (rx_len > 0) {
                    out_str("         ATI3: ");
                    print_clean_ascii(rx, rx_len);
                    out_str("\n");
                }
                break;
            }
        }
    }

    /* 3) 모뎀 미응답 시: 루프백 / 쓰레기값 / 시리얼 마우스 / 무응답 판별 */
    if (!modem_found) {
        if (got_any_data) {
            if (last_rx_len == 3 && last_rx[0] == 'A' &&
                last_rx[1] == 'T' && last_rx[2] == '\r' && last_err == 0) {
                out_str("      -> SERIAL LOOPBACK PLUG DETECTED (Exact TX echo, no OK)\n");
            } else {
                out_str("      -> NON-HAYES / GARBAGE RX (LSR_ERR=0x");
                out_hex8(last_err);
                out_str("): [");
                print_hex_dump(last_rx, last_rx_len);
                out_str("]\n");
            }
        } else {
            /* 시리얼 마우스(1200bps 7N1 DTR/RTS 토글) 확인 */
            uint8_t m_id[4];
            uint8_t m_got = 0, t;
            uart_set_baud(base, 96U);   /* 1200 bps */
            outb(base + 3U, 0x02);      /* 7N1 */
            outb(base + 4U, 0x00);      /* DTR=0, RTS=0 */
            delay_ms_pit(20);
            uart_drain_rx(base);
            outb(base + 4U, 0x03);      /* DTR=1, RTS=1 */

            for (t = 0; t < 10 && m_got < 3; t++) {
                delay_ms_pit(8);
                while ((inb(base + 5U) & 0x01U) != 0 && m_got < 3) {
                    uint8_t ch = (uint8_t)(inb(base + 0U) & 0x7FU);
                    if (ch >= 0x20 && ch <= 0x7E) {
                        m_id[m_got++] = ch;
                    }
                }
            }
            m_id[m_got] = '\0';

            if (m_got > 0 && m_id[0] == 'M') {
                out_str("      -> SERIAL MOUSE DETECTED (PnP ID=\"");
                out_str((char *)m_id);
                out_str("\")\n");
            } else {
                out_str("      -> NO DEVICE RESPONSE (Open line or powered-off DCE)\n");
            }
        }
    }

restore_uart:
    outb(base + 3U, (uint8_t)(orig_lcr | 0x80U));
    outb(base + 0U, dll);
    outb(base + 1U, dlh);
    outb(base + 3U, orig_lcr);
    outb(base + 4U, orig_mcr);
    outb(base + 1U, orig_ier);
}

/* ---------------------------------------------------------
 * 2. LPT 포트 진단 함수 (프린터 상태 가정 및 레거시/확장카드 공용)
 * --------------------------------------------------------- */
void test_lpt_port(uint16_t base, const char *origin)
{
    uint8_t orig_data, st, ctrl, r1, r2, ecr;

    if (base == 0 || is_port_tested(g_tested_lpt, g_tested_lpt_cnt, base)) {
        return;
    }
    mark_port_tested(g_tested_lpt, &g_tested_lpt_cnt, base);

    out_str("  - LPT 0x"); out_hex16(base);
    out_str(" ["); out_str(origin); out_str("] : ");

    st   = inb(base + 1U);
    ctrl = inb(base + 2U);
    if (st == 0xFF && ctrl == 0xFF) {
        out_str("NOT PRESENT\n");
        return;
    }

    /* 스트로브(Bit 0)는 건드리지 않고 데이터 래치(Base+0)만 역읽기 검증 */
    orig_data = inb(base + 0U);
    outb(base + 0U, 0xAA);
    r1 = inb(base + 0U);
    outb(base + 0U, 0x55);
    r2 = inb(base + 0U);
    outb(base + 0U, orig_data);

    if (r1 != 0xAA || r2 != 0x55) {
        out_str("NO DATA LATCH (st=0x"); out_hex8(st); out_str(")\n");
        return;
    }

    /* ECP 확장 레지스터(Base + 0x402) 존재 여부 확인 */
    ecr = inb(base + 0x402U);
    if (ecr != 0xFF && (ecr & 0x03U) == 0x01U) {
        out_str("ECP/EPP Port");
    } else {
        out_str("SPP/Bidir Port");
    }

    /* 프린터 연결 가정 하에 Status 레지스터(Base+1) 해석 */
    st = inb(base + 1U);
    out_str(" | Status=0x"); out_hex8(st); out_str(" -> ");

    if ((st & 0xF8U) == 0x78U || (st & 0xF8U) == 0xF8U) {
        out_str("NO PRINTER / POWERED OFF (Floating pull-ups)\n");
    } else if ((st & 0x20U) != 0) {
        out_str("PRINTER PAPER OUT (PE=1)\n");
    } else if ((st & 0x08U) == 0) {
        out_str("PRINTER FAULT / ERROR (~ERR=0)\n");
    } else if ((st & 0x10U) == 0) {
        out_str("PRINTER OFFLINE (SLCT=0)\n");
    } else if ((st & 0x80U) == 0) {
        out_str("PRINTER BUSY (BUSY=1)\n");
    } else {
        out_str("PRINTER ONLINE & READY\n");
    }
}

/* ---------------------------------------------------------
 * 3. 확장 카드 탐지 (PCI/PCIe & ISA PnP) -> 신규 COM/LPT 추가 테스트
 * --------------------------------------------------------- */
static uint8_t is_386_or_above(void)
{
    uint16_t f;
    _AX = 0x7202;
    __emit__(0x9C, 0x50, 0x9D, 0x9C, 0x58, 0x9D);
    f = _AX;
    return (uint8_t)((f & 0xF000U) != 0x0000U && (f & 0xF000U) != 0xF000U);
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t val;
    val = 0x80000000UL |
          ((uint32_t)bus  << 16) |
          ((uint32_t)dev  << 11) |
          ((uint32_t)func << 8)  |
          ((uint32_t)(off & 0xFCU));

    _SI = (uint16_t)&val;
    _DX = 0x0CF8;
    __emit__(
        0x66, 0x8B, 0x04,       /* MOV EAX, [SI] */
        0x66, 0xEF,             /* OUT DX, EAX   */
        0xBA, 0xFC, 0x0C,       /* MOV DX, 0CFCh */
        0x66, 0xED,             /* IN EAX, DX    */
        0x66, 0x89, 0x04        /* MOV [SI], EAX */
    );
    return val;
}

static void scan_pci_expansion_ports(void)
{
    uint16_t bus;
    uint8_t  dev, func, found_any = 0;

    out_str("\n[B] PCI / PCIe Bus Scan & Add-in COM/LPT Discovery\n");

    if (!is_386_or_above()) {
        out_str("  Skipped (CPU < 80386, no 32-bit PCI Mechanism #1)\n");
        return;
    }

    /* Bus 0~15 스캔 (PCIe 브리지 너머의 애드인 카드 포함) */
    for (bus = 0; bus < 16; bus++) {
        for (dev = 0; dev < 32; dev++) {
            uint8_t max_func = 1;
            for (func = 0; func < max_func; func++) {
                uint32_t id_reg, class_reg, hdr_reg;
                uint16_t vid, did;
                uint8_t  base_cls, sub_cls, prog_if, hdr_type;

                id_reg = pci_cfg_read32((uint8_t)bus, dev, func, 0x00);
                vid = (uint16_t)(id_reg & 0xFFFFU);
                did = (uint16_t)(id_reg >> 16);

                if (vid == 0xFFFFU || vid == 0x0000U) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                if (func == 0) {
                    hdr_reg  = pci_cfg_read32((uint8_t)bus, dev, 0, 0x0C);
                    hdr_type = (uint8_t)((hdr_reg >> 16) & 0xFFU);
                    if ((hdr_type & 0x80U) != 0) {
                        max_func = 8;
                    }
                }

                class_reg = pci_cfg_read32((uint8_t)bus, dev, func, 0x08);
                base_cls  = (uint8_t)((class_reg >> 24) & 0xFFU);
                sub_cls   = (uint8_t)((class_reg >> 16) & 0xFFU);
                prog_if   = (uint8_t)((class_reg >> 8)  & 0xFFU);
                found_any = 1;

                /* PCI Communication Controller (Class 0x07) 탐지 시 BAR 추출 후 추가 포트 테스트 */
                if (base_cls == 0x07U) {
                    uint8_t bar_idx;
                    out_str("  * PCI Comm Card ");
                    out_hex8((uint8_t)bus); out_str(":");
                    out_hex8(dev); out_str("."); out_hex8(func);
                    out_str(" (VID="); out_hex16(vid);
                    out_str(" DID="); out_hex16(did);
                    out_str(" Sub="); out_hex8(sub_cls);
                    out_str(" IF="); out_hex8(prog_if); out_str(")\n");

                    for (bar_idx = 0; bar_idx < 6; bar_idx++) {
                        uint32_t bar = pci_cfg_read32((uint8_t)bus, dev, func,
                                                      (uint8_t)(0x10U + bar_idx * 4U));
                        if (bar != 0 && (bar & 0x01UL) != 0) {
                            uint16_t io_base = (uint16_t)(bar & 0xFFFCU);
                            if (io_base >= 0x0100U) {
                                if (sub_cls == 0x01U) {
                                    test_lpt_port(io_base, "PCI LPT Card");
                                } else if (sub_cls == 0x00U || sub_cls == 0x02U || sub_cls == 0x80U) {
                                    test_com_port(io_base, "PCI COM Card");
                                    /* 다중 포트 카드(2포트 이상) 동일 BAR 내 +8 오프셋 보조 확인 */
                                    if (sub_cls == 0x02U) {
                                        test_com_port((uint16_t)(io_base + 8U), "PCI COM Ch2");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (!found_any) {
        out_str("  No PCI Mechanism #1 bus detected.\n");
    }
}

/* ---------------------------------------------------------
 * ISA Plug-and-Play (ISAPnP) 포트 자원 조회
 * --------------------------------------------------------- */
static void isapnp_write(uint8_t reg, uint8_t val)
{
    outb(0x0279U, reg);
    outb(0x0A79U, val);
}

static uint8_t isapnp_read(uint16_t rdp, uint8_t reg)
{
    outb(0x0279U, reg);
    return inb(rdp);
}

static void scan_isapnp_expansion_ports(void)
{
    /* 표준 ISA PnP 32바이트 LFSR 개시 시퀀스 */
    const uint8_t init_key[32] = {
        0x6A, 0xB5, 0xDA, 0xED, 0xF6, 0xFB, 0x7D, 0xBE,
        0xDF, 0x6F, 0x37, 0x1B, 0x0D, 0x86, 0xC3, 0x61,
        0xB0, 0x58, 0x2C, 0x16, 0x8B, 0x45, 0xA2, 0xD1,
        0xE8, 0x74, 0x3A, 0x9D, 0xCE, 0xE7, 0x73, 0x39
    };
    const uint16_t rdp = 0x020BU;
    uint8_t i, csn, found = 0;

    out_str("\n[C] ISA Plug-and-Play (ISAPnP) Add-in Port Scan\n");

    /* 개시 키 전송 전 Wait-For-Key(0x02)로 상태 정렬 */
    isapnp_write(0x02, 0x02);
    delay_ms_pit(2);
    for (i = 0; i < 32; i++) {
        outb(0x0279U, init_key[i]);
    }
    delay_ms_pit(2);

    /* 모든 카드 Sleep(0x03) 진입 후 Read Data Port 설정(0x00) 및 Wake(0x03) */
    isapnp_write(0x02, 0x04);           /* Reset CSN */
    isapnp_write(0x03, 0x00);           /* Wake[0] */
    isapnp_write(0x00, (uint8_t)(rdp >> 2));
    delay_ms_pit(2);

    /* 순차 격리(Isolation)로 최대 4장 카드의 논리 장치 I/O Base(0x60/0x61) 확인 */
    for (csn = 1; csn <= 4; csn++) {
        uint8_t bit, id_bytes[9], sum = 0x6A, ok = 0;
        outb(0x0279U, 0x01);            /* Serial Isolation 레지스터 */
        delay_ms_pit(1);

        for (bit = 0; bit < 72; bit++) {
            uint8_t b1 = inb(rdp);
            uint8_t b2 = inb(rdp);
            uint8_t b_val = 0;
            delay_ms_pit(1);
            if (b1 == 0x55 && b2 == 0xAA) {
                b_val = 1;
                ok = 1;
            }
            if ((bit & 7U) == 0) {
                id_bytes[bit >> 3] = 0;
            }
            id_bytes[bit >> 3] |= (uint8_t)(b_val << (bit & 7U));
            if (bit < 64) {
                uint8_t fb = (uint8_t)(((sum >> 1) ^ sum ^ b_val) & 0x01U);
                sum = (uint8_t)((sum >> 1) | (fb << 7));
            }
        }

        if (!ok || sum != id_bytes[8]) {
            break;
        }

        found++;
        isapnp_write(0x06, csn);        /* CSN 할당 -> Config 상태 진입 */

        /* 논리 장치 0~3의 활성 여부(0x30)와 I/O Base 0(0x60:0x61) 조회 */
        for (i = 0; i < 4; i++) {
            uint8_t act;
            uint16_t io0;
            isapnp_write(0x07, i);
            act = isapnp_read(rdp, 0x30);
            io0 = ((uint16_t)isapnp_read(rdp, 0x60) << 8) |
                   (uint16_t)isapnp_read(rdp, 0x61);
            if ((act & 0x01U) != 0 && io0 >= 0x0100U && io0 != 0xFFFFU) {
                out_str("  * ISAPnP CSN "); out_ulong(csn);
                out_str(" LDN "); out_ulong(i);
                out_str(" -> I/O 0x"); out_hex16(io0); out_str("\n");

                /* LPT 대역(0x378/0x278/0x3BC) vs COM 포트 판별 호출 */
                if (io0 == 0x0378U || io0 == 0x0278U || io0 == 0x03BCU) {
                    test_lpt_port(io0, "ISAPnP Card");
                } else {
                    test_com_port(io0, "ISAPnP Card");
                }
            }
        }
        isapnp_write(0x03, 0x00);       /* 다음 미할당 카드 격리를 위해 Wake[0] */
    }

    /* 모든 ISAPnP 카드를 정상 동작 상태(Wait For Key)로 복귀 */
    isapnp_write(0x02, 0x02);

    if (!found) {
        out_str("  No unconfigured ISAPnP cards responding.\n");
    }
}

/* ---------------------------------------------------------
 * Step 7 메인 엔트리
 * --------------------------------------------------------- */
void run_port_test(void)
{
    uint16_t far *bda_com = (uint16_t far *)MK_FP(0x0040, 0x0000);
    uint16_t far *bda_lpt = (uint16_t far *)MK_FP(0x0040, 0x0008);
    const uint16_t std_com[4] = { 0x03F8U, 0x02F8U, 0x03E8U, 0x02E8U };
    const uint16_t std_lpt[3] = { 0x03BCU, 0x0378U, 0x0278U };
    uint8_t i;

    g_tested_com_cnt = 0;
    g_tested_lpt_cnt = 0;

    out_str("=== [Step 7] Serial (COM) & Parallel (LPT) Port Check ===\n\n");

    /* Phase 1: BDA 등록 주소 및 레거시 기본 I/O 주소 우선 검사 */
    out_str("[A] Legacy / BDA Registered Ports\n");
    for (i = 0; i < 4; i++) {
        if (bda_com[i] != 0) {
            test_com_port(bda_com[i], "BDA COM");
        }
    }
    for (i = 0; i < 4; i++) {
        test_com_port(std_com[i], "Legacy ISA COM");
    }

    for (i = 0; i < 3; i++) {
        if (bda_lpt[i] != 0) {
            test_lpt_port(bda_lpt[i], "BDA LPT");
        }
    }
    for (i = 0; i < 3; i++) {
        test_lpt_port(std_lpt[i], "Legacy ISA LPT");
    }

    /* Phase 2: 확장 카드(PCI/PCIe, ISA PnP) 스캔 후 신규 주소 추가 테스트 */
    scan_pci_expansion_ports();
    scan_isapnp_expansion_ports();

    out_str("\n");
}