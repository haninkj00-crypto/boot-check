#ifndef EFILOG_H
#define EFILOG_H

#include "efictx.h"

/* LoadedImage 볼륨의 루트에 EFILOG.TXT 열기/생성 및 끝 위치로 이동 */
EFI_STATUS efilog_init(efi_diag_ctx_t *ctx);

/* ASCII 문자열을 CRLF 정규화하여 로그 버퍼에 기록 (가득 차면 자동 플러시) */
VOID efilog_write_ascii(efi_diag_ctx_t *ctx, const char *s);

/* 로그 버퍼를 파일에 기록하고 EFI_FILE_HANDLE->Flush() 호출 */
VOID efilog_flush(efi_diag_ctx_t *ctx);

/* 최종 플러시 후 EFILOG.TXT 및 루트 디렉터리 핸들 닫기 */
VOID efilog_close(efi_diag_ctx_t *ctx);

/* 로그 기록 실패 시 대응 */
BOOLEAN efilog_has_error(const efi_diag_ctx_t *ctx);
VOID    efilog_print_final_status(efi_diag_ctx_t *ctx);

#endif /* EFILOG_H */