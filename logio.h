#ifndef LOGIO_H
#define LOGIO_H

#include "txtio.h"

#define LOG_MAX_SECTORS 120
#define LOG_BUF_SIZE    (LOG_MAX_SECTORS * 512U)
#define LOG_BUF_SEG     0x4000U

#define FP_SEG(fp) ((uint16_t)((uint32_t)(fp) >> 16))
#define FP_OFF(fp) ((uint16_t)(uint32_t)(fp))

struct disk_dap {
    uint8_t  size;
    uint8_t  zero;
    uint16_t count;
    uint16_t buf_off;
    uint16_t buf_seg;
    uint32_t lba_lo;
    uint32_t lba_hi;
};

static uint8_t far *g_log_buf = (uint8_t far *)MK_FP(LOG_BUF_SEG, 0x0000);
static uint8_t  g_sec_tmp[512];
static uint32_t g_log_lba[LOG_MAX_SECTORS];
static uint8_t  g_log_sectors = 0;
static uint16_t g_log_pos = 0;
static uint8_t  g_log_drv = 0x80;
static uint8_t  g_log_ready = 0;
static uint8_t  g_log_is_file = 0;

/* ---------------------------------------------------------
 * BIOS INT 13h 확장 읽기/쓰기 (AH=42h / AH=43h, Far 버퍼 지원)
 * --------------------------------------------------------- */
static uint8_t bios_disk_rw(uint8_t func, uint32_t lba, void far *buf)
{
    struct disk_dap dap;
    uint16_t dap_off;
    uint8_t cf;

    dap.size    = 16;
    dap.zero    = 0;
    dap.count   = 1;
    dap.buf_off = FP_OFF(buf);
    dap.buf_seg = FP_SEG(buf);
    dap.lba_lo  = lba;
    dap.lba_hi  = 0;

    dap_off = (uint16_t)&dap;
    _SI = dap_off;
    _DL = g_log_drv;
    _AL = 0x00;
    _AH = func;
    __emit__(0x55, 0x56, 0x57);       /* PUSH BP, SI, DI */
    __emit__(0xCD, 0x13);             /* INT 13h */
    __emit__(0x5F, 0x5E, 0x5D);       /* POP DI, SI, BP */
    cf = (uint8_t)(_FLAGS & 0x0001);

    return (cf == 0) ? 1 : 0;
}

static uint32_t log_next_clus(uint32_t clus, uint32_t fat_start_lba)
{
    uint32_t fat_sec;
    uint16_t ent_off;
    uint32_t *p32;

    fat_sec = fat_start_lba + (clus >> 7);
    ent_off = (uint16_t)((clus & 0x7FUL) << 2);
    if (!bios_disk_rw(0x42, fat_sec, (void far *)MK_FP(_DS, (uint16_t)g_sec_tmp))) {
        return 0x0FFFFFFFUL;
    }
    p32 = (uint32_t *)(g_sec_tmp + ent_off);
    return (*p32) & 0x0FFFFFFFUL;
}

/* ---------------------------------------------------------
 * 변경된 섹터만 즉시 디스크로 플러시
 * --------------------------------------------------------- */
void log_flush_range(uint16_t start_pos, uint16_t end_pos)
{
    uint8_t s_first, s_last, s;

    if (!g_log_ready || g_log_sectors == 0) {
        return;
    }
    if (end_pos <= start_pos) {
        return;
    }
    s_first = (uint8_t)(start_pos >> 9);
    s_last  = (uint8_t)((end_pos - 1U) >> 9);
    if (s_last >= g_log_sectors) {
        s_last = (uint8_t)(g_log_sectors - 1U);
    }

    for (s = s_first; s <= s_last; s++) {
        bios_disk_rw(0x43, g_log_lba[s], &g_log_buf[(uint16_t)s << 9]);
    }
}

/* ---------------------------------------------------------
 * 로그 초기화: RESULT.TXT 탐색 (미존재 시 예약 섹터 7 사용)
 * --------------------------------------------------------- */
void log_str(const char *s);

void log_init(void)
{
    uint8_t  far *bpb8;
    uint16_t far *bpb16;
    uint32_t far *bpb32;
    uint8_t  sec_per_clus, num_fats;
    uint16_t rsvd_sec, i, ent;
    uint32_t hidd_sec = 0UL;
    uint32_t fat_sz32, root_clus, fat_start_lba, data_lba, cur_clus, lba;
    const char target[11] = "RESULT  TXT";

    for (i = 0; i < LOG_BUF_SIZE; i++) {
        g_log_buf[i] = ' ';
    }
    g_log_pos = 0;

    bpb8  = (uint8_t  far *)MK_FP(0x0000, 0x7C00);
    bpb16 = (uint16_t far *)MK_FP(0x0000, 0x7C00);
    bpb32 = (uint32_t far *)MK_FP(0x0000, 0x7C00);

    g_log_drv    = bpb8[0x40];
    sec_per_clus = bpb8[0x0D];
    num_fats     = bpb8[0x10];
    rsvd_sec     = bpb16[0x0E / 2];
    hidd_sec     = bpb32[0x1C / 4];     /* BPB_HiddSec (MBR 파티션 시작 LBA, 슈퍼플로피는 0) */
    fat_sz32     = bpb32[0x24 / 4];
    root_clus    = bpb32[0x2C / 4] & 0x0FFFFFFFUL;

    if (sec_per_clus == 0 || rsvd_sec == 0) {
        return;
    }

    fat_start_lba = hidd_sec + (uint32_t)rsvd_sec;
    data_lba      = fat_start_lba + ((uint32_t)num_fats * fat_sz32);
    cur_clus      = root_clus;

    /* 루트 디렉터리에서 RESULT.TXT 탐색 */
    while (cur_clus >= 2UL && cur_clus < 0x0FFFFFF8UL) {
        uint8_t s;
        lba = data_lba + (cur_clus - 2UL) * (uint32_t)sec_per_clus;
        for (s = 0; s < sec_per_clus; s++) {
            if (!bios_disk_rw(0x42, lba + s, g_sec_tmp)) {
                goto fallback_sector;
            }
            for (ent = 0; ent < 512; ent += 32) {
                uint8_t match = 1;
                uint8_t k;
                if (g_sec_tmp[ent] == 0x00) {
                    goto fallback_sector;
                }
                for (k = 0; k < 11; k++) {
                    if (g_sec_tmp[ent + k] != (uint8_t)target[k]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    uint32_t fclus;
                    uint8_t  cnt = 0;
                    fclus = ((uint32_t)(*(uint16_t *)(g_sec_tmp + ent + 0x14)) << 16) |
                             (uint32_t)(*(uint16_t *)(g_sec_tmp + ent + 0x1A));
                    while (fclus >= 2UL && fclus < 0x0FFFFFF8UL && cnt < LOG_MAX_SECTORS) {
                        uint8_t cs;
                        uint32_t clba = data_lba + (fclus - 2UL) * (uint32_t)sec_per_clus;
                        for (cs = 0; cs < sec_per_clus && cnt < LOG_MAX_SECTORS; cs++) {
                            g_log_lba[cnt++] = clba + cs;
                        }
                        fclus = log_next_clus(fclus, fat_start_lba);
                    }
                    if (cnt > 0) {
                        uint16_t total_b = (uint16_t)cnt << 9;
                        uint16_t scan;

                        g_log_sectors = cnt;
                        g_log_is_file = 1;
                        g_log_ready   = 1;

                        for (i = 0; i < cnt; i++) {
                            bios_disk_rw(0x42, g_log_lba[i], &g_log_buf[i << 9]);
                        }

                        g_log_pos = 0;
                        for (scan = 0; scan < total_b; scan++) {
                            if (g_log_buf[scan] == 0x00) {
                                g_log_buf[scan] = ' ';
                            } else if (g_log_buf[scan] != ' ') {
                                g_log_pos = scan + 1U;
                            }
                        }
                        /* 남은 공간이 12KB(1세션 분량) 미만이면 처음(0)부터 새로 기록 */
                        if (g_log_pos > 0U && (total_b <= 12288U || g_log_pos > (uint16_t)(total_b - 12288U))) {
                            for (scan = 0; scan < total_b; scan++) {
                                g_log_buf[scan] = ' ';
                            }
                            g_log_pos = 0;
                            for (i = 0; i < cnt; i++) {
                                bios_disk_rw(0x43, g_log_lba[i], &g_log_buf[i << 9]);
                            }
                        } else if (g_log_pos > 0) {
                            log_str("\n==================================================\n");
                        }
                        return;
                    }
                }
            }
        }
        cur_clus = log_next_clus(cur_clus, fat_start_lba);
    }

fallback_sector:
    /* RESULT.TXT 미존재 시 해당 FAT32 볼륨의 예약 섹터 7번에 백업 기록 */
    g_log_lba[0]  = hidd_sec + 7UL;
    g_log_sectors = 1;
    g_log_is_file = 0;
    g_log_ready   = 1;
    bios_disk_rw(0x43, g_log_lba[0], g_log_buf);
}

/* ---------------------------------------------------------
 * 로그 버퍼 기록 함수 (줄바꿈 시 CRLF 변환 후 즉시 섹터 쓰기)
 * --------------------------------------------------------- */
void log_str(const char *s)
{
    uint16_t start = g_log_pos;
    uint16_t max_bytes = (uint16_t)g_log_sectors << 9;

    if (!g_log_ready) {
        return;
    }
    while (*s && g_log_pos < max_bytes) {
        if (*s == '\n') {
            if (g_log_pos + 2U <= max_bytes) {
                g_log_buf[g_log_pos++] = '\r';
                g_log_buf[g_log_pos++] = '\n';
            }
        } else {
            g_log_buf[g_log_pos++] = (uint8_t)(*s);
        }
        s++;
    }
    log_flush_range(start, g_log_pos);
}

/* ---------------------------------------------------------
 * 화면 + RESULT.TXT 동시 출력 유틸리티
 * --------------------------------------------------------- */
void out_str(const char *s)
{
    put_str(s);
    log_str(s);
}

void out_hex16(uint16_t val)
{
    const char hex[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = hex[(val >> 12) & 0x0F];
    buf[1] = hex[(val >> 8)  & 0x0F];
    buf[2] = hex[(val >> 4)  & 0x0F];
    buf[3] = hex[val & 0x0F];
    buf[4] = '\0';
    out_str(buf);
}

void out_ulong(uint32_t val)
{
    char buf[12];
    char rev[12];
    int i = 0, j = 0;

    if (val == 0) {
        out_str("0");
        return;
    }
    while (val > 0) {
        rev[i++] = (char)('0' + (val % 10UL));
        val /= 10UL;
    }
    while (i > 0) {
        buf[j++] = rev[--i];
    }
    buf[j] = '\0';
    out_str(buf);
}

/* 현재 로그 파일 기록 위치 반환 (체크포인트 저장용) */
uint16_t log_get_pos(void)
{
    return g_log_pos;
}

/* 저장된 체크포인트 위치로 되감고 이전 임시 실패 문자열을 공백으로 소거 후 디스크 플러시 */
void log_rewind_to(uint16_t saved_pos)
{
    uint16_t old_end = g_log_pos;
    uint16_t i;

    if (!g_log_ready) {
        return;
    }
    if (saved_pos >= old_end) {
        g_log_pos = saved_pos;
        return;
    }
    for (i = saved_pos; i < old_end; i++) {
        g_log_buf[i] = ' ';
    }
    g_log_pos = saved_pos;
    log_flush_range(saved_pos, old_end);
}

/* 화면에는 출력하지 않고 오직 로그 파일에만 숫자를 기록 */
void log_ulong_only(uint32_t val)
{
    char buf[12], rev[12];
    int i = 0, j = 0;

    if (val == 0) {
        log_str("0");
        return;
    }
    while (val > 0) {
        rev[i++] = (char)('0' + (val % 10UL));
        val /= 10UL;
    }
    while (i > 0) {
        buf[j++] = rev[--i];
    }
    buf[j] = '\0';
    log_str(buf);
}

#endif /* LOGIO_H */