#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#define GOP_MAX_MODES_GUARD 256U

/* -------------------------------------------------------------------------
 * UEFI 2.11 GOP / EDID / EFI 1.10 UGA 표준 GUID 및 구조체 레이아웃
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidGop =
    { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };

static EFI_GUID kGuidUgaDraw =
    { 0x982C298B, 0xF4FA, 0x41CB, { 0xB8, 0x38, 0x77, 0xAA, 0x68, 0x8F, 0xB8, 0x39 } };

static EFI_GUID kGuidEdidActive =
    { 0xBD8C1056, 0x9F36, 0x44EC, { 0x92, 0xA8, 0xA6, 0x33, 0x7F, 0x81, 0x79, 0x86 } };

static EFI_GUID kGuidEdidDiscovered =
    { 0x1C0C34F6, 0xD380, 0x41FA, { 0xA0, 0x49, 0x8A, 0xD0, 0x6C, 0x1A, 0x66, 0xAA } };

typedef enum {
    BC_PIXEL_RGBX_8BPC = 0,
    BC_PIXEL_BGRX_8BPC = 1,
    BC_PIXEL_BITMASK   = 2,
    BC_PIXEL_BLT_ONLY  = 3,
    BC_PIXEL_MAX       = 4
} bc_gop_pixel_fmt_t;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} bc_pixel_bitmask_t;

typedef struct {
    UINT32             Version;
    UINT32             HorizontalResolution;
    UINT32             VerticalResolution;
    UINT32             PixelFormat;
    bc_pixel_bitmask_t PixelInformation;
    UINT32             PixelsPerScanLine;
} bc_gop_mode_info_t;

typedef struct {
    UINT32               MaxMode;
    UINT32               Mode;
    bc_gop_mode_info_t  *Info;
    UINTN                SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN                FrameBufferSize;
} bc_gop_mode_t;

typedef struct _BC_GOP bc_gop_t;

typedef EFI_STATUS (EFIAPI *BC_GOP_QUERY_MODE_FN)(
    bc_gop_t            *This,
    UINT32               ModeNumber,
    UINTN               *SizeOfInfo,
    bc_gop_mode_info_t **Info
);

typedef EFI_STATUS (EFIAPI *BC_GOP_SET_MODE_FN)(
    bc_gop_t *This,
    UINT32    ModeNumber
);

struct _BC_GOP {
    BC_GOP_QUERY_MODE_FN QueryMode;
    BC_GOP_SET_MODE_FN   SetMode;
    VOID                *Blt;
    bc_gop_mode_t       *Mode;
};

typedef struct _BC_UGA_DRAW bc_uga_draw_t;

typedef EFI_STATUS (EFIAPI *BC_UGA_GET_MODE_FN)(
    bc_uga_draw_t *This,
    UINT32        *HorizontalResolution,
    UINT32        *VerticalResolution,
    UINT32        *ColorDepth,
    UINT32        *RefreshRate
);

struct _BC_UGA_DRAW {
    BC_UGA_GET_MODE_FN GetMode;
    VOID              *SetMode;
    VOID              *Blt;
};

typedef struct {
    UINT32  SizeOfEdid;
    UINT8  *Edid;
} bc_edid_proto_t;

/* -------------------------------------------------------------------------
 * 내부 헬퍼 (x86 포트 I/O, 바이트 읽기, 포맷 문자열)
 * ------------------------------------------------------------------------- */
static UINT8 io_in8(UINT16 port)
{
    UINT8 val;

    __asm__ __volatile__ ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static VOID io_out8(UINT16 port, UINT8 val)
{
    __asm__ __volatile__ ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static UINT16 rd_u16(const UINT8 *p)
{
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static const char *pixel_fmt_str(UINT32 fmt)
{
    switch (fmt) {
        case BC_PIXEL_RGBX_8BPC: return "RGBX-32bpp (RedGreenBlueReserved8Bit)";
        case BC_PIXEL_BGRX_8BPC: return "BGRX-32bpp (BlueGreenRedReserved8Bit)";
        case BC_PIXEL_BITMASK:   return "BitMask";
        case BC_PIXEL_BLT_ONLY:  return "PixelBltOnly (No direct LFB)";
        default:                 return "Unknown";
    }
}

static const char *pixel_fmt_short(UINT32 fmt)
{
    switch (fmt) {
        case BC_PIXEL_RGBX_8BPC: return "RGBX";
        case BC_PIXEL_BGRX_8BPC: return "BGRX";
        case BC_PIXEL_BITMASK:   return "Mask";
        case BC_PIXEL_BLT_ONLY:  return "BltOnly";
        default:                 return "?";
    }
}

static BOOLEAN edid_checksum_ok(const UINT8 *edid)
{
    UINT8 sum = 0;
    UINTN i;

    for (i = 0; i < 128U; i++) {
        sum = (UINT8)(sum + edid[i]);
    }
    return (sum == 0U) ? TRUE : FALSE;
}

static BOOLEAN edid_header_ok(const UINT8 *edid)
{
    return (edid[0] == 0x00U && edid[1] == 0xFFU && edid[2] == 0xFFU && edid[3] == 0xFFU &&
            edid[4] == 0xFFU && edid[5] == 0xFFU && edid[6] == 0xFFU && edid[7] == 0x00U)
           ? TRUE : FALSE;
}

/* -------------------------------------------------------------------------
 * [A] GOP 선형 프레임버퍼(LFB) 직접 쓰기/역읽기 검증 (비파괴 저장->검증->복원)
 * ------------------------------------------------------------------------- */
static VOID test_gop_lfb_direct_rw(efi_diag_ctx_t *ctx,
                                   const bc_gop_mode_t *gm,
                                   const bc_gop_mode_info_t *cur)
{
    UINT64            fb_base;
    UINT64            fb_sz;
    UINT32            cmp_mask;
    volatile UINT32  *fb;
    UINT32            saved[4];
    UINT32            pat1[4] = { 0x00AA5533U, 0x0033AA55U, 0x005533AAU, 0x00CC9966U };
    UINT32            pat2[4] = { 0x0055AA66U, 0x006655AAU, 0x00AA6655U, 0x00336699U };
    UINTN             k;
    BOOLEAN           ok1 = TRUE, ok2 = TRUE;

    efi_out_str(ctx, "    LFB Direct R/W   : ");

    if (gm == NULL || cur == NULL || cur->PixelFormat == BC_PIXEL_BLT_ONLY) {
        efi_out_str(ctx, "SKIPPED (PixelBltOnly / no LFB)\n");
        return;
    }

    fb_base = (UINT64)gm->FrameBufferBase;
    fb_sz   = (UINT64)gm->FrameBufferSize;

    if (fb_base == 0ULL || fb_sz < 16ULL) {
        efi_out_str(ctx, "SKIPPED (Unmapped or size < 16B)\n");
        return;
    }

#if !EFI_IS_X64
    if (fb_base > 0xFFFFFFFFULL || (fb_base + 16ULL) > 0x100000000ULL) {
        efi_out_str(ctx, "SKIPPED (>=4GB LFB unreachable in IA32)\n");
        return;
    }
#endif

    if (cur->PixelFormat == BC_PIXEL_BITMASK) {
        cmp_mask = cur->PixelInformation.RedMask
                 | cur->PixelInformation.GreenMask
                 | cur->PixelInformation.BlueMask;
        if (cmp_mask == 0U) {
            cmp_mask = 0x00FFFFFFU;
        }
    } else {
        /* RGBX / BGRX: 상위 예약 바이트(X)는 하드웨어 무시 가능하므로 하위 24비트 비교 */
        cmp_mask = 0x00FFFFFFU;
    }

    efilog_flush(ctx);

    fb = (volatile UINT32 *)(UINTN)fb_base;

    for (k = 0; k < 4U; k++) {
        saved[k] = fb[k];
    }

    for (k = 0; k < 4U; k++) {
        fb[k] = pat1[k];
    }
    __asm__ __volatile__ ("" ::: "memory");
    for (k = 0; k < 4U; k++) {
        if ((fb[k] & cmp_mask) != (pat1[k] & cmp_mask)) {
            ok1 = FALSE;
        }
    }

    for (k = 0; k < 4U; k++) {
        fb[k] = pat2[k];
    }
    __asm__ __volatile__ ("" ::: "memory");
    for (k = 0; k < 4U; k++) {
        if ((fb[k] & cmp_mask) != (pat2[k] & cmp_mask)) {
            ok2 = FALSE;
        }
    }

    for (k = 0; k < 4U; k++) {
        fb[k] = saved[k];
    }
    __asm__ __volatile__ ("" ::: "memory");

    if (ok1 && ok2) {
        efi_out_str(ctx, "PASS (Direct Ring-0 VRAM write/readback verified & restored)\n");
    } else {
        efi_out_str(ctx, "FAIL (Readback mismatch; check WC/MMIO decode)\n");
    }
}

/* -------------------------------------------------------------------------
 * [B] EDID 1.3/1.4 기본 블록(128B) 디코딩
 * ------------------------------------------------------------------------- */
static VOID decode_and_print_edid(efi_diag_ctx_t *ctx, const char *src_tag,
                                  const UINT8 *edid, UINT32 sz)
{
    UINT16 mfg_be, prod_id, pclk_10khz;
    char   mfg[4];
    UINT8  week, year_off, ver, rev;
    UINTN  slot;

    efi_out_str(ctx, "    EDID (");
    efi_out_str(ctx, src_tag);
    efi_out_str(ctx, ")  : PROTOCOL AVAILABLE (Size=");
    efi_out_u64(ctx, (UINT64)sz);
    efi_out_str(ctx, " B)\n");

    if (edid == NULL || sz < 128U) {
        efi_out_str(ctx, "      Parse Status   : INCOMPLETE (<128 B)\n");
        return;
    }

    if (!edid_header_ok(edid) || !edid_checksum_ok(edid)) {
        efi_out_str(ctx, "      Parse Status   : INVALID HEADER/CHECKSUM\n");
        return;
    }

    mfg_be = (UINT16)(((UINT16)edid[8] << 8) | (UINT16)edid[9]);
    mfg[0] = (char)('@' + ((mfg_be >> 10) & 0x1FU));
    mfg[1] = (char)('@' + ((mfg_be >> 5)  & 0x1FU));
    mfg[2] = (char)('@' + (mfg_be & 0x1FU));
    mfg[3] = '\0';

    prod_id  = rd_u16(edid + 10);
    week     = edid[16];
    year_off = edid[17];
    ver      = edid[18];
    rev      = edid[19];

    efi_out_str(ctx, "      Vendor / Prod  : ");
    efi_out_str(ctx, mfg);
    efi_out_str(ctx, " / 0x");
    efi_out_hex16(ctx, prod_id);
    efi_out_str(ctx, " | EDID v");
    efi_out_u64(ctx, (UINT64)ver);
    efi_out_str(ctx, ".");
    efi_out_u64(ctx, (UINT64)rev);
    efi_out_str(ctx, " | Mfg=");
    efi_out_u64(ctx, (UINT64)(1990U + (UINT32)year_off));
    if (week >= 1U && week <= 54U) {
        efi_out_str(ctx, "-W");
        efi_out_u64_pad2(ctx, (UINT64)week);
    }
    efi_out_str(ctx, " | Input=");
    efi_out_str(ctx, (edid[20] & 0x80U) ? "Digital\n" : "Analog\n");

    for (slot = 0; slot < 4U; slot++) {
        const UINT8 *d = edid + 54U + (slot * 18U);
        if (d[0] == 0U && d[1] == 0U && d[2] == 0U && d[3] == 0xFCU && d[4] == 0U) {
            char  name[14];
            UINTN k, end = 0;
            for (k = 0; k < 13U; k++) {
                UINT8 c = d[5U + k];
                if (c == 0x0AU || c == 0x00U) {
                    break;
                }
                name[k] = (c >= 0x20U && c <= 0x7EU) ? (char)c : ' ';
                if (name[k] != ' ') {
                    end = k + 1U;
                }
            }
            name[end] = '\0';
            if (end > 0U) {
                efi_out_str(ctx, "      Monitor Name   : \"");
                efi_out_str(ctx, name);
                efi_out_str(ctx, "\"\n");
                break;
            }
        }
    }

    pclk_10khz = rd_u16(edid + 54);
    if (pclk_10khz != 0U) {
        const UINT8 *d = edid + 54;
        UINT32 h_act   = (UINT32)d[2] | ((UINT32)(d[4] & 0xF0U) << 4);
        UINT32 h_blk   = (UINT32)d[3] | ((UINT32)(d[4] & 0x0FU) << 8);
        UINT32 v_act   = (UINT32)d[5] | ((UINT32)(d[7] & 0xF0U) << 4);
        UINT32 v_blk   = (UINT32)d[6] | ((UINT32)(d[7] & 0x0FU) << 8);
        UINT32 h_tot   = h_act + h_blk;
        UINT32 v_tot   = v_act + v_blk;
        UINT32 px_tot  = h_tot * v_tot;

        efi_out_str(ctx, "      Preferred Mode : ");
        efi_out_u64(ctx, (UINT64)h_act);
        efi_out_str(ctx, "x");
        efi_out_u64(ctx, (UINT64)v_act);
        if (px_tot > 0U) {
            UINT32 hz = (((UINT32)pclk_10khz * 10000U) + (px_tot >> 1)) / px_tot;
            efi_out_str(ctx, " @ ~");
            efi_out_u64(ctx, (UINT64)hz);
            efi_out_str(ctx, " Hz");
        }
        efi_out_str(ctx, " (PClk=");
        efi_out_u64(ctx, (UINT64)((UINT32)pclk_10khz / 100U));
        efi_out_str(ctx, ".");
        efi_out_u64_pad2(ctx, (UINT64)((UINT32)pclk_10khz % 100U));
        efi_out_str(ctx, " MHz)\n");
    }
}

static VOID check_handle_edid(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    bc_edid_proto_t *edid = NULL;
    EFI_STATUS       st;

    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, &kGuidEdidActive, (VOID **)&edid);
    if (!EFI_ERROR(st) && edid != NULL && edid->SizeOfEdid > 0U && edid->Edid != NULL) {
        decode_and_print_edid(ctx, "Active", edid->Edid, edid->SizeOfEdid);
        return;
    }

    edid = NULL;
    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, &kGuidEdidDiscovered, (VOID **)&edid);
    if (!EFI_ERROR(st) && edid != NULL && edid->SizeOfEdid > 0U && edid->Edid != NULL) {
        decode_and_print_edid(ctx, "Discovered", edid->Edid, edid->SizeOfEdid);
        return;
    }

    efi_out_str(ctx, "    EDID Protocol    : NOT EXPOSED (Normal on OVMF / virtual or unprobed displays)\n");
}

/* -------------------------------------------------------------------------
 * [C] 선택적 GOP SetMode() 전환 테스트 (기본 비활성; 실행 전 Flush 필수)
 * ------------------------------------------------------------------------- */
static VOID optional_setmode_test(efi_diag_ctx_t *ctx,
                                  EFI_BOOT_SERVICES *bs,
                                  bc_gop_t *gop,
                                  UINT32 orig_mode,
                                  UINT32 max_mode)
{
    UINT32 target = orig_mode;
    UINT32 m;
    EFI_STATUS st_set, st_rest;

    if (!ctx->allow_gop_setmode) {
        efi_out_str(ctx, "    SetMode Test     : SKIPPED (Default read-only mode)\n");
        return;
    }

    if (max_mode < 2U) {
        efi_out_str(ctx, "    SetMode Test     : SKIPPED (Only 1 mode available)\n");
        return;
    }

    for (m = 0; m < max_mode && m < GOP_MAX_MODES_GUARD; m++) {
        if (m != orig_mode) {
            target = m;
            break;
        }
    }

    efi_out_str(ctx, "    SetMode Test     : Switching Mode ");
    efi_out_u64(ctx, (UINT64)orig_mode);
    efi_out_str(ctx, " -> ");
    efi_out_u64(ctx, (UINT64)target);
    efi_out_str(ctx, " -> ");
    efi_out_u64(ctx, (UINT64)orig_mode);
    efi_out_str(ctx, "...\n");
    efilog_flush(ctx);

    st_set = uefi_call_wrapper(gop->SetMode, 2, gop, target);
    if (!EFI_ERROR(st_set) && bs->Stall != NULL) {
        uefi_call_wrapper(bs->Stall, 1, (UINTN)500000U);
    }
    st_rest = uefi_call_wrapper(gop->SetMode, 2, gop, orig_mode);

    efi_out_str(ctx, "    SetMode Result   : Switch=");
    efi_out_status(ctx, st_set);
    efi_out_str(ctx, ", Restore=");
    efi_out_status(ctx, st_rest);
    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}

/* -------------------------------------------------------------------------
 * [D] GOP 미노출 시 구형 EFI 1.10 UGA Draw Protocol 폴백 조회
 * ------------------------------------------------------------------------- */
static VOID check_uga_fallback(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs)
{
    bc_uga_draw_t *uga = NULL;
    UINT32         w = 0, h = 0, depth = 0, hz = 0;
    EFI_STATUS     st;

    efi_out_str(ctx, "  UGA Draw Fallback  : ");
    if (bs == NULL || bs->LocateProtocol == NULL) {
        efi_out_str(ctx, "UNAVAILABLE\n");
        return;
    }

    st = uefi_call_wrapper(bs->LocateProtocol, 3, &kGuidUgaDraw, NULL, (VOID **)&uga);
    if (EFI_ERROR(st) || uga == NULL || uga->GetMode == NULL) {
        efi_out_str(ctx, "NOT EXPOSED\n");
        return;
    }

    st = uefi_call_wrapper(uga->GetMode, 5, uga, &w, &h, &depth, &hz);
    if (!EFI_ERROR(st)) {
        efi_out_str(ctx, "PROTOCOL AVAILABLE (");
        efi_out_u64(ctx, (UINT64)w);
        efi_out_str(ctx, "x");
        efi_out_u64(ctx, (UINT64)h);
        efi_out_str(ctx, " ");
        efi_out_u64(ctx, (UINT64)depth);
        efi_out_str(ctx, "bpp @ ");
        efi_out_u64(ctx, (UINT64)hz);
        efi_out_str(ctx, "Hz; no LFB pointer in UGA)\n");
    } else {
        efi_out_str(ctx, "PROTOCOL AVAILABLE (GetMode failed: ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
    }
}

/* -------------------------------------------------------------------------
 * [E] 레거시 VGA VRAM(0xB8000) & CRTC(0x3D4) 직접 접근 프로브 (비파괴)
 *     - GOP 부재 시 대안 경로 확인 + UEFI 세션 내 VGA 어퍼처 잔존 여부 진단
 *     - FADT VGA_NOT_PRESENT(Bit 2) == 1이면 규격 준수하여 절대 미접근
 * ------------------------------------------------------------------------- */
static VOID probe_legacy_vga_vram(efi_diag_ctx_t *ctx)
{
    volatile UINT16 *vram;
    UINT8            orig_idx, orig_cur, r1, r2;
    UINT16           orig_w0, orig_w1, v1, v2;
    BOOLEAN          crtc_ok = FALSE;
    BOOLEAN          vram_ok = FALSE;
    BOOLEAN          vram_ff = FALSE;

    efi_out_str(ctx, "\n  [Legacy Direct VRAM / CRTC Fallback Probe]\n");

    if (ctx->fadt_valid && (ctx->iapc_boot_arch & 0x0004U) != 0U) {
        efi_out_str(ctx, "    Status           : SKIPPED (FADT VGA_NOT_PRESENT=1)\n");
        return;
    }

    /* 직접 포트/메모리 접근 전 디스크 플러시 */
    efilog_flush(ctx);

    /* 1. VGA CRTC 0x3D4/0x3D5 Cursor Location Low(0x0F) 비파괴 R/W 검증 */
    orig_idx = io_in8(0x03D4U);
    io_out8(0x03D4U, 0x0FU);
    orig_cur = io_in8(0x03D5U);

    io_out8(0x03D5U, 0x55U);
    r1 = io_in8(0x03D5U);
    io_out8(0x03D5U, 0xAAU);
    r2 = io_in8(0x03D5U);

    io_out8(0x03D5U, orig_cur);
    io_out8(0x03D4U, orig_idx);

    if (r1 == 0x55U && r2 == 0xAAU) {
        crtc_ok = TRUE;
    }

    /* 2. 0xB8000 텍스트 VRAM 2워드 비파괴 저장 -> 쓰기 -> 역읽기 -> 복원 */
    vram    = (volatile UINT16 *)(UINTN)0x000B8000ULL;
    orig_w0 = vram[0];
    orig_w1 = vram[1];

    vram[0] = 0x0755U;
    vram[1] = 0x07AAU;
    __asm__ __volatile__ ("" ::: "memory");
    v1 = vram[0];
    v2 = vram[1];

    vram[0] = orig_w0;
    vram[1] = orig_w1;
    __asm__ __volatile__ ("" ::: "memory");

    if (v1 == 0x0755U && v2 == 0x07AAU) {
        vram_ok = TRUE;
    } else if (v1 == 0xFFFFU && v2 == 0xFFFFU) {
        vram_ff = TRUE;
    }

    efi_out_str(ctx, "    CRTC (0x3D4/3D5) : ");
    efi_out_str(ctx, crtc_ok ? "RESPONDING (R/W verified & restored)\n"
                             : "NO RESPONSE (Decoded off or non-VGA mode)\n");

    efi_out_str(ctx, "    VRAM (0x000B8000): ");
    if (vram_ok) {
        efi_out_str(ctx, "WRITABLE (R/W verified & restored)\n");
    } else if (vram_ff) {
        efi_out_str(ctx, "UNMAPPED (Bus float 0xFFFF)\n");
    } else {
        efi_out_str(ctx, "READBACK MISMATCH (Read 0x");
        efi_out_hex16(ctx, v1);
        efi_out_str(ctx, "/0x");
        efi_out_hex16(ctx, v2);
        efi_out_str(ctx, ")\n");
    }

    efi_out_str(ctx, "    Fallback Verdict : ");
    if (crtc_ok && vram_ok) {
        efi_out_str(ctx, "LEGACY VGA APERTURE DECODED (CRTC + 0xB8000 active; note: display may still be in GOP scanout)\n");
    } else if (vram_ok) {
        efi_out_str(ctx, "0xB8000 WRITABLE WITHOUT CRTC (Likely RAM or inactive aperture; not guaranteed visible)\n");
    } else {
        efi_out_str(ctx, "NO LEGACY VGA DIRECT PATH (Pure GOP/UEFI graphics decode)\n");
    }
}

/* -------------------------------------------------------------------------
 * Step 4 메인 엔트리: GOP · LFB 직접 쓰기 · EDID · UGA · Legacy VRAM 폴백
 * ------------------------------------------------------------------------- */
void efi_run_step4_graphics_gop(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    EFI_HANDLE        *handles = NULL;
    UINTN              cnt = 0;
    UINTN              i;
    EFI_STATUS         st;

    efi_out_str(ctx, "=== [Step 4] Graphics Output Protocol (GOP), LFB & EDID ===\n");

    efi_out_str(ctx, "  FADT VGA Flag      : ");
    if (!ctx->fadt_valid) {
        efi_out_str(ctx, "UNKNOWN (FADT unavailable)\n");
    } else if (ctx->iapc_boot_arch & 0x0004U) {
        efi_out_str(ctx, "VGA_NOT_PRESENT=1 (Legacy VGA hardware absent -> GOP only)\n");
    } else {
        efi_out_str(ctx, "VGA_NOT_PRESENT=0 (Legacy VGA hardware reported; UEFI uses GOP)\n");
    }

    if (bs == NULL || bs->LocateHandleBuffer == NULL) {
        efi_out_str(ctx, "  GOP Status         : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, "\n");
        probe_legacy_vga_vram(ctx);
        efi_out_str(ctx, "\n");
        efilog_flush(ctx);
        return;
    }

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidGop, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0) {
        efi_out_str(ctx, "  GOP Status         : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
        check_uga_fallback(ctx, bs);
        probe_legacy_vga_vram(ctx);
        efi_out_str(ctx, "\n");
        efilog_flush(ctx);
        return;
    }

    efi_out_str(ctx, "  GOP Instances      : ");
    efi_out_u64(ctx, (UINT64)cnt);
    efi_out_str(ctx, " (");
    efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
    efi_out_str(ctx, ")\n");

    for (i = 0; i < cnt; i++) {
        bc_gop_t           *gop = NULL;
        bc_gop_mode_t      *gm;
        bc_gop_mode_info_t *cur;
        UINT32              m, lim;
        BOOLEAN             is_blt_only = FALSE;

        st = uefi_call_wrapper(bs->HandleProtocol, 3,
                               handles[i], &kGuidGop, (VOID **)&gop);
        if (EFI_ERROR(st) || gop == NULL || gop->Mode == NULL) {
            continue;
        }

        gm  = gop->Mode;
        cur = gm->Info;

        efi_out_str(ctx, "\n  [GOP #");
        efi_out_u64(ctx, (UINT64)i);
        efi_out_str(ctx, "]\n");

        if (cur != NULL) {
            is_blt_only = (cur->PixelFormat == BC_PIXEL_BLT_ONLY) ? TRUE : FALSE;

            efi_out_str(ctx, "    Current Mode     : #");
            efi_out_u64(ctx, (UINT64)gm->Mode);
            efi_out_str(ctx, " of ");
            efi_out_u64(ctx, (UINT64)gm->MaxMode);
            efi_out_str(ctx, " -> ");
            efi_out_u64(ctx, (UINT64)cur->HorizontalResolution);
            efi_out_str(ctx, "x");
            efi_out_u64(ctx, (UINT64)cur->VerticalResolution);
            efi_out_str(ctx, " (ScanLine=");
            efi_out_u64(ctx, (UINT64)cur->PixelsPerScanLine);
            efi_out_str(ctx, " px)\n");

            efi_out_str(ctx, "    Pixel Format     : ");
            efi_out_str(ctx, pixel_fmt_str(cur->PixelFormat));
            if (cur->PixelFormat == BC_PIXEL_BITMASK) {
                efi_out_str(ctx, " [R=0x"); efi_out_hex32(ctx, cur->PixelInformation.RedMask);
                efi_out_str(ctx, " G=0x");  efi_out_hex32(ctx, cur->PixelInformation.GreenMask);
                efi_out_str(ctx, " B=0x");  efi_out_hex32(ctx, cur->PixelInformation.BlueMask);
                efi_out_str(ctx, " X=0x");  efi_out_hex32(ctx, cur->PixelInformation.ReservedMask);
                efi_out_str(ctx, "]");
            }
            efi_out_str(ctx, "\n");
        } else {
            efi_out_str(ctx, "    Current Mode     : #");
            efi_out_u64(ctx, (UINT64)gm->Mode);
            efi_out_str(ctx, " (Mode->Info is NULL)\n");
        }

        efi_out_str(ctx, "    Framebuffer (LFB): ");
        if (is_blt_only || gm->FrameBufferBase == 0ULL || gm->FrameBufferSize == 0U) {
            efi_out_str(ctx, "N/A (BltOnly or unmapped)\n");
        } else {
            UINT64 fb_base = (UINT64)gm->FrameBufferBase;
            UINT64 fb_sz   = (UINT64)gm->FrameBufferSize;
            BOOLEAN abv4   = (fb_base >= 0x100000000ULL ||
                              (fb_base + fb_sz) > 0x100000000ULL ||
                              (fb_base + fb_sz) < fb_base) ? TRUE : FALSE;

            efi_out_str(ctx, "Base=0x");
            efi_out_hex64(ctx, fb_base);
            efi_out_str(ctx, ", Size=");
            efi_out_size_human(ctx, fb_sz);
            efi_out_str(ctx, abv4 ? " [>=4GB]\n" : " [<4GB]\n");
        }

        /* GOP LFB 직접 쓰기/역읽기 검증 */
        test_gop_lfb_direct_rw(ctx, gm, cur);

        /* 전체 지원 모드 열거 */
        lim = (gm->MaxMode > GOP_MAX_MODES_GUARD) ? GOP_MAX_MODES_GUARD : gm->MaxMode;
        efi_out_str(ctx, "    Supported Modes  :\n");
        for (m = 0; m < lim; m++) {
            bc_gop_mode_info_t *mi = NULL;
            UINTN               mi_sz = 0;
            EFI_STATUS          qst;

            qst = uefi_call_wrapper(gop->QueryMode, 4, gop, m, &mi_sz, &mi);
            if (!EFI_ERROR(qst) && mi != NULL) {
                efi_out_str(ctx, "      [");
                efi_out_u64_pad2(ctx, (UINT64)m);
                efi_out_str(ctx, (m == gm->Mode) ? "*] " : " ] ");
                efi_out_u64(ctx, (UINT64)mi->HorizontalResolution);
                efi_out_str(ctx, "x");
                efi_out_u64(ctx, (UINT64)mi->VerticalResolution);
                efi_out_str(ctx, " | ");
                efi_out_str(ctx, pixel_fmt_short(mi->PixelFormat));
                efi_out_str(ctx, " | Pitch=");
                efi_out_u64(ctx, (UINT64)mi->PixelsPerScanLine);
                efi_out_str(ctx, "\n");
                uefi_call_wrapper(bs->FreePool, 1, mi);
            }
        }
        if (gm->MaxMode > GOP_MAX_MODES_GUARD) {
            efi_out_str(ctx, "      ... (truncated at 256 modes)\n");
        }

        check_handle_edid(ctx, bs, handles[i]);
        optional_setmode_test(ctx, bs, gop, gm->Mode, gm->MaxMode);
    }

    uefi_call_wrapper(bs->FreePool, 1, handles);

    /* GOP 유무와 무관하게 UEFI 세션 내 레거시 VGA 어퍼처 잔존 여부 기록 */
    probe_legacy_vga_vram(ctx);

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}