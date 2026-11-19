typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

#define OUT_NONE 0
#define OUT_VRAM 1
#define OUT_BIOS 2

struct io_caps {
    uint8_t  vram_ok;
    uint16_t vram_seg;
    uint16_t crtc_port;
    uint8_t  vram_row;
    uint8_t  vram_col;
    uint8_t  vram_attr;
    uint8_t  bios_ok;
    uint8_t  kbd_bios_ok;
    uint8_t  active_out;
};

extern struct io_caps g_io;
extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void delay_ms_pit(uint16_t ms);

static void out_hex32(uint32_t val)
{
    out_hex16((uint16_t)(val >> 16));
    out_hex16((uint16_t)(val & 0xFFFFU));
}

/* ---------------------------------------------------------
 * 저수준 INT 10h 래퍼 (일부 VBE BIOS의 BP/SI/DI/ES 파괴 방지)
 * --------------------------------------------------------- */
static uint16_t int10_safe(uint16_t ax_val, uint16_t bx_val, uint16_t cx_val,
                           uint16_t dx_val, uint16_t es_val, uint16_t di_val,
                           uint16_t *out_bx, uint16_t *out_cx)
{
    uint16_t r_ax, r_bx, r_cx;

    _ES = es_val;
    _DI = di_val;
    _DX = dx_val;
    _CX = cx_val;
    _BX = bx_val;
    _AX = ax_val;
    __emit__(
        0x55, 0x56, 0x57, 0x06,     /* PUSH BP, SI, DI, ES */
        0xCD, 0x10,                 /* INT 10h */
        0x07, 0x5F, 0x5E, 0x5D      /* POP ES, DI, SI, BP */
    );
    r_ax = _AX;
    r_bx = _BX;
    r_cx = _CX;

    if (out_bx) *out_bx = r_bx;
    if (out_cx) *out_cx = r_cx;
    return r_ax;
}

/* ---------------------------------------------------------
 * 1. 수직동기(VSync) 펄스 감시 (0x3DA Color / 0x3BA Mono·HGC)
 * --------------------------------------------------------- */
static uint8_t probe_vsync_toggle(uint16_t port, uint8_t bit_mask)
{
    uint8_t first, cur, toggles = 0;
    uint16_t i;

    first = (uint8_t)(inb(port) & bit_mask);
    if (inb(port) == 0xFF) {
        return 0;
    }

    for (i = 0; i < 40; i++) {
        delay_ms_pit(1);
        cur = (uint8_t)(inb(port) & bit_mask);
        if (cur != first) {
            first = cur;
            toggles++;
            if (toggles >= 2) {
                return 1;
            }
        }
    }
    return 0;
}

/* VGA 아날로그 DAC(0x3C7/0x3C8/0x3C9) 읽기/쓰기 응답 확인 (실물 EGA와 VGA 구분) */
static uint8_t probe_vga_dac(void)
{
    uint8_t orig_r, orig_g, orig_b, rd_r, rd_g, rd_b;

    /* 255번 팔레트 엔트리 백업 */
    outb(0x03C7U, 0xFF);
    orig_r = inb(0x03C9U);
    orig_g = inb(0x03C9U);
    orig_b = inb(0x03C9U);

    /* 6비트 테스트 패턴(0x15, 0x2A, 0x3F) 기록 후 Read Mode(0x3C7)로 역읽기 */
    outb(0x03C8U, 0xFF);
    outb(0x03C9U, 0x15);
    outb(0x03C9U, 0x2A);
    outb(0x03C9U, 0x3F);

    outb(0x03C7U, 0xFF);
    rd_r = inb(0x03C9U);
    rd_g = inb(0x03C9U);
    rd_b = inb(0x03C9U);

    /* 원본 팔레트 복원 */
    outb(0x03C8U, 0xFF);
    outb(0x03C9U, orig_r);
    outb(0x03C9U, orig_g);
    outb(0x03C9U, orig_b);

    return (uint8_t)(rd_r == 0x15 && rd_g == 0x2A && rd_b == 0x3F);
}

/* ---------------------------------------------------------
 * 2. VESA VBE 정보 및 주요 해상도 LFB 지원 여부 조회
 * --------------------------------------------------------- */
struct vbe_summary {
    uint8_t  supported;
    uint16_t version;
    uint16_t vram_64k;
    char     oem_str[42];
    uint8_t  has_101;           /* 640x480x8bpp */
    uint8_t  lfb_101;
    uint32_t lfb_addr_101;
    uint8_t  has_103;           /* 800x600x8bpp */
    uint8_t  has_105;           /* 1024x768x8bpp */
    uint8_t  has_truecolor;     /* 640x480x16/24/32bpp */
};

static void query_vbe_mode_detail(uint16_t mode, uint8_t *has_mode,
                                  uint8_t *has_lfb, uint32_t *lfb_addr)
{
    uint8_t mbuf[256];
    uint16_t i, ret, attr;

    for (i = 0; i < 256; i++) {
        mbuf[i] = 0;
    }

    ret = int10_safe(0x4F01U, 0, mode, 0, _DS, (uint16_t)mbuf, 0, 0);
    if (ret != 0x004FU) {
        return;
    }

    attr = *(uint16_t *)(mbuf + 0);
    /* Bit 0: Hardware supported, Bit 4: Graphics mode */
    if ((attr & 0x0011U) == 0x0011U) {
        if (has_mode) *has_mode = 1;
        if ((attr & 0x0080U) != 0) {
            if (has_lfb)  *has_lfb  = 1;
            if (lfb_addr) *lfb_addr = *(uint32_t *)(mbuf + 40);
        }
    }
}

static void probe_vesa_vbe(struct vbe_summary *vbe)
{
    uint8_t  vbuf[512];
    uint16_t i, ret, oem_off, oem_seg;
    uint8_t  far *p_oem;

    vbe->supported     = 0;
    vbe->version       = 0;
    vbe->vram_64k      = 0;
    vbe->oem_str[0]    = '\0';
    vbe->has_101       = 0;
    vbe->lfb_101       = 0;
    vbe->lfb_addr_101  = 0;
    vbe->has_103       = 0;
    vbe->has_105       = 0;
    vbe->has_truecolor = 0;

    for (i = 0; i < 512; i++) {
        vbuf[i] = 0;
    }
    vbuf[0] = 'V';
    vbuf[1] = 'B';
    vbuf[2] = 'E';
    vbuf[3] = '2';

    ret = int10_safe(0x4F00U, 0, 0, 0, _DS, (uint16_t)vbuf, 0, 0);
    if (ret != 0x004FU || vbuf[0] != 'V' || vbuf[1] != 'E' ||
        vbuf[2] != 'S' || vbuf[3] != 'A') {
        return;
    }

    vbe->supported = 1;
    vbe->version   = *(uint16_t *)(vbuf + 4);
    vbe->vram_64k  = *(uint16_t *)(vbuf + 18);

    oem_off = *(uint16_t *)(vbuf + 6);
    oem_seg = *(uint16_t *)(vbuf + 8);
    if (oem_off != 0 || oem_seg != 0) {
        p_oem = (uint8_t far *)MK_FP(oem_seg, oem_off);
        for (i = 0; i < 40; i++) {
            uint8_t c = p_oem[i];
            if (c == 0) {
                break;
            }
            vbe->oem_str[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
        }
        vbe->oem_str[i] = '\0';
    }

    query_vbe_mode_detail(0x0101U, &vbe->has_101, &vbe->lfb_101, &vbe->lfb_addr_101);
    query_vbe_mode_detail(0x0103U, &vbe->has_103, 0, 0);
    query_vbe_mode_detail(0x0105U, &vbe->has_105, 0, 0);
    query_vbe_mode_detail(0x0110U, &vbe->has_truecolor, 0, 0);
    query_vbe_mode_detail(0x0111U, &vbe->has_truecolor, 0, 0);
    query_vbe_mode_detail(0x0112U, &vbe->has_truecolor, 0, 0);
}

/* ---------------------------------------------------------
 * 3. 실제 그래픽 모드 전환 + 테스트 패턴 + 복귀 검증
 * --------------------------------------------------------- */
static void restore_text_mode(uint8_t orig_mode)
{
    uint8_t target = (orig_mode == 0x07) ? 0x07 : 0x03;
    int10_safe((uint16_t)target, 0, 0, 0, 0, 0, 0, 0);

    if (g_io.active_out == OUT_VRAM) {
        g_io.vram_row = 0;
        g_io.vram_col = 0;
    }
}

static uint8_t test_legacy_gfx_mode(uint8_t mode, uint16_t vseg, uint8_t orig_mode)
{
    uint16_t far *vmem;
    uint16_t i, orig_w, r1;
    uint8_t  cur_mode;

    /* 모드 전환 직전 디스크 로그에 기록 (행 발생 대비) */
    int10_safe((uint16_t)mode, 0, 0, 0, 0, 0, 0, 0);

    cur_mode = (uint8_t)(int10_safe(0x0F00U, 0, 0, 0, 0, 0, 0, 0) & 0x7FU);
    if (cur_mode != mode) {
        restore_text_mode(orig_mode);
        return 0;
    }

    /* VRAM 패턴 쓰기 및 역읽기 검증 + 간단한 가로 컬러 바 표시 */
    vmem = (uint16_t far *)MK_FP(vseg, 0x0000);
    orig_w = vmem[0];
    vmem[0] = 0xAA55U;
    r1 = vmem[0];
    vmem[0] = orig_w;

    if (r1 != 0xAA55U) {
        restore_text_mode(orig_mode);
        return 0;
    }

    for (i = 0; i < 4000U; i++) {
        vmem[i] = (uint16_t)((i & 0x0FU) * 0x1111U);
    }
    delay_ms_pit(450);

    restore_text_mode(orig_mode);
    return 1;
}

static uint8_t test_svga_vbe_mode(uint16_t mode, uint8_t orig_mode, uint8_t *bank_sw_ok)
{
    uint16_t ret;
    uint8_t  far *win_a = (uint8_t far *)MK_FP(0xA000, 0x0000);
    uint16_t i;
    uint8_t  r0, r1;

    ret = int10_safe(0x4F02U, mode, 0, 0, 0, 0, 0, 0);
    if (ret != 0x004FU) {
        restore_text_mode(orig_mode);
        return 0;
    }

    /* Bank 0 쓰기 및 확인 */
    win_a[0] = 0x5A;
    if (win_a[0] != 0x5A) {
        restore_text_mode(orig_mode);
        return 0;
    }

    /* VESA Window A(BL=0) Bank 1 전환(AX=4F05h, DX=1) 및 독립성 검증 */
    if (bank_sw_ok != 0) {
        ret = int10_safe(0x4F05U, 0x0000U, 0, 1U, 0, 0, 0, 0);
        if (ret == 0x004FU) {
            win_a[0] = 0xC3;
            r1 = win_a[0];
            /* Bank 0(DX=0) 복귀 후 기존 0x5A가 보존되어 있으면 뱅크 스위칭 정상 */
            int10_safe(0x4F05U, 0x0000U, 0, 0U, 0, 0, 0, 0);
            r0 = win_a[0];
            if (r1 == 0xC3 && r0 == 0x5A) {
                *bank_sw_ok = 1;
            }
        }
    }

    for (i = 0; i < 32000U; i++) {
        win_a[i] = (uint8_t)(i & 0xFFU);
    }
    delay_ms_pit(400);

    restore_text_mode(orig_mode);
    return 1;
}

/* ---------------------------------------------------------
 * Step 8 메인 엔트리
 * --------------------------------------------------------- */
void run_gfx_test(void)
{
    uint8_t  orig_mode, dcc_active = 0, ega_ok = 0, ega_mem_idx = 0;
    uint8_t  vsync_3da = 0, vsync_3ba = 0, has_3ba = 0;
    uint8_t  cga4_ok = 0, cga2_ok = 0, vga13_ok = 0, vga12_ok = 0, vbe101_ok = 0;
    uint8_t dac_ok = 0;
    uint8_t ega0d_ok = 0, ega10_ok = 0;
    uint8_t svga103_ok = 0, svga111_ok = 0, vbe_bank_ok = 0;
    uint16_t ret, bx_out, cx_out;
    struct vbe_summary vbe;

    orig_mode = (uint8_t)(int10_safe(0x0F00U, 0, 0, 0, 0, 0, 0, 0) & 0x7FU);

    /* 1. VGA Display Combination Code (INT 10h AX=1A00h) */
    ret = int10_safe(0x1A00U, 0, 0, 0, 0, 0, &bx_out, 0);
    if ((ret & 0xFFU) == 0x1AU) {
        dcc_active = (uint8_t)(bx_out & 0xFFU);
    }

    /* 2. EGA/VGA BIOS 확장 정보 (INT 10h AH=12h, BL=10h) */
    ret = int10_safe(0x1200U, 0x0010U, 0, 0, 0, 0, &bx_out, &cx_out);
    if ((bx_out & 0xFFU) != 0x10U) {
        ega_ok      = 1;
        ega_mem_idx = (uint8_t)(bx_out & 0x03U);
    }

    /* 2-1. VGA DAC 확인 (ega_ok == 1 이면서 dac_ok == 0 이면 실물 EGA 카드) */
    dac_ok = probe_vga_dac();

    /* 3. 수직동기(VSync) 하드웨어 포트 반응 검사 */
    vsync_3da = probe_vsync_toggle(0x03DAU, 0x08U);
    if (inb(0x03BAU) != 0xFF) {
        has_3ba   = 1;
        vsync_3ba = probe_vsync_toggle(0x03BAU, 0x80U);
    }

    /* 4. VESA VBE 조회 */
    probe_vesa_vbe(&vbe);

    /* 5. 실제 그래픽 모드 전환 및 복귀 테스트 (전환 전 로그 선기록) */
    out_str("=== [Step 8] Graphics Adapter & Video Mode Check ===\n");
    out_str("  Testing physical mode switches (auto-restores Text Mode 0x03)...\n");

    /* 5. 실제 그래픽 모드 전환 목록에 EGA(0x0D, 0x10) 및 SVGA(0x103, 0x111) 추가 */
    if (g_io.bios_ok) {
        cga4_ok  = test_legacy_gfx_mode(0x04, 0xB800U, orig_mode);
        cga2_ok  = test_legacy_gfx_mode(0x06, 0xB800U, orig_mode);
        ega0d_ok = test_legacy_gfx_mode(0x0D, 0xA000U, orig_mode);
        ega10_ok = test_legacy_gfx_mode(0x10, 0xA000U, orig_mode);
        vga13_ok = test_legacy_gfx_mode(0x13, 0xA000U, orig_mode);
        vga12_ok = test_legacy_gfx_mode(0x12, 0xA000U, orig_mode);
        if (vbe.supported && vbe.has_101) {
            vbe101_ok = test_svga_vbe_mode(0x0101U, orig_mode, &vbe_bank_ok);
        }
        if (vbe.supported && vbe.has_103) {
            svga103_ok = test_svga_vbe_mode(0x0103U, orig_mode, 0);
        }
        if (vbe.supported && vbe.has_truecolor) {
            svga111_ok = test_svga_vbe_mode(0x0111U, orig_mode, 0);
        }
    }

    /* 6. 텍스트 모드 복귀 후 종합 결과 출력 및 디스크 기록 */
    out_str("\n[A] Display Subsystem & VBIOS Capabilities\n");
    out_str("  Initial Mode    : 0x"); out_hex16(orig_mode); out_str("\n");
    out_str("  VGA DCC (1A00h) : ");
    if (dcc_active == 0)      out_str("UNSUPPORTED (Pre-VGA / CGA / MDA / HGC)\n");
    else if (dcc_active == 1) out_str("MDA Monochrome (0x01)\n");
    else if (dcc_active == 2) out_str("CGA Color (0x02)\n");
    else if (dcc_active == 4) out_str("EGA Color (0x04)\n");
    else if (dcc_active == 5) out_str("EGA Monochrome (0x05)\n");
    else if (dcc_active == 7) out_str("VGA Analog Monochrome (0x07)\n");
    else if (dcc_active == 8) out_str("VGA Analog Color (0x08)\n");
    else { out_str("Code 0x"); out_hex16(dcc_active); out_str("\n"); }

    out_str("  EGA/VGA BIOS    : ");
    if (ega_ok) {
        out_str("PRESENT (Base VRAM=");
        out_ulong((uint32_t)(ega_mem_idx + 1U) * 64UL);
        out_str(" KB)\n");
    } else {
        out_str("NOT DETECTED\n");
    }

    out_str("  Port 0x3DA VSync: ");
    out_str(vsync_3da ? "PULSING (DOS retrace waits safe)\n"
                      : "STATIC/ABSENT (Risk of hang on 3DA loop)\n");

    out_str("  Port 0x3BA Mono : ");
    if (!has_3ba)        out_str("NOT PRESENT\n");
    else if (vsync_3ba)  out_str("HERCULES HGC DETECTED (Bit 7 pulsing, 0xB000 conflict risk)\n");
    else                 out_str("IBM MDA PRESENT (Static Bit 7)\n");

    /* [A] 섹션 내 EGA vs VGA 하드웨어 구분 */
    out_str("  EGA vs VGA Regs : ");
    if (ega_ok && !dac_ok && dcc_active == 0) {
        out_str("GENUINE EGA (No DAC 0x3C9, Write-Only CRTC/Seq regs)\n");
    } else if (dac_ok) {
        out_str("VGA COMPATIBLE (6-bit RAMDAC 0x3C9 R/W verified)\n");
    } else {
        out_str("PRE-EGA / NON-VGA\n");
    }

    out_str("\n[B] VESA VBE Extension\n");
    if (!vbe.supported) {
        out_str("  VESA VBE        : NOT SUPPORTED\n");
    } else {
        out_str("  VESA VBE Ver    : ");
        out_ulong(vbe.version >> 8); out_str("."); out_ulong(vbe.version & 0xFFU);
        out_str(" | Total VRAM: ");
        out_ulong((uint32_t)vbe.vram_64k * 64UL); out_str(" KB\n");
        if (vbe.oem_str[0] != '\0') {
            out_str("  VBE OEM String  : "); out_str(vbe.oem_str); out_str("\n");
        }
        out_str("  VBE Modes       : 640x480x8="); out_str(vbe.has_101 ? "YES" : "NO");
        out_str(", 800x600x8="); out_str(vbe.has_103 ? "YES" : "NO");
        out_str(", 1024x768x8="); out_str(vbe.has_105 ? "YES" : "NO");
        out_str(", TrueColor="); out_str(vbe.has_truecolor ? "YES\n" : "NO\n");
        out_str("  Linear FB (LFB) : ");
        if (vbe.lfb_101 && vbe.lfb_addr_101 != 0) {
            out_str("SUPPORTED at 0x"); out_hex32(vbe.lfb_addr_101); out_str("\n");
        } else {
            out_str("NOT AVAILABLE (Banked window only)\n");
        }
    }

    out_str("\n[C] Live Mode Switch & VRAM Read-back Verification\n");
    out_str("  - Mode 0x04 (CGA 320x200 4-col)   : "); out_str(cga4_ok  ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x06 (CGA 640x200 2-col)   : "); out_str(cga2_ok  ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x13 (VGA 320x200 256-col) : "); out_str(vga13_ok ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x12 (VGA 640x480 16-col)  : "); out_str(vga12_ok ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x101 (VESA 640x480 256-c) : "); out_str(vbe101_ok ? "PASS\n\n" : "FAIL/UNSUPPORTED\n\n");
    /* [C] 섹션 내 EGA 및 Super VGA(VESA) 결과 */
    out_str("  - Mode 0x0D (EGA 320x200 16-col)  : "); out_str(ega0d_ok ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x10 (EGA 640x350 16-col)  : "); out_str(ega10_ok ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x101 (SVGA 640x480x256)   : "); out_str(vbe101_ok ? "PASS" : "FAIL/UNSUPPORTED");
    if (vbe101_ok) {
        out_str(vbe_bank_ok ? " (4F05h Bank Switch OK)\n" : " (4F05h Bank Switch FAIL)\n");
    } else {
        out_str("\n");
    }
    out_str("  - Mode 0x103 (SVGA 800x600x256)   : "); out_str(svga103_ok ? "PASS\n" : "FAIL/UNSUPPORTED\n");
    out_str("  - Mode 0x111 (SVGA 640x480 16bpp) : "); out_str(svga111_ok ? "PASS\n\n" : "FAIL/UNSUPPORTED\n\n");
}