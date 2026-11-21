#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#define EFILOG_BUF_CAP 4096U

static UINT8   g_log_buf[EFILOG_BUF_CAP];
static UINTN   g_log_len = 0;
static BOOLEAN g_log_io_failed = FALSE;
static EFI_STATUS g_log_io_status = EFI_SUCCESS;

/* 표준 UEFI GUID 직접 정의 (GNU-EFI 버전별 심볼명 차이 회피) */
static EFI_GUID kGuidLoadedImage =
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidSimpleFs =
    { 0x964E5B22, 0x6459, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

/* -------------------------------------------------------------------------
 * 내부 헬퍼: IA32 -ffreestanding 환경에서 __udivdi3 없이 64비트 / 10 계산
 * ------------------------------------------------------------------------- */
static UINT64 u64_divmod10(UINT64 n, UINT32 *rem_out)
{
    UINT64 q = 0;
    UINT32 r = 0;
    UINTN  i;

    for (i = 0; i < 64; i++) {
        r = (r << 1) | (UINT32)((n >> 63) & 1ULL);
        n <<= 1;
        q <<= 1;
        if (r >= 10U) {
            r -= 10U;
            q |= 1ULL;
        }
    }
    if (rem_out != NULL) {
        *rem_out = r;
    }
    return q;
}

/* -------------------------------------------------------------------------
 * EFILOG.TXT 파일 I/O 구현 (efilog.h)
 * ------------------------------------------------------------------------- */
static EFI_STATUS log_buf_commit(efi_diag_ctx_t *ctx)
{
    EFI_STATUS st;
    UINTN      request_sz;
    UINTN      write_sz;
    UINTN      i;

    if (ctx == NULL || !ctx->log_ready || ctx->log_file == NULL ||
        g_log_len == 0U) {
        return EFI_SUCCESS;
    }
    if (g_log_io_failed) {
        return g_log_io_status;
    }

    request_sz = g_log_len;
    write_sz   = request_sz;
    st = uefi_call_wrapper(ctx->log_file->Write, 3,
                           ctx->log_file, &write_sz, g_log_buf);

    /* 오류 상태에서는 기록된 바이트 수를 신뢰하지 않고 버퍼를 보존한다. */
    if (EFI_ERROR(st)) {
        g_log_io_failed = TRUE;
        g_log_io_status = st;
        return st;
    }

    /* UEFI File.Write의 반환 크기는 실제 기록된 바이트 수여야 한다. */
    if (write_sz == 0U || write_sz > request_sz) {
        g_log_io_failed = TRUE;
        g_log_io_status = EFI_DEVICE_ERROR;
        return g_log_io_status;
    }

    g_log_len -= write_sz;
    for (i = 0U; i < g_log_len; i++) {
        g_log_buf[i] = g_log_buf[write_sz + i];
    }

    return EFI_SUCCESS;
}

EFI_STATUS efilog_init(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES     *bs = ctx->st->BootServices;
    EFI_LOADED_IMAGE      *loaded = NULL;
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_STATUS             st;
    UINT64                 pos = 0;

    g_log_len             = 0;
    g_log_io_failed       = FALSE;
    g_log_io_status       = EFI_SUCCESS;
    ctx->root_dir         = NULL;
    ctx->log_file         = NULL;
    ctx->log_ready        = FALSE;
    ctx->log_appended     = FALSE;
    ctx->log_initial_size = 0;

    /* 1. LoadedImageProtocol 조회 -> 부팅 장치 핸들 및 적재 주소 확보 */
    st = uefi_call_wrapper(bs->HandleProtocol, 3,
                           ctx->image_handle, &kGuidLoadedImage, (VOID **)&loaded);
    if (EFI_ERROR(st) || loaded == NULL) {
        ctx->log_init_status = st;
        return st;
    }

    ctx->boot_dev_handle = loaded->DeviceHandle;
    ctx->image_base      = (EFI_PHYSICAL_ADDRESS)(UINTN)loaded->ImageBase;
    ctx->image_size      = loaded->ImageSize;

    if (ctx->boot_dev_handle == NULL) {
        ctx->log_init_status = EFI_NOT_FOUND;
        return EFI_NOT_FOUND;
    }

    /* 2. 부팅 볼륨의 SimpleFileSystem 열기 */
    st = uefi_call_wrapper(bs->HandleProtocol, 3,
                           ctx->boot_dev_handle, &kGuidSimpleFs, (VOID **)&fs);
    if (EFI_ERROR(st) || fs == NULL) {
        ctx->log_init_status = st;
        return st;
    }

    st = uefi_call_wrapper(fs->OpenVolume, 2, fs, &ctx->root_dir);
    if (EFI_ERROR(st) || ctx->root_dir == NULL) {
        ctx->log_init_status = st;
        return st;
    }

    /* 3. 루트 디렉터리에 EFILOG.TXT 열기/생성 (RESULT.TXT 불간섭) */
    st = uefi_call_wrapper(ctx->root_dir->Open, 5,
                           ctx->root_dir,
                           &ctx->log_file,
                           (CHAR16 *)L"EFILOG.TXT",
                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                           0);
    if (EFI_ERROR(st) || ctx->log_file == NULL) {
        uefi_call_wrapper(ctx->root_dir->Close, 1, ctx->root_dir);
        ctx->root_dir        = NULL;
        ctx->log_init_status = st;
        return st;
    }

    /* 4. 파일 끝으로 이동해 기존 세션 이어쓰기 여부 판별 */
    st = uefi_call_wrapper(ctx->log_file->SetPosition, 2,
                           ctx->log_file, 0xFFFFFFFFFFFFFFFFULL);
    if (!EFI_ERROR(st)) {
        st = uefi_call_wrapper(ctx->log_file->GetPosition, 2,
                               ctx->log_file, &pos);
        if (!EFI_ERROR(st)) {
            ctx->log_initial_size = pos;
        }
    }

    ctx->log_ready       = TRUE;
    ctx->log_init_status = EFI_SUCCESS;

    if (ctx->log_initial_size > 0) {
        ctx->log_appended = TRUE;
        efilog_write_ascii(ctx, "\n==================================================\n");
    }

    return EFI_SUCCESS;
}

VOID efilog_write_ascii(efi_diag_ctx_t *ctx, const char *s)
{
    if (ctx == NULL || !ctx->log_ready || ctx->log_file == NULL || s == NULL ||
        g_log_io_failed) {
        return;
    }

    while (*s != '\0') {
        char c = *s++;

        if (c == '\r') {
            continue;
        }

        /* 커밋이 부분 기록되면 남은 공간이 생길 때까지 재시도한다.
         * 오류/무진행이면 기존 버퍼를 보존하고 새 로그 입력을 중단한다. */
        while (g_log_len + 2U >= EFILOG_BUF_CAP && !g_log_io_failed) {
            UINTN old_len = g_log_len;
            EFI_STATUS st = log_buf_commit(ctx);

            if (EFI_ERROR(st) || g_log_len >= old_len) {
                if (!g_log_io_failed) {
                    g_log_io_failed = TRUE;
                    g_log_io_status = EFI_DEVICE_ERROR;
                }
                return;
            }
        }

        if (c == '\n') {
            g_log_buf[g_log_len++] = '\r';
            g_log_buf[g_log_len++] = '\n';
        } else {
            g_log_buf[g_log_len++] = (UINT8)c;
        }
    }
}

VOID efilog_flush(efi_diag_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->log_ready || ctx->log_file == NULL) {
        return;
    }

    /* 성공적인 부분 기록은 버퍼가 빌 때까지 이어서 기록한다. */
    while (g_log_len > 0U && !g_log_io_failed) {
        UINTN old_len = g_log_len;
        EFI_STATUS st = log_buf_commit(ctx);

        if (EFI_ERROR(st) || g_log_len >= old_len) {
            if (!g_log_io_failed) {
                g_log_io_failed = TRUE;
                g_log_io_status = EFI_DEVICE_ERROR;
            }
            break;
        }
    }

    {
        EFI_STATUS st = uefi_call_wrapper(ctx->log_file->Flush, 1,
                                          ctx->log_file);
        if (EFI_ERROR(st) && !g_log_io_failed) {
            g_log_io_failed = TRUE;
            g_log_io_status = st;
        }
    }
}

VOID efilog_close(efi_diag_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->log_file != NULL) {
        efilog_flush(ctx);
        uefi_call_wrapper(ctx->log_file->Close, 1, ctx->log_file);
        ctx->log_file = NULL;
    }
    if (ctx->root_dir != NULL) {
        uefi_call_wrapper(ctx->root_dir->Close, 1, ctx->root_dir);
        ctx->root_dir = NULL;
    }
    ctx->log_ready = FALSE;
}

/* -------------------------------------------------------------------------
 * ConOut + EFILOG 동시 출력 유틸리티 (efitxt.h)
 * ------------------------------------------------------------------------- */
static VOID con_out_ascii(efi_diag_ctx_t *ctx, const char *s)
{
    CHAR16 wbuf[128];
    UINTN  idx = 0;

    if (ctx == NULL || ctx->st == NULL || ctx->st->ConOut == NULL || s == NULL) {
        return;
    }

    while (*s != '\0') {
        char c = *s++;
        if (c == '\r') {
            continue;
        }
        if (idx >= 125U) {
            wbuf[idx] = L'\0';
            uefi_call_wrapper(ctx->st->ConOut->OutputString, 2, ctx->st->ConOut, wbuf);
            idx = 0;
        }
        if (c == '\n') {
            wbuf[idx++] = L'\r';
            wbuf[idx++] = L'\n';
        } else {
            wbuf[idx++] = (CHAR16)(UINT8)c;
        }
    }
    if (idx > 0) {
        wbuf[idx] = L'\0';
        uefi_call_wrapper(ctx->st->ConOut->OutputString, 2, ctx->st->ConOut, wbuf);
    }
}

VOID efi_out_str(efi_diag_ctx_t *ctx, const char *s)
{
    if (s == NULL) {
        return;
    }
    con_out_ascii(ctx, s);
    efilog_write_ascii(ctx, s);
}

VOID efi_out_wstr(efi_diag_ctx_t *ctx, const CHAR16 *ws)
{
    char abuf[128];
    UINTN idx = 0;

    if (ws == NULL) {
        efi_out_str(ctx, "N/A");
        return;
    }

    while (*ws != L'\0') {
        CHAR16 ch = *ws++;
        char  ac;

        if (ch == L'\r') {
            continue;
        } else if (ch == L'\n') {
            ac = '\n';
        } else if (ch >= 0x20U && ch <= 0x7EU) {
            ac = (char)ch;
        } else {
            ac = '?';
        }

        abuf[idx++] = ac;
        if (idx >= 127U) {
            abuf[idx] = '\0';
            efi_out_str(ctx, abuf);
            idx = 0;
        }
    }
    if (idx > 0) {
        abuf[idx] = '\0';
        efi_out_str(ctx, abuf);
    }
}

VOID efi_out_fixed_ascii(efi_diag_ctx_t *ctx, const UINT8 *buf, UINTN len)
{
    char tmp[2];
    UINTN i;

    if (buf == NULL) {
        return;
    }
    tmp[1] = '\0';
    for (i = 0; i < len; i++) {
        UINT8 c = buf[i];
        tmp[0] = (c >= 0x20U && c <= 0x7EU) ? (char)c : ' ';
        efi_out_str(ctx, tmp);
    }
}

VOID efi_out_u64(efi_diag_ctx_t *ctx, UINT64 val)
{
    char  rev[24];
    char  out[24];
    UINTN  i = 0, j = 0;
    UINT32 rem = 0;

    if (val == 0) {
        efi_out_str(ctx, "0");
        return;
    }
    while (val > 0) {
        val = u64_divmod10(val, &rem);
        rev[i++] = (char)('0' + rem);
    }
    while (i > 0) {
        out[j++] = rev[--i];
    }
    out[j] = '\0';
    efi_out_str(ctx, out);
}

VOID efi_out_u64_pad2(efi_diag_ctx_t *ctx, UINT64 val)
{
    if (val < 10ULL) {
        efi_out_str(ctx, "0");
    }
    efi_out_u64(ctx, val);
}

static VOID out_hex_n(efi_diag_ctx_t *ctx, UINT64 val, UINTN digits)
{
    static const char kHex[] = "0123456789ABCDEF";
    char buf[17];
    UINTN i;

    if (digits > 16U) {
        digits = 16U;
    }
    for (i = 0; i < (16U - digits); i++) {
        val <<= 4;
    }
    for (i = 0; i < digits; i++) {
        buf[i] = kHex[(UINTN)((val >> 60) & 0x0FULL)];
        val <<= 4;
    }
    buf[digits] = '\0';
    efi_out_str(ctx, buf);
}

VOID efi_out_hex8(efi_diag_ctx_t *ctx, UINT8 val)
{
    out_hex_n(ctx, (UINT64)val, 2U);
}

VOID efi_out_hex16(efi_diag_ctx_t *ctx, UINT16 val)
{
    out_hex_n(ctx, (UINT64)val, 4U);
}

VOID efi_out_hex32(efi_diag_ctx_t *ctx, UINT32 val)
{
    out_hex_n(ctx, (UINT64)val, 8U);
}

VOID efi_out_hex64(efi_diag_ctx_t *ctx, UINT64 val)
{
    out_hex_n(ctx, val, 16U);
}

VOID efi_out_size_human(efi_diag_ctx_t *ctx, UINT64 bytes)
{
    if (bytes >= (1ULL << 30)) {
        UINT64 gb = bytes >> 30;
        UINT64 mb = bytes >> 20;
        UINT32 frac = (UINT32)((((bytes >> 20) & 1023ULL) * 10ULL) >> 10);
        efi_out_u64(ctx, gb);
        efi_out_str(ctx, ".");
        efi_out_u64(ctx, (UINT64)frac);
        efi_out_str(ctx, " GiB (");
        efi_out_u64(ctx, mb);
        efi_out_str(ctx, " MiB)");
    } else if (bytes >= (1ULL << 20)) {
        UINT64 mb = bytes >> 20;
        UINT32 frac = (UINT32)((((bytes >> 10) & 1023ULL) * 10ULL) >> 10);
        efi_out_u64(ctx, mb);
        efi_out_str(ctx, ".");
        efi_out_u64(ctx, (UINT64)frac);
        efi_out_str(ctx, " MiB");
    } else if (bytes >= 1024ULL) {
        efi_out_u64(ctx, bytes >> 10);
        efi_out_str(ctx, " KiB");
    } else {
        efi_out_u64(ctx, bytes);
        efi_out_str(ctx, " B");
    }
}

VOID efi_out_status(efi_diag_ctx_t *ctx, EFI_STATUS status)
{
    const char *name = "ERROR";

    if      (status == EFI_SUCCESS)           name = "SUCCESS";
    else if (status == EFI_NOT_FOUND)         name = "NOT_FOUND";
    else if (status == EFI_UNSUPPORTED)       name = "UNSUPPORTED";
    else if (status == EFI_WRITE_PROTECTED)   name = "WRITE_PROTECTED";
    else if (status == EFI_ACCESS_DENIED)     name = "ACCESS_DENIED";
    else if (status == EFI_BUFFER_TOO_SMALL)  name = "BUFFER_TOO_SMALL";
    else if (status == EFI_INVALID_PARAMETER) name = "INVALID_PARAMETER";
    else if (status == EFI_DEVICE_ERROR)      name = "DEVICE_ERROR";
    else if (status == EFI_NO_MEDIA)          name = "NO_MEDIA";

    efi_out_str(ctx, name);
    efi_out_str(ctx, " (0x");
#if EFI_IS_X64
    efi_out_hex64(ctx, (UINT64)status);
#else
    efi_out_hex32(ctx, (UINT32)status);
#endif
    efi_out_str(ctx, ")");
}

const char *efi_diag_state_str(efi_diag_state_t state)
{
    switch (state) {
        case DIAG_PCI_FOUND:    return "PCI DEVICE FOUND";
        case DIAG_DRIVER_BOUND: return "UEFI DRIVER BOUND";
        case DIAG_PROTO_AVAIL:  return "PROTOCOL AVAILABLE";
        case DIAG_DEV_ENUM:     return "DEVICE ENUMERATED";
        case DIAG_XFER_OK:      return "TRANSFER SUCCESS";
        default:                return "NOT EXPOSED";
    }
}

const char *efi_conn_state_str(efi_conn_state_t state)
{
    switch (state) {
        case CONN_BOUND_AT_ENTRY:         return "BOUND_AT_ENTRY";
        case CONN_UNCONNECTED_AT_ENTRY:   return "UNCONNECTED_AT_ENTRY";
        case CONN_CONNECTED_AFTER_RESCAN: return "CONNECTED_AFTER_RESCAN";
        default:                          return "UNKNOWN";
    }
}

VOID efi_wait_any_key(efi_diag_ctx_t *ctx)
{
    EFI_INPUT_KEY key;
    UINTN         index = 0;

    if (ctx == NULL || ctx->st == NULL || ctx->st->ConIn == NULL) {
        return;
    }
    uefi_call_wrapper(ctx->st->ConIn->Reset, 2, ctx->st->ConIn, FALSE);
    uefi_call_wrapper(ctx->st->BootServices->WaitForEvent, 3,
                      1, &ctx->st->ConIn->WaitForKey, &index);
    uefi_call_wrapper(ctx->st->ConIn->ReadKeyStroke, 2, ctx->st->ConIn, &key);
}