#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#define INPUT_MAX_TRACK_PTRS 8U

/* -------------------------------------------------------------------------
 * UEFI 2.11 Input / Pointer / DevicePath 표준 GUID
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidSimpleTextIn =
    { 0x387477C1, 0x69C7, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidSimpleTextInEx =
    { 0xDD9E7534, 0x7762, 0x4698, { 0x8C, 0x14, 0xF5, 0x85, 0x17, 0xA6, 0x25, 0xAA } };

static EFI_GUID kGuidSimplePointer =
    { 0x31878C87, 0x0B75, 0x11D5, { 0x9A, 0x4F, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static EFI_GUID kGuidAbsPointer =
    { 0x8D59D32B, 0xC655, 0x4AE9, { 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43 } };

static EFI_GUID kGuidDevicePath =
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

/* -------------------------------------------------------------------------
 * UEFI 2.11 입력 프로토콜 구조체 레이아웃 (IA32/X64 정렬 일치)
 * ------------------------------------------------------------------------- */
typedef struct {
    UINT32 KeyShiftState;
    UINT8  KeyToggleState;
} bc_key_state_t;

typedef struct {
    EFI_INPUT_KEY  Key;
    bc_key_state_t KeyState;
} bc_key_data_t;

typedef struct _BC_TEXT_IN_EX bc_text_in_ex_t;

typedef EFI_STATUS (EFIAPI *BC_READ_KEY_EX_FN)(
    bc_text_in_ex_t *This,
    bc_key_data_t   *KeyData
);

struct _BC_TEXT_IN_EX {
    VOID              *Reset;
    BC_READ_KEY_EX_FN  ReadKeyStrokeEx;
    EFI_EVENT          WaitForKeyEx;
    VOID              *SetState;
    VOID              *RegisterKeyNotify;
    VOID              *UnregisterKeyNotify;
};

typedef struct {
    UINT64  ResolutionX;
    UINT64  ResolutionY;
    UINT64  ResolutionZ;
    BOOLEAN LeftButton;
    BOOLEAN RightButton;
} bc_simple_ptr_mode_t;

typedef struct {
    INT32   RelativeMovementX;
    INT32   RelativeMovementY;
    INT32   RelativeMovementZ;
    BOOLEAN LeftButton;
    BOOLEAN RightButton;
} bc_simple_ptr_state_t;

typedef struct _BC_SIMPLE_PTR bc_simple_ptr_t;

typedef EFI_STATUS (EFIAPI *BC_SPTR_GET_ST_FN)(
    bc_simple_ptr_t       *This,
    bc_simple_ptr_state_t *State
);

struct _BC_SIMPLE_PTR {
    VOID                 *Reset;
    BC_SPTR_GET_ST_FN     GetState;
    EFI_EVENT             WaitForInput;
    bc_simple_ptr_mode_t *Mode;
};

typedef struct {
    UINT64 AbsoluteMinX;
    UINT64 AbsoluteMinY;
    UINT64 AbsoluteMinZ;
    UINT64 AbsoluteMaxX;
    UINT64 AbsoluteMaxY;
    UINT64 AbsoluteMaxZ;
    UINT32 Attributes;
} bc_abs_ptr_mode_t;

typedef struct {
    UINT64 CurrentX;
    UINT64 CurrentY;
    UINT64 CurrentZ;
    UINT32 ActiveButtons;
} bc_abs_ptr_state_t;

typedef struct _BC_ABS_PTR bc_abs_ptr_t;

typedef EFI_STATUS (EFIAPI *BC_APTR_GET_ST_FN)(
    bc_abs_ptr_t       *This,
    bc_abs_ptr_state_t *State
);

struct _BC_ABS_PTR {
    VOID              *Reset;
    BC_APTR_GET_ST_FN  GetState;
    EFI_EVENT          WaitForInput;
    bc_abs_ptr_mode_t *Mode;
};

/* -------------------------------------------------------------------------
 * 내부 유틸리티
 * ------------------------------------------------------------------------- */
static UINT8 io_in8(UINT16 port)
{
    UINT8 val;

    __asm__ __volatile__ ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static UINT16 rd_u16(const UINT8 *p)
{
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static UINT32 rd_u32(const UINT8 *p)
{
    return (UINT32)p[0]
         | ((UINT32)p[1] << 8)
         | ((UINT32)p[2] << 16)
         | ((UINT32)p[3] << 24);
}

static VOID out_i32(efi_diag_ctx_t *ctx, INT32 v)
{
    if (v < 0) {
        efi_out_str(ctx, "-");
        efi_out_u64(ctx, (UINT64)(-(INT64)v));
    } else {
        efi_out_u64(ctx, (UINT64)v);
    }
}

static VOID out_input_dp(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    const UINT8 *dp = NULL;
    UINTN        guard = 0U;
    BOOLEAN      printed = FALSE;
    EFI_STATUS   st;

    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, &kGuidDevicePath, (VOID **)&dp);
    if (EFI_ERROR(st) || dp == NULL) {
        efi_out_str(ctx, "(Virtual/ConSplitter - No DevicePath)");
        return;
    }

    while (guard++ < 64U) {
        UINT8  type = dp[0];
        UINT8  sub  = dp[1];
        UINT16 len  = rd_u16(dp + 2);
        if (len < 4U || (type == 0x7FU && sub == 0xFFU)) break;

        if (type == 0x01U && sub == 0x01U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "PCI(");
            efi_out_hex8(ctx, dp[5]); efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)(dp[4] & 0x07U)); efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x02U && sub == 0x01U && len >= 12U) {
            UINT32 hid = rd_u32(dp + 4);
            if ((hid & 0xFFFFU) == 0x41D0U) {
                UINT16 pnp = (UINT16)(hid >> 16);
                if (pnp == 0x0303U || pnp == 0x0F03U || pnp == 0x0F13U || pnp == 0x0501U) {
                    if (printed) efi_out_str(ctx, " -> ");
                    efi_out_str(ctx, "PNP");
                    efi_out_hex16(ctx, pnp);
                    printed = TRUE;
                }
            }
        } else if (type == 0x03U && sub == 0x05U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "USB(P");
            efi_out_u64(ctx, (UINT64)dp[4]); efi_out_str(ctx, ",If");
            efi_out_u64(ctx, (UINT64)dp[5]); efi_out_str(ctx, ")");
            printed = TRUE;
        }
        dp += len;
    }
    if (!printed) efi_out_str(ctx, "(ACPI/Other DevicePath)");
}

/* -------------------------------------------------------------------------
 * [1] 레거시 PS/2 8042 상태 확인 (FADT Bit 1 엄수, 0x60 소비 금지)
 * ------------------------------------------------------------------------- */
static VOID check_legacy_8042(efi_diag_ctx_t *ctx)
{
    efi_out_str(ctx, "  [1] Legacy PS/2 8042 Controller (FADT Bit 1 Guarded)\n");

    if (ctx->fadt_valid && (ctx->iapc_boot_arch & 0x0002U) == 0U) {
        efi_out_str(ctx, "    Status           : SKIPPED (FADT 8042=0 -> Direct 0x60/0x64 prohibited)\n");
        return;
    }

    efilog_flush(ctx);
    {
        UINT8 st64 = io_in8(0x0064U);
        efi_out_str(ctx, "    FADT 8042 Flag   : ");
        efi_out_str(ctx, ctx->fadt_valid ? "1 (Declared Present)\n" : "UNKNOWN\n");
        efi_out_str(ctx, "    Port 0x64 (RO)   : 0x");
        efi_out_hex8(ctx, st64);
        if (st64 == 0xFFU) {
            efi_out_str(ctx, " (NO CONTROLLER / Bus float 0xFF)\n");
        } else {
            efi_out_str(ctx, " [OBF="); efi_out_str(ctx, (st64 & 0x01U) ? "1" : "0");
            efi_out_str(ctx, ", IBF=");  efi_out_str(ctx, (st64 & 0x02U) ? "1" : "0");
            efi_out_str(ctx, ", SYS=");  efi_out_str(ctx, (st64 & 0x04U) ? "1" : "0");
            efi_out_str(ctx, ", AUXB="); efi_out_str(ctx, (st64 & 0x20U) ? "1" : "0");
            efi_out_str(ctx, "] (Port 0x60 not read)\n");
        }
    }
}

/* -------------------------------------------------------------------------
 * [2] SimpleTextInput / SimpleTextInputEx 열거
 * ------------------------------------------------------------------------- */
static VOID check_text_inputs(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs,
                              bc_text_in_ex_t **conin_ex_out)
{
    EFI_HANDLE *handles = NULL;
    UINTN       cnt = 0U, i;
    EFI_STATUS  st;

    *conin_ex_out = NULL;
    efi_out_str(ctx, "\n  [2] Keyboard / Text Input Protocols\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidSimpleTextIn, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0U) {
        efi_out_str(ctx, "    SimpleTextIn     : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, "\n");
    } else {
        efi_out_str(ctx, "    SimpleTextIn     : ");
        efi_out_u64(ctx, (UINT64)cnt);
        efi_out_str(ctx, " handles (");
        efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
        efi_out_str(ctx, ")\n");
        for (i = 0U; i < cnt; i++) {
            efi_out_str(ctx, "      [TextIn #");
            efi_out_u64(ctx, (UINT64)i);
            efi_out_str(ctx, "] ");
            out_input_dp(ctx, bs, handles[i]);
            efi_out_str(ctx, "\n");
        }
        uefi_call_wrapper(bs->FreePool, 1, handles);
    }

    /* SimpleTextInputEx: ConIn 핸들 우선 조회 + 전체 개수 보고 */
    if (ctx->st->ConsoleInHandle != NULL) {
        bc_text_in_ex_t *ex = NULL;
        if (!EFI_ERROR(uefi_call_wrapper(bs->HandleProtocol, 3,
                                         ctx->st->ConsoleInHandle,
                                         &kGuidSimpleTextInEx, (VOID **)&ex)) &&
            ex != NULL) {
            *conin_ex_out = ex;
        }
    }

    handles = NULL;
    cnt = 0U;
    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidSimpleTextInEx, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0U) {
        efi_out_str(ctx, "    SimpleTextInEx   : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, "\n");
    } else {
        efi_out_str(ctx, "    SimpleTextInEx   : ");
        efi_out_u64(ctx, (UINT64)cnt);
        efi_out_str(ctx, " handles (ConInEx=");
        efi_out_str(ctx, (*conin_ex_out != NULL) ? "BOUND)\n" : "SEPARATE)\n");
        if (*conin_ex_out == NULL) {
            uefi_call_wrapper(bs->HandleProtocol, 3,
                              handles[0], &kGuidSimpleTextInEx, (VOID **)conin_ex_out);
        }
        uefi_call_wrapper(bs->FreePool, 1, handles);
    }
}

/* -------------------------------------------------------------------------
 * [3] SimplePointer (상대 좌표 마우스) & [4] AbsolutePointer (터치/태블릿)
 * ------------------------------------------------------------------------- */
static VOID check_pointers(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs,
                           bc_simple_ptr_t **sptrs, UINTN *sptr_cnt,
                           bc_abs_ptr_t **aptrs, UINTN *aptr_cnt)
{
    EFI_HANDLE *handles = NULL;
    UINTN       cnt = 0U, i;
    EFI_STATUS  st;

    *sptr_cnt = 0U;
    *aptr_cnt = 0U;

    efi_out_str(ctx, "\n  [3] Simple Pointer Protocol (Mice / Touchpads)\n");
    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidSimplePointer, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0U) {
        efi_out_str(ctx, "    SimplePointer    : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, "\n");
    } else {
        efi_out_str(ctx, "    SimplePointer    : ");
        efi_out_u64(ctx, (UINT64)cnt);
        efi_out_str(ctx, " handles (");
        efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
        efi_out_str(ctx, ")\n");

        for (i = 0U; i < cnt; i++) {
            bc_simple_ptr_t      *sp = NULL;
            bc_simple_ptr_state_t ps;
            EFI_STATUS            gst;

            if (EFI_ERROR(uefi_call_wrapper(bs->HandleProtocol, 3,
                                            handles[i], &kGuidSimplePointer, (VOID **)&sp)) ||
                sp == NULL || sp->Mode == NULL) {
                continue;
            }
            if (*sptr_cnt < INPUT_MAX_TRACK_PTRS) {
                sptrs[(*sptr_cnt)++] = sp;
            }

            efi_out_str(ctx, "      [Ptr #");
            efi_out_u64(ctx, (UINT64)i);
            efi_out_str(ctx, "] Res=(");
            efi_out_u64(ctx, sp->Mode->ResolutionX); efi_out_str(ctx, ",");
            efi_out_u64(ctx, sp->Mode->ResolutionY); efi_out_str(ctx, ",");
            efi_out_u64(ctx, sp->Mode->ResolutionZ); efi_out_str(ctx, ") | Btn=[L:");
            efi_out_str(ctx, sp->Mode->LeftButton ? "Y" : "N");
            efi_out_str(ctx, ",R:");
            efi_out_str(ctx, sp->Mode->RightButton ? "Y" : "N");
            efi_out_str(ctx, "]\n        Path         : ");
            out_input_dp(ctx, bs, handles[i]);
            efi_out_str(ctx, "\n");

            if (sp->GetState != NULL) {
                SetMem(&ps, sizeof(ps), 0);
                gst = uefi_call_wrapper(sp->GetState, 2, sp, &ps);
                efi_out_str(ctx, "        Poll State   : ");
                if (gst == EFI_SUCCESS || gst == EFI_NOT_READY) {
                    efi_out_str(ctx, (gst == EFI_SUCCESS) ? "READY (Event queued)\n"
                                                          : "ONLINE (Idle / NOT_READY)\n");
                } else {
                    efi_out_status(ctx, gst);
                    efi_out_str(ctx, "\n");
                }
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, handles);
    }

    efi_out_str(ctx, "\n  [4] Absolute Pointer Protocol (Touchscreens / Tablets / VM Pointer)\n");
    handles = NULL;
    cnt = 0U;
    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidAbsPointer, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0U) {
        efi_out_str(ctx, "    AbsolutePointer  : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, "\n");
    } else {
        efi_out_str(ctx, "    AbsolutePointer  : ");
        efi_out_u64(ctx, (UINT64)cnt);
        efi_out_str(ctx, " handles (");
        efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
        efi_out_str(ctx, ")\n");

        for (i = 0U; i < cnt; i++) {
            bc_abs_ptr_t      *ap = NULL;
            bc_abs_ptr_state_t as;
            EFI_STATUS         gst;

            if (EFI_ERROR(uefi_call_wrapper(bs->HandleProtocol, 3,
                                            handles[i], &kGuidAbsPointer, (VOID **)&ap)) ||
                ap == NULL || ap->Mode == NULL) {
                continue;
            }
            if (*aptr_cnt < INPUT_MAX_TRACK_PTRS) {
                aptrs[(*aptr_cnt)++] = ap;
            }

            efi_out_str(ctx, "      [AbsPtr #");
            efi_out_u64(ctx, (UINT64)i);
            efi_out_str(ctx, "] X=[");
            efi_out_u64(ctx, ap->Mode->AbsoluteMinX); efi_out_str(ctx, "..");
            efi_out_u64(ctx, ap->Mode->AbsoluteMaxX); efi_out_str(ctx, "] Y=[");
            efi_out_u64(ctx, ap->Mode->AbsoluteMinY); efi_out_str(ctx, "..");
            efi_out_u64(ctx, ap->Mode->AbsoluteMaxY); efi_out_str(ctx, "] Attr=0x");
            efi_out_hex32(ctx, ap->Mode->Attributes);
            efi_out_str(ctx, "\n        Path         : ");
            out_input_dp(ctx, bs, handles[i]);
            efi_out_str(ctx, "\n");

            if (ap->GetState != NULL) {
                SetMem(&as, sizeof(as), 0);
                gst = uefi_call_wrapper(ap->GetState, 2, ap, &as);
                efi_out_str(ctx, "        Poll State   : ");
                if (gst == EFI_SUCCESS || gst == EFI_NOT_READY) {
                    efi_out_str(ctx, (gst == EFI_SUCCESS) ? "READY (Event queued)\n"
                                                          : "ONLINE (Idle / NOT_READY)\n");
                } else {
                    efi_out_status(ctx, gst);
                    efi_out_str(ctx, "\n");
                }
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, handles);
    }
}

/* -------------------------------------------------------------------------
 * [5] 5초 제한 실시간 입력 검증 (키/마우스 움직임 확인, ESC 즉시 스킵)
 * ------------------------------------------------------------------------- */
static VOID run_timed_input_probe(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs,
                                  bc_text_in_ex_t *conin_ex,
                                  bc_simple_ptr_t **sptrs, UINTN sptr_cnt,
                                  bc_abs_ptr_t **aptrs, UINTN aptr_cnt)
{
    EFI_EVENT ev_tick = NULL;
    UINTN     tick, i;
    BOOLEAN   got_key = FALSE, got_sptr = FALSE, got_aptr = FALSE;

    efi_out_str(ctx, "\n  [5] Live Input Probe (5s window; press key or move mouse, ESC=skip)\n");
    efilog_flush(ctx);

    if (bs->CreateEvent == NULL || bs->SetTimer == NULL || bs->WaitForEvent == NULL) {
        efi_out_str(ctx, "    Live Probe       : SKIPPED (Timer unavailable)\n");
        return;
    }

    if (ctx->st->ConIn != NULL) {
        uefi_call_wrapper(ctx->st->ConIn->Reset, 2, ctx->st->ConIn, FALSE);
    }

    if (EFI_ERROR(uefi_call_wrapper(bs->CreateEvent, 5,
                                    EVT_TIMER, TPL_CALLBACK, NULL, NULL, &ev_tick))) {
        efi_out_str(ctx, "    Live Probe       : SKIPPED (CreateEvent failed)\n");
        return;
    }

    uefi_call_wrapper(bs->SetTimer, 3, ev_tick, TimerPeriodic, 500000ULL); /* 50ms tick */

    for (tick = 0U; tick < 100U; tick++) { /* 100 * 50ms = 5s */
        UINTN ev_idx = 0U;
        uefi_call_wrapper(bs->WaitForEvent, 3, 1, &ev_tick, &ev_idx);

        /* 키 입력 확인 (SimpleTextInEx 우선, 없으면 기본 ConIn) */
        if (!got_key) {
            if (conin_ex != NULL && conin_ex->ReadKeyStrokeEx != NULL) {
                bc_key_data_t kd;
                SetMem(&kd, sizeof(kd), 0);
                if (!EFI_ERROR(uefi_call_wrapper(conin_ex->ReadKeyStrokeEx, 2, conin_ex, &kd))) {
                    if (kd.Key.ScanCode == 0x17U) { /* ESC */
                        efi_out_str(ctx, "    Live Probe       : Skipped by ESC\n");
                        break;
                    }
                    got_key = TRUE;
                    efi_out_str(ctx, "    Key Event (Ex)   : ");
                    efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
                    efi_out_str(ctx, " | Scan=0x"); efi_out_hex16(ctx, kd.Key.ScanCode);
                    efi_out_str(ctx, " Char=0x");   efi_out_hex16(ctx, kd.Key.UnicodeChar);
                    efi_out_str(ctx, " Shift=0x");  efi_out_hex32(ctx, kd.KeyState.KeyShiftState);
                    efi_out_str(ctx, " Toggle=0x"); efi_out_hex8(ctx, kd.KeyState.KeyToggleState);
                    efi_out_str(ctx, "\n");
                }
            } else if (ctx->st->ConIn != NULL) {
                EFI_INPUT_KEY k;
                if (!EFI_ERROR(uefi_call_wrapper(ctx->st->ConIn->ReadKeyStroke, 2,
                                                 ctx->st->ConIn, &k))) {
                    if (k.ScanCode == 0x17U) {
                        efi_out_str(ctx, "    Live Probe       : Skipped by ESC\n");
                        break;
                    }
                    got_key = TRUE;
                    efi_out_str(ctx, "    Key Event        : ");
                    efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
                    efi_out_str(ctx, " | Scan=0x"); efi_out_hex16(ctx, k.ScanCode);
                    efi_out_str(ctx, " Char=0x");   efi_out_hex16(ctx, k.UnicodeChar);
                    efi_out_str(ctx, "\n");
                }
            }
        }

        /* SimplePointer 움직임/버튼 확인 */
        if (!got_sptr) {
            for (i = 0U; i < sptr_cnt; i++) {
                bc_simple_ptr_state_t ps;
                SetMem(&ps, sizeof(ps), 0);
                if (sptrs[i]->GetState != NULL &&
                    !EFI_ERROR(uefi_call_wrapper(sptrs[i]->GetState, 2, sptrs[i], &ps))) {
                    if (ps.RelativeMovementX != 0 || ps.RelativeMovementY != 0 ||
                        ps.RelativeMovementZ != 0 || ps.LeftButton || ps.RightButton) {
                        got_sptr = TRUE;
                        efi_out_str(ctx, "    Pointer Event    : ");
                        efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
                        efi_out_str(ctx, " (#"); efi_out_u64(ctx, (UINT64)i);
                        efi_out_str(ctx, " dX="); out_i32(ctx, ps.RelativeMovementX);
                        efi_out_str(ctx, " dY="); out_i32(ctx, ps.RelativeMovementY);
                        efi_out_str(ctx, " L=");  efi_out_str(ctx, ps.LeftButton ? "1" : "0");
                        efi_out_str(ctx, " R=");  efi_out_str(ctx, ps.RightButton ? "1" : "0");
                        efi_out_str(ctx, ")\n");
                        break;
                    }
                }
            }
        }

        /* AbsolutePointer 입력 확인 */
        if (!got_aptr) {
            for (i = 0U; i < aptr_cnt; i++) {
                bc_abs_ptr_state_t as;
                SetMem(&as, sizeof(as), 0);
                if (aptrs[i]->GetState != NULL &&
                    !EFI_ERROR(uefi_call_wrapper(aptrs[i]->GetState, 2, aptrs[i], &as))) {
                    got_aptr = TRUE;
                    efi_out_str(ctx, "    AbsPtr Event     : ");
                    efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
                    efi_out_str(ctx, " (#"); efi_out_u64(ctx, (UINT64)i);
                    efi_out_str(ctx, " X="); efi_out_u64(ctx, as.CurrentX);
                    efi_out_str(ctx, " Y="); efi_out_u64(ctx, as.CurrentY);
                    efi_out_str(ctx, " Btn=0x"); efi_out_hex32(ctx, as.ActiveButtons);
                    efi_out_str(ctx, ")\n");
                    break;
                }
            }
        }

        if (got_key && (got_sptr || sptr_cnt == 0U) && (got_aptr || aptr_cnt == 0U)) {
            break;
        }
    }

    uefi_call_wrapper(bs->SetTimer, 3, ev_tick, TimerCancel, 0ULL);
    uefi_call_wrapper(bs->CloseEvent, 1, ev_tick);

    if (!got_key && !got_sptr && !got_aptr) {
        efi_out_str(ctx, "    Live Probe       : No input within 5s (Auto-continued)\n");
    }
}

/* -------------------------------------------------------------------------
 * Step 7-3 메인 엔트리
 * ------------------------------------------------------------------------- */
void efi_run_step7_input(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    bc_text_in_ex_t   *conin_ex = NULL;
    bc_simple_ptr_t   *sptrs[INPUT_MAX_TRACK_PTRS];
    bc_abs_ptr_t      *aptrs[INPUT_MAX_TRACK_PTRS];
    UINTN              sptr_cnt = 0U, aptr_cnt = 0U;

    efi_out_str(ctx, "=== [Step 7-3] Input Devices (TextInEx, Pointer, AbsPointer & 8042) ===\n");
    efilog_flush(ctx);

    if (bs == NULL) {
        efi_out_str(ctx, "  BootServices UNAVAILABLE\n\n");
        return;
    }

    check_legacy_8042(ctx);
    check_text_inputs(ctx, bs, &conin_ex);
    check_pointers(ctx, bs, sptrs, &sptr_cnt, aptrs, &aptr_cnt);
    run_timed_input_probe(ctx, bs, conin_ex, sptrs, sptr_cnt, aptrs, aptr_cnt);

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}