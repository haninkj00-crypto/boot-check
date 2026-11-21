#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#ifndef EFI_OPEN_PROTOCOL_BY_DRIVER
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x00000010U
#endif

#define PCI_MAX_SORT_HANDLES 256U

/* -------------------------------------------------------------------------
 * UEFI 2.11 EFI_PCI_IO_PROTOCOL 표준 GUID 및 VTable 레이아웃
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidPciIo =
    { 0x4CF5B200, 0x68B8, 0x4CA5, { 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A } };

typedef enum {
    BC_PCI_WIDTH_UINT8  = 0,
    BC_PCI_WIDTH_UINT16 = 1,
    BC_PCI_WIDTH_UINT32 = 2,
    BC_PCI_WIDTH_UINT64 = 3
} bc_pci_width_t;

typedef struct _BC_PCI_IO bc_pci_io_t;

typedef EFI_STATUS (EFIAPI *BC_PCI_IO_CONFIG_FN)(
    bc_pci_io_t    *This,
    bc_pci_width_t  Width,
    UINT32          Offset,
    UINTN           Count,
    VOID           *Buffer
);

typedef struct {
    BC_PCI_IO_CONFIG_FN Read;
    BC_PCI_IO_CONFIG_FN Write;
} bc_pci_io_config_t;

typedef EFI_STATUS (EFIAPI *BC_PCI_GET_LOCATION_FN)(
    bc_pci_io_t *This,
    UINTN       *SegmentNumber,
    UINTN       *BusNumber,
    UINTN       *DeviceNumber,
    UINTN       *FunctionNumber
);

typedef EFI_STATUS (EFIAPI *BC_PCI_GET_BAR_ATTR_FN)(
    bc_pci_io_t *This,
    UINT8        BarIndex,
    UINT64      *Supports,
    VOID       **Resources
);

struct _BC_PCI_IO {
    VOID                   *PollMem;
    VOID                   *PollIo;
    VOID                   *MemRead;
    VOID                   *MemWrite;
    VOID                   *IoRead;
    VOID                   *IoWrite;
    bc_pci_io_config_t      Pci;
    VOID                   *CopyMem;
    VOID                   *Map;
    VOID                   *Unmap;
    VOID                   *AllocateBuffer;
    VOID                   *FreeBuffer;
    VOID                   *Flush;
    BC_PCI_GET_LOCATION_FN  GetLocation;
    VOID                   *Attributes;
    BC_PCI_GET_BAR_ATTR_FN  GetBarAttributes;
    VOID                   *SetBarAttributes;
    UINT64                  RomSize;
    VOID                   *RomImage;
};

/* -------------------------------------------------------------------------
 * 내부 유틸리티 (리틀엔디안 바이트 읽기, Config Space 읽기 전용 접근)
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

static BOOLEAN pci_cfg_r8(bc_pci_io_t *pci, UINT32 off, UINT8 *out)
{
    EFI_STATUS st;

    *out = 0xFFU;
    st = uefi_call_wrapper(pci->Pci.Read, 5, pci, BC_PCI_WIDTH_UINT8, off, (UINTN)1, out);
    return (!EFI_ERROR(st)) ? TRUE : FALSE;
}

static BOOLEAN pci_cfg_r16(bc_pci_io_t *pci, UINT32 off, UINT16 *out)
{
    EFI_STATUS st;

    *out = 0xFFFFU;
    st = uefi_call_wrapper(pci->Pci.Read, 5, pci, BC_PCI_WIDTH_UINT16, off, (UINTN)1, out);
    return (!EFI_ERROR(st)) ? TRUE : FALSE;
}

static BOOLEAN pci_cfg_r32(bc_pci_io_t *pci, UINT32 off, UINT32 *out)
{
    EFI_STATUS st;

    *out = 0xFFFFFFFFU;
    st = uefi_call_wrapper(pci->Pci.Read, 5, pci, BC_PCI_WIDTH_UINT32, off, (UINTN)1, out);
    return (!EFI_ERROR(st)) ? TRUE : FALSE;
}

/* -------------------------------------------------------------------------
 * 드라이버 바인딩 여부 조회 (BY_DRIVER 오픈 여부 확인)
 * ------------------------------------------------------------------------- */
static BOOLEAN pci_has_bound_driver(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN      cnt = 0;
    UINTN      i;
    BOOLEAN    bound = FALSE;
    EFI_STATUS st;

    if (bs == NULL || bs->OpenProtocolInformation == NULL) {
        return FALSE;
    }

    st = uefi_call_wrapper(bs->OpenProtocolInformation, 4,
                           handle, &kGuidPciIo, &entries, &cnt);
    if (EFI_ERROR(st) || entries == NULL) {
        return FALSE;
    }

    for (i = 0; i < cnt; i++) {
        if ((entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0U) {
            bound = TRUE;
            break;
        }
    }

    uefi_call_wrapper(bs->FreePool, 1, entries);
    return bound;
}

/* -------------------------------------------------------------------------
 * 클래스 / PCIe 속도 / SBDF 출력 헬퍼
 * ------------------------------------------------------------------------- */
static const char *pci_class_label(UINT8 cls, UINT8 sub, UINT8 pif)
{
    switch (cls) {
        case 0x00: return "Legacy";
        case 0x01:
            if (sub == 0x01) return "Storage/IDE";
            if (sub == 0x04) return "Storage/RAID";
            if (sub == 0x06) return "Storage/SATA";
            if (sub == 0x08) return "Storage/NVMe";
            return "Storage";
        case 0x02:
            if (sub == 0x00) return "Net/Ethernet";
            if (sub == 0x80) return "Net/Wireless";
            return "Network";
        case 0x03:
            if (sub == 0x00) return "Display/VGA";
            if (sub == 0x02) return "Display/3D";
            return "Display";
        case 0x04:
          if (sub == 0x00) return "Multimedia/Video";
          if (sub == 0x01) return "Audio/MM";
          if (sub == 0x03) return "Audio/HDA-Dev";
          return "Multimedia";
        case 0x05: return "Memory";
        case 0x06:
            if (sub == 0x00) return "Bridge/Host";
            if (sub == 0x01) return "Bridge/ISA";
            if (sub == 0x04) return "Bridge/PCI-PCI";
            return "Bridge";
        case 0x07:
            if (sub == 0x00) return "Comm/Serial";
            if (sub == 0x01) return "Comm/Parallel";
            return "Comm";
        case 0x08: return "System";
        case 0x0C:
            if (sub == 0x03) {
                if (pif == 0x00) return "USB/UHCI";
                if (pif == 0x10) return "USB/OHCI";
                if (pif == 0x20) return "USB/EHCI";
                if (pif == 0x30) return "USB/xHCI";
                return "USB/Other";
            }
            if (sub == 0x05) return "SMBus";
            return "SerialBus";
        case 0x10: return "Crypto/PSP";
        case 0x13: return "Non-Essent/Inst";
        default:   return "Device";
    }
}

static const char *pcie_speed_str(UINT8 spd)
{
    switch (spd) {
        case 1: return "2.5GT/s(G1)";
        case 2: return "5.0GT/s(G2)";
        case 3: return "8.0GT/s(G3)";
        case 4: return "16GT/s(G4)";
        case 5: return "32GT/s(G5)";
        case 6: return "64GT/s(G6)";
        default: return "?";
    }
}

static VOID out_sbdf(efi_diag_ctx_t *ctx, UINTN seg, UINTN bus, UINTN dev, UINTN func)
{
    efi_out_hex16(ctx, (UINT16)seg);
    efi_out_str(ctx, ":");
    efi_out_hex8(ctx, (UINT8)bus);
    efi_out_str(ctx, ":");
    efi_out_hex8(ctx, (UINT8)dev);
    efi_out_str(ctx, ".");
    efi_out_u64(ctx, (UINT64)(func & 0x07U));
}

/* -------------------------------------------------------------------------
 * PCIe Capability (ID 0x10) 탐색 -> Max/Current Link Speed & Width
 * ------------------------------------------------------------------------- */
typedef struct {
    BOOLEAN found;
    BOOLEAN has_link;
    UINT8   max_spd;
    UINT8   max_wid;
    UINT8   cur_spd;
    UINT8   cur_wid;
} pcie_link_info_t;

static VOID probe_pcie_link(bc_pci_io_t *pci, UINT8 hdr_layout, pcie_link_info_t *out)
{
    UINT16 status = 0;
    UINT8  cap_ptr = 0;
    UINTN  guard = 0;

    SetMem(out, sizeof(*out), 0);

    if (hdr_layout > 1U) {
        return;
    }
    if (!pci_cfg_r16(pci, 0x06U, &status) || (status & 0x0010U) == 0U) {
        return;
    }
    if (!pci_cfg_r8(pci, 0x34U, &cap_ptr)) {
        return;
    }

    cap_ptr &= 0xFCU;
    while (cap_ptr >= 0x40U && guard++ < 48U) {
        UINT8 cap_id = 0, next_ptr = 0;

        if (!pci_cfg_r8(pci, (UINT32)cap_ptr, &cap_id) || cap_id == 0xFFU) {
            break;
        }
        if (!pci_cfg_r8(pci, (UINT32)cap_ptr + 1U, &next_ptr)) {
            break;
        }

        if (cap_id == 0x10U) {
            UINT32 lnkcap = 0;
            UINT16 lnksta = 0;

            out->found = TRUE;
            if (pci_cfg_r32(pci, (UINT32)cap_ptr + 0x0CU, &lnkcap) &&
                pci_cfg_r16(pci, (UINT32)cap_ptr + 0x12U, &lnksta)) {
                out->max_spd = (UINT8)(lnkcap & 0x0FU);
                out->max_wid = (UINT8)((lnkcap >> 4) & 0x3FU);
                out->cur_spd = (UINT8)(lnksta & 0x0FU);
                out->cur_wid = (UINT8)((lnksta >> 4) & 0x3FU);
                if (out->max_spd > 0U && out->max_wid > 0U) {
                    out->has_link = TRUE;
                }
            }
            return;
        }

        next_ptr &= 0xFCU;
        if (next_ptr == cap_ptr || next_ptr < 0x40U) {
            break;
        }
        cap_ptr = next_ptr;
    }
}

/* -------------------------------------------------------------------------
 * BAR 조사 (1순위: GetBarAttributes ACPI QWORD 0x8A / 2순위: Config 읽기 전용)
 * ------------------------------------------------------------------------- */
typedef struct {
    BOOLEAN bar32_above4g;
    BOOLEAN has_64bit_bar;
    BOOLEAN mmio_above4g;
    UINTN   above4g_bar_cnt;
    UINTN   above4g_dev_cnt;
} pci_mmio_stats_t;

typedef struct {
    UINTN   seg, bus, dev, func;
    UINT16  vid, did;
    UINT8   sub, pif, rev;
    BOOLEAN bound;
} pci_audio_rec_t;

#define MAX_AUDIO_RECS 16U

static BOOLEAN bar_exceeds_4g(UINT64 base, UINT64 len)
{
    if (base >= 0x100000000ULL) {
        return TRUE;
    }
    if (len > 0ULL) {
        UINT64 end = base + len;
        if (end < base || end > 0x100000000ULL) {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID inspect_device_bars(efi_diag_ctx_t *ctx,
                                EFI_BOOT_SERVICES *bs,
                                bc_pci_io_t *pci,
                                UINT8 hdr_layout,
                                pci_mmio_stats_t *stats)
{
    UINT8   max_bars = (hdr_layout == 0U) ? 6U : ((hdr_layout == 1U) ? 2U : 1U);
    UINT8   b;
    BOOLEAN dev_has_above4g = FALSE;

    for (b = 0; b < max_bars; b++) {
        UINT32  raw_lo = 0, raw_hi = 0;
        BOOLEAN raw_ok;
        BOOLEAN raw_is_io = FALSE;
        BOOLEAN raw_is_64 = FALSE;
        BOOLEAN raw_pref  = FALSE;
        VOID   *res_void  = NULL;
        EFI_STATUS st;

        raw_ok = pci_cfg_r32(pci, 0x10U + ((UINT32)b * 4U), &raw_lo);
        if (raw_ok && raw_lo != 0xFFFFFFFFU && raw_lo != 0U) {
            if ((raw_lo & 0x01U) != 0U) {
                raw_is_io = TRUE;
            } else {
                raw_is_64 = (((raw_lo >> 1) & 0x03U) == 0x02U) ? TRUE : FALSE;
                raw_pref  = ((raw_lo & 0x08U) != 0U) ? TRUE : FALSE;
                if (raw_is_64 && (UINT8)(b + 1U) < max_bars) {
                    pci_cfg_r32(pci, 0x10U + ((UINT32)(b + 1U) * 4U), &raw_hi);
                }
            }
        }

        /* 1순위: GetBarAttributes() -> ACPI QWORD Address Space Descriptor (0x8A) */
        st = uefi_call_wrapper(pci->GetBarAttributes, 4, pci, b, NULL, &res_void);
        if (!EFI_ERROR(st) && res_void != NULL) {
            const UINT8 *res = (const UINT8 *)res_void;

            if (res[0] == 0x8AU) {
                UINT16 dlen = rd_u16(res + 1);
                if (dlen >= 43U && res[3U + (UINTN)dlen] == 0x79U) {
                    UINT8  rtype = res[3];
                    UINT8  sflag = res[5];
                    UINT64 gran  = rd_u64(res + 6);
                    UINT64 amin  = rd_u64(res + 14);
                    UINT64 alen  = rd_u64(res + 38);

                    if (alen > 0ULL && (rtype == 0U || rtype == 1U)) {
                        BOOLEAN is_64 = (gran == 64ULL || raw_is_64) ? TRUE : FALSE;
                        BOOLEAN pref  = ((sflag & 0x06U) == 0x06U || raw_pref) ? TRUE : FALSE;
                        BOOLEAN abv4  = (rtype == 0U) ? bar_exceeds_4g(amin, alen) : FALSE;

                        efi_out_str(ctx, "      BAR");
                        efi_out_u64(ctx, (UINT64)b);
                        efi_out_str(ctx, ": ");

                        if (rtype == 1U) {
                            efi_out_str(ctx, "I/O  0x");
                            efi_out_hex16(ctx, (UINT16)amin);
                        } else {
                            efi_out_str(ctx, is_64 ? "MMIO64 0x" : "MMIO32 0x");
                            if (is_64 || abv4) {
                                efi_out_hex64(ctx, amin);
                            } else {
                                efi_out_hex32(ctx, (UINT32)amin);
                            }
                            if (is_64) {
                                stats->has_64bit_bar = TRUE;
                            }
                            if (abv4) {
                                stats->mmio_above4g = TRUE;
                                stats->above4g_bar_cnt++;
                                dev_has_above4g = TRUE;
                                if (!is_64) {
                                    stats->bar32_above4g = TRUE;
                                }
                            }
                        }

                        efi_out_str(ctx, " [size=");
                        efi_out_size_human(ctx, alen);
                        if (rtype == 0U && pref) {
                            efi_out_str(ctx, ", pref");
                        }
                        if (abv4) {
                            efi_out_str(ctx, ", >4GB");
                        }
                        efi_out_str(ctx, "]\n");
                    }
                }
            }
            /* GetBarAttributes() 반환 버퍼 반드시 해제 */
            uefi_call_wrapper(bs->FreePool, 1, res_void);
        } else if (raw_ok && raw_lo != 0U && raw_lo != 0xFFFFFFFFU) {
            /* 2순위 폴백: Config Space BAR 읽기 전용 판독 (0xFFFFFFFF 쓰기 금지) */
            if (raw_is_io) {
                UINT32 io_base = raw_lo & ~0x03U;
                if (io_base != 0U) {
                    efi_out_str(ctx, "      BAR");
                    efi_out_u64(ctx, (UINT64)b);
                    efi_out_str(ctx, ": I/O  0x");
                    efi_out_hex16(ctx, (UINT16)io_base);
                    efi_out_str(ctx, " [cfg-ro]\n");
                }
            } else {
                UINT64 mbase = (UINT64)(raw_lo & ~0x0FU);
                if (raw_is_64) {
                    mbase |= ((UINT64)raw_hi << 32);
                    stats->has_64bit_bar = TRUE;
                }
                if (mbase != 0ULL) {
                    BOOLEAN abv4 = (mbase >= 0x100000000ULL) ? TRUE : FALSE;
                    if (abv4) {
                        stats->mmio_above4g = TRUE;
                        stats->above4g_bar_cnt++;
                        dev_has_above4g = TRUE;
                    }
                    efi_out_str(ctx, "      BAR");
                    efi_out_u64(ctx, (UINT64)b);
                    efi_out_str(ctx, raw_is_64 ? ": MMIO64 0x" : ": MMIO32 0x");
                    if (raw_is_64 || abv4) {
                        efi_out_hex64(ctx, mbase);
                    } else {
                        efi_out_hex32(ctx, (UINT32)mbase);
                    }
                    efi_out_str(ctx, raw_pref ? " [cfg-ro, pref" : " [cfg-ro");
                    if (abv4) {
                        efi_out_str(ctx, ", >4GB");
                    }
                    efi_out_str(ctx, "]\n");
                }
            }
        }

        /* 64비트 BAR의 상위 32비트 슬롯(b+1)은 별도 BAR로 해석하지 않고 건너뜀 */
        if (raw_is_64 && (UINT8)(b + 1U) < max_bars) {
            b++;
        }
    }

    if (dev_has_above4g) {
        stats->above4g_dev_cnt++;
    }
}

/* -------------------------------------------------------------------------
 * SBDF 순 정렬 (lspci 출력과 대조 용이, 256개 이하 시 스택 키 배열 사용)
 * ------------------------------------------------------------------------- */
static VOID sort_pci_handles(EFI_BOOT_SERVICES *bs, EFI_HANDLE *handles, UINTN cnt)
{
    UINT32 keys[PCI_MAX_SORT_HANDLES];
    UINTN  i, j;

    if (cnt < 2U || cnt > PCI_MAX_SORT_HANDLES) {
        return;
    }

    for (i = 0; i < cnt; i++) {
        bc_pci_io_t *pci = NULL;
        UINTN seg = 0xFF, bus = 0xFF, dev = 0x1F, func = 0x07;
        EFI_STATUS st = uefi_call_wrapper(bs->HandleProtocol, 3,
                                          handles[i], &kGuidPciIo, (VOID **)&pci);
        if (!EFI_ERROR(st) && pci != NULL) {
            uefi_call_wrapper(pci->GetLocation, 5, pci, &seg, &bus, &dev, &func);
        }
        keys[i] = ((UINT32)(seg & 0xFFU) << 16)
                | ((UINT32)(bus & 0xFFU) << 8)
                | ((UINT32)(dev & 0x1FU) << 3)
                | (UINT32)(func & 0x07U);
    }

    for (i = 1; i < cnt; i++) {
        UINT32     k = keys[i];
        EFI_HANDLE h = handles[i];
        j = i;
        while (j > 0 && keys[j - 1U] > k) {
            keys[j]    = keys[j - 1U];
            handles[j] = handles[j - 1U];
            j--;
        }
        keys[j]    = k;
        handles[j] = h;
    }
}

/* -------------------------------------------------------------------------
 * Step 3 메인 엔트리: PCI/PCIe 전체 인벤토리 · BAR · 4GB MMIO · Audio/USB 요약
 * ------------------------------------------------------------------------- */
void efi_run_step3_pci_and_audio(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    EFI_HANDLE        *handles = NULL;
    UINTN              cnt = 0;
    UINTN              i;
    EFI_STATUS         st;

    pci_mmio_stats_t   mmio;
    pci_audio_rec_t    audios[MAX_AUDIO_RECS];
    UINTN              audio_cnt = 0;
    UINTN              bound_cnt = 0;
    UINTN              bridge_cnt = 0;
    UINTN              rom_cnt = 0;
    UINTN              usb_uhci = 0, usb_ohci = 0, usb_ehci = 0, usb_xhci = 0, usb_other = 0;

    SetMem(&mmio,  sizeof(mmio),  0);
    SetMem(audios, sizeof(audios), 0);

    efi_out_str(ctx, "=== [Step 3] PCI/PCIe Inventory, BARs & Audio Controllers ===\n");
    efilog_flush(ctx);

    if (bs == NULL || bs->LocateHandleBuffer == NULL) {
        efi_out_str(ctx, "  LocateHandleBuffer() : NOT EXPOSED\n\n");
        efilog_flush(ctx);
        return;
    }

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidPciIo, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0) {
        efi_out_str(ctx, "  EFI_PCI_IO_PROTOCOL : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n\n");
        efilog_flush(ctx);
        return;
    }

    sort_pci_handles(bs, handles, cnt);

    efi_out_str(ctx, "  [1] PCI/PCIe Device Enumeration (PHASE A: Initial, Total=");
    efi_out_u64(ctx, (UINT64)cnt);
    efi_out_str(ctx, ")\n");

    for (i = 0; i < cnt; i++) {
        bc_pci_io_t     *pci = NULL;
        UINTN            seg = 0, bus = 0, dev = 0, func = 0;
        UINT32           id_reg = 0, cls_reg = 0;
        UINT16           vid, did;
        UINT8            rev, pif, sub, cls, hdr_raw = 0, hdr_layout;
        BOOLEAN          bound;
        pcie_link_info_t pcie;

        st = uefi_call_wrapper(bs->HandleProtocol, 3,
                               handles[i], &kGuidPciIo, (VOID **)&pci);
        if (EFI_ERROR(st) || pci == NULL) {
            continue;
        }
        if (EFI_ERROR(uefi_call_wrapper(pci->GetLocation, 5,
                                        pci, &seg, &bus, &dev, &func))) {
            continue;
        }

        if (!pci_cfg_r32(pci, 0x00U, &id_reg) || id_reg == 0xFFFFFFFFU) {
            continue;
        }
        vid = (UINT16)(id_reg & 0xFFFFU);
        did = (UINT16)(id_reg >> 16);

        pci_cfg_r32(pci, 0x08U, &cls_reg);
        rev = (UINT8)(cls_reg & 0xFFU);
        pif = (UINT8)((cls_reg >> 8) & 0xFFU);
        sub = (UINT8)((cls_reg >> 16) & 0xFFU);
        cls = (UINT8)((cls_reg >> 24) & 0xFFU);

        pci_cfg_r8(pci, 0x0EU, &hdr_raw);
        hdr_layout = (UINT8)(hdr_raw & 0x7FU);

        bound = pci_has_bound_driver(bs, handles[i]);
        if (bound) {
            bound_cnt++;
        }

        /* 헤더 라인 출력 */
        efi_out_str(ctx, "    ");
        out_sbdf(ctx, seg, bus, dev, func);
        efi_out_str(ctx, " ");
        efi_out_hex16(ctx, vid);
        efi_out_str(ctx, ":");
        efi_out_hex16(ctx, did);
        efi_out_str(ctx, " [");
        efi_out_hex8(ctx, cls);
        efi_out_hex8(ctx, sub);
        efi_out_str(ctx, "/");
        efi_out_hex8(ctx, pif);
        efi_out_str(ctx, " r");
        efi_out_hex8(ctx, rev);
        efi_out_str(ctx, "] ");
        efi_out_str(ctx, pci_class_label(cls, sub, pif));
        efi_out_str(ctx, " | ");
        efi_out_str(ctx, bound ? efi_diag_state_str(DIAG_DRIVER_BOUND)
                               : efi_diag_state_str(DIAG_PCI_FOUND));

        /* PCI-to-PCI 브리지 버스 범위 (HeaderType 1) */
        if (hdr_layout == 1U) {
            UINT32 bus_reg = 0;
            bridge_cnt++;
            if (pci_cfg_r32(pci, 0x18U, &bus_reg)) {
                UINT8 pri = (UINT8)(bus_reg & 0xFFU);
                UINT8 sec = (UINT8)((bus_reg >> 8) & 0xFFU);
                UINT8 subb = (UINT8)((bus_reg >> 16) & 0xFFU);
                efi_out_str(ctx, " | Bus ");
                efi_out_hex8(ctx, pri);
                efi_out_str(ctx, "->[");
                efi_out_hex8(ctx, sec);
                efi_out_str(ctx, "..");
                efi_out_hex8(ctx, subb);
                efi_out_str(ctx, "]");
            }
        }

        /* PCIe 링크 속도/폭 */
        probe_pcie_link(pci, hdr_layout, &pcie);
        if (pcie.found && pcie.has_link) {
          if (pcie.cur_wid == 0U) {
                efi_out_str(ctx, " | PCIe LinkDown (max ");
                efi_out_str(ctx, pcie_speed_str(pcie.max_spd));
                efi_out_str(ctx, " x");
                efi_out_u64(ctx, (UINT64)pcie.max_wid);
                efi_out_str(ctx, ")");
            } else {
                efi_out_str(ctx, " | PCIe ");
                efi_out_str(ctx, pcie_speed_str(pcie.cur_spd));
                efi_out_str(ctx, " x");
                efi_out_u64(ctx, (UINT64)pcie.cur_wid);
                if (pcie.cur_spd != pcie.max_spd || pcie.cur_wid != pcie.max_wid) {
                    efi_out_str(ctx, " (max ");
                    efi_out_str(ctx, pcie_speed_str(pcie.max_spd));
                    efi_out_str(ctx, " x");
                    efi_out_u64(ctx, (UINT64)pcie.max_wid);
                    efi_out_str(ctx, ")");
                }
            }
        }

        /* Option ROM 이미지 노출 여부 */
        if (pci->RomSize > 0ULL && pci->RomImage != NULL) {
            rom_cnt++;
            efi_out_str(ctx, " | OptROM=");
            efi_out_size_human(ctx, pci->RomSize);
        }
        efi_out_str(ctx, "\n");

        /* BAR 자원 출력 및 4GB 경계 통계 수집 */
        inspect_device_bars(ctx, bs, pci, hdr_layout, &mmio);

        /* Audio 컨트롤러 별도 기록 (04/01, 04/03 모두 수집) */
        if (cls == 0x04U && (sub == 0x01U || sub == 0x03U) && audio_cnt < MAX_AUDIO_RECS) {
            audios[audio_cnt].seg   = seg;
            audios[audio_cnt].bus   = bus;
            audios[audio_cnt].dev   = dev;
            audios[audio_cnt].func  = func;
            audios[audio_cnt].vid   = vid;
            audios[audio_cnt].did   = did;
            audios[audio_cnt].sub   = sub;
            audios[audio_cnt].pif   = pif;
            audios[audio_cnt].rev   = rev;
            audios[audio_cnt].bound = bound;
            audio_cnt++;
        }

        /* USB 호스트 컨트롤러 ProgIF 분류 집계 (0C/03) */
        if (cls == 0x0CU && sub == 0x03U) {
            if      (pif == 0x00U) usb_uhci++;
            else if (pif == 0x10U) usb_ohci++;
            else if (pif == 0x20U) usb_ehci++;
            else if (pif == 0x30U) usb_xhci++;
            else                   usb_other++;
        }
    }

    uefi_call_wrapper(bs->FreePool, 1, handles);
    efilog_flush(ctx);

    /* [2] PCI MMIO 4GB 경계 호환성 요약 */
    efi_out_str(ctx, "\n  [2] PCI MMIO Compatibility (<4GB vs >=4GB)\n");
    efi_out_str(ctx, "    Total PCI Funcs      : ");
    efi_out_u64(ctx, (UINT64)cnt);
    efi_out_str(ctx, " (DriverBound=");
    efi_out_u64(ctx, (UINT64)bound_cnt);
    efi_out_str(ctx, ", Bridges=");
    efi_out_u64(ctx, (UINT64)bridge_cnt);
    efi_out_str(ctx, ", OptROMs=");
    efi_out_u64(ctx, (UINT64)rom_cnt);
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "    32-bit BAR above 4GB : ");
    efi_out_str(ctx, mmio.bar32_above4g ? "YES\n" : "NO\n");
    efi_out_str(ctx, "    64-bit BAR present   : ");
    efi_out_str(ctx, mmio.has_64bit_bar ? "YES\n" : "NO\n");
    efi_out_str(ctx, "    MMIO above 4GB       : ");
    if (mmio.mmio_above4g) {
        efi_out_str(ctx, "YES (");
        efi_out_u64(ctx, (UINT64)mmio.above4g_bar_cnt);
        efi_out_str(ctx, " BARs across ");
        efi_out_u64(ctx, (UINT64)mmio.above4g_dev_cnt);
        efi_out_str(ctx, " devices)\n");
        efi_out_str(ctx, "    Legacy OS Risk       :\n");
        efi_out_str(ctx, "      HIGH        (Non-PAE / legacy 32-bit driver)\n");
        efi_out_str(ctx, "      CONDITIONAL (PAE-aware OS and 64-bit-capable driver)\n");
    } else {
        efi_out_str(ctx, "NO (All observed MMIO BARs reside below 4GB)\n");
        efi_out_str(ctx, "    Legacy OS Risk       : LOW (No MMIO BAR mapped >= 4GB)\n");
    }

    /* [3] PCI USB 호스트 컨트롤러 구성 요약 (Step 5 연계) */
    efi_out_str(ctx, "\n  [3] PCI USB Host Controller Mix (Class 0C/03)\n");
    efi_out_str(ctx, "    UHCI(00h)="); efi_out_u64(ctx, (UINT64)usb_uhci);
    efi_out_str(ctx, ", OHCI(10h)="); efi_out_u64(ctx, (UINT64)usb_ohci);
    efi_out_str(ctx, ", EHCI(20h)="); efi_out_u64(ctx, (UINT64)usb_ehci);
    efi_out_str(ctx, ", xHCI(30h)="); efi_out_u64(ctx, (UINT64)usb_xhci);
    if (usb_other > 0) {
        efi_out_str(ctx, ", Other="); efi_out_u64(ctx, (UINT64)usb_other);
    }
    efi_out_str(ctx, "\n    Legacy USB Outlook   : ");
    if (usb_xhci > 0 && (usb_uhci + usb_ohci + usb_ehci) == 0) {
        efi_out_str(ctx, "xHCI-ONLY (DOS/WinXP require dedicated xHCI driver or PS/2)\n");
    } else if ((usb_uhci + usb_ohci + usb_ehci) > 0) {
        efi_out_str(ctx, "LEGACY HC PRESENT (UHCI/OHCI/EHCI available alongside xHCI)\n");
    } else {
        efi_out_str(ctx, "NONE DETECTED via PCI\n");
    }

    /* [4] Audio 컨트롤러 요약 (04/01 Multimedia Audio, 04/03 Audio Device) */
    efi_out_str(ctx, "\n  [4] PCI Audio Controllers (Class 04/01 & 04/03, Total=");
    efi_out_u64(ctx, (UINT64)audio_cnt);
    efi_out_str(ctx, ")\n");
    if (audio_cnt == 0) {
        efi_out_str(ctx, "    NONE DETECTED (PC Speaker excluded by default in UEFI)\n");
    } else {
        for (i = 0; i < audio_cnt; i++) {
            efi_out_str(ctx, "    - ");
            out_sbdf(ctx, audios[i].seg, audios[i].bus, audios[i].dev, audios[i].func);
            efi_out_str(ctx, " ");
            efi_out_hex16(ctx, audios[i].vid);
            efi_out_str(ctx, ":");
            efi_out_hex16(ctx, audios[i].did);
            efi_out_str(ctx, " [04");
            efi_out_hex8(ctx, audios[i].sub);
            efi_out_str(ctx, "/");
            efi_out_hex8(ctx, audios[i].pif);
            efi_out_str(ctx, " r");
            efi_out_hex8(ctx, audios[i].rev);
            efi_out_str(ctx, "] ");
            if (audios[i].sub == 0x01U) {
                efi_out_str(ctx, "Multimedia Audio Controller (e.g. AC'97; codec unverified)");
            } else {
                efi_out_str(ctx, "Audio Device (e.g. HDA / HDMI Audio; codec unverified)");
            }
            efi_out_str(ctx, " | ");
            efi_out_str(ctx, audios[i].bound ? efi_diag_state_str(DIAG_DRIVER_BOUND)
                                             : efi_diag_state_str(DIAG_PCI_FOUND));
            efi_out_str(ctx, "\n");
        }
    }

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}