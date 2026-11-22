#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

/* -------------------------------------------------------------------------
 * 표준 UEFI / ACPI / SMBIOS / CSM GUID 직접 정의
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidGlobalVar =
    { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };

static EFI_GUID kGuidLegacyBios =
    { 0xDB9A1E3D, 0x45CB, 0x4ABB, { 0x85, 0x3B, 0xE5, 0x38, 0x7F, 0xDB, 0x2E, 0x2D } };

static EFI_GUID kGuidAcpi20 =
    { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };

static EFI_GUID kGuidAcpi10 =
    { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static EFI_GUID kGuidSmbios3 =
    { 0xF2FD1544, 0x9794, 0x4A2C, { 0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94 } };

static EFI_GUID kGuidSmbios2 =
    { 0xEB9D2D31, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* -------------------------------------------------------------------------
 * 저수준 유틸리티 (정렬 독립 바이트 읽기, 체크섬, GUID 비교, 주소 검증)
 * ------------------------------------------------------------------------- */
static BOOLEAN guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    UINTN i;

    if (a == NULL || b == NULL) {
        return FALSE;
    }
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return FALSE;
    }
    for (i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOLEAN bytes_eq(const UINT8 *a, const char *b, UINTN len)
{
    UINTN i;

    for (i = 0; i < len; i++) {
        if (a[i] != (UINT8)b[i]) {
            return FALSE;
        }
    }
    return TRUE;
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

static UINT64 rd_u64(const UINT8 *p)
{
    return (UINT64)rd_u32(p) | ((UINT64)rd_u32(p + 4) << 32);
}

static BOOLEAN checksum8_ok(const UINT8 *p, UINTN len)
{
    UINT8 sum = 0;
    UINTN i;

    if (p == NULL || len == 0) {
        return FALSE;
    }
    for (i = 0; i < len; i++) {
        sum = (UINT8)(sum + p[i]);
    }
    return (sum == 0) ? TRUE : FALSE;
}

static BOOLEAN phys_accessible(UINT64 addr, UINT64 len)
{
    if (addr == 0 || len == 0) {
        return FALSE;
    }
    if (addr + len < addr) {
        return FALSE; /* 64-bit wraparound */
    }
#if !EFI_IS_X64
    if (addr > 0xFFFFFFFFULL || (addr + len) > 0x100000000ULL) {
        return FALSE; /* IA32 32비트 주소 공간 초과 */
    }
#endif
    return TRUE;
}

static VOID *find_config_table(EFI_SYSTEM_TABLE *st, const EFI_GUID *guid)
{
    UINTN i;

    if (st == NULL || st->ConfigurationTable == NULL) {
        return NULL;
    }
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, guid)) {
            return st->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * [1] Secure Boot 변수 조회 (읽기 전용)
 * ------------------------------------------------------------------------- */
static VOID probe_sb_var(efi_diag_ctx_t *ctx, const char *label,
                         const CHAR16 *wname, BOOLEAN optional)
{
    EFI_RUNTIME_SERVICES *rt = ctx->st->RuntimeServices;
    UINT8      val  = 0;
    UINTN      sz   = sizeof(val);
    UINT32     attr = 0;
    EFI_STATUS st;

    efi_out_str(ctx, "    ");
    efi_out_str(ctx, label);

    if (rt == NULL || rt->GetVariable == NULL) {
        efi_out_str(ctx, "UNAVAILABLE (No RuntimeServices)\n");
        return;
    }

    st = uefi_call_wrapper(rt->GetVariable, 5,
                           (CHAR16 *)wname, &kGuidGlobalVar, &attr, &sz, &val);
    if (!EFI_ERROR(st) && sz >= 1) {
        efi_out_u64(ctx, (UINT64)val);
        efi_out_str(ctx, (val != 0) ? " (ENABLED)\n" : " (DISABLED)\n");
    } else if (st == EFI_NOT_FOUND) {
        efi_out_str(ctx, optional ? "NOT SUPPORTED\n" : "NOT PRESENT\n");
    } else {
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
    }
}

static VOID check_secure_boot(efi_diag_ctx_t *ctx)
{
    efi_out_str(ctx, "  [1] Secure Boot Variables (Read-Only)\n");
    probe_sb_var(ctx, "SecureBoot   : ", L"SecureBoot",   FALSE);
    probe_sb_var(ctx, "SetupMode    : ", L"SetupMode",    FALSE);
    probe_sb_var(ctx, "AuditMode    : ", L"AuditMode",    TRUE);
    probe_sb_var(ctx, "DeployedMode : ", L"DeployedMode", TRUE);
}

/* -------------------------------------------------------------------------
 * [2] CSM 프로토콜 존재 여부 (EFI_LEGACY_BIOS_PROTOCOL)
 * ------------------------------------------------------------------------- */
static VOID check_csm_protocol(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    VOID              *iface = NULL;
    EFI_STATUS         st;

    ctx->csm_present = FALSE;
    efi_out_str(ctx, "  [2] CSM Protocol (EFI_LEGACY_BIOS_PROTOCOL)\n");
    efi_out_str(ctx, "    Status       : ");

    if (bs == NULL || bs->LocateProtocol == NULL) {
        efi_out_str(ctx, "UNKNOWN (LocateProtocol unavailable)\n");
        return;
    }

    st = uefi_call_wrapper(bs->LocateProtocol, 3, &kGuidLegacyBios, NULL, &iface);
    if (!EFI_ERROR(st) && iface != NULL) {
        ctx->csm_present = TRUE;
        efi_out_str(ctx, "PRESENT (CSM exposed in current session)\n");
    } else if (st == EFI_NOT_FOUND) {
        efi_out_str(ctx, "NOT EXPOSED (Not conclusive for Class 3; CSM may be disabled/uninitialized)\n");
    } else {
        efi_out_str(ctx, "UNKNOWN - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
    }
}

/* -------------------------------------------------------------------------
 * [3] ACPI RSDP / XSDT / RSDT / FADT (IAPC_BOOT_ARCH)
 * ------------------------------------------------------------------------- */
static const UINT8 *find_fadt_in_xsdt(UINT64 xsdt_addr)
{
    const UINT8 *xsdt;
    UINT32       len, cnt, i;

    if (!phys_accessible(xsdt_addr, 36ULL)) {
        return NULL;
    }
    xsdt = (const UINT8 *)(UINTN)xsdt_addr;
    if (!bytes_eq(xsdt, "XSDT", 4)) {
        return NULL;
    }
    len = rd_u32(xsdt + 4);
    if (len < 36U || len > 0x100000U || !phys_accessible(xsdt_addr, (UINT64)len)) {
        return NULL;
    }
    if (!checksum8_ok(xsdt, (UINTN)len)) {
        return NULL;
    }

    cnt = (len - 36U) >> 3;
    for (i = 0; i < cnt; i++) {
        UINT64 ent = rd_u64(xsdt + 36U + (i * 8U));
        if (phys_accessible(ent, 36ULL)) {
            const UINT8 *tbl = (const UINT8 *)(UINTN)ent;
            if (bytes_eq(tbl, "FACP", 4)) {
                return tbl;
            }
        }
    }
    return NULL;
}

static const UINT8 *find_fadt_in_rsdt(UINT64 rsdt_addr)
{
    const UINT8 *rsdt;
    UINT32       len, cnt, i;

    if (!phys_accessible(rsdt_addr, 36ULL)) {
        return NULL;
    }
    rsdt = (const UINT8 *)(UINTN)rsdt_addr;
    if (!bytes_eq(rsdt, "RSDT", 4)) {
        return NULL;
    }
    len = rd_u32(rsdt + 4);
    if (len < 36U || len > 0x100000U || !phys_accessible(rsdt_addr, (UINT64)len)) {
        return NULL;
    }
    if (!checksum8_ok(rsdt, (UINTN)len)) {
        return NULL;
    }

    cnt = (len - 36U) >> 2;
    for (i = 0; i < cnt; i++) {
        UINT64 ent = (UINT64)rd_u32(rsdt + 36U + (i * 4U));
        if (phys_accessible(ent, 36ULL)) {
            const UINT8 *tbl = (const UINT8 *)(UINTN)ent;
            if (bytes_eq(tbl, "FACP", 4)) {
                return tbl;
            }
        }
    }
    return NULL;
}

static VOID check_acpi_fadt(efi_diag_ctx_t *ctx)
{
    const UINT8 *rsdp;
    const UINT8 *fadt = NULL;
    BOOLEAN      is_v2_guid = TRUE;
    UINT8        rsdp_rev;
    UINT32       rsdp_len;
    UINT64       rsdt_addr, xsdt_addr = 0;
    UINT32       fadt_len;
    UINT8        fadt_rev;
    UINT16       arch;

    ctx->fadt_valid     = FALSE;
    ctx->iapc_boot_arch = 0;

    efi_out_str(ctx, "  [3] ACPI & FADT Legacy Boot Flags (IAPC_BOOT_ARCH)\n");

    rsdp = (const UINT8 *)find_config_table(ctx->st, &kGuidAcpi20);
    if (rsdp == NULL) {
        rsdp = (const UINT8 *)find_config_table(ctx->st, &kGuidAcpi10);
        is_v2_guid = FALSE;
    }
    if (rsdp == NULL) {
        efi_out_str(ctx, "    ACPI RSDP    : NOT EXPOSED\n");
        return;
    }

    if (!bytes_eq(rsdp, "RSD PTR ", 8) || !checksum8_ok(rsdp, 20U)) {
        efi_out_str(ctx, "    ACPI RSDP    : INVALID SIGNATURE/CHECKSUM\n");
        return;
    }

    rsdp_rev  = rsdp[15];
    rsdt_addr = (UINT64)rd_u32(rsdp + 16);

    efi_out_str(ctx, "    ACPI RSDP    : Rev=");
    efi_out_u64(ctx, (UINT64)rsdp_rev);
    efi_out_str(ctx, is_v2_guid ? " (ACPI 2.0+ GUID), OEM=\"" : " (ACPI 1.0 GUID), OEM=\"");
    efi_out_fixed_ascii(ctx, rsdp + 9, 6U);
    efi_out_str(ctx, "\"\n");

    if (rsdp_rev >= 2U) {
        rsdp_len = rd_u32(rsdp + 20);
        if (rsdp_len >= 36U && rsdp_len <= 1024U && checksum8_ok(rsdp, (UINTN)rsdp_len)) {
            xsdt_addr = rd_u64(rsdp + 24);
            fadt = find_fadt_in_xsdt(xsdt_addr);
        }
    }
    if (fadt == NULL && rsdt_addr != 0) {
        fadt = find_fadt_in_rsdt(rsdt_addr);
    }

    if (fadt == NULL) {
        efi_out_str(ctx, "    ACPI FADT    : NOT FOUND / UNREACHABLE\n");
        return;
    }

    fadt_len = rd_u32(fadt + 4);
    fadt_rev = fadt[8];

    efi_out_str(ctx, "    ACPI FADT    : Rev=");
    efi_out_u64(ctx, (UINT64)fadt_rev);
    efi_out_str(ctx, ", Len=");
    efi_out_u64(ctx, (UINT64)fadt_len);
    efi_out_str(ctx, ", OEM=\"");
    efi_out_fixed_ascii(ctx, fadt + 10, 6U);
    efi_out_str(ctx, "\"");

    if (fadt_len < 36U || fadt_len > 0x10000U || !checksum8_ok(fadt, (UINTN)fadt_len)) {
        efi_out_str(ctx, " [CHECKSUM/LEN INVALID]\n");
        return;
    }
    efi_out_str(ctx, " [CHECKSUM OK]\n");

    if (fadt_rev < 2U || fadt_len < 111U) {
        efi_out_str(ctx, "    IAPC_BOOT_ARCH: NOT DEFINED (FADT Rev 1 / Len < 111)\n");
        return;
    }

    arch = rd_u16(fadt + 109);
    ctx->fadt_valid     = TRUE;
    ctx->iapc_boot_arch = arch;

    efi_out_str(ctx, "    IAPC_BOOT_ARCH: 0x");
    efi_out_hex16(ctx, arch);
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "      Bit 0 LEGACY_DEVICES       : ");
    efi_out_str(ctx, (arch & 0x0001U) ? "1 (LPC/ISA user devices present)\n"
                                      : "0 (None reported)\n");
    efi_out_str(ctx, "      Bit 1 8042                 : ");
    efi_out_str(ctx, (arch & 0x0002U) ? "1 (PS/2 8042 supported)\n"
                                      : "0 (No 8042 -> avoid direct 0x60/0x64)\n");
    efi_out_str(ctx, "      Bit 2 VGA_NOT_PRESENT      : ");
    efi_out_str(ctx, (arch & 0x0004U) ? "1 (NO VGA HW -> avoid legacy VGA probe)\n"
                                      : "0 (VGA hardware may be present)\n");
    efi_out_str(ctx, "      Bit 3 MSI_NOT_SUPPORTED    : ");
    efi_out_str(ctx, (arch & 0x0008U) ? "1 (Do not enable MSI)\n"
                                      : "0 (MSI supported)\n");
    efi_out_str(ctx, "      Bit 4 PCIE_ASPM_CONTROLS   : ");
    efi_out_str(ctx, (arch & 0x0010U) ? "1 (OS must not touch PCIe ASPM)\n"
                                      : "0 (OS may control PCIe ASPM)\n");
    efi_out_str(ctx, "      Bit 5 CMOS_RTC_NOT_PRESENT : ");
    efi_out_str(ctx, (arch & 0x0020U) ? "1 (NO legacy CMOS RTC)\n"
                                      : "0 (Legacy CMOS RTC present)\n");
}

/* -------------------------------------------------------------------------
 * [4] SMBIOS 3.x / 2.x 파싱 (Type 0 BIOS, Type 1 System, Type 2 Baseboard)
 * ------------------------------------------------------------------------- */
static BOOLEAN smbios_copy_str(const UINT8 *tbl, UINTN str_start, UINTN str_end,
                               UINT8 idx, char *out, UINTN out_cap)
{
    UINTN cur = str_start;
    UINT8 num = 1;
    UINTN w   = 0;

    if (out_cap == 0) {
        return FALSE;
    }
    out[0] = '\0';

    if (idx == 0 || str_start >= str_end) {
        return FALSE;
    }

    while (cur < str_end && tbl[cur] != 0) {
        if (num == idx) {
            while (cur < str_end && tbl[cur] != 0) {
                UINT8 c = tbl[cur++];
                if (w + 1U < out_cap) {
                    out[w++] = (c >= 0x20U && c <= 0x7EU) ? (char)c : ' ';
                }
            }
            while (w > 0 && out[w - 1U] == ' ') {
                w--;
            }
            out[w] = '\0';
            return (w > 0) ? TRUE : FALSE;
        }
        while (cur < str_end && tbl[cur] != 0) {
            cur++;
        }
        if (cur < str_end) {
            cur++; /* NUL 건너뜀 */
        }
        num++;
    }
    return FALSE;
}

static VOID print_smbios_field(efi_diag_ctx_t *ctx, const char *label,
                               const UINT8 *tbl, UINTN s_start, UINTN s_end, UINT8 idx)
{
    char buf[96];

    efi_out_str(ctx, label);
    if (smbios_copy_str(tbl, s_start, s_end, idx, buf, sizeof(buf))) {
        efi_out_str(ctx, "\"");
        efi_out_str(ctx, buf);
        efi_out_str(ctx, "\"\n");
    } else {
        efi_out_str(ctx, "N/A\n");
    }
}

static VOID parse_smbios_table(efi_diag_ctx_t *ctx, UINT64 tbl_addr, UINT32 max_len)
{
    const UINT8 *tbl;
    UINTN        pos = 0;
    UINTN        guard = 0;
    BOOLEAN      got_t0 = FALSE, got_t1 = FALSE, got_t2 = FALSE;

    if (max_len == 0 || max_len > 0x200000U || !phys_accessible(tbl_addr, (UINT64)max_len)) {
        efi_out_str(ctx, "    Table Data   : UNREACHABLE (Addr=0x");
        efi_out_hex64(ctx, tbl_addr);
        efi_out_str(ctx, ", Len=");
        efi_out_u64(ctx, (UINT64)max_len);
        efi_out_str(ctx, ")\n");
        return;
    }

    tbl = (const UINT8 *)(UINTN)tbl_addr;

    while (pos + 4U <= (UINTN)max_len && guard++ < 4096U) {
        UINT8 type = tbl[pos];
        UINT8 slen = tbl[pos + 1U];
        UINTN s_start, s_end, next_pos;

        if (slen < 4U || pos + (UINTN)slen > (UINTN)max_len) {
            break;
        }

        s_start = pos + (UINTN)slen;
        s_end   = s_start;
        if (s_start + 2U <= (UINTN)max_len && tbl[s_start] == 0 && tbl[s_start + 1U] == 0) {
            next_pos = s_start + 2U;
        } else {
            while (s_end + 1U < (UINTN)max_len) {
                if (tbl[s_end] == 0 && tbl[s_end + 1U] == 0) {
                    break;
                }
                s_end++;
            }
            next_pos = (s_end + 2U <= (UINTN)max_len) ? (s_end + 2U) : (UINTN)max_len;
        }

        if (type == 0 && !got_t0) {
            got_t0 = TRUE;
            efi_out_str(ctx, "    [Type 0] BIOS Information\n");
            if (slen >= 9U) {
                print_smbios_field(ctx, "      Vendor       : ", tbl, s_start, s_end, tbl[pos + 4U]);
                print_smbios_field(ctx, "      Version      : ", tbl, s_start, s_end, tbl[pos + 5U]);
                print_smbios_field(ctx, "      Release Date : ", tbl, s_start, s_end, tbl[pos + 8U]);
            }
            if (slen >= 20U) {
                UINT8 ext2 = tbl[pos + 19U];
                efi_out_str(ctx, "      Ext2 Flags   : 0x");
                efi_out_hex8(ctx, ext2);
                efi_out_str(ctx, " [UEFI=");
                efi_out_str(ctx, (ext2 & 0x08U) ? "YES" : "NO");
                efi_out_str(ctx, ", VM=");
                efi_out_str(ctx, (ext2 & 0x10U) ? "YES" : "NO");
                efi_out_str(ctx, "]\n");
            }
        } else if (type == 1 && !got_t1) {
            got_t1 = TRUE;
            efi_out_str(ctx, "    [Type 1] System Information\n");
            if (slen >= 7U) {
                print_smbios_field(ctx, "      Manufacturer : ", tbl, s_start, s_end, tbl[pos + 4U]);
                print_smbios_field(ctx, "      Product Name : ", tbl, s_start, s_end, tbl[pos + 5U]);
                print_smbios_field(ctx, "      Version      : ", tbl, s_start, s_end, tbl[pos + 6U]);
            }
        } else if (type == 2 && !got_t2) {
            got_t2 = TRUE;
            efi_out_str(ctx, "    [Type 2] Baseboard Information\n");
            if (slen >= 6U) {
                print_smbios_field(ctx, "      Manufacturer : ", tbl, s_start, s_end, tbl[pos + 4U]);
                print_smbios_field(ctx, "      Product      : ", tbl, s_start, s_end, tbl[pos + 5U]);
            }
        } else if (type == 127U) {
            break;
        }

        if (got_t0 && got_t1 && got_t2) {
            break;
        }
        pos = next_pos;
    }

    if (!got_t0 && !got_t1 && !got_t2) {
        efi_out_str(ctx, "    Structures   : No Type 0/1/2 records decoded\n");
    }
}

static VOID check_smbios(efi_diag_ctx_t *ctx)
{
    const UINT8 *ep3 = (const UINT8 *)find_config_table(ctx->st, &kGuidSmbios3);
    const UINT8 *ep2 = (const UINT8 *)find_config_table(ctx->st, &kGuidSmbios2);
    UINT64       tbl_addr = 0;
    UINT32       tbl_len  = 0;

    efi_out_str(ctx, "  [4] SMBIOS Tables\n");

    if (ep3 != NULL && bytes_eq(ep3, "_SM3_", 5)) {
        UINT8 ep_len = ep3[6];
        if (ep_len >= 24U && checksum8_ok(ep3, (UINTN)ep_len)) {
            tbl_len  = rd_u32(ep3 + 12);
            tbl_addr = rd_u64(ep3 + 16);
            efi_out_str(ctx, "    Entry Point  : SMBIOS 3.x (v");
            efi_out_u64(ctx, (UINT64)ep3[7]);
            efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)ep3[8]);
            efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)ep3[9]);
            efi_out_str(ctx, ", Addr=0x");
            efi_out_hex64(ctx, tbl_addr);
            efi_out_str(ctx, ", MaxLen=");
            efi_out_u64(ctx, (UINT64)tbl_len);
            efi_out_str(ctx, ")\n");

            if (phys_accessible(tbl_addr, (UINT64)tbl_len)) {
                parse_smbios_table(ctx, tbl_addr, tbl_len);
                return;
            }
            efi_out_str(ctx, "    Note         : 64-bit SMBIOS 3.x table unreachable on this arch; trying 2.x...\n");
        }
    }

    if (ep2 != NULL && bytes_eq(ep2, "_SM_", 4)) {
        UINT8 ep_len = ep2[5];
        if (ep_len >= 31U && checksum8_ok(ep2, (UINTN)ep_len)) {
            tbl_len  = (UINT32)rd_u16(ep2 + 22);
            tbl_addr = (UINT64)rd_u32(ep2 + 24);
            efi_out_str(ctx, "    Entry Point  : SMBIOS 2.x (v");
            efi_out_u64(ctx, (UINT64)ep2[6]);
            efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)ep2[7]);
            efi_out_str(ctx, ", Addr=0x");
            efi_out_hex64(ctx, tbl_addr);
            efi_out_str(ctx, ", Len=");
            efi_out_u64(ctx, (UINT64)tbl_len);
            efi_out_str(ctx, ")\n");

            parse_smbios_table(ctx, tbl_addr, tbl_len);
            return;
        }
    }

    efi_out_str(ctx, "    Entry Point  : NOT EXPOSED / INVALID\n");
}

/* -------------------------------------------------------------------------
 * Step 0.5 메인 엔트리
 * ------------------------------------------------------------------------- */
void efi_run_step0_5_firmware(efi_diag_ctx_t *ctx)
{
    efi_out_str(ctx, "=== [Step 0.5] Firmware Tables, Security & Legacy Readiness ===\n");

    check_secure_boot(ctx);
    check_csm_protocol(ctx);
    check_acpi_fadt(ctx);
    check_smbios(ctx);

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}