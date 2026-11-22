typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);

/* ---------------------------------------------------------
 * 드라이브 프로빙 결과 구조체
 * --------------------------------------------------------- */
#define MAX_DISKS 16

struct disk_info {
    uint8_t  dl;                /* BIOS 드라이브 번호 (0x00.., 0x80.., 0xE0..) */
    uint8_t  is_boot;           /* 1 = 현재 부팅 미디어 */
    uint8_t  emu_class;         /* 1=FDD, 2=HDD, 3=CD-ROM */

    /* BIOS 레거시 CHS (INT 13h AH=08h) */
    uint8_t  chs_ok;
    uint16_t chs_cyls;
    uint16_t chs_heads;
    uint16_t chs_secs;
    uint8_t  fdd_cmos_type;     /* 1=360K, 2=1.2M, 3=720K, 4=1.44M, 5/6=2.88M */
    uint32_t emu_mb;            /* BIOS가 노출하는 에뮬레이션 용량 (MB) */
    uint32_t emu_kb;            /* 플로피용 정밀 KB */

    /* EDD LBA 확장 물리 정보 (INT 13h AH=41h / AH=48h) */
    uint8_t  edd_ok;
    uint16_t edd_flags;         /* Bit 2 = Removable */
    uint32_t phys_sec_lo;
    uint32_t phys_sec_hi;
    uint16_t bytes_per_sec;
    uint32_t phys_mb;           /* 실제 물리 용량 (MB) */
    char     bus_str[5];        /* "PCI ", "USB ", "ISA " 등 */
    char     if_str[9];         /* "SATA    ", "USB     ", "ATA     ", "ATAPI   " 등 */

    /* El Torito CD-ROM 에뮬레이션 (INT 13h AX=4B01h) */
    uint8_t  eltorito_ok;
    uint8_t  eltorito_media;    /* 0=NoEmu, 1=1.2M, 2=1.44M, 3=2.88M, 4=HDD */
};

static struct disk_info g_disks[MAX_DISKS];
static uint8_t g_disk_count = 0;

/* ---------------------------------------------------------
 * 저수준 INT 13h 래퍼 (BP/SI/DI/ES 보존)
 * --------------------------------------------------------- */
static uint8_t int13_get_chs(uint8_t drv, uint16_t *cyls, uint16_t *heads,
                             uint16_t *secs, uint8_t *fdd_type)
{
    uint8_t  cf, r_bl, r_dh;
    uint16_t r_cx;

    _DL = drv;
    _AH = 0x08;
    __emit__(0x55, 0x56, 0x57, 0x06, 0xCD, 0x13, 0x07, 0x5F, 0x5E, 0x5D);
    cf   = (uint8_t)(_FLAGS & 0x0001);
    r_bl = _BL;
    r_cx = _CX;
    r_dh = _DH;

    if (cf != 0) {
        return 0;
    }
    *secs     = (uint16_t)(r_cx & 0x003FU);
    *cyls     = (uint16_t)((((r_cx & 0x00C0U) << 2) | (r_cx >> 8)) + 1U);
    *heads    = (uint16_t)r_dh + 1U;
    *fdd_type = r_bl;
    return (*secs > 0 && *heads > 0) ? 1 : 0;
}

static uint8_t int13_check_edd(uint8_t drv)
{
    uint8_t  cf;
    uint16_t r_bx;

    _BX = 0x55AA;
    _DL = drv;
    _AH = 0x41;
    __emit__(0x55, 0x56, 0x57, 0x06, 0xCD, 0x13, 0x07, 0x5F, 0x5E, 0x5D);
    cf   = (uint8_t)(_FLAGS & 0x0001);
    r_bx = _BX;

    return (cf == 0 && r_bx == 0xAA55U) ? 1 : 0;
}

static uint8_t int13_get_edd_params(uint8_t drv, uint8_t *buf66)
{
    uint8_t  cf;
    uint16_t i, r_ax, bps;
    uint32_t sec_lo, sec_hi;

    for (i = 0; i < 66; i++) {
        buf66[i] = 0;
    }
    *(uint16_t *)(buf66 + 0) = 66U;

    _SI = (uint16_t)buf66;
    _DL = drv;
    _AH = 0x48;
    __emit__(0x55, 0x56, 0x57, 0x06, 0xCD, 0x13, 0x07, 0x5F, 0x5E, 0x5D);
    r_ax = _AX;
    cf   = (uint8_t)(_FLAGS & 0x0001);

    if (cf != 0 || (r_ax >> 8) != 0 || *(uint16_t *)(buf66 + 0) < 26U) {
        return 0;
    }
    sec_lo = *(uint32_t *)(buf66 + 16);
    sec_hi = *(uint32_t *)(buf66 + 20);
    bps    = *(uint16_t *)(buf66 + 24);
    return (bps > 0U && (sec_lo > 0UL || sec_hi > 0UL)) ? 1 : 0;
}

static uint8_t int13_get_eltorito(uint8_t drv, uint8_t *pkt20)
{
    uint8_t  cf, i;
    uint16_t r_ax;

    for (i = 0; i < 20; i++) {
        pkt20[i] = 0;
    }
    pkt20[0] = 0x13;

    _SI = (uint16_t)pkt20;
    _DL = drv;
    _AX = 0x4B01;
    __emit__(0x55, 0x56, 0x57, 0x06, 0xCD, 0x13, 0x07, 0x5F, 0x5E, 0x5D);
    r_ax = _AX;
    cf   = (uint8_t)(_FLAGS & 0x0001);

    return (cf == 0 && (r_ax >> 8) == 0 && pkt20[0] >= 0x13U && pkt20[2] == drv) ? 1 : 0;
}

/* ---------------------------------------------------------
 * 용량 계산 및 문자열 변환 유틸리티
 * --------------------------------------------------------- */
static uint32_t sectors_to_mb(uint32_t sec_lo, uint32_t sec_hi, uint16_t bps)
{
    uint32_t mult, s_lo, s_hi;

    if (bps == 0) {
        bps = 512U;
    }
    mult = (uint32_t)(bps >> 9);
    if (mult == 0) {
        mult = 1UL;
    }
    s_lo = sec_lo * mult;
    s_hi = sec_hi * mult;
    return (s_hi << 21) | (s_lo >> 11);
}

/* MB 값을 "X.Y GB" 또는 "Z MB" 형태로 가독성 있게 출력 */
static void print_size_human(uint32_t mb, uint32_t kb_if_small)
{
    if (mb == 0 && kb_if_small > 0) {
        uint32_t mb_int  = kb_if_small >> 10;
        uint32_t mb_frac = ((kb_if_small & 1023UL) * 100UL) >> 10;
        if (mb_int > 0) {
            out_ulong(mb_int);
            out_str(".");
            if (mb_frac < 10UL) {
                out_str("0");
            }
            out_ulong(mb_frac);
            out_str(" MB (");
            out_ulong(kb_if_small);
            out_str(" KB)");
        } else {
            out_ulong(kb_if_small);
            out_str(" KB");
        }
        return;
    }

    if (mb >= 1024UL) {
        uint32_t gb_int  = mb >> 10;
        uint32_t gb_frac = ((mb & 1023UL) * 10UL) >> 10;
        out_ulong(gb_int);
        out_str(".");
        out_ulong(gb_frac);
        out_str(" GB (");
        out_ulong(mb);
        out_str(" MB)");
    } else {
        out_ulong(mb);
        out_str(" MB");
    }
}

static uint8_t str_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static void copy_trimmed(char *dst, const uint8_t *src, uint8_t len)
{
    uint8_t i, end = 0;
    for (i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c < 0x20 || c > 0x7E) {
            c = ' ';
        }
        dst[i] = (char)c;
        if (c != ' ') {
            end = (uint8_t)(i + 1U);
        }
    }
    dst[end] = '\0';
}

/* ---------------------------------------------------------
 * 개별 BIOS 드라이브 프로빙
 * --------------------------------------------------------- */
static uint8_t probe_single_drive(uint8_t drv, uint8_t boot_dl)
{
    struct disk_info d;
    uint8_t  edd_buf[66];
    uint8_t  et_pkt[20];
    uint16_t sz;
    uint32_t chs_secs;

    if (g_disk_count >= MAX_DISKS) {
        return 0;
    }

    d.dl            = drv;
    d.is_boot       = (uint8_t)(drv == boot_dl);
    d.emu_class     = (drv < 0x80) ? 1 : ((drv >= 0xE0) ? 3 : 2);
    d.chs_ok        = 0;
    d.chs_cyls      = 0;
    d.chs_heads     = 0;
    d.chs_secs      = 0;
    d.fdd_cmos_type = 0;
    d.emu_mb        = 0;
    d.emu_kb        = 0;
    d.edd_ok        = 0;
    d.edd_flags     = 0;
    d.phys_sec_lo   = 0;
    d.phys_sec_hi   = 0;
    d.bytes_per_sec = 512U;
    d.phys_mb       = 0;
    d.bus_str[0]    = '\0';
    d.if_str[0]     = '\0';
    d.eltorito_ok   = 0;
    d.eltorito_media = 0;

    /* 1. El Torito 광학 부팅 에뮬레이션 여부 조회 */
    if (int13_get_eltorito(drv, et_pkt)) {
        d.eltorito_ok    = 1;
        d.eltorito_media = (uint8_t)(et_pkt[1] & 0x0F);
        if (d.eltorito_media == 0) {
            d.emu_class = 3;
        }
    }

    /* 2. 레거시 CHS 기하구조 조회 (INT 13h AH=08h) */
    if (int13_get_chs(drv, &d.chs_cyls, &d.chs_heads, &d.chs_secs, &d.fdd_cmos_type)) {
        d.chs_ok = 1;
        chs_secs = (uint32_t)d.chs_cyls * (uint32_t)d.chs_heads * (uint32_t)d.chs_secs;
        d.emu_kb = chs_secs >> 1;
        d.emu_mb = chs_secs >> 11;
    }

    /* 3. EDD 확장 물리 드라이브 파라미터 조회 (INT 13h AH=41h / AH=48h) */
    if (int13_check_edd(drv) && int13_get_edd_params(drv, edd_buf)) {
        d.edd_ok        = 1;
        sz              = *(uint16_t *)(edd_buf + 0);
        d.edd_flags     = *(uint16_t *)(edd_buf + 2);
        d.phys_sec_lo   = *(uint32_t *)(edd_buf + 16);
        d.phys_sec_hi   = *(uint32_t *)(edd_buf + 20);
        d.bytes_per_sec = *(uint16_t *)(edd_buf + 24);
        d.phys_mb       = sectors_to_mb(d.phys_sec_lo, d.phys_sec_hi, d.bytes_per_sec);

        if (sz >= 66U && *(uint16_t *)(edd_buf + 30) == 0xBEDDU) {
            copy_trimmed(d.bus_str, edd_buf + 36, 4);
            copy_trimmed(d.if_str,  edd_buf + 40, 8);
        }
    }

    if (!d.chs_ok && !d.edd_ok && !d.eltorito_ok) {
        return 0;
    }

    g_disks[g_disk_count++] = d;
    return 1;
}

/* ---------------------------------------------------------
 * 물리 미디어 유형 문자열 및 부트 미디어 비교 판정
 * --------------------------------------------------------- */
static void print_real_media_desc(const struct disk_info *d, uint32_t real_mb, uint32_t real_kb)
{
    print_size_human(real_mb, real_kb);
    out_str(" ");

    if (d->eltorito_ok || str_starts_with(d->if_str, "ATAPI")) {
        out_str("CD/DVD-ROM (Optical)");
        return;
    }
    if (str_starts_with(d->bus_str, "USB") || str_starts_with(d->if_str, "USB")) {
        out_str("USB Flash / External Drive");
        return;
    }
    if (d->dl < 0x80 && real_mb <= 4UL) {
        out_str("Legacy Floppy Diskette");
        return;
    }
    if ((d->edd_flags & 0x0004U) != 0) {
        out_str("Removable Flash / Card Media");
        return;
    }
    if (d->if_str[0] != '\0') {
        out_str(d->if_str);
        out_str(" Fixed Disk");
        return;
    }
    if (d->bus_str[0] != '\0') {
        out_str(d->bus_str);
        out_str(" Fixed Disk");
        return;
    }
    out_str((d->dl < 0x80) ? "Floppy Media" : "Fixed Hard Disk (ATA/BIOS)");
}

static void print_emu_media_desc(const struct disk_info *d)
{
    if (d->eltorito_ok) {
        if (d->eltorito_media == 1)      { out_str("1.2 MB Floppy (El Torito)"); return; }
        else if (d->eltorito_media == 2) { out_str("1.44 MB Floppy (El Torito)"); return; }
        else if (d->eltorito_media == 3) { out_str("2.88 MB Floppy (El Torito)"); return; }
        else if (d->eltorito_media == 0) { out_str("No-Emulation CD-ROM (2048B/sec)"); return; }
    }

    print_size_human(d->emu_mb, d->emu_kb);
    if (d->dl < 0x80) {
        out_str(" Floppy Drive (FDD 0x");
        out_hex16(d->dl);
        out_str(")");
    } else if (d->dl >= 0xE0) {
        out_str(" Optical Drive (0x");
        out_hex16(d->dl);
        out_str(")");
    } else {
        out_str(" Hard Disk (HDD 0x");
        out_hex16(d->dl);
        out_str(")");
    }
}

static void analyze_boot_media(uint8_t boot_dl)
{
    const struct disk_info *bd = 0;
    uint32_t far *bpb_tot32 = (uint32_t far *)MK_FP(0x0000, 0x7C20);
    uint16_t far *bpb_tot16 = (uint16_t far *)MK_FP(0x0000, 0x7C13);
    uint32_t bpb_secs, bpb_mb, bpb_kb, real_mb, real_kb;
    uint8_t  i;

    for (i = 0; i < g_disk_count; i++) {
        if (g_disks[i].dl == boot_dl) {
            bd = &g_disks[i];
            break;
        }
    }

    out_str("[A] Boot Media & BIOS Emulation Analysis\n");
    out_str("  Boot Drive DL  : 0x"); out_hex16(boot_dl); out_str("\n");

    if (bd == 0) {
        out_str("  Status         : Boot drive parameters unavailable via INT 13h.\n\n");
        return;
    }

    /* BPB 파티션 총 섹터 수와 EDD 물리 섹터 수 중 큰 값을 실제 미디어 용량으로 산출 */
    bpb_secs = (*bpb_tot32 != 0) ? *bpb_tot32 : (uint32_t)(*bpb_tot16);
    bpb_kb   = bpb_secs >> 1;
    bpb_mb   = bpb_secs >> 11;

    real_mb = bd->edd_ok ? bd->phys_mb : bd->emu_mb;
    real_kb = bd->edd_ok ? (bd->phys_sec_lo >> 1) : bd->emu_kb;
    if (bpb_mb > real_mb) {
        real_mb = bpb_mb;
        real_kb = bpb_kb;
    }

    out_str("  Physical Media : ");
    print_real_media_desc(bd, real_mb, real_kb);
    out_str("\n");

    out_str("  BIOS Emulation : ");
    print_emu_media_desc(bd);
    if (bd->chs_ok) {
        out_str(" [CHS ");
        out_ulong(bd->chs_cyls); out_str("/");
        out_ulong(bd->chs_heads); out_str("/");
        out_ulong(bd->chs_secs); out_str("]");
    }
    out_str("\n");

    /* 요청 형식 요약 문장 출력 */
    out_str("  Verdict        : Originally ");
    print_real_media_desc(bd, real_mb, real_kb);
    out_str(",\n                   emulated by BIOS as ");
    print_emu_media_desc(bd);
    out_str(".\n\n");
}

/* ---------------------------------------------------------
 * Step 5 메인 엔트리: 전체 디스크 둘러보기 및 목록 출력
 * --------------------------------------------------------- */
void run_disk_test(void)
{
    uint8_t  far *p_boot_dl = (uint8_t far *)MK_FP(0x0000, 0x7C40);
    uint16_t far *bda_equip = (uint16_t far *)MK_FP(0x0040, 0x0010);
    uint8_t  far *bda_nhdd  = (uint8_t  far *)MK_FP(0x0040, 0x0075);
    uint8_t  boot_dl = *p_boot_dl;
    uint8_t  fdd_cnt, hdd_cnt, d, i, boot_found = 0;

    g_disk_count = 0;

    /* 1. 플로피 드라이브(0x00~0x03) 스캔 */
    fdd_cnt = ((*bda_equip & 0x0001U) != 0)
              ? (uint8_t)(((*bda_equip >> 6) & 0x03U) + 1U) : 0;
    for (d = 0x00; d <= 0x03; d++) {
        if (d < fdd_cnt || d == boot_dl) {
            if (probe_single_drive(d, boot_dl) && d == boot_dl) {
                boot_found = 1;
            }
        }
    }

    /* 2. 하드 디스크(0x80~0x8F) 스캔 */
    hdd_cnt = *bda_nhdd;
    for (d = 0x80; d <= 0x8F; d++) {
        uint8_t idx = (uint8_t)(d - 0x80U);
        if (probe_single_drive(d, boot_dl)) {
            if (d == boot_dl) {
                boot_found = 1;
            }
        } else if (idx >= hdd_cnt && d > boot_dl) {
            break;
        }
    }

    /* 3. El Torito / 광학 드라이브(0xE0~0xE3) 및 미포함 부트 DL 스캔 */
    for (d = 0xE0; d <= 0xE3; d++) {
        if (probe_single_drive(d, boot_dl) && d == boot_dl) {
            boot_found = 1;
        }
    }
    if (!boot_found) {
        probe_single_drive(boot_dl, boot_dl);
    }

    out_str("=== [Step 5] Boot Media & Disk Inventory ===\n\n");

    /* 부팅 미디어 에뮬레이션 판별 출력 */
    analyze_boot_media(boot_dl);

    /* 전체 디스크 목록 출력 */
    out_str("[B] Detected BIOS Disk List (Total: ");
    out_ulong(g_disk_count);
    out_str(")\n");

    for (i = 0; i < g_disk_count; i++) {
        const struct disk_info *dk = &g_disks[i];

        out_str("  - Drive 0x");
        out_hex16(dk->dl);
        out_str(dk->is_boot ? " [BOOT] : " : "        : ");

        if (dk->dl < 0x80)       out_str("FDD | ");
        else if (dk->dl >= 0xE0) out_str("CD  | ");
        else                     out_str("HDD | ");

        out_str("Phys=");
        if (dk->edd_ok) {
            print_size_human(dk->phys_mb, dk->phys_sec_lo >> 1);
        } else {
            print_size_human(dk->emu_mb, dk->emu_kb);
        }

        out_str(" | Emu=");
        print_size_human(dk->emu_mb, dk->emu_kb);

        if (dk->chs_ok) {
            out_str(" (CHS ");
            out_ulong(dk->chs_cyls); out_str("/");
            out_ulong(dk->chs_heads); out_str("/");
            out_ulong(dk->chs_secs); out_str(")");
        }

        if (dk->bus_str[0] != '\0' || dk->if_str[0] != '\0') {
            out_str(" [");
            if (dk->bus_str[0] != '\0') {
                out_str(dk->bus_str);
                if (dk->if_str[0] != '\0') {
                    out_str("/");
                }
            }
            if (dk->if_str[0] != '\0') {
                out_str(dk->if_str);
            }
            out_str("]");
        } else if ((dk->edd_flags & 0x0004U) != 0) {
            out_str(" [Removable]");
        }
        out_str("\n");
    }
    out_str("\n");
}