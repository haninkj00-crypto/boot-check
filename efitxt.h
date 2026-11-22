#ifndef EFITXT_H
#define EFITXT_H

#include "efictx.h"

/* 화면(ConOut) + EFILOG.TXT 동시 출력 */
VOID efi_out_str(efi_diag_ctx_t *ctx, const char *s);
VOID efi_out_wstr(efi_diag_ctx_t *ctx, const CHAR16 *ws);
VOID efi_out_fixed_ascii(efi_diag_ctx_t *ctx, const UINT8 *buf, UINTN len);

/* 숫자 / 16진수 / 용량 / 상태 코드 출력 (IA32 libgcc 의존성 없음) */
VOID efi_out_u64(efi_diag_ctx_t *ctx, UINT64 val);
VOID efi_out_u64_pad2(efi_diag_ctx_t *ctx, UINT64 val);
VOID efi_out_hex8(efi_diag_ctx_t *ctx, UINT8 val);
VOID efi_out_hex16(efi_diag_ctx_t *ctx, UINT16 val);
VOID efi_out_hex32(efi_diag_ctx_t *ctx, UINT32 val);
VOID efi_out_hex64(efi_diag_ctx_t *ctx, UINT64 val);
VOID efi_out_size_human(efi_diag_ctx_t *ctx, UINT64 bytes);
VOID efi_out_status(efi_diag_ctx_t *ctx, EFI_STATUS status);

/* 6단계 진단 상태 / 연결 상태 문자열 변환 */
const char *efi_diag_state_str(efi_diag_state_t state);
const char *efi_conn_state_str(efi_conn_state_t state);

/* 키 입력 유틸리티 */
VOID efi_wait_any_key(efi_diag_ctx_t *ctx);

#endif /* EFITXT_H */