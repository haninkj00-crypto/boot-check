#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#ifndef EFI_OPEN_PROTOCOL_BY_DRIVER
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x00000010U
#endif

/* -------------------------------------------------------------------------
 * UEFI 2.11 Network / PCI / DevicePath 표준 GUID
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidPciIo =
    { 0x4CF5B200, 0x68B8, 0x4CA5, { 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A } };

static EFI_GUID kGuidDevicePath =
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidSnp =
    { 0xA19832B9, 0xAC25, 0x11D3, { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static EFI_GUID kGuidPxeBaseCode =
    { 0x03C4E603, 0xAC28, 0x11D3, { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static EFI_GUID kGuidIp4Sb =
    { 0xC51711E7, 0xB4BF, 0x404A, { 0xBF, 0xB8, 0x0A, 0x04, 0x8E, 0xF1, 0xFF, 0xE4 } };

static EFI_GUID kGuidIp6Sb =
    { 0xEC835DD3, 0xFE0F, 0x617B, { 0xA6, 0x21, 0xB3, 0x50, 0xC3, 0xE1, 0x33, 0x88 } };

static EFI_GUID kGuidDhcp4Sb =
    { 0x9D9A39D8, 0xBD42, 0x4A73, { 0xA4, 0xD5, 0x8E, 0xE9, 0x4B, 0xE1, 0x13, 0x80 } };

static EFI_GUID kGuidDhcp6Sb =
    { 0x9FB9A8A1, 0x2F4A, 0x43A6, { 0x88, 0x9C, 0xD0, 0xF7, 0xB6, 0xC4, 0x7A, 0xD5 } };

static EFI_GUID kGuidHttpSb =
    { 0xBDC8E6AF, 0xD9BC, 0x4379, { 0xA7, 0x2A, 0xE0, 0xC4, 0xE7, 0x5D, 0xAE, 0x1C } };

/* -------------------------------------------------------------------------
 * 최소 PCI I/O 레이아웃 (Class 02h 컨트롤러 대조용)
 * ------------------------------------------------------------------------- */
typedef struct _BC_NET_PCI_IO bc_net_pci_io_t;

typedef EFI_STATUS (EFIAPI *BC_NET_PCI_CFG_FN)(
    bc_net_pci_io_t *This,
    UINT32           Width,
    UINT32           Offset,
    UINTN            Count,
    VOID            *Buffer
);

typedef struct {
    BC_NET_PCI_CFG_FN Read;
    BC_NET_PCI_CFG_FN Write;
} bc_net_pci_cfg_t;

typedef EFI_STATUS (EFIAPI *BC_NET_PCI_LOC_FN)(
    bc_net_pci_io_t *This,
    UINTN           *Segment,
    UINTN           *Bus,
    UINTN           *Device,
    UINTN           *Function
);

struct _BC_NET_PCI_IO {
    VOID             *PollMem;
    VOID             *PollIo;
    VOID             *MemRead;
    VOID             *MemWrite;
    VOID             *IoRead;
    VOID             *IoWrite;
    bc_net_pci_cfg_t  Pci;
    VOID             *CopyMem;
    VOID             *Map;
    VOID             *Unmap;
    VOID             *AllocateBuffer;
    VOID             *FreeBuffer;
    VOID             *Flush;
    BC_NET_PCI_LOC_FN GetLocation;
    VOID             *Attributes;
    VOID             *GetBarAttributes;
    VOID             *SetBarAttributes;
    UINT64            RomSize;
    VOID             *RomImage;
};

/* -------------------------------------------------------------------------
 * UEFI 2.11 EFI_SIMPLE_NETWORK_PROTOCOL 레이아웃 (IA32/X64 정렬 동일)
 * ------------------------------------------------------------------------- */
typedef struct {
    UINT8 Addr[32];
} bc_mac_addr_t;

typedef struct {
    UINT32        State;
    UINT32        HwAddressSize;
    UINT32        MediaHeaderSize;
    UINT32        MaxPacketSize;
    UINT32        NvRamSize;
    UINT32        NvRamAccessSize;
    UINT32        ReceiveFilterMask;
    UINT32        ReceiveFilterSetting;
    UINT32        MaxMCastFilterCount;
    UINT32        MCastFilterCount;
    bc_mac_addr_t MCastFilter[16];
    bc_mac_addr_t CurrentAddress;
    bc_mac_addr_t BroadcastAddress;
    bc_mac_addr_t PermanentAddress;
    UINT8         IfType;
    BOOLEAN       MacAddressChangeable;
    BOOLEAN       MultipleTxSupported;
    BOOLEAN       MediaPresentSupported;
    BOOLEAN       MediaPresent;
} bc_snp_mode_t;

typedef struct _BC_SNP bc_snp_t;

typedef EFI_STATUS (EFIAPI *BC_SNP_GET_STATUS_FN)(
    bc_snp_t *This,
    UINT32   *InterruptStatus,
    VOID    **TxBuf
);

struct _BC_SNP {
    UINT64               Revision;
    VOID                *Start;
    VOID                *Stop;
    VOID                *Initialize;
    VOID                *Reset;
    VOID                *Shutdown;
    VOID                *ReceiveFilters;
    VOID                *StationAddress;
    VOID                *Statistics;
    VOID                *MCastIpToMac;
    VOID                *NvData;
    BC_SNP_GET_STATUS_FN GetStatus;
    VOID                *Transmit;
    VOID                *Receive;
    EFI_EVENT            WaitForPacket;
    bc_snp_mode_t       *Mode;
};

/* -------------------------------------------------------------------------
 * UEFI 2.11 EFI_PXE_BASE_CODE_PROTOCOL 최소 레이아웃 (읽기 전용 Mode 앞부분)
 * ------------------------------------------------------------------------- */
typedef struct {
    BOOLEAN Started;
    BOOLEAN Ipv6Available;
    BOOLEAN Ipv6Supported;
    BOOLEAN UsingIpv6;
    BOOLEAN BisSupported;
    BOOLEAN BisDetected;
    BOOLEAN AutoArp;
    BOOLEAN SendGUID;
    BOOLEAN DhcpDiscoverValid;
    BOOLEAN DhcpAckReceived;
    BOOLEAN ProxyOfferReceived;
    BOOLEAN PxeDiscoverValid;
    BOOLEAN PxeReplyReceived;
    BOOLEAN PxeBisReplyReceived;
} bc_pxe_mode_head_t;

typedef struct {
    UINT64              Revision;
    VOID               *Start;
    VOID               *Stop;
    VOID               *Dhcp;
    VOID               *Discover;
    VOID               *Mtftp;
    VOID               *UdpWrite;
    VOID               *UdpRead;
    VOID               *SetIpFilter;
    VOID               *Arp;
    VOID               *SetParameters;
    VOID               *SetStationIp;
    VOID               *SetPackets;
    bc_pxe_mode_head_t *Mode;
} bc_pxe_bc_t;

/* -------------------------------------------------------------------------
 * 내부 유틸리티
 * ------------------------------------------------------------------------- */
static UINT16 rd_u16(const UINT8 *p)
{
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static BOOLEAN has_protocol(EFI_BOOT_SERVICES *bs, EFI_HANDLE h, EFI_GUID *guid)
{
    VOID      *iface = NULL;
    EFI_STATUS st;

    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, guid, &iface);
    return (!EFI_ERROR(st) && iface != NULL) ? TRUE : FALSE;
}

static UINTN count_protocol_handles(EFI_BOOT_SERVICES *bs, EFI_GUID *guid)
{
    EFI_HANDLE *handles = NULL;
    UINTN       cnt = 0U;
    EFI_STATUS  st;

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, guid, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return 0U;
    }
    uefi_call_wrapper(bs->FreePool, 1, handles);
    return cnt;
}

static BOOLEAN pci_handle_is_bound(EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN      cnt = 0U, i;
    BOOLEAN    bound = FALSE;
    EFI_STATUS st;

    if (bs->OpenProtocolInformation == NULL) {
        return FALSE;
    }
    st = uefi_call_wrapper(bs->OpenProtocolInformation, 4,
                           h, &kGuidPciIo, &entries, &cnt);
    if (EFI_ERROR(st) || entries == NULL) {
        return FALSE;
    }
    for (i = 0U; i < cnt; i++) {
        if ((entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0U) {
            bound = TRUE;
            break;
        }
    }
    uefi_call_wrapper(bs->FreePool, 1, entries);
    return bound;
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

static VOID out_mac_addr(efi_diag_ctx_t *ctx, const UINT8 *addr, UINT32 len)
{
    UINT32 lim = (len > 32U) ? 32U : len;
    UINT32 k;

    if (lim == 0U) {
        efi_out_str(ctx, "N/A");
        return;
    }
    for (k = 0U; k < lim; k++) {
        if (k > 0U) {
            efi_out_str(ctx, ":");
        }
        efi_out_hex8(ctx, addr[k]);
    }
}

static const char *pci_net_sub_str(UINT8 sub)
{
    switch (sub) {
        case 0x00: return "Ethernet";
        case 0x01: return "TokenRing";
        case 0x02: return "FDDI";
        case 0x03: return "ATM";
        case 0x04: return "ISDN";
        case 0x80: return "Other/Wireless";
        default:   return "Network";
    }
}

static const char *snp_state_str(UINT32 state)
{
    switch (state) {
        case 0U: return "STOPPED";
        case 1U: return "STARTED";
        case 2U: return "INITIALIZED";
        default: return "UNKNOWN";
    }
}

static const char *snp_iftype_str(UINT8 iftype)
{
    switch (iftype) {
        case 1U:  return "Ethernet (RFC1700)";
        case 6U:  return "Ethernet (RFC3232)";
        case 71U: return "IEEE 802.11 Wi-Fi";
        default:  return "Other";
    }
}

static VOID out_net_device_path(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
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

        if (len < 4U) {
            break;
        }
        if (type == 0x7FU && sub == 0xFFU) {
            break;
        }

        if (type == 0x01U && sub == 0x01U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "PCI(");
            efi_out_hex8(ctx, dp[5]);
            efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)(dp[4] & 0x07U));
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x05U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "USB(P");
            efi_out_u64(ctx, (UINT64)dp[4]);
            efi_out_str(ctx, ",If");
            efi_out_u64(ctx, (UINT64)dp[5]);
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x0BU && len >= 37U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "MAC(");
            out_mac_addr(ctx, dp + 4, 6U);
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x14U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "VLAN(");
            efi_out_u64(ctx, (UINT64)rd_u16(dp + 4));
            efi_out_str(ctx, ")");
            printed = TRUE;
        }

        dp += len;
    }

    if (!printed) {
        efi_out_str(ctx, "(non-PCI/MAC path)");
    }
}

/* -------------------------------------------------------------------------
 * Step 7-1 메인 엔트리: 네트워크 (PCI NIC vs SNP / PXE / IPv4 / IPv6)
 * ------------------------------------------------------------------------- */
void efi_run_step7_network(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    EFI_HANDLE        *pci_handles = NULL;
    EFI_HANDLE        *snp_handles = NULL;
    EFI_HANDLE        *pxe_handles = NULL;
    UINTN              pci_cnt = 0U, snp_cnt = 0U, pxe_cnt = 0U;
    UINTN              pci_net_total = 0U, pci_net_bound = 0U;
    UINTN              ip4_cnt, ip6_cnt, dhcp4_cnt, dhcp6_cnt, http_cnt;
    UINTN              i;
    EFI_STATUS         st;

    efi_out_str(ctx, "=== [Step 7-1] Network Adapters (PCI vs SNP / PXE / IP Stack) ===\n");
    efi_out_str(ctx, "  Safety Policy : Read-Only (No SNP Start/Init or Packet Transmit)\n");
    efilog_flush(ctx);

    if (bs == NULL || bs->LocateHandleBuffer == NULL) {
        efi_out_str(ctx, "  LocateHandleBuffer() : NOT EXPOSED\n\n");
        efilog_flush(ctx);
        return;
    }

    /* ---------------------------------------------------------------------
     * [1] PCI Network 컨트롤러 (Class 02h) vs UEFI 드라이버 바인딩 대조
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [1] PCI Network Controllers (Class 02h)\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidPciIo, NULL, &pci_cnt, &pci_handles);
    if (!EFI_ERROR(st) && pci_handles != NULL) {
        for (i = 0U; i < pci_cnt; i++) {
            bc_net_pci_io_t *pci = NULL;
            UINTN  seg = 0U, bus = 0U, dev = 0U, func = 0U;
            UINT32 id_reg = 0U, cls_reg = 0U;
            UINT8  cls, sub;

            st = uefi_call_wrapper(bs->HandleProtocol, 3,
                                   pci_handles[i], &kGuidPciIo, (VOID **)&pci);
            if (EFI_ERROR(st) || pci == NULL) {
                continue;
            }
            if (EFI_ERROR(uefi_call_wrapper(pci->Pci.Read, 5,
                                            pci, (UINT32)2, (UINT32)0x08U, (UINTN)1, &cls_reg))) {
                continue;
            }
            cls = (UINT8)((cls_reg >> 24) & 0xFFU);
            sub = (UINT8)((cls_reg >> 16) & 0xFFU);
            if (cls != 0x02U) {
                continue;
            }

            pci_net_total++;
            uefi_call_wrapper(pci->Pci.Read, 5, pci, (UINT32)2, (UINT32)0x00U, (UINTN)1, &id_reg);
            uefi_call_wrapper(pci->GetLocation, 5, pci, &seg, &bus, &dev, &func);

            {
                BOOLEAN on_snp    = has_protocol(bs, pci_handles[i], &kGuidSnp);
                BOOLEAN drv_bound = pci_handle_is_bound(bs, pci_handles[i]);

                if (on_snp || drv_bound) {
                    pci_net_bound++;
                }

                efi_out_str(ctx, "    ");
                out_sbdf(ctx, seg, bus, dev, func);
                efi_out_str(ctx, " ");
                efi_out_hex16(ctx, (UINT16)(id_reg & 0xFFFFU));
                efi_out_str(ctx, ":");
                efi_out_hex16(ctx, (UINT16)(id_reg >> 16));
                efi_out_str(ctx, " [02");
                efi_out_hex8(ctx, sub);
                efi_out_str(ctx, " ");
                efi_out_str(ctx, pci_net_sub_str(sub));
                efi_out_str(ctx, "] | ");

                if (on_snp) {
                    efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
                    efi_out_str(ctx, " (SNP on PCI handle)");
                } else if (drv_bound) {
                    efi_out_str(ctx, efi_diag_state_str(DIAG_DRIVER_BOUND));
                    efi_out_str(ctx, " (UNDI/Child SNP handles below)");
                } else {
                    efi_out_str(ctx, efi_diag_state_str(DIAG_PCI_FOUND));
                    efi_out_str(ctx, " / ");
                    efi_out_str(ctx, efi_conn_state_str(CONN_UNCONNECTED_AT_ENTRY));
                }
                efi_out_str(ctx, "\n");
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, pci_handles);
    }
    if (pci_net_total == 0U) {
        efi_out_str(ctx, "    NONE DETECTED via PCI\n");
    }

    /* ---------------------------------------------------------------------
     * [2] EFI_SIMPLE_NETWORK_PROTOCOL (SNP) 인터페이스 상세
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [2] Simple Network Protocol Interfaces (EFI_SIMPLE_NETWORK_PROTOCOL)\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidSnp, NULL, &snp_cnt, &snp_handles);
    if (EFI_ERROR(st) || snp_handles == NULL || snp_cnt == 0U) {
        efi_out_str(ctx, "    SNP Status       : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
        if (pci_net_total > 0U && pci_net_bound == 0U) {
            efi_out_str(ctx, "    Hint             : PCI NIC exists, but BIOS Network Stack may be disabled or UEFI UNDI driver is absent.\n");
        }
    } else {
        efi_out_str(ctx, "    Total SNP Handles: ");
        efi_out_u64(ctx, (UINT64)snp_cnt);
        efi_out_str(ctx, " (");
        efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
        efi_out_str(ctx, ")\n");

        for (i = 0U; i < snp_cnt; i++) {
            bc_snp_t      *snp = NULL;
            bc_snp_mode_t *m;
            UINT32         int_st = 0U;
            BOOLEAN        st_polled = FALSE;

            st = uefi_call_wrapper(bs->HandleProtocol, 3,
                                   snp_handles[i], &kGuidSnp, (VOID **)&snp);
            if (EFI_ERROR(st) || snp == NULL || snp->Mode == NULL) {
                continue;
            }
            m = snp->Mode;

            /* 이미 INITIALIZED 상태일 때만 비파괴 GetStatus()로 링크 상태 갱신 */
            if (m->State == 2U && snp->GetStatus != NULL) {
                efilog_flush(ctx);
                if (!EFI_ERROR(uefi_call_wrapper(snp->GetStatus, 3, snp, &int_st, NULL))) {
                    st_polled = TRUE;
                }
            }

            efi_out_str(ctx, "    [SNP #");
            efi_out_u64(ctx, (UINT64)i);
            efi_out_str(ctx, "] State=");
            efi_out_str(ctx, snp_state_str(m->State));
            efi_out_str(ctx, " | IfType=");
            efi_out_u64(ctx, (UINT64)m->IfType);
            efi_out_str(ctx, " (");
            efi_out_str(ctx, snp_iftype_str(m->IfType));
            efi_out_str(ctx, ")\n");

            efi_out_str(ctx, "      Path           : ");
            out_net_device_path(ctx, bs, snp_handles[i]);
            efi_out_str(ctx, "\n");

            efi_out_str(ctx, "      Current MAC    : ");
            out_mac_addr(ctx, m->CurrentAddress.Addr, m->HwAddressSize);
            efi_out_str(ctx, " (Len=");
            efi_out_u64(ctx, (UINT64)m->HwAddressSize);
            efi_out_str(ctx, " B)\n");

            efi_out_str(ctx, "      Permanent MAC  : ");
            out_mac_addr(ctx, m->PermanentAddress.Addr, m->HwAddressSize);
            efi_out_str(ctx, m->MacAddressChangeable ? " [Changeable]\n" : " [Fixed]\n");

            efi_out_str(ctx, "      Link / Media   : ");
            if (!m->MediaPresentSupported) {
                efi_out_str(ctx, "UNSUPPORTED BY DRIVER (MediaPresentSupported=0)\n");
            } else {
                efi_out_str(ctx, m->MediaPresent ? "LINK UP (MediaPresent=1)"
                                                 : "LINK DOWN / NO CABLE (MediaPresent=0)");
                if (m->State != 2U) {
                    efi_out_str(ctx, " [Uninit; state may be stale]");
                } else if (st_polled) {
                    efi_out_str(ctx, " [Polled via GetStatus]");
                }
                efi_out_str(ctx, "\n");
            }

            efi_out_str(ctx, "      Packet / Filter: MaxPacket=");
            efi_out_u64(ctx, (UINT64)m->MaxPacketSize);
            efi_out_str(ctx, " B, Header=");
            efi_out_u64(ctx, (UINT64)m->MediaHeaderSize);
            efi_out_str(ctx, " B, RxMask=0x");
            efi_out_hex32(ctx, m->ReceiveFilterMask);
            efi_out_str(ctx, "\n");

            /* 동일 핸들 상위 네트워크 프로토콜 결합 확인 */
            efi_out_str(ctx, "      Upper Protocols: ");
            {
                BOOLEAN pxe = has_protocol(bs, snp_handles[i], &kGuidPxeBaseCode);
                BOOLEAN ip4 = has_protocol(bs, snp_handles[i], &kGuidIp4Sb);
                BOOLEAN ip6 = has_protocol(bs, snp_handles[i], &kGuidIp6Sb);
                BOOLEAN d4  = has_protocol(bs, snp_handles[i], &kGuidDhcp4Sb);
                BOOLEAN d6  = has_protocol(bs, snp_handles[i], &kGuidDhcp6Sb);
                BOOLEAN htp = has_protocol(bs, snp_handles[i], &kGuidHttpSb);

                if (pxe || ip4 || ip6 || d4 || d6 || htp) {
                    BOOLEAN first = TRUE;
                    if (pxe) { efi_out_str(ctx, "PXE"); first = FALSE; }
                    if (ip4) { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "IPv4"); first = FALSE; }
                    if (ip6) { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "IPv6"); first = FALSE; }
                    if (d4)  { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "DHCPv4"); first = FALSE; }
                    if (d6)  { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "DHCPv6"); first = FALSE; }
                    if (htp) { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "HTTP"); }
                    efi_out_str(ctx, "\n");
                } else {
                    efi_out_str(ctx, "NONE on SNP handle (Raw SNP layer only)\n");
                }
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, snp_handles);
    }

    /* ---------------------------------------------------------------------
     * [3] PXE Base Code & 펌웨어 IPv4 / IPv6 / DHCP / HTTP 스택 존재 여부
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [3] Firmware Network Stack & PXE Base Code\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidPxeBaseCode, NULL, &pxe_cnt, &pxe_handles);
    if (EFI_ERROR(st) || pxe_handles == NULL || pxe_cnt == 0U) {
        efi_out_str(ctx, "    PXE Base Code    : NOT EXPOSED\n");
    } else {
        efi_out_str(ctx, "    PXE Base Code    : PROTOCOL AVAILABLE (");
        efi_out_u64(ctx, (UINT64)pxe_cnt);
        efi_out_str(ctx, " handles)\n");
        for (i = 0U; i < pxe_cnt; i++) {
            bc_pxe_bc_t *pxe = NULL;
            if (!EFI_ERROR(uefi_call_wrapper(bs->HandleProtocol, 3,
                                             pxe_handles[i], &kGuidPxeBaseCode, (VOID **)&pxe)) &&
                pxe != NULL && pxe->Mode != NULL) {
                bc_pxe_mode_head_t *pm = pxe->Mode;
                efi_out_str(ctx, "      [PXE #");
                efi_out_u64(ctx, (UINT64)i);
                efi_out_str(ctx, "] Started=");
                efi_out_str(ctx, pm->Started ? "YES" : "NO");
                efi_out_str(ctx, ", IPv6Sup=");
                efi_out_str(ctx, pm->Ipv6Supported ? "YES" : "NO");
                efi_out_str(ctx, ", UsingIPv6=");
                efi_out_str(ctx, pm->UsingIpv6 ? "YES" : "NO");
                efi_out_str(ctx, ", DhcpAck=");
                efi_out_str(ctx, pm->DhcpAckReceived ? "YES" : "NO");
                efi_out_str(ctx, ", PxeReply=");
                efi_out_str(ctx, pm->PxeReplyReceived ? "YES\n" : "NO\n");
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, pxe_handles);
    }

    ip4_cnt   = count_protocol_handles(bs, &kGuidIp4Sb);
    ip6_cnt   = count_protocol_handles(bs, &kGuidIp6Sb);
    dhcp4_cnt = count_protocol_handles(bs, &kGuidDhcp4Sb);
    dhcp6_cnt = count_protocol_handles(bs, &kGuidDhcp6Sb);
    http_cnt  = count_protocol_handles(bs, &kGuidHttpSb);

    efi_out_str(ctx, "    Service Bindings : IPv4="); efi_out_u64(ctx, (UINT64)ip4_cnt);
    efi_out_str(ctx, ", IPv6=");   efi_out_u64(ctx, (UINT64)ip6_cnt);
    efi_out_str(ctx, ", DHCPv4="); efi_out_u64(ctx, (UINT64)dhcp4_cnt);
    efi_out_str(ctx, ", DHCPv6="); efi_out_u64(ctx, (UINT64)dhcp6_cnt);
    efi_out_str(ctx, ", HTTP=");   efi_out_u64(ctx, (UINT64)http_cnt);
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "    Stack Note       : ");
    if (snp_cnt > 0U && (ip4_cnt + ip6_cnt) == 0U) {
        efi_out_str(ctx, "SNP driver present, but IPv4/IPv6 network stack is NOT loaded.\n\n");
    } else if (snp_cnt > 0U) {
        efi_out_str(ctx, "Both SNP driver and IP service bindings exposed in UEFI.\n\n");
    } else {
        efi_out_str(ctx, "No active UEFI network driver exposed at entry.\n\n");
    }

    efilog_flush(ctx);
}