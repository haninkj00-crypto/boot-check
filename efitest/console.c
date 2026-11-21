#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"
#include "../efi_spkio.h"

void efi_run_step0_console_and_log(efi_diag_ctx_t *ctx)
{
    EFI_SYSTEM_TABLE *st = ctx->st;
    UINT32            uefi_rev = st->Hdr.Revision;
    UINT32            rev_maj  = (uefi_rev >> 16) & 0xFFFFU;
    UINT32            rev_min  = uefi_rev & 0xFFFFU;
    INT32             cur_mode = -1;
    INT32             max_mode = 0;
    INT32             m;
    UINTN             cols = 0, rows = 0;
    EFI_STATUS        qst;

    /* 1. 최우선: EFILOG.TXT 열기 (화면 먹통/프리징 대비) */
    efilog_init(ctx);

    /* 화면 초기화 후 배너를 화면과 EFILOG.TXT에 동시 기록 */
    if (st->ConOut != NULL) {
        uefi_call_wrapper(st->ConOut->ClearScreen, 1, st->ConOut);
    }

    efi_out_str(ctx, "============================================================\n");
    efi_out_str(ctx, "  boot-check UEFI Diagnostic Suite (");
    efi_out_str(ctx, EFI_ARCH_TAG);
    efi_out_str(ctx, ")\n");
    efi_out_str(ctx, "  Boot Services Active | ExitBootServices() NOT called\n");
    efi_out_str(ctx, "============================================================\n\n");

    efi_out_str(ctx, "=== [Step 0] UEFI Environment, Console & Log Initialization ===\n");

    efi_out_str(ctx, "  1. Binary Target   : ");
    efi_out_str(ctx, EFI_IMAGE_NAME);
    efi_out_str(ctx, " (");
    efi_out_str(ctx, EFI_ARCH_TAG);
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "  2. Execution Mode  : ");
    efi_out_str(ctx, EFI_EXEC_MODE_STR);
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "  3. UEFI Spec Rev   : ");
    efi_out_u64(ctx, (UINT64)rev_maj);
    efi_out_str(ctx, ".");
    efi_out_u64_pad2(ctx, (UINT64)rev_min);
    efi_out_str(ctx, " (Raw 0x");
    efi_out_hex32(ctx, uefi_rev);
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "  4. Firmware Vendor : ");
    efi_out_wstr(ctx, st->FirmwareVendor);
    efi_out_str(ctx, " (Rev 0x");
    efi_out_hex32(ctx, st->FirmwareRevision);
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "  5. System Tables   : BootServices=");
    efi_out_str(ctx, (st->BootServices != NULL) ? "YES" : "NO");
    efi_out_str(ctx, ", RuntimeServices=");
    efi_out_str(ctx, (st->RuntimeServices != NULL) ? "YES" : "NO");
    efi_out_str(ctx, ", ConfigTables=");
    efi_out_u64(ctx, (UINT64)st->NumberOfTableEntries);
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "  6. Loaded Image    : Base=0x");
    efi_out_hex64(ctx, (UINT64)ctx->image_base);
    efi_out_str(ctx, ", Size=");
    efi_out_size_human(ctx, ctx->image_size);
    efi_out_str(ctx, "\n");

    /* 7. ConOut 텍스트 모드 조사 */
    efi_out_str(ctx, "  7. Console (ConOut): ");
    if (st->ConOut == NULL || st->ConOut->Mode == NULL) {
        efi_out_str(ctx, "NOT EXPOSED\n");
        efi_speaker_no_display_alert(ctx->st->BootServices);
    } else {
        cur_mode = st->ConOut->Mode->Mode;
        max_mode = st->ConOut->Mode->MaxMode;
        qst = uefi_call_wrapper(st->ConOut->QueryMode, 4,
                                st->ConOut, (UINTN)cur_mode, &cols, &rows);
        if (!EFI_ERROR(qst)) {
            efi_out_str(ctx, "Mode ");
            efi_out_u64(ctx, (UINT64)cur_mode);
            efi_out_str(ctx, " (");
            efi_out_u64(ctx, (UINT64)cols);
            efi_out_str(ctx, "x");
            efi_out_u64(ctx, (UINT64)rows);
            efi_out_str(ctx, "), Total Text Modes=");
            efi_out_u64(ctx, (UINT64)max_mode);
            efi_out_str(ctx, "\n");
        } else {
            efi_out_str(ctx, "QueryMode failed: ");
            efi_out_status(ctx, qst);
            efi_out_str(ctx, "\n");
        }

        efi_out_str(ctx, "     Supported Modes : ");
        for (m = 0; m < max_mode; m++) {
            UINTN mc = 0, mr = 0;
            qst = uefi_call_wrapper(st->ConOut->QueryMode, 4,
                                    st->ConOut, (UINTN)m, &mc, &mr);
            if (!EFI_ERROR(qst)) {
                efi_out_str(ctx, "[");
                efi_out_u64(ctx, (UINT64)m);
                efi_out_str(ctx, ":");
                efi_out_u64(ctx, (UINT64)mc);
                efi_out_str(ctx, "x");
                efi_out_u64(ctx, (UINT64)mr);
                if (m == cur_mode) {
                    efi_out_str(ctx, "*");
                }
                efi_out_str(ctx, "] ");
            }
        }
        efi_out_str(ctx, "\n");
    }

    efi_out_str(ctx, "  8. Console Input   : ConIn=");
    efi_out_str(ctx, (st->ConIn != NULL) ? "PROTOCOL AVAILABLE" : "NOT EXPOSED");
    efi_out_str(ctx, ", StdErr=");
    efi_out_str(ctx, (st->StdErr != NULL) ? "PROTOCOL AVAILABLE" : "NOT EXPOSED");
    efi_out_str(ctx, "\n");

    /* 9. EFILOG.TXT 상태 기록 */
    efi_out_str(ctx, "  9. Disk Log Target : ");
    if (ctx->log_ready) {
        efi_out_str(ctx, "EFILOG.TXT (");
        if (ctx->log_appended) {
            efi_out_str(ctx, "APPEND after ");
            efi_out_u64(ctx, ctx->log_initial_size);
            efi_out_str(ctx, " B)\n\n");
        } else {
            efi_out_str(ctx, "NEW FILE, ASCII+CRLF)\n\n");
        }
    } else {
        efi_out_str(ctx, "UNAVAILABLE - ");
        efi_out_status(ctx, ctx->log_init_status);
        efi_out_str(ctx, "\n\n");
    }

    /* Step 0 완료 시점 디스크 즉시 플러시 */
    efilog_flush(ctx);
}