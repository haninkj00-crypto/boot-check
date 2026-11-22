/* ---------------------------------------------------------
 * Step 0.5: 부팅 경로(Legacy BIOS vs UEFI CSM) 및 펌웨어 지원 옵션 기록
 * --------------------------------------------------------- */

typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);

static void print_fixed_str(const uint8_t far *p, uint8_t len)
{
    char buf[2];
    uint8_t i;
    buf[1] = '\0';
    for (i = 0; i < len; i++) {
        uint8_t c = p[i];
        buf[0] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
        out_str(buf);
    }
}

static void print_smbios_str(const uint8_t far *str_start, uint8_t idx)
{
    const uint8_t far *p = str_start;
    char ch[2];
    uint8_t cur = 1;

    if (idx == 0) {
        out_str("N/A");
        return;
    }
    while (cur < idx) {
        while (*p != 0) p++;
        p++;
        if (*p == 0) {
            out_str("N/A");
            return;
        }
        cur++;
    }
    ch[1] = '\0';
    while (*p >= 0x20 && *p <= 0x7E) {
        ch[0] = (char)(*p++);
        out_str(ch);
    }
}

static void out_hex8(uint8_t val)
{
    const char hex[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(val >> 4) & 0x0F];
    buf[1] = hex[val & 0x0F];
    buf[2] = '\0';
    out_str(buf);
}

void probe_firmware_and_boot_path(void)
{
    uint8_t  boot_dl = *(uint8_t far *)MK_FP(0x0000, 0x7C40);
    uint32_t hidd_sec = *(uint32_t far *)MK_FP(0x0000, 0x7C1C);
    uint16_t ebda_seg = *(uint16_t far *)MK_FP(0x0040, 0x000E);
    uint16_t seg, off;

    uint8_t  acpi_found = 0, acpi_rev = 0;
    uint8_t  far *acpi_ptr = 0;

    uint8_t  smbios_found = 0, sm_maj = 0, sm_min = 0;
    uint8_t  uefi_csm_bit = 0, vm_bit = 0, type0_parsed = 0;
    uint8_t  far *type0_ptr = 0;
    uint8_t  far *type0_strs = 0;

    uint8_t  pnp_found = 0;
    uint8_t  apm_ok = 0, apm_maj = 0, apm_min = 0;
    uint8_t  pci_bios_ok = 0, pci_maj = 0, pci_min = 0, pci_mech = 0;
    uint8_t  edd_ok = 0, edd_ver = 0;
    uint16_t edd_sub = 0;
    uint8_t  sys_model = 0, sys_sub = 0, sys_feat1 = 0, sys_cfg_ok = 0;

    out_str("=== [Step 0.5] Boot Path & Firmware Option Capabilities ===\n");

    /* 1. 부팅 진입 경로 및 디스크 레이아웃(Superfloppy vs MBR) 판별 */
    out_str("  1. Execution Mode : 16-bit Real Mode (Legacy INT 19h -> 0x7C00)\n");
    out_str("  2. Boot Drive DL  : 0x"); out_hex16(boot_dl);
    if (boot_dl < 0x80U)       out_str(" (Floppy / USB-FDD Emulation)\n");
    else if (boot_dl == 0x80U) out_str(" (Primary HDD / USB-HDD Emulation)\n");
    else                       out_str(" (Secondary HDD / El Torito CD)\n");

    out_str("  3. Volume Layout  : ");
    if (hidd_sec == 0UL) {
        out_str("FAT32 Superfloppy (VBR at LBA 0, BPB_HiddSec=0)\n");
    } else {
        out_str("MBR Partitioned FAT32 (Partition Start LBA=");
        out_ulong(hidd_sec);
        out_str(")\n");
    }

    /* 2. ACPI RSDP ("RSD PTR ") 탐색: EBDA 첫 1KB 및 E000:0000~FFFF:000F */
    if (ebda_seg >= 0x8000U && ebda_seg < 0xA000U) {
        for (off = 0; off < 0x0400U; off += 16U) {
            uint8_t far *p = (uint8_t far *)MK_FP(ebda_seg, off);
            if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
                p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
                acpi_found = 1; acpi_ptr = p; acpi_rev = p[15];
                break;
            }
        }
    }
    if (!acpi_found) {
        for (seg = 0xE000U; seg != 0x0000U; seg += 0x1000U) {
            for (off = 0; off < 0xFFF0U; off += 16U) {
                uint8_t far *p = (uint8_t far *)MK_FP(seg, off);
                if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
                    p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
                    acpi_found = 1; acpi_ptr = p; acpi_rev = p[15];
                    break;
                }
            }
            if (acpi_found) break;
        }
    }

    /* 3. F000:0000 ~ F000:FFF0 에서 SMBIOS("_SM_") 및 PnP BIOS("$PnP") 탐색 */
    for (off = 0; off < 0xFFF0U; off += 16U) {
        uint8_t far *p = (uint8_t far *)MK_FP(0xF000U, off);
        if (!smbios_found && p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '_') {
            uint32_t tbl_phys = *(uint32_t far *)(p + 0x18U);
            smbios_found = 1;
            sm_maj = p[6];
            sm_min = p[7];
            /* 테이블이 1MB 이하(예: 0xE0000~0xFFFFF)에 있으면 Type 0 직접 파싱 */
            if (tbl_phys >= 0x000E0000UL && tbl_phys < 0x000FFF00UL) {
                uint8_t far *t0 = (uint8_t far *)MK_FP((uint16_t)(tbl_phys >> 4),
                                                       (uint16_t)(tbl_phys & 0x0FU));
                if (t0[0] == 0 && t0[1] >= 0x12U) {
                    type0_parsed = 1;
                    type0_ptr    = t0;
                    type0_strs   = t0 + t0[1];
                    if (t0[1] >= 0x14U) {
                        if ((t0[0x13] & 0x08U) != 0) uefi_csm_bit = 1; /* Bit 3: UEFI */
                        if ((t0[0x13] & 0x10U) != 0) vm_bit = 1;       /* Bit 4: VM */
                    }
                }
            }
        }
        if (!pnp_found && p[0] == '$' && p[1] == 'P' && p[2] == 'n' && p[3] == 'P') {
            pnp_found = 1;
        }
    }

    /* 4. INT 15h AH=C0h (System Configuration Descriptor) */
    {
        uint16_t r_ax, r_bx, r_dx, r_fl;
        _AH = 0xC0;
        __emit__(0x55, 0x06, 0xCD, 0x15, 0x8C, 0xC2, 0x07, 0x5D); /* ES -> DX */
        r_ax = _AX;
        r_bx = _BX;
        r_dx = _DX;
        r_fl = _FLAGS;
        if ((r_fl & 0x0001U) == 0 && (r_ax >> 8) == 0) {
            uint8_t far *cfg = (uint8_t far *)MK_FP(r_dx, r_bx);
            sys_cfg_ok = 1;
            sys_model  = cfg[2];
            sys_sub    = cfg[3];
            sys_feat1  = cfg[5];
        }
    }

    /* 5. APM (INT 15h AX=5300h) */
    {
        uint16_t r_ax, r_bx, r_fl;
        _BX = 0x0000;
        _AX = 0x5300;
        __emit__(0x55, 0xCD, 0x15, 0x5D);
        r_ax = _AX;
        r_bx = _BX;
        r_fl = _FLAGS;
        if ((r_fl & 0x0001U) == 0 && r_bx == 0x504DU) {
            apm_ok  = 1;
            apm_maj = (uint8_t)(r_ax >> 8);
            apm_min = (uint8_t)(r_ax & 0xFFU);
        }
    }

    /* 6. PCI BIOS 2.x (INT 1Ah AX=B101h) */
    {
        uint16_t r_ax, r_bx, r_dx;
        _DI = 0x0000;
        _AX = 0xB101;
        __emit__(0x55, 0xCD, 0x1A, 0x5D);
        r_ax = _AX;
        r_bx = _BX;
        r_dx = _DX;
        if ((r_ax >> 8) == 0 && r_dx == 0x4350U) {
            pci_bios_ok = 1;
            pci_mech    = (uint8_t)(r_ax & 0xFFU);
            pci_maj     = (uint8_t)(r_bx >> 8);
            pci_min     = (uint8_t)(r_bx & 0xFFU);
        }
    }

    /* 7. INT 13h EDD 확장 지원 여부 (AH=41h, BX=55AAh) */
    {
        uint16_t r_ax, r_bx, r_cx, r_fl;
        _DL = boot_dl;
        _BX = 0x55AAU;
        _AH = 0x41;
        __emit__(0x55, 0xCD, 0x13, 0x5D);
        r_ax = _AX;
        r_bx = _BX;
        r_cx = _CX;
        r_fl = _FLAGS;
        if ((r_fl & 0x0001U) == 0 && r_bx == 0xAA55U) {
            edd_ok  = 1;
            edd_ver = (uint8_t)(r_ax >> 8);
            edd_sub = r_cx;
        }
    }

    /* 결과 출력 */
    out_str("  4. Firmware Type  : ");
    if (uefi_csm_bit) {
        out_str("UEFI Firmware running in CSM / Legacy BIOS Mode");
    } else if (acpi_found && acpi_rev >= 2) {
        out_str("Modern BIOS / UEFI-CSM (ACPI 2.0+ XSDT present)");
    } else {
        out_str("Legacy BIOS");
    }
    if (vm_bit) out_str(" [Virtual Machine Flag=1]");
    out_str("\n");

    if (type0_parsed) {
        out_str("  5. BIOS Identity  : Vendor=\"");
        print_smbios_str(type0_strs, type0_ptr[4]);
        out_str("\", Ver=\"");
        print_smbios_str(type0_strs, type0_ptr[5]);
        out_str("\", Date=\"");
        print_smbios_str(type0_strs, type0_ptr[8]);
        out_str("\"\n");
    } else {
        out_str("  5. BIOS ROM Date  : ");
        print_fixed_str((const uint8_t far *)MK_FP(0xF000U, 0xFFF5U), 8);
        out_str("\n");
    }

    out_str("  6. Firmware Tables: ");
    out_str("ACPI=");
    if (acpi_found) {
        out_str(acpi_rev >= 2 ? "2.0+(" : "1.0(");
        print_fixed_str(acpi_ptr + 9, 6);
        out_str(") ");
    } else {
        out_str("NO ");
    }
    out_str("SMBIOS=");
    if (smbios_found) {
        out_ulong(sm_maj); out_str("."); out_ulong(sm_min); out_str(" ");
    } else {
        out_str("NO ");
    }
    out_str("PnP="); out_str(pnp_found ? "YES\n" : "NO\n");

    out_str("  7. BIOS Services  : ");
    out_str("PCI_BIOS=");
    if (pci_bios_ok) {
        out_ulong(pci_maj); out_str("."); out_hex8(pci_min);
        out_str("(Mech=0x"); out_hex8(pci_mech); out_str(") ");
    } else {
        out_str("NO ");
    }
    out_str("APM=");
    if (apm_ok) {
        out_ulong(apm_maj); out_str("."); out_hex8(apm_min); out_str(" ");
    } else {
        out_str("NO ");
    }
    out_str("EDD=");
    if (edd_ok) {
        out_ulong(edd_ver >> 4); out_str("."); out_ulong(edd_ver & 0x0F);
        out_str("(Sub=0x"); out_hex16(edd_sub); out_str(")\n");
    } else {
        out_str("NO\n");
    }

    if (sys_cfg_ok) {
        out_str("  8. Sys Desc (C0h) : Model=0x"); out_hex16(sys_model);
        out_str(", Sub=0x"); out_hex16(sys_sub);
        out_str(", Feat1=0x"); out_hex16(sys_feat1);
        if (sys_feat1 & 0x40U) out_str(" [2nd-8259-PIC]");
        if (sys_feat1 & 0x10U) out_str(" [RTC-Present]");
        if (sys_feat1 & 0x04U) out_str(" [EBDA-Allocated]");
        out_str("\n\n");
    } else {
        out_str("\n");
    }
}