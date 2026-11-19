#ifndef TXTIO_H
#define TXTIO_H

/* 외부 헤더 0개 Turbo C 2.01 / Turbo C++ 3.0 (C89) 호환 */

typedef signed long int32_t;
typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

#define OUT_NONE 0
#define OUT_VRAM 1
#define OUT_BIOS 2

/* 입출력 진단 결과 기록 구조체 (이후 포트/그래픽/보호모드 단계에서 공용 참조) */
struct io_caps {
    uint8_t  vram_ok;       /* 1: VRAM 버스 역읽기 성공 */
    uint16_t vram_seg;      /* 0xB800 (Color) 또는 0xB000 (Mono) */
    uint16_t crtc_port;     /* 0x03D4 또는 0x03B4 */
    uint8_t  vram_row;      /* VRAM 직접 출력용 현재 행 (0~24) */
    uint8_t  vram_col;      /* VRAM 직접 출력용 현재 열 (0~79) */
    uint8_t  vram_attr;     /* 기본 텍스트 속성 (0x07: 회색 글씨) */

    uint8_t  bios_ok;       /* 1: INT 10h 유효 및 커서 전진 확인 */
    uint8_t  kbd_bios_ok;   /* 1: INT 16h 키보드 벡터 유효 */
    uint8_t  active_out;    /* 최종 채택된 출력 모드 (OUT_VRAM / OUT_BIOS) */
};

struct io_caps g_io;

/* 전역 출력 함수 포인터 ? 검증 완료 후 성공한 구현체로 바인딩됨 */
void null_put_ch(char c) { (void)c; }
void (*put_ch)(char c) = null_put_ch;

/* ---------------------------------------------------------
 * 저수준 포트 입출력
 * --------------------------------------------------------- */
void outb(uint16_t port, uint8_t val)
{
    _DX = port;
    _AL = val;
    __emit__(0xEE);         /* OUT DX, AL */
}

uint8_t inb(uint16_t port)
{
    _DX = port;
    __emit__(0xEC);         /* IN AL, DX */
    return _AL;
}

/* ---------------------------------------------------------
 * 백엔드 A: Direct VRAM 드라이버
 * --------------------------------------------------------- */
void vram_sync_hw_cursor(void)
{
    uint16_t pos;
    if (g_io.crtc_port == 0) {
        return;
    }
    pos = (uint16_t)g_io.vram_row * 80U + (uint16_t)g_io.vram_col;
    outb(g_io.crtc_port, 0x0E);
    outb(g_io.crtc_port + 1U, (uint8_t)(pos >> 8));
    outb(g_io.crtc_port, 0x0F);
    outb(g_io.crtc_port + 1U, (uint8_t)(pos & 0xFF));
}

void vram_scroll_up(void)
{
    uint16_t far *vmem;
    uint16_t blank;
    int i;

    vmem = (uint16_t far *)MK_FP(g_io.vram_seg, 0x0000);
    blank = ((uint16_t)g_io.vram_attr << 8) | (uint16_t)' ';

    for (i = 0; i < 80 * 24; i++) {
        vmem[i] = vmem[i + 80];
    }
    for (i = 80 * 24; i < 80 * 25; i++) {
        vmem[i] = blank;
    }
    g_io.vram_row = 24;
}

void vram_put_ch(char c)
{
    uint16_t far *vmem;
    uint16_t offset;

    if (c == '\r') {
        g_io.vram_col = 0;
        vram_sync_hw_cursor();
        return;
    }
    if (c == '\n') {
        g_io.vram_col = 0;
        g_io.vram_row++;
        if (g_io.vram_row >= 25) {
            vram_scroll_up();
        }
        vram_sync_hw_cursor();
        return;
    }

    vmem = (uint16_t far *)MK_FP(g_io.vram_seg, 0x0000);
    offset = (uint16_t)g_io.vram_row * 80U + (uint16_t)g_io.vram_col;
    vmem[offset] = ((uint16_t)g_io.vram_attr << 8) | (uint8_t)c;

    g_io.vram_col++;
    if (g_io.vram_col >= 80) {
        g_io.vram_col = 0;
        g_io.vram_row++;
        if (g_io.vram_row >= 25) {
            vram_scroll_up();
        }
    }
    vram_sync_hw_cursor();
}

void vram_write_at(uint8_t row, uint8_t col, uint8_t attr, const char *s)
{
    uint16_t far *vmem;
    uint16_t pos;

    vmem = (uint16_t far *)MK_FP(g_io.vram_seg, 0x0000);
    pos = (uint16_t)row * 80U + (uint16_t)col;
    while (*s && pos < 80U * 25U) {
        vmem[pos++] = ((uint16_t)attr << 8) | (uint8_t)(*s++);
    }
}

/* ---------------------------------------------------------
 * 백엔드 B: BIOS INT 10h (AH=0Eh) 드라이버
 * --------------------------------------------------------- */
void bios_put_ch(char c)
{
    if (c == '\n') {
        _AH = 0x0E;
        _AL = '\r';
        _BH = 0;
        __emit__(0xCD, 0x10);
    }
    _AH = 0x0E;
    _AL = (uint8_t)c;
    _BH = 0;
    __emit__(0xCD, 0x10);
}

/* ---------------------------------------------------------
 * 공통 문자열 / 숫자 출력 유틸리티
 * --------------------------------------------------------- */
void put_str(const char *s)
{
    while (*s) {
        put_ch(*s++);
    }
}

void put_hex16(uint16_t val)
{
    const char hex[] = "0123456789ABCDEF";
    int shift;
    for (shift = 12; shift >= 0; shift -= 4) {
        put_ch(hex[(val >> shift) & 0x0F]);
    }
}

void put_ulong(uint32_t val)
{
    char buf[12];
    int idx = 0;

    if (val == 0) {
        put_ch('0');
        return;
    }
    while (val > 0) {
        buf[idx++] = (char)('0' + (val % 10UL));
        val /= 10UL;
    }
    while (idx > 0) {
        put_ch(buf[--idx]);
    }
}

/* ---------------------------------------------------------
 * IVT 벡터 검사 및 텍스트 콘솔 자동 검증·바인딩
 * --------------------------------------------------------- */
uint8_t is_ivt_valid(uint8_t int_num)
{
    uint16_t far *ivt;
    uint16_t off, seg;
    uint8_t far *entry;

    ivt = (uint16_t far *)MK_FP(0x0000, (uint16_t)int_num * 4U);
    off = ivt[0];
    seg = ivt[1];

    if (off == 0 && seg == 0) {
        return 0;
    }
    entry = (uint8_t far *)MK_FP(seg, off);
    if (*entry == 0xCF) {
        return 0;
    }
    return 1;
}

void probe_and_bind_console(void)
{
    uint16_t far *bda_crtc;
    uint8_t  far *bda_cur_col;
    uint8_t  far *bda_cur_row;
    volatile uint32_t far *ticks;
    uint16_t far *vmem;
    uint16_t saved_word, r1, r2;
    uint8_t  col_before, row_before;
    uint32_t t0;
    const char *msg;

    bda_crtc    = (uint16_t far *)MK_FP(0x0040, 0x0063);
    bda_cur_col = (uint8_t  far *)MK_FP(0x0040, 0x0050);
    bda_cur_row = (uint8_t  far *)MK_FP(0x0040, 0x0051);
    ticks       = (volatile uint32_t far *)MK_FP(0x0040, 0x006C);

    /* 1. Direct VRAM 하드웨어 역읽기 검증 */
    g_io.crtc_port = *bda_crtc;
    if (g_io.crtc_port == 0x03B4) {
        g_io.vram_seg = 0xB000;
    } else {
        g_io.vram_seg = 0xB800;
        if (g_io.crtc_port == 0) {
            g_io.crtc_port = 0x03D4;
        }
    }
    g_io.vram_attr = 0x07;

    vmem = (uint16_t far *)MK_FP(g_io.vram_seg, 0x0000);
    saved_word = vmem[0];

    vmem[0] = 0x1F56;
    r1 = vmem[0];
    vmem[0] = 0x707E;
    r2 = vmem[0];
    vmem[0] = saved_word;

    if (r1 == 0x1F56 && r2 == 0x707E) {
        g_io.vram_ok = 1;
        vram_write_at(0, 0, 0x1F,
            "[TEST 1] DIRECT VRAM WRITE : HW OK (Press [1] to force VRAM)   ");
    } else {
        g_io.vram_ok = 0;
    }

    /* 2. BIOS INT 10h 커서 이동 + Teletype 출력 검증 */
    if (is_ivt_valid(0x10)) {
        _AH = 0x02;
        _BH = 0;
        _DH = 2;
        _DL = 0;
        __emit__(0xCD, 0x10);

        col_before = *bda_cur_col;
        row_before = *bda_cur_row;

        msg = "[TEST 2] BIOS INT 10h OUT  : HW OK (Press [2] to force BIOS)\r\n";
        while (*msg) {
            bios_put_ch(*msg++);
        }

        if (*bda_cur_col != col_before || *bda_cur_row != row_before) {
            g_io.bios_ok = 1;
        }
    }

    g_io.vram_row = 4;
    g_io.vram_col = 0;

    /* 3. 기본 출력 모드 결정 (둘 다 성공 시 VRAM 우선, 3초 수동 선택 허용) */
    if (g_io.vram_ok) {
        g_io.active_out = OUT_VRAM;
    } else if (g_io.bios_ok) {
        g_io.active_out = OUT_BIOS;
    } else {
        g_io.active_out = OUT_NONE;
        return;
    }

    g_io.kbd_bios_ok = is_ivt_valid(0x16);
    if (g_io.kbd_bios_ok && g_io.vram_ok && g_io.bios_ok) {
        t0 = *ticks;
        while ((*ticks - t0) < 55UL) {
            _AH = 0x01;
            __emit__(0xCD, 0x16);
            if ((_FLAGS & 0x0040) == 0) {
                uint8_t ascii;
                _AH = 0x00;
                __emit__(0xCD, 0x16);
                ascii = _AL;
                if (ascii == '1') {
                    g_io.active_out = OUT_VRAM;
                    break;
                }
                if (ascii == '2') {
                    g_io.active_out = OUT_BIOS;
                    break;
                }
            }
        }
    }

    /* 4. 확정된 출력 드라이버를 전역 put_ch 함수 포인터에 차용 */
    if (g_io.active_out == OUT_VRAM) {
        put_ch = vram_put_ch;
    } else if (g_io.active_out == OUT_BIOS) {
        _AH = 0x02;
        _BH = 0;
        _DH = 4;
        _DL = 0;
        __emit__(0xCD, 0x10);
        put_ch = bios_put_ch;
    }
}

#endif /* TXTIO_H */