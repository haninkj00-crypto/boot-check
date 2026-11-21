#include "efilog.h"
#include "efitxt.h"
#include "../efi_spkio.h"

#define MAX_PORT_CANDS 16U

/* -------------------------------------------------------------------------
 * UEFI 2.11 SerialIo / PCI / DevicePath / ACPI 표준 GUID
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidSerialIo =
    { 0xBB25CF6F, 0xF1D4, 0x11D2, { 0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD } };

static EFI_GUID kGuidPciIo =
    { 0x4CF5B200, 0x68B8, 0x4CA5, { 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A } };

static EFI_GUID kGuidDevicePath =
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidAcpi20 =
    { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };

static EFI_GUID kGuidAcpi10 =
    { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* -------------------------------------------------------------------------
 * EFI_SERIAL_IO_PROTOCOL / 최소 PCI I/O 레이아웃
 * ------------------------------------------------------------------------- */
typedef struct {
    UINT32 ControlMask;
    UINT32 Timeout;
    UINT64 BaudRate;
    UINT32 ReceiveFifoDepth;
    UINT32 DataBits;
    UINT32 Parity;
    UINT32 StopBits;
} bc_serial_io_mode_t;

typedef struct _BC_SERIAL_IO bc_serial_io_t;

typedef EFI_STATUS (EFIAPI *BC_SERIAL_GET_CTRL_FN)(
    bc_serial_io_t *This,
    UINT32         *Control
);

struct _BC_SERIAL_IO {
    UINT32                Revision;
    VOID                 *Reset;
    VOID                 *SetAttributes;
    VOID                 *SetControl;
    BC_SERIAL_GET_CTRL_FN GetControl;
    VOID                 *Write;
    VOID                 *Read;
    bc_serial_io_mode_t  *Mode;
};

typedef struct _BC_PRT_PCI_IO bc_prt_pci_io_t;

typedef EFI_STATUS (EFIAPI *BC_PRT_PCI_CFG_FN)(
    bc_prt_pci_io_t *This,
    UINT32           Width,
    UINT32           Offset,
    UINTN            Count,
    VOID            *Buffer
);

typedef struct {
    BC_PRT_PCI_CFG_FN Read;
    BC_PRT_PCI_CFG_FN Write;
} bc_prt_pci_cfg_t;

typedef EFI_STATUS (EFIAPI *BC_PRT_PCI_LOC_FN)(
    bc_prt_pci_io_t *This,
    UINTN           *Segment,
    UINTN           *Bus,
    UINTN           *Device,
    UINTN           *Function
);

struct _BC_PRT_PCI_IO {
    VOID             *PollMem;
    VOID             *PollIo;
    VOID             *MemRead;
    VOID             *MemWrite;
    VOID             *IoRead;
    VOID             *IoWrite;
    bc_prt_pci_cfg_t  Pci;
    VOID             *CopyMem;
    VOID             *Map;
    VOID             *Unmap;
    VOID             *AllocateBuffer;
    VOID             *FreeBuffer;
    VOID             *Flush;
    BC_PRT_PCI_LOC_FN GetLocation;
    VOID             *Attributes;
    VOID             *GetBarAttributes;
    VOID             *SetBarAttributes;
    UINT64            RomSize;
    VOID             *RomImage;
};

typedef struct {
    UINT16      base;
    const char *src;
} port_cand_t;

/* -------------------------------------------------------------------------
 * 공통 헬퍼
 * ------------------------------------------------------------------------- */
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

static BOOLEAN guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    UINTN i;

    if (a == NULL || b == NULL) return FALSE;
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return FALSE;
    for (i = 0U; i < 8U; i++) {
        if (a->Data4[i] != b->Data4[i]) return FALSE;
    }
    return TRUE;
}

static BOOLEAN bytes_eq(const UINT8 *a, const char *b, UINTN len)
{
    UINTN i;

    for (i = 0U; i < len; i++) {
        if (a[i] != (UINT8)b[i]) return FALSE;
    }
    return TRUE;
}

static BOOLEAN checksum8_ok(const UINT8 *p, UINTN len)
{
    UINT8 sum = 0U;
    UINTN i;

    if (p == NULL || len == 0U) return FALSE;
    for (i = 0U; i < len; i++) {
        sum = (UINT8)(sum + p[i]);
    }
    return (sum == 0U) ? TRUE : FALSE;
}

static BOOLEAN phys_accessible(UINT64 addr, UINT64 len)
{
    if (addr == 0ULL || len == 0ULL || (addr + len) < addr) return FALSE;
#if !EFI_IS_X64
    if (addr > 0xFFFFFFFFULL || (addr + len) > 0x100000000ULL) return FALSE;
#endif
    return TRUE;
}

static VOID add_port_cand(port_cand_t *list, UINTN *cnt, UINT16 base, const char *src)
{
    UINTN i;

    if (base == 0U || *cnt >= MAX_PORT_CANDS) return;
    for (i = 0U; i < *cnt; i++) {
        if (list[i].base == base) return;
    }
    list[*cnt].base = base;
    list[*cnt].src  = src;
    (*cnt)++;
}

static VOID out_sbdf(efi_diag_ctx_t *ctx, UINTN seg, UINTN bus, UINTN dev, UINTN func)
{
    efi_out_hex16(ctx, (UINT16)seg); efi_out_str(ctx, ":");
    efi_out_hex8(ctx, (UINT8)bus);   efi_out_str(ctx, ":");
    efi_out_hex8(ctx, (UINT8)dev);   efi_out_str(ctx, ".");
    efi_out_u64(ctx, (UINT64)(func & 0x07U));
}

static VOID out_serial_dp(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    const UINT8 *dp = NULL;
    UINTN        guard = 0U;
    BOOLEAN      printed = FALSE;
    EFI_STATUS   st;

    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, &kGuidDevicePath, (VOID **)&dp);
    if (EFI_ERROR(st) || dp == NULL) {
        efi_out_str(ctx, "N/A");
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
            UINT32 uid = rd_u32(dp + 8);
            if ((hid & 0xFFFFU) == 0x41D0U && ((hid >> 16) == 0x0501U || (hid >> 16) == 0x0500U)) {
                if (printed) efi_out_str(ctx, " -> ");
                efi_out_str(ctx, "PNP0501(UID=");
                efi_out_u64(ctx, (UINT64)uid);
                efi_out_str(ctx, ")");
                printed = TRUE;
            }
        } else if (type == 0x03U && sub == 0x0EU && len >= 19U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "UART(");
            efi_out_u64(ctx, rd_u64(dp + 8));
            efi_out_str(ctx, "bps)");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x05U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "USB(P");
            efi_out_u64(ctx, (UINT64)dp[4]); efi_out_str(ctx, ",If");
            efi_out_u64(ctx, (UINT64)dp[5]); efi_out_str(ctx, ")");
            printed = TRUE;
        }
        dp += len;
    }
    if (!printed) efi_out_str(ctx, "(other DevicePath)");
}

/* -------------------------------------------------------------------------
 * [1] UEFI EFI_SERIAL_IO_PROTOCOL 열거
 * ------------------------------------------------------------------------- */
static const char *serial_parity_str(UINT32 p)
{
    switch (p) {
        case 1U: return "N";
        case 2U: return "E";
        case 3U: return "O";
        case 4U: return "M";
        case 5U: return "S";
        default: return "?";
    }
}

static const char *serial_stop_str(UINT32 s)
{
    switch (s) {
        case 1U: return "1";
        case 2U: return "1.5";
        case 3U: return "2";
        default: return "?";
    }
}

static VOID check_uefi_serial_io(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs)
{
    EFI_HANDLE *handles = NULL;
    UINTN       cnt = 0U, i;
    EFI_STATUS  st;

    efi_out_str(ctx, "  [1] UEFI Serial I/O Protocol (EFI_SERIAL_IO_PROTOCOL)\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidSerialIo, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0U) {
        efi_out_str(ctx, "    Status           : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (Normal if Serial Redirection is off in BIOS)\n");
        return;
    }

    efi_out_str(ctx, "    Instances        : ");
    efi_out_u64(ctx, (UINT64)cnt);
    efi_out_str(ctx, " (");
    efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
    efi_out_str(ctx, ")\n");

    for (i = 0U; i < cnt; i++) {
        bc_serial_io_t *sio = NULL;
        UINT32          ctrl = 0U;

        st = uefi_call_wrapper(bs->HandleProtocol, 3,
                               handles[i], &kGuidSerialIo, (VOID **)&sio);
        if (EFI_ERROR(st) || sio == NULL || sio->Mode == NULL) continue;

        efi_out_str(ctx, "    [SerialIo #");
        efi_out_u64(ctx, (UINT64)i);
        efi_out_str(ctx, "] ");
        efi_out_u64(ctx, sio->Mode->BaudRate);
        efi_out_str(ctx, " bps ");
        efi_out_u64(ctx, (UINT64)sio->Mode->DataBits);
        efi_out_str(ctx, serial_parity_str(sio->Mode->Parity));
        efi_out_str(ctx, serial_stop_str(sio->Mode->StopBits));
        efi_out_str(ctx, " | FIFO=");
        efi_out_u64(ctx, (UINT64)sio->Mode->ReceiveFifoDepth);
        efi_out_str(ctx, "\n      Path           : ");
        out_serial_dp(ctx, bs, handles[i]);
        efi_out_str(ctx, "\n");

        if (sio->GetControl != NULL &&
            !EFI_ERROR(uefi_call_wrapper(sio->GetControl, 2, sio, &ctrl))) {
            efi_out_str(ctx, "      Control Lines  : [");
            efi_out_str(ctx, (ctrl & 0x0010U) ? "CTS " : "cts ");
            efi_out_str(ctx, (ctrl & 0x0020U) ? "DSR " : "dsr ");
            efi_out_str(ctx, (ctrl & 0x0040U) ? "RI "  : "ri ");
            efi_out_str(ctx, (ctrl & 0x0080U) ? "DCD"  : "dcd");
            efi_out_str(ctx, "] (raw=0x");
            efi_out_hex32(ctx, ctrl);
            efi_out_str(ctx, ")\n");
        }
    }
    uefi_call_wrapper(bs->FreePool, 1, handles);
}

/* -------------------------------------------------------------------------
 * [2] ACPI SPCR 조회
 * ------------------------------------------------------------------------- */
static const UINT8 *find_acpi_table_sig(EFI_SYSTEM_TABLE *st, const char *sig4)
{
    const UINT8 *rsdp = NULL;
    UINTN        i;
    UINT8        rev;
    UINT64       xsdt_addr = 0ULL;
    UINT32       rsdt_addr = 0U;

    if (st == NULL || st->ConfigurationTable == NULL) return NULL;
    for (i = 0U; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &kGuidAcpi20)) {
            rsdp = (const UINT8 *)st->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (rsdp == NULL) {
        for (i = 0U; i < st->NumberOfTableEntries; i++) {
            if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &kGuidAcpi10)) {
                rsdp = (const UINT8 *)st->ConfigurationTable[i].VendorTable;
                break;
            }
        }
    }
    if (rsdp == NULL || !bytes_eq(rsdp, "RSD PTR ", 8U) || !checksum8_ok(rsdp, 20U)) {
        return NULL;
    }

    rev       = rsdp[15];
    rsdt_addr = rd_u32(rsdp + 16);
    if (rev >= 2U) {
        UINT32 rlen = rd_u32(rsdp + 20);
        if (rlen >= 36U && rlen <= 1024U && checksum8_ok(rsdp, (UINTN)rlen)) {
            xsdt_addr = rd_u64(rsdp + 24);
        }
    }

    if (xsdt_addr != 0ULL && phys_accessible(xsdt_addr, 36ULL)) {
        const UINT8 *xsdt = (const UINT8 *)(UINTN)xsdt_addr;
        UINT32 len = rd_u32(xsdt + 4);
        if (bytes_eq(xsdt, "XSDT", 4U) && len >= 36U && len <= 0x100000U &&
            phys_accessible(xsdt_addr, (UINT64)len) && checksum8_ok(xsdt, (UINTN)len)) {
            UINT32 cnt = (len - 36U) >> 3;
            for (i = 0U; i < cnt; i++) {
                UINT64 ent = rd_u64(xsdt + 36U + (i * 8U));
                if (phys_accessible(ent, 36ULL)) {
                    const UINT8 *tbl = (const UINT8 *)(UINTN)ent;
                    if (bytes_eq(tbl, sig4, 4U)) return tbl;
                }
            }
        }
    }

    if (rsdt_addr != 0U && phys_accessible((UINT64)rsdt_addr, 36ULL)) {
        const UINT8 *rsdt = (const UINT8 *)(UINTN)rsdt_addr;
        UINT32 len = rd_u32(rsdt + 4);
        if (bytes_eq(rsdt, "RSDT", 4U) && len >= 36U && len <= 0x100000U &&
            phys_accessible((UINT64)rsdt_addr, (UINT64)len) && checksum8_ok(rsdt, (UINTN)len)) {
            UINT32 cnt = (len - 36U) >> 2;
            for (i = 0U; i < cnt; i++) {
                UINT64 ent = (UINT64)rd_u32(rsdt + 36U + (i * 4U));
                if (phys_accessible(ent, 36ULL)) {
                    const UINT8 *tbl = (const UINT8 *)(UINTN)ent;
                    if (bytes_eq(tbl, sig4, 4U)) return tbl;
                }
            }
        }
    }

    return NULL;
}

static const char *spcr_baud_str(UINT8 b)
{
    switch (b) {
        case 0U: return "OS/Firmware-Default";
        case 3U: return "9600";
        case 4U: return "19200";
        case 6U: return "57600";
        case 7U: return "115200";
        default: return "Other";
    }
}

static VOID check_acpi_spcr(efi_diag_ctx_t *ctx, port_cand_t *com_cands, UINTN *com_cnt)
{
    const UINT8 *spcr = find_acpi_table_sig(ctx->st, "SPCR");
    UINT32       len;
    UINT8        iftype, asid, baud, irq;
    UINT64       addr;

    efi_out_str(ctx, "\n  [2] ACPI SPCR Table (Firmware Serial Console Only; Not All COMs)\n");
    if (spcr == NULL) {
        efi_out_str(ctx, "    ACPI SPCR        : NOT EXPOSED\n");
        return;
    }

    len = rd_u32(spcr + 4);
    if (len < 64U || len > 4096U || !checksum8_ok(spcr, (UINTN)len)) {
        efi_out_str(ctx, "    ACPI SPCR        : INVALID LEN/CHECKSUM\n");
        return;
    }

    iftype = spcr[36];
    asid   = spcr[40];
    addr   = rd_u64(spcr + 44);
    irq    = spcr[53];
    baud   = spcr[58];

    efi_out_str(ctx, "    ACPI SPCR        : PRESENT (Rev=");
    efi_out_u64(ctx, (UINT64)spcr[8]);
    efi_out_str(ctx, ", IfType=0x"); efi_out_hex8(ctx, iftype);
    efi_out_str(ctx, ", Space=");
    efi_out_str(ctx, (asid == 1U) ? "I/O" : ((asid == 0U) ? "MMIO" : "Other"));
    efi_out_str(ctx, " 0x"); efi_out_hex64(ctx, addr);
    efi_out_str(ctx, ", Baud="); efi_out_str(ctx, spcr_baud_str(baud));
    efi_out_str(ctx, ", IRQ="); efi_out_u64(ctx, (UINT64)irq);
    efi_out_str(ctx, ")\n");

    if (asid == 1U && addr > 0ULL && addr <= 0xFFF8ULL) {
        add_port_cand(com_cands, com_cnt, (UINT16)addr, "ACPI-SPCR");
    }
}

/* -------------------------------------------------------------------------
 * [3] PCI Communication Controllers (Class 07h: Serial/Parallel/Modem)
 * ------------------------------------------------------------------------- */
static VOID scan_pci_comm_ports(efi_diag_ctx_t *ctx,
                                EFI_BOOT_SERVICES *bs,
                                port_cand_t *com_cands, UINTN *com_cnt,
                                port_cand_t *lpt_cands, UINTN *lpt_cnt)
{
    EFI_HANDLE *handles = NULL;
    UINTN       cnt = 0U, i, found = 0U;
    EFI_STATUS  st;

    efi_out_str(ctx, "\n  [3] PCI Communication Controllers (Class 07h)\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidPciIo, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        efi_out_str(ctx, "    PCI Enum         : UNAVAILABLE\n");
        return;
    }

    for (i = 0U; i < cnt; i++) {
        bc_prt_pci_io_t *pci = NULL;
        UINT32 id_reg = 0U, cls_reg = 0U;
        UINTN  seg = 0U, bus = 0U, dev = 0U, func = 0U;
        UINT8  cls, sub, pif, b;

        if (EFI_ERROR(uefi_call_wrapper(bs->HandleProtocol, 3,
                                        handles[i], &kGuidPciIo, (VOID **)&pci)) ||
            pci == NULL) {
            continue;
        }
        if (EFI_ERROR(uefi_call_wrapper(pci->Pci.Read, 5,
                                        pci, (UINT32)2, (UINT32)0x08U, (UINTN)1, &cls_reg))) {
            continue;
        }
        cls = (UINT8)((cls_reg >> 24) & 0xFFU);
        sub = (UINT8)((cls_reg >> 16) & 0xFFU);
        pif = (UINT8)((cls_reg >> 8)  & 0xFFU);

        if (cls != 0x07U || sub > 0x03U) {
            continue;
        }

        found++;
        uefi_call_wrapper(pci->Pci.Read, 5, pci, (UINT32)2, (UINT32)0x00U, (UINTN)1, &id_reg);
        uefi_call_wrapper(pci->GetLocation, 5, pci, &seg, &bus, &dev, &func);

        efi_out_str(ctx, "    ");
        out_sbdf(ctx, seg, bus, dev, func);
        efi_out_str(ctx, " ");
        efi_out_hex16(ctx, (UINT16)(id_reg & 0xFFFFU));
        efi_out_str(ctx, ":");
        efi_out_hex16(ctx, (UINT16)(id_reg >> 16));
        efi_out_str(ctx, " [07"); efi_out_hex8(ctx, sub);
        efi_out_str(ctx, "/");    efi_out_hex8(ctx, pif);
        efi_out_str(ctx, "] ");
        if      (sub == 0x00U) efi_out_str(ctx, "PCI-Serial");
        else if (sub == 0x01U) efi_out_str(ctx, "PCI-Parallel");
        else if (sub == 0x02U) efi_out_str(ctx, "PCI-MultiportSerial");
        else                   efi_out_str(ctx, "PCI-Modem");

        for (b = 0U; b < 6U; b++) {
            UINT32 raw = 0U;
            if (!EFI_ERROR(uefi_call_wrapper(pci->Pci.Read, 5,
                                             pci, (UINT32)2,
                                             (UINT32)(0x10U + ((UINT32)b * 4U)),
                                             (UINTN)1, &raw))) {
                if (raw != 0U && raw != 0xFFFFFFFFU && (raw & 0x01U) != 0U) {
                    UINT16 iobase = (UINT16)(raw & 0xFFFCU);
                    if (iobase >= 0x0100U) {
                        efi_out_str(ctx, " I/O=0x");
                        efi_out_hex16(ctx, iobase);
                        if (sub == 0x01U) {
                            add_port_cand(lpt_cands, lpt_cnt, iobase, "PCI-LPT");
                        } else {
                            add_port_cand(com_cands, com_cnt, iobase, "PCI-COM");
                        }
                    }
                }
            }
        }
        efi_out_str(ctx, "\n");
    }

    if (found == 0U) {
        efi_out_str(ctx, "    NONE DETECTED (No PCI Serial/Parallel/Modem cards)\n");
    }
    uefi_call_wrapper(bs->FreePool, 1, handles);
}

/* -------------------------------------------------------------------------
 * [4] COM (UART) 직접 검사 및 AT 모뎀 쿼리
 * ------------------------------------------------------------------------- */
static BOOLEAN uart_probe_chip(UINT16 base, const char **chip_name_out)
{
    UINT8 orig_ier, orig_lcr, orig_mcr, orig_scr;
    UINT8 msr_hi, msr_lo, iir, s1, s2;

    if (spk_io_in8((UINT16)(base + 5U)) == 0xFFU && spk_io_in8((UINT16)(base + 4U)) == 0xFFU) {
        return FALSE;
    }

    orig_ier = spk_io_in8((UINT16)(base + 1U));
    orig_lcr = spk_io_in8((UINT16)(base + 3U));
    orig_mcr = spk_io_in8((UINT16)(base + 4U));
    orig_scr = spk_io_in8((UINT16)(base + 7U));

    spk_io_out8((UINT16)(base + 4U), 0x10U);
    msr_lo = (UINT8)(spk_io_in8((UINT16)(base + 6U)) & 0xF0U);

    spk_io_out8((UINT16)(base + 4U), 0x1FU);
    msr_hi = (UINT8)(spk_io_in8((UINT16)(base + 6U)) & 0xF0U);

    spk_io_out8((UINT16)(base + 4U), orig_mcr);

    if (msr_lo != 0x00U || msr_hi != 0xF0U) {
        spk_io_out8((UINT16)(base + 3U), orig_lcr);
        spk_io_out8((UINT16)(base + 1U), orig_ier);
        return FALSE;
    }

    spk_io_out8((UINT16)(base + 2U), 0xE7U);
    iir = spk_io_in8((UINT16)(base + 2U));

    if ((iir & 0xC0U) == 0xC0U) {
        *chip_name_out = ((iir & 0x20U) != 0U) ? "16750" : "16550A";
    } else if ((iir & 0xC0U) == 0x80U) {
        *chip_name_out = "16550 (Early/No-FIFO)";
    } else {
        spk_io_out8((UINT16)(base + 7U), 0x55U);
        s1 = spk_io_in8((UINT16)(base + 7U));
        spk_io_out8((UINT16)(base + 7U), 0xAAU);
        s2 = spk_io_in8((UINT16)(base + 7U));
        spk_io_out8((UINT16)(base + 7U), orig_scr);
        *chip_name_out = (s1 == 0x55U && s2 == 0xAAU) ? "16450" : "8250";
    }

    spk_io_out8((UINT16)(base + 2U), 0x00U);
    spk_io_out8((UINT16)(base + 3U), orig_lcr);
    spk_io_out8((UINT16)(base + 1U), orig_ier);
    return TRUE;
}

static VOID uart_drain_rx(UINT16 base)
{
    UINTN n;

    for (n = 0U; n < 64U; n++) {
        if ((spk_io_in8((UINT16)(base + 5U)) & 0x01U) == 0U) break;
        (void)spk_io_in8(base);
    }
}

static VOID uart_send_str(EFI_BOOT_SERVICES *bs, UINT16 base, const char *s)
{
    while (*s != '\0') {
        UINTN wait = 0U;
        while ((spk_io_in8((UINT16)(base + 5U)) & 0x20U) == 0U && wait++ < 2000U) {
            spk_stall_us(bs, 50U);
        }
        spk_io_out8(base, (UINT8)(*s++));
    }
}

static UINTN uart_recv_resp(EFI_BOOT_SERVICES *bs, UINT16 base,
                            char *out, UINTN out_cap, UINTN max_ms)
{
    UINTN len = 0U, elapsed_ms = 0U, idle_ms = 0U;
    BOOLEAN got_any = FALSE;

    if (out_cap == 0U) return 0U;
    out[0] = '\0';

    while (elapsed_ms < max_ms) {
        BOOLEAN rx_now = FALSE;
        while ((spk_io_in8((UINT16)(base + 5U)) & 0x01U) != 0U) {
            UINT8 c = spk_io_in8(base);
            rx_now  = TRUE;
            got_any = TRUE;
            idle_ms = 0U;

            if (len + 1U < out_cap) {
                if (c >= 0x20U && c <= 0x7EU) {
                    out[len++] = (char)c;
                } else if (c == '\r' || c == '\n') {
                    if (len > 0U && out[len - 1U] != ' ') {
                        out[len++] = ' ';
                    }
                }
            }
        }
        if (got_any && !rx_now) {
            idle_ms++;
            if (idle_ms >= 60U) break;
        }
        spk_stall_us(bs, 1000U);
        elapsed_ms++;
    }

    while (len > 0U && out[len - 1U] == ' ') {
        len--;
    }
    out[len] = '\0';
    return len;
}

static BOOLEAN str_has_ok_ci(const char *s)
{
    UINTN i = 0U;

    while (s[i] != '\0' && s[i + 1U] != '\0') {
        char a = s[i];
        char b = s[i + 1U];
        if ((a == 'O' || a == 'o') && (b == 'K' || b == 'k')) {
            return TRUE;
        }
        i++;
    }
    return FALSE;
}

static VOID uart_set_8n1_div(UINT16 base, UINT16 div)
{
    spk_io_out8((UINT16)(base + 1U), 0x00U);
    spk_io_out8((UINT16)(base + 3U), 0x83U);
    spk_io_out8((UINT16)(base + 0U), (UINT8)(div & 0xFFU));
    spk_io_out8((UINT16)(base + 1U), (UINT8)(div >> 8));
    spk_io_out8((UINT16)(base + 3U), 0x03U);
    spk_io_out8((UINT16)(base + 2U), 0x07U);
    spk_io_out8((UINT16)(base + 4U), 0x0BU);
}

static VOID query_com_modem(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs, UINT16 base)
{
    static const UINT16 kDivs[3]  = { 1U, 3U, 12U };      /* 115200, 38400, 9600 */
    static const UINT32 kBauds[3] = { 115200U, 38400U, 9600U };

    UINT8   orig_ier, orig_lcr, orig_mcr, orig_dll, orig_dlm, msr;
    UINTN   bi;
    BOOLEAN modem_found = FALSE;
    char    resp[128];

    orig_ier = spk_io_in8((UINT16)(base + 1U));
    orig_lcr = spk_io_in8((UINT16)(base + 3U));
    orig_mcr = spk_io_in8((UINT16)(base + 4U));
    spk_io_out8((UINT16)(base + 3U), (UINT8)(orig_lcr | 0x80U));
    orig_dll = spk_io_in8((UINT16)(base + 0U));
    orig_dlm = spk_io_in8((UINT16)(base + 1U));
    spk_io_out8((UINT16)(base + 3U), (UINT8)(orig_lcr & 0x7FU));

    spk_io_out8((UINT16)(base + 4U), 0x0BU);
    spk_stall_us(bs, 5000U);
    msr = spk_io_in8((UINT16)(base + 6U));

    efi_out_str(ctx, "      Modem Lines    : [");
    efi_out_str(ctx, (msr & 0x10U) ? "CTS " : "cts ");
    efi_out_str(ctx, (msr & 0x20U) ? "DSR " : "dsr ");
    efi_out_str(ctx, (msr & 0x40U) ? "RI "  : "ri ");
    efi_out_str(ctx, (msr & 0x80U) ? "DCD"  : "dcd");
    efi_out_str(ctx, "] (MSR=0x");
    efi_out_hex8(ctx, msr);
    efi_out_str(ctx, ")\n");

    {
        UINTN wait_ms = ((msr & 0x30U) != 0U) ? 280U : 120U;

        for (bi = 0U; bi < 3U; bi++) {
            UINTN rlen;
            uart_set_8n1_div(base, kDivs[bi]);
            spk_stall_us(bs, 10000U);
            uart_drain_rx(base);

            uart_send_str(bs, base, "AT\r");
            rlen = uart_recv_resp(bs, base, resp, sizeof(resp), wait_ms);

            if (rlen > 0U && str_has_ok_ci(resp)) {
                char ati0[128], ati3[128];

                modem_found = TRUE;
                efi_out_str(ctx, "      Modem Query    : ");
                efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
                efi_out_str(ctx, " @ ");
                efi_out_u64(ctx, (UINT64)kBauds[bi]);
                efi_out_str(ctx, " bps (AT -> \"");
                efi_out_str(ctx, resp);
                efi_out_str(ctx, "\")\n");

                uart_drain_rx(base);
                uart_send_str(bs, base, "ATI0\r");
                if (uart_recv_resp(bs, base, ati0, sizeof(ati0), 350U) > 0U) {
                    efi_out_str(ctx, "      Modem ATI0     : \"");
                    efi_out_str(ctx, ati0);
                    efi_out_str(ctx, "\"\n");
                }

                uart_drain_rx(base);
                uart_send_str(bs, base, "ATI3\r");
                if (uart_recv_resp(bs, base, ati3, sizeof(ati3), 350U) > 0U) {
                    efi_out_str(ctx, "      Modem ATI3     : \"");
                    efi_out_str(ctx, ati3);
                    efi_out_str(ctx, "\"\n");
                }
                break;
            }
        }
    }

    if (!modem_found) {
        efi_out_str(ctx, "      Modem Query    : NO AT RESPONSE (No modem / powered off / non-AT device)\n");
    }

    spk_io_out8((UINT16)(base + 3U), (UINT8)(orig_lcr | 0x80U));
    spk_io_out8((UINT16)(base + 0U), orig_dll);
    spk_io_out8((UINT16)(base + 1U), orig_dlm);
    spk_io_out8((UINT16)(base + 3U), orig_lcr);
    spk_io_out8((UINT16)(base + 2U), 0x07U);
    spk_io_out8((UINT16)(base + 4U), orig_mcr);
    spk_io_out8((UINT16)(base + 1U), orig_ier);
}

static VOID check_com_ports(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs,
                            const port_cand_t *cands, UINTN cnt)
{
    UINTN i, found = 0U;

    efi_out_str(ctx, "\n  [4] COM / UART Hardware Probe & AT Modem Query\n");
    efilog_flush(ctx);

    for (i = 0U; i < cnt; i++) {
        const char *chip = "Unknown";
        if (!uart_probe_chip(cands[i].base, &chip)) {
            continue;
        }
        found++;
        efi_out_str(ctx, "    [COM @ 0x");
        efi_out_hex16(ctx, cands[i].base);
        efi_out_str(ctx, "] PRESENT | Chip=");
        efi_out_str(ctx, chip);
        efi_out_str(ctx, " | Source=");
        efi_out_str(ctx, cands[i].src);
        efi_out_str(ctx, "\n");

        efilog_flush(ctx);
        query_com_modem(ctx, bs, cands[i].base);
    }

    if (found == 0U) {
        efi_out_str(ctx, "    NO PHYSICAL UART DETECTED (0x3F8/0x2F8/0x3E8/0x2E8 + PCI)\n");
    }
}

/* -------------------------------------------------------------------------
 * [5] LPT 병렬 포트 검사 · 프린터 상태 · 비인쇄 IEEE 1284 Device ID 조회
 * ------------------------------------------------------------------------- */
static BOOLEAN lpt_probe_port(UINT16 base, const char **mode_out)
{
    UINT8 orig_data, orig_ctrl, stat, r1, r2;

    stat      = spk_io_in8((UINT16)(base + 1U));
    orig_ctrl = spk_io_in8((UINT16)(base + 2U));
    if (stat == 0xFFU && orig_ctrl == 0xFFU) {
        return FALSE;
    }

    orig_data = spk_io_in8(base);
    spk_io_out8((UINT16)(base + 2U), (UINT8)((orig_ctrl & ~0x21U) | 0x04U));
    spk_io_out8(base, 0x55U);
    r1 = spk_io_in8(base);
    spk_io_out8(base, 0xAAU);
    r2 = spk_io_in8(base);

    spk_io_out8(base, orig_data);
    spk_io_out8((UINT16)(base + 2U), orig_ctrl);

    if (r1 != 0x55U || r2 != 0xAAU) {
        return FALSE;
    }

    *mode_out = "SPP/PS2";
    if (base == 0x0378U || base == 0x0278U) {
        UINT16 ecr_port = (UINT16)(base + 0x0402U);
        UINT8  orig_ecr = spk_io_in8(ecr_port);
        if ((orig_ecr & 0x03U) == 0x01U) {
            spk_io_out8(ecr_port, (UINT8)((orig_ecr & 0x1FU) | 0x20U));
            if ((spk_io_in8(ecr_port) & 0xE0U) == 0x20U) {
                *mode_out = "ECP";
            }
            spk_io_out8(ecr_port, orig_ecr);
        }
    }
    if (bytes_eq((const UINT8 *)(*mode_out), "SPP", 3U)) {
        if ((spk_io_in8((UINT16)(base + 1U)) & 0x01U) != 0U) {
            *mode_out = "EPP-Capable";
        }
    }
    return TRUE;
}

static BOOLEAN lpt_wait_status(EFI_BOOT_SERVICES *bs, UINT16 base,
                               UINT8 mask, UINT8 expect, UINTN timeout_ms)
{
    UINTN t;

    for (t = 0U; t < timeout_ms * 10U; t++) {
        if ((spk_io_in8((UINT16)(base + 1U)) & mask) == expect) {
            return TRUE;
        }
        spk_stall_us(bs, 100U);
    }
    return FALSE;
}

static BOOLEAN lpt_read_nibble(EFI_BOOT_SERVICES *bs, UINT16 base, UINT8 *nib_out)
{
    UINT8 s;

    spk_io_out8((UINT16)(base + 2U), 0x06U);
    if (!lpt_wait_status(bs, base, 0x40U, 0x00U, 25U)) {
        return FALSE;
    }

    s = spk_io_in8((UINT16)(base + 1U));
    *nib_out = (UINT8)(((s >> 3) & 0x07U) | ((((s >> 7) & 0x01U) ^ 0x01U) << 3));

    spk_io_out8((UINT16)(base + 2U), 0x04U);
    if (!lpt_wait_status(bs, base, 0x40U, 0x40U, 25U)) {
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN lpt_query_ieee1284_id(EFI_BOOT_SERVICES *bs, UINT16 base,
                                     char *out, UINTN out_cap)
{
    UINT8   orig_data = spk_io_in8(base);
    UINT8   orig_ctrl = spk_io_in8((UINT16)(base + 2U));
    UINT8   lo = 0U, hi = 0U;
    UINT16  id_len = 0U;
    UINTN   i, w = 0U;
    BOOLEAN ok = FALSE;

    if (out_cap == 0U) return FALSE;
    out[0] = '\0';

    spk_io_out8((UINT16)(base + 2U), 0x0CU);
    spk_stall_us(bs, 1000U);
    spk_io_out8(base, 0x04U);
    spk_stall_us(bs, 50U);
    spk_io_out8((UINT16)(base + 2U), 0x06U);

    if (!lpt_wait_status(bs, base, 0x78U, 0x38U, 40U)) {
        goto terminate;
    }

    spk_io_out8((UINT16)(base + 2U), 0x07U);
    spk_stall_us(bs, 50U);
    spk_io_out8((UINT16)(base + 2U), 0x04U);

    if (!lpt_wait_status(bs, base, 0x40U, 0x40U, 40U)) {
        goto terminate;
    }
    if ((spk_io_in8((UINT16)(base + 1U)) & 0x10U) == 0U) {
        goto terminate;
    }

    for (i = 0U; i < 2U; i++) {
        if ((spk_io_in8((UINT16)(base + 1U)) & 0x08U) != 0U) goto terminate;
        if (!lpt_read_nibble(bs, base, &lo) || !lpt_read_nibble(bs, base, &hi)) goto terminate;
        id_len = (UINT16)((id_len << 8) | ((UINT16)(hi << 4) | lo));
    }

    if (id_len < 3U || id_len > 1024U) {
        goto terminate;
    }

    for (i = 2U; i < (UINTN)id_len; i++) {
        UINT8 ch;
        if ((spk_io_in8((UINT16)(base + 1U)) & 0x08U) != 0U) break;
        if (!lpt_read_nibble(bs, base, &lo) || !lpt_read_nibble(bs, base, &hi)) break;
        ch = (UINT8)((hi << 4) | lo);
        if (w + 1U < out_cap) {
            out[w++] = (ch >= 0x20U && ch <= 0x7EU) ? (char)ch : ' ';
        }
    }
    out[w] = '\0';
    ok = (w > 0U) ? TRUE : FALSE;

terminate:
    spk_io_out8((UINT16)(base + 2U), 0x0CU);
    spk_stall_us(bs, 1000U);
    spk_io_out8(base, orig_data);
    spk_io_out8((UINT16)(base + 2U), orig_ctrl);
    return ok;
}

static VOID check_lpt_ports(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs,
                            const port_cand_t *cands, UINTN cnt)
{
    UINTN i, found = 0U;

    efi_out_str(ctx, "\n  [5] LPT Parallel Port & Printer Status / IEEE 1284 ID\n");
    efilog_flush(ctx);

    for (i = 0U; i < cnt; i++) {
        const char *mode = "SPP";
        UINT8       st_reg;
        BOOLEAN     not_busy, ack_hi, paper_out, online, no_err;
        char        id_buf[128];

        if (!lpt_probe_port(cands[i].base, &mode)) {
            continue;
        }
        found++;

        st_reg    = spk_io_in8((UINT16)(cands[i].base + 1U));
        not_busy  = ((st_reg & 0x80U) != 0U) ? TRUE : FALSE;
        ack_hi    = ((st_reg & 0x40U) != 0U) ? TRUE : FALSE;
        paper_out = ((st_reg & 0x20U) != 0U) ? TRUE : FALSE;
        online    = ((st_reg & 0x10U) != 0U) ? TRUE : FALSE;
        no_err    = ((st_reg & 0x08U) != 0U) ? TRUE : FALSE;

        efi_out_str(ctx, "    [LPT @ 0x");
        efi_out_hex16(ctx, cands[i].base);
        efi_out_str(ctx, "] PRESENT | Mode=");
        efi_out_str(ctx, mode);
        efi_out_str(ctx, " | Source=");
        efi_out_str(ctx, cands[i].src);
        efi_out_str(ctx, "\n      Printer Status : [");
        efi_out_str(ctx, not_busy  ? "READY " : "BUSY ");
        efi_out_str(ctx, online    ? "ONLINE " : "OFFLINE ");
        efi_out_str(ctx, paper_out ? "PAPER_OUT " : "PAPER_OK ");
        efi_out_str(ctx, no_err    ? "NO_ERR " : "FAULT ");
        efi_out_str(ctx, ack_hi    ? "ACK_HI" : "ACK_LO");
        efi_out_str(ctx, "] (Status=0x");
        efi_out_hex8(ctx, st_reg);
        efi_out_str(ctx, ")\n");

        efilog_flush(ctx);
        if (lpt_query_ieee1284_id(bs, cands[i].base, id_buf, sizeof(id_buf))) {
            efi_out_str(ctx, "      IEEE 1284 ID   : ");
            efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
            efi_out_str(ctx, " \"");
            efi_out_str(ctx, id_buf);
            efi_out_str(ctx, "\"\n");
        } else {
            efi_out_str(ctx, "      IEEE 1284 ID   : NO RESPONSE (No printer / legacy non-1284 printer)\n");
        }
    }

    if (found == 0U) {
        efi_out_str(ctx, "    NO PHYSICAL LPT PORT DETECTED (0x378/0x278/0x3BC + PCI)\n");
    }
}

/* -------------------------------------------------------------------------
 * Step 7-2a 메인 엔트리 (Serial/COM/Modem & Parallel/LPT/Printer)
 * ------------------------------------------------------------------------- */
void efi_run_step7_port(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    port_cand_t com_cands[MAX_PORT_CANDS];
    port_cand_t lpt_cands[MAX_PORT_CANDS];
    UINTN       com_cnt = 0U, lpt_cnt = 0U;

    efi_out_str(ctx, "=== [Step 7-2a] Serial (COM/Modem) & Parallel (LPT/Printer) Ports ===\n");
    efilog_flush(ctx);

    if (bs == NULL) {
        efi_out_str(ctx, "  BootServices UNAVAILABLE\n\n");
        return;
    }

    add_port_cand(com_cands, &com_cnt, 0x03F8U, "ISA-COM1");
    add_port_cand(com_cands, &com_cnt, 0x02F8U, "ISA-COM2");
    add_port_cand(com_cands, &com_cnt, 0x03E8U, "ISA-COM3");
    add_port_cand(com_cands, &com_cnt, 0x02E8U, "ISA-COM4");

    add_port_cand(lpt_cands, &lpt_cnt, 0x0378U, "ISA-LPT1");
    add_port_cand(lpt_cands, &lpt_cnt, 0x0278U, "ISA-LPT2");
    add_port_cand(lpt_cands, &lpt_cnt, 0x03BCU, "ISA-LPT-MDA");

    check_uefi_serial_io(ctx, bs);
    check_acpi_spcr(ctx, com_cands, &com_cnt);
    scan_pci_comm_ports(ctx, bs, com_cands, &com_cnt, lpt_cands, &lpt_cnt);
    check_com_ports(ctx, bs, com_cands, com_cnt);
    check_lpt_ports(ctx, bs, lpt_cands, lpt_cnt);

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}