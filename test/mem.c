typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);
extern uint8_t inb(uint16_t port);

static void out_hex32(uint32_t val)
{
    out_hex16((uint16_t)(val >> 16));
    out_hex16((uint16_t)(val & 0xFFFFU));
}

/* ---------------------------------------------------------
 * 진단 결과 기록 구조체
 * --------------------------------------------------------- */
#define MAX_OPT_ROMS  8
#define MAX_E820_REC  20

struct opt_rom_info {
    uint32_t start_addr;
    uint32_t end_addr;
};

struct e820_rec {
    uint32_t base;
    uint32_t len;
    uint32_t type;      /* 1=Usable, 2=Reserved, 3=ACPI Reclaim, 4=ACPI NVS */
};

struct e820_ctx {
    uint32_t ebx;
    uint32_t eax;
    uint32_t ecx;
    uint8_t  cf;
    uint8_t  pad;
    uint8_t  buf[24];
};

struct mem_report {
    uint16_t cpu_gen;           /* 86, 286, 386 */

    /* 기본 메모리 (< 1MB) */
    uint16_t bda_conv_kb;       /* 0040:0013h 값 */
    uint16_t int12_conv_kb;     /* INT 12h 반환 값 */
    uint16_t ebda_seg;          /* 0040:000Eh (0이면 없음) */
    uint32_t conv_usable_top;   /* 실제 사용 가능 상한 물리 주소 */
    uint32_t conv_usable_kb;    /* 부트로더 제외 순수 가용 KB */

    /* UMA (0xA0000 ~ 0xFFFFF) */
    uint8_t  uma_free_16k;      /* 빈 16KB UMB 후보 블록 수 */
    uint16_t ems_frame_seg;     /* 연속 64KB EMS 페이지 프레임 후보 세그먼트 (0=없음) */
    uint8_t  rom_count;
    struct opt_rom_info roms[MAX_OPT_ROMS];

    /* A20 & 확장 메모리 (> 1MB, <= 4GB) */
    uint8_t  a20_enabled;       /* 1=A20 활성, 0=1MB 랩어라운드 */
    uint8_t  a20_port92_ok;     /* 1=Fast A20(0x92) 포트 응답 */
    uint8_t  has_int15_88;
    uint16_t ext88_kb;
    uint8_t  has_e801;
    uint32_t e801_total_kb;
    uint8_t  isa_15m_hole;      /* 1=15MB~16MB ISA 메모리 홀 감지 */
    uint8_t  has_e820;
    uint32_t e820_ext_kb;       /* E820 기준 1MB~4GB 가용 RAM 합계 (KB) */
    uint8_t  e820_count;
    struct e820_rec e820[MAX_E820_REC];

    /* 64비트 공간(> 4GB) */
    uint8_t  e820_above4g_cnt;  /* 4GB 이상 E820 엔트리 개수 */
    uint32_t e820_above4g_kb;   /* 4GB 이상 Usable(Type 1) 합계 KB (최대 4TB) */

    /* EMS / XMS 가능성 */
    uint8_t  hw_ems_port;       /* 감지된 하드웨어 EMS 보드 포트 (상위 바이트>>4, 0=없음) */
    uint16_t hw_ems_io;         /* 실제 포트 번호 (예: 0x0258) */
    uint8_t  int67_present;     /* INT 67h 벡터 존재 여부 */
};

static struct mem_report g_mem;

/* ---------------------------------------------------------
 * 1. CPU 세대 판별 (8086 vs 286 vs 386+ : 32비트 명령어 안전 실행용)
 * --------------------------------------------------------- */
static uint16_t swap_flags(uint16_t val)
{
    _AX = val;
    __emit__(
        0x9C,       /* PUSHF */
        0x50,       /* PUSH AX */
        0x9D,       /* POPF */
        0x9C,       /* PUSHF */
        0x58,       /* POP AX */
        0x9D        /* POPF (원래 플래그 복원) */
    );
    return _AX;
}

static uint16_t detect_cpu_gen(void)
{
    uint16_t f;
    f = swap_flags(0x0202);             /* bits 12..15 = 0 시도 */
    if ((f & 0xF000U) == 0xF000U) {
        return 86;                      /* 8086 / 80186 / V20 / V30 */
    }
    f = swap_flags(0x7202);             /* bits 12..14 = 1 시도 */
    if ((f & 0xF000U) == 0x0000U) {
        return 286;                     /* 80286 */
    }
    return 386;                         /* 80386 이상 */
}

/* ---------------------------------------------------------
 * 2. 기본 메모리 (< 1MB) 및 EBDA 비파괴 실측
 * --------------------------------------------------------- */
static void probe_conventional_mem(void)
{
    uint16_t far *bda_mem  = (uint16_t far *)MK_FP(0x0040, 0x0013);
    uint16_t far *bda_ebda = (uint16_t far *)MK_FP(0x0040, 0x000E);
    uint32_t reported_top, ebda_top, scan_limit, addr;
    uint16_t seg;

    g_mem.bda_conv_kb = *bda_mem;

    __emit__(0xCD, 0x12);               /* INT 12h -> AX = Conventional KB */
    g_mem.int12_conv_kb = _AX;

    g_mem.ebda_seg = *bda_ebda;

    reported_top = (uint32_t)g_mem.int12_conv_kb << 10;
    if (reported_top == 0 || reported_top > 0xA0000UL) {
        reported_top = 0xA0000UL;
    }

    scan_limit = reported_top;
    if (g_mem.ebda_seg >= 0x8000U && g_mem.ebda_seg < 0xA000U) {
        ebda_top = (uint32_t)g_mem.ebda_seg << 4;
        if (ebda_top < scan_limit) {
            scan_limit = ebda_top;
        }
    }

    /* 부트로더/스택(0x07C00~0x17FFF) 위쪽(0x18000)부터 16KB 단위로 실제 RAM 검증 */
    g_mem.conv_usable_top = 0x18000UL;
    for (addr = 0x18000UL; addr < scan_limit; addr += 0x4000UL) {
        uint16_t far *p;
        uint16_t orig, r1, r2;

        seg = (uint16_t)(addr >> 4);
        p = (uint16_t far *)MK_FP(seg, 0x0000);
        orig = *p;
        *p = 0x5AA5U;
        r1 = *p;
        *p = 0xA55AU;
        r2 = *p;
        *p = orig;

        if (r1 == 0x5AA5U && r2 == 0xA55AU) {
            g_mem.conv_usable_top = addr + 0x4000UL;
        } else {
            break;
        }
    }
    if (g_mem.conv_usable_top > scan_limit) {
        g_mem.conv_usable_top = scan_limit;
    }

    /* 0x00500 ~ 0x07BFF (30KB) + 0x18000 ~ conv_usable_top 합산 */
    g_mem.conv_usable_kb = ((0x07C00UL - 0x00500UL) +
                            (g_mem.conv_usable_top - 0x18000UL)) >> 10;
}

/* ---------------------------------------------------------
 * 3. UMA (0xA0000 ~ 0xFFFFF) Option ROM 및 UMB/EMS 프레임 스캔
 * --------------------------------------------------------- */
static void scan_uma_and_roms(void)
{
    uint16_t seg = 0xC000U;
    uint8_t  block_map[24];     /* 0xC0000~0xEFFFF를 8KB 단위 24슬롯으로 추적 (0=빈공간, 1=점유) */
    uint8_t  i, run;

    for (i = 0; i < 24; i++) {
        block_map[i] = 0;
    }
    g_mem.rom_count = 0;

    /* 0xC0000 ~ 0xEFFFF 영역의 확장 ROM(0x55 0xAA) 탐색 */
    while (seg < 0xF000U) {
        uint8_t far *p = (uint8_t far *)MK_FP(seg, 0x0000);
        if (p[0] == 0x55 && p[1] == 0xAA && p[2] > 0) {
            uint32_t rom_bytes = (uint32_t)p[2] * 512UL;
            uint32_t s_addr = (uint32_t)seg << 4;
            uint32_t e_addr = s_addr + rom_bytes - 1UL;
            uint16_t next_seg;

            if (g_mem.rom_count < MAX_OPT_ROMS) {
                g_mem.roms[g_mem.rom_count].start_addr = s_addr;
                g_mem.roms[g_mem.rom_count].end_addr   = e_addr;
                g_mem.rom_count++;
            }

            /* 점유된 8KB 슬롯 표시 */
            for (i = 0; i < 24; i++) {
                uint32_t b_start = 0xC0000UL + ((uint32_t)i << 13);
                uint32_t b_end   = b_start + 0x1FFFUL;
                if (!(e_addr < b_start || s_addr > b_end)) {
                    block_map[i] = 1;
                }
            }

            /* 다음 2KB(0x80 세그먼트) 경계로 이동 */
            next_seg = seg + (uint16_t)((rom_bytes + 2047UL) >> 4);
            next_seg = (uint16_t)((next_seg + 0x007FU) & 0xFF80U);
            if (next_seg <= seg) {
                next_seg = seg + 0x0080U;
            }
            seg = next_seg;
        } else {
            /* ROM이 아니면 기존 RAM 매핑 여부 비파괴 확인 */
            uint16_t far *pw = (uint16_t far *)p;
            uint16_t orig = *pw;
            if (orig != 0xFFFFU && orig != 0x0000U) {
                uint8_t idx = (uint8_t)((seg - 0xC000U) >> 9);
                if (idx < 24) {
                    block_map[idx] = 1;
                }
            }
            seg += 0x0080U;     /* 2KB 전진 */
        }
    }

    /* 16KB 정렬 UMB 후보 개수 및 연속 64KB(8슬롯 = 4x16KB) EMS 페이지 프레임 탐색 */
    g_mem.uma_free_16k = 0;
    g_mem.ems_frame_seg = 0;

    for (i = 0; i < 24; i += 2) {
        if (block_map[i] == 0 && block_map[i + 1] == 0) {
            g_mem.uma_free_16k++;
        }
    }

    /* 16KB 경계(짝수 인덱스)에서 연속 8슬롯(64KB)이 모두 빈 곳 탐색 (D000/E000 우선) */
    for (i = 0; i <= 16; i += 2) {
        uint8_t free64 = 1;
        for (run = 0; run < 8; run++) {
            if (block_map[i + run] != 0) {
                free64 = 0;
                break;
            }
        }
        if (free64) {
            uint16_t cand = (uint16_t)(0xC000U + ((uint16_t)i << 9));
            if (g_mem.ems_frame_seg == 0 || cand == 0xD000U || cand == 0xE000U) {
                g_mem.ems_frame_seg = cand;
            }
        }
    }
}

/* ---------------------------------------------------------
 * 4. A20 게이트 비파괴 상태 및 제어 포트 확인
 * --------------------------------------------------------- */
static void probe_a20_and_hma(void)
{
    uint8_t far *low  = (uint8_t far *)MK_FP(0x0000, 0x0000);
    uint8_t far *high = (uint8_t far *)MK_FP(0xFFFF, 0x0010);
    uint8_t i, diff = 0, p92;

    for (i = 0; i < 16; i++) {
        if (low[i] != high[i]) {
            diff = 1;
            break;
        }
    }
    g_mem.a20_enabled = diff;

    p92 = inb(0x92);
    g_mem.a20_port92_ok = (uint8_t)(p92 != 0xFF);
}

/* ---------------------------------------------------------
 * 5. 확장 메모리 조회 (INT 15h 88h / E801h / E820h, 4GB 초과 무시)
 * --------------------------------------------------------- */
static void call_int15_e820(struct e820_ctx *ctx)
{
    uint16_t p_ctx = (uint16_t)ctx;
    uint16_t p_buf = (uint16_t)(ctx->buf);

    _ES = _DS;
    _DI = p_buf;
    _SI = p_ctx;
    __emit__(
        0x55,                               /* PUSH BP */
        0x56,                               /* PUSH SI */
        0x66, 0xB8, 0x20, 0xE8, 0x00, 0x00, /* MOV EAX, 0000E820h */
        0x66, 0x8B, 0x1C,                   /* MOV EBX, [SI] */
        0x66, 0xB9, 0x18, 0x00, 0x00, 0x00, /* MOV ECX, 24 */
        0x66, 0xBA, 0x50, 0x41, 0x4D, 0x53, /* MOV EDX, 'SMAP' */
        0xCD, 0x15,                         /* INT 15h */
        0x5E,                               /* POP SI */
        0x5D,                               /* POP BP */
        0x66, 0x89, 0x1C,                   /* MOV [SI], EBX */
        0x66, 0x89, 0x44, 0x04,             /* MOV [SI+4], EAX */
        0x66, 0x89, 0x4C, 0x08,             /* MOV [SI+8], ECX */
        0x18, 0xC0,                         /* SBB AL, AL */
        0x88, 0x44, 0x0C                    /* MOV [SI+12], AL */
    );
}

static void probe_extended_mem(void)
{
    uint16_t r_ax, r_bx, r_cx, r_dx;
    uint8_t  cf;

    if (g_mem.cpu_gen < 286) {
        return;
    }

    /* 5-1. INT 15h AH=88h (최대 64MB 확장 메모리 KB) */
    _AH = 0x88;
    __emit__(0x55, 0xCD, 0x15, 0x5D);
    cf   = (uint8_t)(_FLAGS & 0x0001);
    r_ax = _AX;
    if (cf == 0 && r_ax > 0 && r_ax < 0xFFFFU) {
        g_mem.has_int15_88 = 1;
        g_mem.ext88_kb = r_ax;
    }

    /* 5-2. INT 15h AX=E801h (1MB~16MB 및 16MB~4GB) */
    _AX = 0xE801;
    __emit__(0x55, 0xCD, 0x15, 0x5D);
    cf   = (uint8_t)(_FLAGS & 0x0001);
    r_ax = _AX;
    r_bx = _BX;
    r_cx = _CX;
    r_dx = _DX;
    if (cf == 0) {
        uint16_t low_kb  = (r_ax != 0) ? r_ax : r_cx;
        uint16_t high_64 = (r_bx != 0) ? r_bx : r_dx;
        if (low_kb > 0 || high_64 > 0) {
            g_mem.has_e801 = 1;
            g_mem.e801_total_kb = (uint32_t)low_kb + ((uint32_t)high_64 << 6);
            if (high_64 > 0 && low_kb < 15360U) {
                g_mem.isa_15m_hole = 1;
            }
        }
    }

    /* 5-3. INT 15h EAX=E820h (386 이상 전용, >=4GB 상위 주소 무시) */
    if (g_mem.cpu_gen >= 386) {
        struct e820_ctx ctx;
        uint8_t guard = 0;

        ctx.ebx = 0;
        g_mem.e820_count  = 0;
        g_mem.e820_ext_kb = 0;

        while (guard++ < 48) {
            uint32_t base_lo, base_hi, len_lo, len_hi, rtype;

            call_int15_e820(&ctx);
            if (ctx.cf != 0 || ctx.eax != 0x534D4150UL || ctx.ecx < 20UL) {
                break;
            }

            base_lo = *(uint32_t *)(ctx.buf + 0);
            base_hi = *(uint32_t *)(ctx.buf + 4);
            len_lo  = *(uint32_t *)(ctx.buf + 8);
            len_hi  = *(uint32_t *)(ctx.buf + 12);
            rtype   = *(uint32_t *)(ctx.buf + 16);

            /* 4GB 이상(base_hi != 0) 미확인/고위 메모리는 무시 */
            if (base_hi == 0 && len_lo > 0) {
                uint32_t max_len = 0xFFFFFFFFUL - base_lo;
                if (len_hi > 0 || len_lo > max_len) {
                    len_lo = max_len;
                }

                if (g_mem.e820_count < MAX_E820_REC) {
                    g_mem.e820[g_mem.e820_count].base = base_lo;
                    g_mem.e820[g_mem.e820_count].len  = len_lo;
                    g_mem.e820[g_mem.e820_count].type = rtype;
                    g_mem.e820_count++;
                }

                /* 1MB(0x100000) 이상 Usable(Type 1) RAM 용량 합산 */
                if (rtype == 1UL) {
                    if (base_lo >= 0x100000UL) {
                        g_mem.e820_ext_kb += (len_lo >> 10);
                    } else if (base_lo + len_lo > 0x100000UL) {
                        g_mem.e820_ext_kb += ((base_lo + len_lo - 0x100000UL) >> 10);
                    }
                }
            } else if (base_hi > 0 && (len_lo > 0 || len_hi > 0)) {
                g_mem.e820_above4g_cnt++;
                if (rtype == 1UL) {
                    /* 64비트 바이트 길이를 32비트 KB로 변환: (len_hi:len_lo) >> 10 */
                    g_mem.e820_above4g_kb += (len_hi << 22) | (len_lo >> 10);
                }
            }

            if (ctx.ebx == 0) {
                break;
            }
        }
        if (g_mem.e820_count > 0) {
            g_mem.has_e820 = 1;
        }
    }
}

/* ---------------------------------------------------------
 * 6. EMS / XMS 하드웨어 및 펌웨어 지원 가능성 조사
 * --------------------------------------------------------- */
static void probe_ems_xms_feasibility(void)
{
    const uint16_t ems_ports[7] = { 0x0208, 0x0218, 0x0258, 0x0268, 0x02A8, 0x02B8, 0x02E8 };
    uint16_t far *ivt67 = (uint16_t far *)MK_FP(0x0000, 0x0067U * 4U);
    uint8_t i;

    g_mem.int67_present = (uint8_t)(ivt67[0] != 0 || ivt67[1] != 0);
    g_mem.hw_ems_port   = 0;
    g_mem.hw_ems_io     = 0;

    /* 클래식 LIM EMS I/O 보드 매핑 포트 비파괴 읽기 점검 */
    for (i = 0; i < 7; i++) {
        uint8_t v = inb(ems_ports[i]);
        if (v != 0xFF) {
            g_mem.hw_ems_port = 1;
            g_mem.hw_ems_io   = ems_ports[i];
            break;
        }
    }
}

/* ---------------------------------------------------------
 * 7. 결과 출력 및 모드별 용량 · 충돌 주소 후보 기록
 * --------------------------------------------------------- */
static uint32_t get_best_ext_kb(void)
{
    if (g_mem.has_e820 && g_mem.e820_ext_kb > 0) {
        return g_mem.e820_ext_kb;
    }
    if (g_mem.has_e801 && g_mem.e801_total_kb > 0) {
        return g_mem.e801_total_kb;
    }
    if (g_mem.has_int15_88) {
        return (uint32_t)g_mem.ext88_kb;
    }
    return 0;
}

void run_mem_test(void)
{
    uint32_t ext_kb, pm16_ext_kb, total_32_kb;
    uint8_t  i;

    g_mem.cpu_gen = detect_cpu_gen();
    probe_conventional_mem();
    scan_uma_and_roms();
    probe_a20_and_hma();
    probe_extended_mem();
    probe_ems_xms_feasibility();

    ext_kb = get_best_ext_kb();

    out_str("=== [Step 4] Memory Compatibility & Address Map ===\n");
    out_str("  CPU Arch Class  : ");
    if (g_mem.cpu_gen == 86)       out_str("8086/80186 (16-bit, 1MB bus)\n");
    else if (g_mem.cpu_gen == 286) out_str("80286 (16-bit PM, 16MB bus)\n");
    else                           out_str("80386+ (32-bit PM, <=4GB scope)\n");

    /* [A] 기본 메모리 (< 1MB) */
    out_str("\n[A] Conventional Memory (< 1MB)\n");
    out_str("  BDA (0040:0013) : "); out_ulong(g_mem.bda_conv_kb); out_str(" KB\n");
    out_str("  BIOS INT 12h    : "); out_ulong(g_mem.int12_conv_kb); out_str(" KB\n");
    out_str("  EBDA Segment    : ");
    if (g_mem.ebda_seg) {
        out_str("0x"); out_hex16(g_mem.ebda_seg); out_str("0\n");
    } else {
        out_str("NONE\n");
    }
    out_str("  Verified Range  : 0x00000000 - 0x");
    out_hex32(g_mem.conv_usable_top - 1UL);
    out_str(" ("); out_ulong(g_mem.conv_usable_top >> 10); out_str(" KB)\n");

    /* [B] UMA & Option ROMs */
    out_str("\n[B] Upper Memory Area (0xA0000 - 0xFFFFF)\n");
    out_str("  Free 16KB UMBs  : "); out_ulong(g_mem.uma_free_16k);
    out_str(" blocks ("); out_ulong((uint32_t)g_mem.uma_free_16k * 16UL); out_str(" KB)\n");
    out_str("  EMS Page Frame  : ");
    if (g_mem.ems_frame_seg) {
        out_str("CANDIDATE at 0x"); out_hex16(g_mem.ems_frame_seg);
        out_str("0 (64KB contiguous)\n");
    } else {
        out_str("NONE (No 64KB contiguous window)\n");
    }

    /* [C] A20 & Extended Memory */
    out_str("\n[C] A20 Gate & Extended Memory (> 1MB, <= 4GB)\n");
    out_str("  A20 Line State  : ");
    out_str(g_mem.a20_enabled ? "ENABLED (HMA accessible)\n" : "DISABLED (1MB wrap-around)\n");
    out_str("  Fast A20 (0x92) : ");
    out_str(g_mem.a20_port92_ok ? "PORT RESPONDING\n" : "NOT PRESENT\n");
    out_str("  INT 15h AH=88h  : ");
    if (g_mem.has_int15_88) { out_ulong(g_mem.ext88_kb); out_str(" KB\n"); }
    else                    { out_str("UNSUPPORTED / 0 KB\n"); }
    out_str("  INT 15h AX=E801 : ");
    if (g_mem.has_e801)     { out_ulong(g_mem.e801_total_kb); out_str(" KB\n"); }
    else                    { out_str("UNSUPPORTED\n"); }
    out_str("  INT 15h E820h   : ");
    if (g_mem.has_e820) {
        out_ulong(g_mem.e820_ext_kb);
        out_str(" KB (<4GB), ");
        out_ulong(g_mem.e820_above4g_kb);
        out_str(" KB (>=4GB, ");
        out_ulong(g_mem.e820_above4g_cnt);
        out_str(" entries)\n");
    } else {
        out_str("UNSUPPORTED\n");
    }

    /* [D] EMS / XMS 지원 가능성 평가 */
    out_str("\n[D] EMS & XMS Feasibility (Driverless Hardware/Firmware Check)\n");
    out_str("  XMS Feasibility : ");
    if (ext_kb > 0 && (g_mem.a20_enabled || g_mem.cpu_gen >= 286)) {
        out_str("FEASIBLE (Ext="); out_ulong(ext_kb); out_str(" KB, HMA=");
        out_str((ext_kb >= 64UL) ? "YES" : "NO"); out_str(")\n");
    } else {
        out_str("NOT FEASIBLE (No extended RAM > 1MB)\n");
    }

    out_str("  EMS (Hardware)  : ");
    if (g_mem.hw_ems_port) {
        out_str("POSSIBLE I/O PORT at 0x"); out_hex16(g_mem.hw_ems_io); out_str("\n");
    } else if (g_mem.int67_present) {
        out_str("INT 67h VECTOR PRESENT\n");
    } else {
        out_str("NO HARDWARE EMS BOARD\n");
    }

    out_str("  EMS (Soft/V86)  : ");
    if (g_mem.cpu_gen >= 386 && ext_kb >= 256UL && g_mem.ems_frame_seg != 0) {
        out_str("FEASIBLE (386+ V86 + Frame @ 0x");
        out_hex16(g_mem.ems_frame_seg); out_str("0)\n");
    } else {
        out_str("NOT FEASIBLE (");
        if (g_mem.cpu_gen < 386)         out_str("CPU < 386");
        else if (ext_kb < 256UL)         out_str("Ext RAM < 256KB");
        else                             out_str("No 64KB UMA frame");
        out_str(")\n");
    }

    /* [E] 모드별 사용 가능 주소 및 총 용량 요약 */
    out_str("\n[E] Usable Address & Capacity Summary by CPU Mode\n");
    out_str("  1. Real Mode (A20 OFF)   : 0x00000500-0x00007BFF, 0x00018000-0x");
    out_hex32(g_mem.conv_usable_top - 1UL);
    out_str(" | Total: "); out_ulong(g_mem.conv_usable_kb); out_str(" KB\n");

    out_str("  2. Real Mode + HMA       : ");
    if (ext_kb >= 64UL && (g_mem.a20_enabled || g_mem.cpu_gen >= 286)) {
        out_str("Above + 0x00100000-0x0010FFEF (HMA 64KB) | Total: ");
        out_ulong(g_mem.conv_usable_kb + 64UL); out_str(" KB\n");
    } else {
        out_str("N/A (Same as Real Mode: ");
        out_ulong(g_mem.conv_usable_kb); out_str(" KB)\n");
    }

    out_str("  3. 16-bit ProtMode (286) : ");
    if (g_mem.cpu_gen >= 286 && ext_kb > 0) {
        pm16_ext_kb = (ext_kb > 15360UL) ? 15360UL : ext_kb;
        out_str("Base + 0x00100000-0x");
        out_hex32(0x100000UL + (pm16_ext_kb << 10) - 1UL);
        out_str(" | Total: "); out_ulong(g_mem.conv_usable_kb + pm16_ext_kb); out_str(" KB\n");
    } else {
        out_str("N/A\n");
    }

    out_str("  4. 32-bit ProtMode (<4G) : ");
    if (g_mem.cpu_gen >= 386 && ext_kb > 0) {
        total_32_kb = g_mem.conv_usable_kb + ext_kb;
        out_str("Base + Extended (<4GB) | Total: ");
        out_ulong(total_32_kb); out_str(" KB\n");
    } else {
        out_str("N/A\n");
    }

    /* [F] 사용 시 충돌 가능 메모리 주소 후보 */
    out_str("\n[F] Conflict / Hazard Address Candidates\n");
    out_str("  - 0x00000000 - 0x000004FF : IVT & BIOS Data Area (BDA)\n");
    out_str("  - 0x00007C00 - 0x00017FFF : Boot Sector, START.COM & Real-Mode Stack\n");
    if (g_mem.ebda_seg) {
        out_str("  - 0x"); out_hex32((uint32_t)g_mem.ebda_seg << 4);
        out_str(" - 0x0009FFFF : Extended BIOS Data Area (EBDA)\n");
    }
    if (g_mem.int12_conv_kb < 640U) {
        out_str("  - 0x"); out_hex32((uint32_t)g_mem.int12_conv_kb << 10);
        out_str(" - 0x0009FFFF : Unpopulated Conventional Gap (<640KB)\n");
    }
    out_str("  - 0x000A0000 - 0x000BFFFF : VGA/MDA Video VRAM Aperture\n");
    for (i = 0; i < g_mem.rom_count; i++) {
        out_str("  - 0x"); out_hex32(g_mem.roms[i].start_addr);
        out_str(" - 0x"); out_hex32(g_mem.roms[i].end_addr);
        out_str(" : Option ROM (0x55AA signature)\n");
    }
    out_str("  - 0x000F0000 - 0x000FFFFF : System BIOS ROM\n");
    if (g_mem.isa_15m_hole) {
        out_str("  - 0x00F00000 - 0x00FFFFFF : 15MB-16MB ISA Memory Hole\n");
    }
    if (g_mem.has_e820) {
        for (i = 0; i < g_mem.e820_count; i++) {
            if (g_mem.e820[i].type != 1UL && g_mem.e820[i].base >= 0x100000UL) {
                out_str("  - 0x"); out_hex32(g_mem.e820[i].base);
                out_str(" - 0x"); out_hex32(g_mem.e820[i].base + g_mem.e820[i].len - 1UL);
                out_str(" : E820 Reserved/ACPI/MMIO (Type ");
                out_ulong(g_mem.e820[i].type); out_str(")\n");
            }
        }
    }
    out_str("\n");
}