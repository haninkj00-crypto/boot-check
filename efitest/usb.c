#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#ifndef EFI_OPEN_PROTOCOL_BY_DRIVER
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x00000010U
#endif

#define USB_MAX_HC_PORTS    64U
#define USB_MAX_HUB_PORTS   16U
#define USB_MAX_ENDPOINTS   16U
#define USB_MAX_TRACK_HCS   32U

/* -------------------------------------------------------------------------
 * UEFI 2.11 USB / PCI / DevicePath / 상위 프로토콜 표준 GUID
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidPciIo =
    { 0x4CF5B200, 0x68B8, 0x4CA5, { 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A } };

static EFI_GUID kGuidUsb2Hc =
    { 0x3E745226, 0x9818, 0x45B6, { 0xA2, 0xAC, 0xD7, 0xCD, 0x0E, 0x8B, 0xA2, 0xBC } };

static EFI_GUID kGuidUsbHcLegacy =
    { 0xF5089266, 0x1AA0, 0x4953, { 0x97, 0xD8, 0x56, 0x2F, 0x8A, 0x73, 0xB5, 0x19 } };

static EFI_GUID kGuidUsbIo =
    { 0x2B2F68D6, 0x0CD2, 0x44CF, { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } };

static EFI_GUID kGuidDevicePath =
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidBlockIo =
    { 0x964E5B21, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidSimpleTextIn =
    { 0x387477C1, 0x69C7, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidSimplePointer =
    { 0x31878C87, 0x0B75, 0x11D5, { 0x9A, 0x4F, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static EFI_GUID kGuidAbsPointer =
    { 0x8D59D32B, 0xC655, 0x4AE9, { 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43 } };

/* -------------------------------------------------------------------------
 * 최소 PCI I/O 레이아웃 (PCI USB 컨트롤러 대조 및 SBDF 조회용)
 * ------------------------------------------------------------------------- */
typedef struct _BC_USB_PCI_IO bc_usb_pci_io_t;

typedef EFI_STATUS (EFIAPI *BC_USB_PCI_CFG_FN)(
    bc_usb_pci_io_t *This,
    UINT32           Width,
    UINT32           Offset,
    UINTN            Count,
    VOID            *Buffer
);

typedef struct {
    BC_USB_PCI_CFG_FN Read;
    BC_USB_PCI_CFG_FN Write;
} bc_usb_pci_cfg_t;

typedef EFI_STATUS (EFIAPI *BC_USB_PCI_LOC_FN)(
    bc_usb_pci_io_t *This,
    UINTN           *Segment,
    UINTN           *Bus,
    UINTN           *Device,
    UINTN           *Function
);

struct _BC_USB_PCI_IO {
    VOID             *PollMem;
    VOID             *PollIo;
    VOID             *MemRead;
    VOID             *MemWrite;
    VOID             *IoRead;
    VOID             *IoWrite;
    bc_usb_pci_cfg_t  Pci;
    VOID             *CopyMem;
    VOID             *Map;
    VOID             *Unmap;
    VOID             *AllocateBuffer;
    VOID             *FreeBuffer;
    VOID             *Flush;
    BC_USB_PCI_LOC_FN GetLocation;
    VOID             *Attributes;
    VOID             *GetBarAttributes;
    VOID             *SetBarAttributes;
    UINT64            RomSize;
    VOID             *RomImage;
};

/* -------------------------------------------------------------------------
 * UEFI 2.11 EFI_USB2_HC_PROTOCOL 레이아웃
 * ------------------------------------------------------------------------- */
typedef struct {
    UINT16 PortStatus;
    UINT16 PortChangeStatus;
} bc_usb_port_status_t;

typedef struct _BC_USB2_HC bc_usb2_hc_t;

typedef EFI_STATUS (EFIAPI *BC_USB2_HC_GET_CAP_FN)(
    bc_usb2_hc_t *This,
    UINT8        *MaxSpeed,
    UINT8        *PortNumber,
    UINT8        *Is64BitCapable
);

typedef EFI_STATUS (EFIAPI *BC_USB2_HC_GET_STATE_FN)(
    bc_usb2_hc_t *This,
    UINT32       *State
);

typedef EFI_STATUS (EFIAPI *BC_USB2_HC_GET_PORT_ST_FN)(
    bc_usb2_hc_t         *This,
    UINT8                 PortNumber,
    bc_usb_port_status_t *PortStatus
);

struct _BC_USB2_HC {
    BC_USB2_HC_GET_CAP_FN     GetCapability;
    VOID                     *Reset;
    BC_USB2_HC_GET_STATE_FN   GetState;
    VOID                     *SetState;
    VOID                     *ControlTransfer;
    VOID                     *BulkTransfer;
    VOID                     *AsyncInterruptTransfer;
    VOID                     *SyncInterruptTransfer;
    VOID                     *IsochronousTransfer;
    VOID                     *AsyncIsochronousTransfer;
    BC_USB2_HC_GET_PORT_ST_FN GetRootHubPortStatus;
    VOID                     *SetRootHubPortFeature;
    VOID                     *ClearRootHubPortFeature;
    UINT16                    MajorRevision;
    UINT16                    MinorRevision;
};

/* -------------------------------------------------------------------------
 * UEFI 2.11 EFI_USB_IO_PROTOCOL 레이아웃 및 표준 USB 디스크립터
 * ------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    UINT8  RequestType;
    UINT8  Request;
    UINT16 Value;
    UINT16 Index;
    UINT16 Length;
} bc_usb_dev_req_t;

typedef struct __attribute__((packed)) {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT16 BcdUSB;
    UINT8  DeviceClass;
    UINT8  DeviceSubClass;
    UINT8  DeviceProtocol;
    UINT8  MaxPacketSize0;
    UINT16 IdVendor;
    UINT16 IdProduct;
    UINT16 BcdDevice;
    UINT8  StrManufacturer;
    UINT8  StrProduct;
    UINT8  StrSerialNumber;
    UINT8  NumConfigurations;
} bc_usb_dev_desc_t;

typedef struct __attribute__((packed)) {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT16 TotalLength;
    UINT8  NumInterfaces;
    UINT8  ConfigurationValue;
    UINT8  Configuration;
    UINT8  Attributes;
    UINT8  MaxPower;
} bc_usb_cfg_desc_t;

typedef struct __attribute__((packed)) {
    UINT8 Length;
    UINT8 DescriptorType;
    UINT8 InterfaceNumber;
    UINT8 AlternateSetting;
    UINT8 NumEndpoints;
    UINT8 InterfaceClass;
    UINT8 InterfaceSubClass;
    UINT8 InterfaceProtocol;
    UINT8 Interface;
} bc_usb_if_desc_t;

typedef struct __attribute__((packed)) {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT8  EndpointAddress;
    UINT8  Attributes;
    UINT16 MaxPacketSize;
    UINT8  Interval;
} bc_usb_ep_desc_t;

typedef struct _BC_USB_IO bc_usb_io_t;

typedef EFI_STATUS (EFIAPI *BC_USB_CTRL_XFER_FN)(
    bc_usb_io_t      *This,
    bc_usb_dev_req_t *Request,
    UINT32            Direction,
    UINT32            Timeout,
    VOID             *Data,
    UINTN             DataLength,
    UINT32           *Status
);

typedef EFI_STATUS (EFIAPI *BC_USB_GET_DEV_DESC_FN)(
    bc_usb_io_t       *This,
    bc_usb_dev_desc_t *DeviceDescriptor
);

typedef EFI_STATUS (EFIAPI *BC_USB_GET_CFG_DESC_FN)(
    bc_usb_io_t       *This,
    bc_usb_cfg_desc_t *ConfigurationDescriptor
);

typedef EFI_STATUS (EFIAPI *BC_USB_GET_IF_DESC_FN)(
    bc_usb_io_t      *This,
    bc_usb_if_desc_t *InterfaceDescriptor
);

typedef EFI_STATUS (EFIAPI *BC_USB_GET_EP_DESC_FN)(
    bc_usb_io_t      *This,
    UINT8             EndpointIndex,
    bc_usb_ep_desc_t *EndpointDescriptor
);

typedef EFI_STATUS (EFIAPI *BC_USB_GET_STR_DESC_FN)(
    bc_usb_io_t *This,
    UINT16       LangID,
    UINT8        StringID,
    CHAR16     **String
);

typedef EFI_STATUS (EFIAPI *BC_USB_GET_LANGS_FN)(
    bc_usb_io_t *This,
    UINT16     **LangIDTable,
    UINT16      *TableSize
);

struct _BC_USB_IO {
    BC_USB_CTRL_XFER_FN     UsbControlTransfer;
    VOID                   *UsbBulkTransfer;
    VOID                   *UsbAsyncInterruptTransfer;
    VOID                   *UsbSyncInterruptTransfer;
    VOID                   *UsbIsochronousTransfer;
    VOID                   *UsbAsyncIsochronousTransfer;
    BC_USB_GET_DEV_DESC_FN  UsbGetDeviceDescriptor;
    BC_USB_GET_CFG_DESC_FN  UsbGetConfigDescriptor;
    BC_USB_GET_IF_DESC_FN   UsbGetInterfaceDescriptor;
    BC_USB_GET_EP_DESC_FN   UsbGetEndpointDescriptor;
    BC_USB_GET_STR_DESC_FN  UsbGetStringDescriptor;
    BC_USB_GET_LANGS_FN     UsbGetSupportedLanguages;
    VOID                   *UsbPortReset;
};

/* -------------------------------------------------------------------------
 * 내부 유틸리티 (포맷팅, 프로토콜 확인, DevicePath 토폴로지 출력)
 * ------------------------------------------------------------------------- */
static UINT16 rd_u16(const UINT8 *p)
{
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static BOOLEAN has_protocol(EFI_BOOT_SERVICES *bs, EFI_HANDLE h, EFI_GUID *guid)
{
    VOID *iface = NULL;
    EFI_STATUS st;

    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, guid, &iface);
    return (!EFI_ERROR(st) && iface != NULL) ? TRUE : FALSE;
}

static BOOLEAN pci_handle_is_bound(EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN      cnt = 0, i;
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
    for (i = 0; i < cnt; i++) {
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

static VOID out_bcd16(efi_diag_ctx_t *ctx, UINT16 bcd)
{
    UINT8 hi = (UINT8)((bcd >> 8) & 0xFFU);
    UINT8 d1 = (UINT8)((hi >> 4) & 0x0FU);
    UINT8 d0 = (UINT8)(hi & 0x0FU);
    UINT8 f1 = (UINT8)((bcd >> 4) & 0x0FU);
    UINT8 f0 = (UINT8)(bcd & 0x0FU);

    if (d1 > 0U) {
        efi_out_u64(ctx, (UINT64)d1);
    }
    efi_out_u64(ctx, (UINT64)d0);
    efi_out_str(ctx, ".");
    efi_out_u64(ctx, (UINT64)f1);
    efi_out_u64(ctx, (UINT64)f0);
}

static const char *usb_progif_str(UINT8 pif)
{
    switch (pif) {
        case 0x00: return "UHCI (USB 1.1)";
        case 0x10: return "OHCI (USB 1.1)";
        case 0x20: return "EHCI (USB 2.0)";
        case 0x30: return "xHCI (USB 3.x)";
        default:   return "Unclassified USB HC";
    }
}

static const char *usb_hc_speed_str(UINT8 spd)
{
    switch (spd) {
        case 0: return "FULL (12 Mbps)";
        case 1: return "LOW (1.5 Mbps)";
        case 2: return "HIGH (480 Mbps)";
        case 3: return "SUPER (5+ Gbps)";
        default: return "UNKNOWN";
    }
}

static const char *usb_hc_state_str(UINT32 st)
{
    switch (st) {
        case 0: return "HALT";
        case 1: return "OPERATIONAL";
        case 2: return "SUSPEND";
        default: return "UNKNOWN";
    }
}

static const char *usb_if_class_label(UINT8 cls, UINT8 sub, UINT8 proto)
{
    switch (cls) {
        case 0x01: return "Audio";
        case 0x02: return "CDC/Comm";
        case 0x03:
            if (sub == 0x01 && proto == 0x01) return "HID/Boot-Keyboard";
            if (sub == 0x01 && proto == 0x02) return "HID/Boot-Mouse";
            return "HID/Other";
        case 0x06: return "Imaging";
        case 0x07: return "Printer";
        case 0x08: return "Mass-Storage";
        case 0x09:
            if (proto == 0x03) return "Hub/SuperSpeed";
            if (proto == 0x02) return "Hub/HighSpeed-MultiTT";
            if (proto == 0x01) return "Hub/HighSpeed-SingleTT";
            return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "SmartCard";
        case 0x0E: return "Video/UVC";
        case 0xE0: return "Wireless/Bluetooth";
        case 0xEF: return "Misc/IAD";
        case 0xFE: return "App-Specific";
        case 0xFF: return "Vendor-Specific";
        default:   return "Other";
    }
}

static const char *usb_ep_type_str(UINT8 attr)
{
    switch (attr & 0x03U) {
        case 0: return "Ctrl";
        case 1: return "Iso";
        case 2: return "Bulk";
        case 3: return "Intr";
        default: return "?";
    }
}

static VOID out_usb_device_path(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    const UINT8 *dp = NULL;
    UINTN        guard = 0;
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

        /* Hardware PCI Node (Type 0x01, Sub 0x01, Len 6): [4]=Func, [5]=Dev */
        if (type == 0x01U && sub == 0x01U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "PCI(");
            efi_out_hex8(ctx, dp[5]);
            efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)(dp[4] & 0x07U));
            efi_out_str(ctx, ")");
            printed = TRUE;
        }
        /* Messaging USB Node (Type 0x03, Sub 0x05, Len 6): [4]=ParentPort, [5]=IfNum */
        else if (type == 0x03U && sub == 0x05U && len >= 6U) {
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "USB(P");
            efi_out_u64(ctx, (UINT64)dp[4]);
            efi_out_str(ctx, ",If");
            efi_out_u64(ctx, (UINT64)dp[5]);
            efi_out_str(ctx, ")");
            printed = TRUE;
        }

        dp += len;
    }

    if (!printed) {
        efi_out_str(ctx, "(non-PCI/USB path)");
    }
}

static UINT16 usb_select_lang_id(bc_usb_io_t *uio)
{
    UINT16    *tbl = NULL;
    UINT16     sz  = 0;
    UINTN      cnt, i;
    EFI_STATUS st;

    if (uio->UsbGetSupportedLanguages == NULL) {
        return 0x0409U;
    }
    /* UEFI 규격: LangIDTable은 드라이버 소유 버퍼이므로 FreePool 금지 */
    st = uefi_call_wrapper(uio->UsbGetSupportedLanguages, 3, uio, &tbl, &sz);
    if (EFI_ERROR(st) || tbl == NULL || sz < 2U) {
        return 0x0409U;
    }
    cnt = (UINTN)(sz >> 1);
    for (i = 0; i < cnt; i++) {
        if (tbl[i] == 0x0409U) {
            return 0x0409U;
        }
    }
    return tbl[0];
}

static VOID usb_out_str_desc(efi_diag_ctx_t *ctx, EFI_BOOT_SERVICES *bs,
                             bc_usb_io_t *uio, UINT16 lang_id, UINT8 str_id)
{
    CHAR16    *wstr = NULL;
    EFI_STATUS st;

    if (str_id == 0U || uio->UsbGetStringDescriptor == NULL) {
        efi_out_str(ctx, "N/A");
        return;
    }
    st = uefi_call_wrapper(uio->UsbGetStringDescriptor, 4, uio, lang_id, str_id, &wstr);
    if (EFI_ERROR(st) || wstr == NULL) {
        efi_out_str(ctx, "N/A");
        return;
    }
    efi_out_str(ctx, "\"");
    efi_out_wstr(ctx, wstr);
    efi_out_str(ctx, "\"");
    /* UEFI 규격: UsbGetStringDescriptor 반환 문자열은 호출자가 FreePool 해제 */
    uefi_call_wrapper(bs->FreePool, 1, wstr);
}

/* -------------------------------------------------------------------------
 * 외부 USB 허브(Class 09h) 하위 포트 협상 속도 조회 (읽기 전용 ControlTransfer)
 * ------------------------------------------------------------------------- */
static VOID probe_hub_downstream_ports(efi_diag_ctx_t *ctx,
                                       bc_usb_io_t *uio,
                                       UINT16 bcd_usb)
{
    bc_usb_dev_req_t req;
    UINT8            desc[12];
    UINT32           xfer_st = 0;
    EFI_STATUS       st;
    BOOLEAN          is_ss_hub = FALSE;
    UINT8            nports, p;
    UINTN            conn_cnt = 0;

    if (uio->UsbControlTransfer == NULL) {
        return;
    }

    efilog_flush(ctx);
    SetMem(desc, sizeof(desc), 0);

    /* USB 3.x 허브면 SuperSpeed Hub Descriptor(0x2A) 우선 시도, 실패 시 0x29 폴백 */
    if (bcd_usb >= 0x0300U) {
        req.RequestType = 0xA0U; /* IN | Class | Device */
        req.Request     = 0x06U; /* GET_DESCRIPTOR */
        req.Value       = 0x2A00U;
        req.Index       = 0x0000U;
        req.Length      = 12U;
        st = uefi_call_wrapper(uio->UsbControlTransfer, 7,
                               uio, &req, (UINT32)0, (UINT32)500U,
                               desc, (UINTN)12U, &xfer_st);
        if (!EFI_ERROR(st) && xfer_st == 0U && desc[1] == 0x2AU) {
            is_ss_hub = TRUE;
        }
    }

    if (!is_ss_hub) {
        SetMem(desc, sizeof(desc), 0);
        req.RequestType = 0xA0U;
        req.Request     = 0x06U;
        req.Value       = 0x2900U; /* USB 2.0 Hub Descriptor */
        req.Index       = 0x0000U;
        req.Length      = 9U;
        st = uefi_call_wrapper(uio->UsbControlTransfer, 7,
                               uio, &req, (UINT32)0, (UINT32)500U,
                               desc, (UINTN)9U, &xfer_st);
        if (EFI_ERROR(st) || xfer_st != 0U || desc[1] != 0x29U) {
            efi_out_str(ctx, "      Hub Ports      : Descriptor unreadable (");
            efi_out_status(ctx, st);
            efi_out_str(ctx, ")\n");
            return;
        }
    }

    nports = desc[2];
    if (nports > USB_MAX_HUB_PORTS) {
        nports = USB_MAX_HUB_PORTS;
    }

    efi_out_str(ctx, "      Hub Downstream : ");
    efi_out_u64(ctx, (UINT64)desc[2]);
    efi_out_str(ctx, is_ss_hub ? " ports (SuperSpeed Hub)\n" : " ports (USB 2.0 Hub)\n");

    /* USB Hub Class GET_STATUS는 1-based 포트 번호 사용 */
    for (p = 1U; p <= nports; p++) {
        UINT8 pbuf[4];
        SetMem(pbuf, sizeof(pbuf), 0);
        req.RequestType = 0xA3U; /* IN | Class | Other (Port) */
        req.Request     = 0x00U; /* GET_STATUS */
        req.Value       = 0x0000U;
        req.Index       = (UINT16)p;
        req.Length      = 4U;

        xfer_st = 0;
        st = uefi_call_wrapper(uio->UsbControlTransfer, 7,
                               uio, &req, (UINT32)0, (UINT32)300U,
                               pbuf, (UINTN)4U, &xfer_st);
        if (EFI_ERROR(st) || xfer_st != 0U) {
            continue;
        }

        {
            UINT16 pst = rd_u16(pbuf);
            if ((pst & 0x0001U) != 0U) {
                conn_cnt++;
                efi_out_str(ctx, "        HubPort ");
                efi_out_u64(ctx, (UINT64)p);
                efi_out_str(ctx, "    : CONNECTED | Speed=");
                if (is_ss_hub) {
                    UINT8 pls = (UINT8)((pst >> 5) & 0x0FU);
                    efi_out_str(ctx, "SUPER (USB 3.x, PLS=");
                    efi_out_u64(ctx, (UINT64)pls);
                    efi_out_str(ctx, ")");
                } else {
                    if ((pst & 0x0200U) != 0U)      efi_out_str(ctx, "LOW (1.5 Mbps)");
                    else if ((pst & 0x0400U) != 0U) efi_out_str(ctx, "HIGH (480 Mbps)");
                    else                            efi_out_str(ctx, "FULL (12 Mbps)");
                }
                efi_out_str(ctx, (pst & 0x0002U) ? " [ENABLED]" : " [DISABLED]");
                if ((pst & 0x0008U) != 0U) {
                    efi_out_str(ctx, " [OVERCURRENT]");
                }
                efi_out_str(ctx, "\n");
            }
        }
    }

    if (conn_cnt == 0U) {
        efi_out_str(ctx, "        (No downstream devices connected)\n");
    }
}

/* -------------------------------------------------------------------------
 * Step 5 메인 엔트리: USB 호스트 컨트롤러 · 루트 허브 · 연결 장치 열거
 * ------------------------------------------------------------------------- */
void efi_run_step5_usb_stack(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    EFI_HANDLE        *pci_handles = NULL;
    EFI_HANDLE        *hc_handles  = NULL;
    EFI_HANDLE        *io_handles  = NULL;
    UINTN              pci_cnt = 0, hc_cnt = 0, io_cnt = 0;
    UINTN              i;
    EFI_STATUS         st;

    UINTN pci_uhci = 0, pci_ohci = 0, pci_ehci = 0, pci_xhci = 0, pci_other = 0;
    UINTN pci_usb_total = 0, pci_usb_bound_hc2 = 0;
    UINTN root_conn_low = 0, root_conn_full = 0, root_conn_high = 0, root_conn_super = 0;
    UINTN dev_hid_kbd = 0, dev_hid_mse = 0, dev_hid_oth = 0;
    UINTN dev_msc = 0, dev_msc_bio = 0, dev_hub = 0, dev_audio = 0, dev_other = 0;

    efi_out_str(ctx, "=== [Step 5] USB Host Controllers, Root Hubs & Devices ===\n");
    efi_out_str(ctx, "  Safety Policy : Read-Only (Controller/Port Reset NEVER invoked)\n");
    efilog_flush(ctx);

    if (bs == NULL || bs->LocateHandleBuffer == NULL) {
        efi_out_str(ctx, "  LocateHandleBuffer() : NOT EXPOSED\n\n");
        efilog_flush(ctx);
        return;
    }

    /* ---------------------------------------------------------------------
     * [1] PCI USB 호스트 컨트롤러(Class 0C/03) vs UEFI 드라이버 바인딩 대조
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [1] PCI USB Host Controllers (Class 0C/03) vs UEFI Binding\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidPciIo, NULL, &pci_cnt, &pci_handles);
    if (!EFI_ERROR(st) && pci_handles != NULL) {
        for (i = 0; i < pci_cnt; i++) {
            bc_usb_pci_io_t *pci = NULL;
            UINTN  seg = 0, bus = 0, dev = 0, func = 0;
            UINT32 id_reg = 0, cls_reg = 0;
            UINT8  cls, sub, pif;

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
            pif = (UINT8)((cls_reg >> 8)  & 0xFFU);

            if (cls != 0x0CU || sub != 0x03U) {
                continue;
            }

            pci_usb_total++;
            if      (pif == 0x00U) pci_uhci++;
            else if (pif == 0x10U) pci_ohci++;
            else if (pif == 0x20U) pci_ehci++;
            else if (pif == 0x30U) pci_xhci++;
            else                   pci_other++;

            uefi_call_wrapper(pci->Pci.Read, 5, pci, (UINT32)2, (UINT32)0x00U, (UINTN)1, &id_reg);
            uefi_call_wrapper(pci->GetLocation, 5, pci, &seg, &bus, &dev, &func);

            {
                BOOLEAN has_hc2   = has_protocol(bs, pci_handles[i], &kGuidUsb2Hc);
                BOOLEAN has_hc1   = has_protocol(bs, pci_handles[i], &kGuidUsbHcLegacy);
                BOOLEAN drv_bound = pci_handle_is_bound(bs, pci_handles[i]);

                if (has_hc2) {
                    pci_usb_bound_hc2++;
                }

                efi_out_str(ctx, "    ");
                out_sbdf(ctx, seg, bus, dev, func);
                efi_out_str(ctx, " ");
                efi_out_hex16(ctx, (UINT16)(id_reg & 0xFFFFU));
                efi_out_str(ctx, ":");
                efi_out_hex16(ctx, (UINT16)(id_reg >> 16));
                efi_out_str(ctx, " | ");
                efi_out_str(ctx, usb_progif_str(pif));
                efi_out_str(ctx, " | ");

                if (has_hc2) {
                    efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
                    efi_out_str(ctx, " (USB2_HC");
                    if (has_hc1) efi_out_str(ctx, "+USB_HC");
                    efi_out_str(ctx, ")");
                } else if (has_hc1) {
                    efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
                    efi_out_str(ctx, " (Legacy USB_HC only)");
                } else if (drv_bound) {
                    efi_out_str(ctx, efi_diag_state_str(DIAG_DRIVER_BOUND));
                    efi_out_str(ctx, " (No USB2_HC on handle)");
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
    if (pci_usb_total == 0U) {
        efi_out_str(ctx, "    NONE DETECTED via PCI\n");
    }

    /* ---------------------------------------------------------------------
     * [2] EFI_USB2_HC_PROTOCOL 상태 · 지원 속도 · 루트 허브 포트 실제 속도
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [2] Active USB2 Host Controllers & Root Hub Ports (EFI_USB2_HC_PROTOCOL)\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidUsb2Hc, NULL, &hc_cnt, &hc_handles);
    if (EFI_ERROR(st) || hc_handles == NULL || hc_cnt == 0U) {
        efi_out_str(ctx, "    EFI_USB2_HC_PROTOCOL : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
    } else {
        for (i = 0; i < hc_cnt && i < USB_MAX_TRACK_HCS; i++) {
            bc_usb2_hc_t    *hc  = NULL;
            bc_usb_pci_io_t *pci = NULL;
            UINT8            max_spd = 0xFFU, nports = 0U, is64 = 0U;
            UINT32           hc_state = 0xFFU;
            UINT8            p, lim;
            UINTN            conn_here = 0U;

            st = uefi_call_wrapper(bs->HandleProtocol, 3,
                                   hc_handles[i], &kGuidUsb2Hc, (VOID **)&hc);
            if (EFI_ERROR(st) || hc == NULL) {
                continue;
            }

            efi_out_str(ctx, "    [HC #");
            efi_out_u64(ctx, (UINT64)i);
            efi_out_str(ctx, "] ");

            if (!EFI_ERROR(uefi_call_wrapper(bs->HandleProtocol, 3,
                                             hc_handles[i], &kGuidPciIo, (VOID **)&pci)) &&
                pci != NULL) {
                UINTN  seg = 0, bus = 0, dev = 0, func = 0;
                UINT32 id_reg = 0, cls_reg = 0;
                uefi_call_wrapper(pci->GetLocation, 5, pci, &seg, &bus, &dev, &func);
                uefi_call_wrapper(pci->Pci.Read, 5, pci, (UINT32)2, (UINT32)0x00U, (UINTN)1, &id_reg);
                uefi_call_wrapper(pci->Pci.Read, 5, pci, (UINT32)2, (UINT32)0x08U, (UINTN)1, &cls_reg);
                out_sbdf(ctx, seg, bus, dev, func);
                efi_out_str(ctx, " (");
                efi_out_hex16(ctx, (UINT16)(id_reg & 0xFFFFU));
                efi_out_str(ctx, ":");
                efi_out_hex16(ctx, (UINT16)(id_reg >> 16));
                efi_out_str(ctx, " ");
                efi_out_str(ctx, usb_progif_str((UINT8)((cls_reg >> 8) & 0xFFU)));
                efi_out_str(ctx, ")\n");
            } else {
                efi_out_str(ctx, "Non-PCI / ACPI USB2 HC\n");
            }

            efi_out_str(ctx, "      HC Rev / State : USB ");
            efi_out_u64(ctx, (UINT64)hc->MajorRevision);
            efi_out_str(ctx, ".");
            efi_out_u64(ctx, (UINT64)hc->MinorRevision);
            efi_out_str(ctx, " | State=");
            if (hc->GetState != NULL &&
                !EFI_ERROR(uefi_call_wrapper(hc->GetState, 2, hc, &hc_state))) {
                efi_out_str(ctx, usb_hc_state_str(hc_state));
            } else {
                efi_out_str(ctx, "UNKNOWN");
            }
            efi_out_str(ctx, "\n");

            if (hc->GetCapability != NULL &&
                !EFI_ERROR(uefi_call_wrapper(hc->GetCapability, 4,
                                             hc, &max_spd, &nports, &is64))) {
                efi_out_str(ctx, "      Capability     : MaxSpeed=");
                efi_out_str(ctx, usb_hc_speed_str(max_spd));
                efi_out_str(ctx, ", RootPorts=");
                efi_out_u64(ctx, (UINT64)nports);
                efi_out_str(ctx, ", 64-bit DMA=");
                efi_out_str(ctx, is64 ? "YES\n" : "NO\n");
            } else {
                efi_out_str(ctx, "      Capability     : GetCapability() FAILED\n");
                continue;
            }

            if (hc->GetRootHubPortStatus == NULL) {
                continue;
            }

            lim = (nports > USB_MAX_HC_PORTS) ? USB_MAX_HC_PORTS : nports;
            /* UEFI 규격: GetRootHubPortStatus 포트 번호는 0-based */
            for (p = 0U; p < lim; p++) {
                bc_usb_port_status_t pst;
                SetMem(&pst, sizeof(pst), 0);

                st = uefi_call_wrapper(hc->GetRootHubPortStatus, 3, hc, p, &pst);
                if (EFI_ERROR(st)) {
                    continue;
                }

                if ((pst.PortStatus & 0x0001U) != 0U || (pst.PortStatus & 0x0008U) != 0U) {
                    UINT16 f = pst.PortStatus;
                    conn_here++;
                    efi_out_str(ctx, "      RootPort ");
                    efi_out_u64_pad2(ctx, (UINT64)p);
                    efi_out_str(ctx, "    : ");

                    if ((f & 0x0001U) != 0U) {
                        efi_out_str(ctx, "CONNECTED | Link=");
                        if ((f & 0x0800U) != 0U) {
                            efi_out_str(ctx, "SUPER (5+ Gbps)");
                            root_conn_super++;
                        } else if ((f & 0x0400U) != 0U) {
                            efi_out_str(ctx, "HIGH (480 Mbps)");
                            root_conn_high++;
                        } else if ((f & 0x0200U) != 0U) {
                            efi_out_str(ctx, "LOW (1.5 Mbps)");
                            root_conn_low++;
                        } else {
                            efi_out_str(ctx, "FULL (12 Mbps)");
                            root_conn_full++;
                        }
                    } else {
                        efi_out_str(ctx, "DISCONNECTED");
                    }

                    efi_out_str(ctx, " [");
                    efi_out_str(ctx, (f & 0x0002U) ? "EN" : "DIS");
                    if (f & 0x0100U) efi_out_str(ctx, ",PWR");
                    if (f & 0x0004U) efi_out_str(ctx, ",SUSP");
                    if (f & 0x0008U) efi_out_str(ctx, ",OC!");
                    if (f & 0x0010U) efi_out_str(ctx, ",RST");
                    efi_out_str(ctx, " raw=0x");
                    efi_out_hex16(ctx, f);
                    efi_out_str(ctx, "]\n");
                }
            }

            if (conn_here == 0U) {
                efi_out_str(ctx, "      RootPorts      : 0 active/connected ports\n");
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, hc_handles);
    }

    efilog_flush(ctx);

    /* ---------------------------------------------------------------------
     * [3] 열거된 USB 장치 및 인터페이스 (EFI_USB_IO_PROTOCOL)
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [3] Enumerated USB Interfaces / Devices (EFI_USB_IO_PROTOCOL)\n");

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidUsbIo, NULL, &io_cnt, &io_handles);
    if (EFI_ERROR(st) || io_handles == NULL || io_cnt == 0U) {
        efi_out_str(ctx, "    EFI_USB_IO_PROTOCOL  : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
    } else {
        efi_out_str(ctx, "    Total USB Interfaces : ");
        efi_out_u64(ctx, (UINT64)io_cnt);
        efi_out_str(ctx, " (");
        efi_out_str(ctx, efi_diag_state_str(DIAG_DEV_ENUM));
        efi_out_str(ctx, ")\n");

        for (i = 0; i < io_cnt; i++) {
            bc_usb_io_t       *uio = NULL;
            bc_usb_dev_desc_t  dd;
            bc_usb_cfg_desc_t  cd;
            bc_usb_if_desc_t   id;
            BOOLEAN            got_dd = FALSE, got_cd = FALSE, got_id = FALSE;
            UINT16             lang_id;
            BOOLEAN            has_bio, has_tin, has_ptr, has_abs;

            st = uefi_call_wrapper(bs->HandleProtocol, 3,
                                   io_handles[i], &kGuidUsbIo, (VOID **)&uio);
            if (EFI_ERROR(st) || uio == NULL) {
                continue;
            }

            SetMem(&dd, sizeof(dd), 0);
            SetMem(&cd, sizeof(cd), 0);
            SetMem(&id, sizeof(id), 0);

            if (uio->UsbGetDeviceDescriptor != NULL &&
                !EFI_ERROR(uefi_call_wrapper(uio->UsbGetDeviceDescriptor, 2, uio, &dd))) {
                got_dd = TRUE;
            }
            if (uio->UsbGetConfigDescriptor != NULL &&
                !EFI_ERROR(uefi_call_wrapper(uio->UsbGetConfigDescriptor, 2, uio, &cd))) {
                got_cd = TRUE;
            }
            if (uio->UsbGetInterfaceDescriptor != NULL &&
                !EFI_ERROR(uefi_call_wrapper(uio->UsbGetInterfaceDescriptor, 2, uio, &id))) {
                got_id = TRUE;
            }

            has_bio = has_protocol(bs, io_handles[i], &kGuidBlockIo);
            has_tin = has_protocol(bs, io_handles[i], &kGuidSimpleTextIn);
            has_ptr = has_protocol(bs, io_handles[i], &kGuidSimplePointer);
            has_abs = has_protocol(bs, io_handles[i], &kGuidAbsPointer);

            efi_out_str(ctx, "    [USB #");
            efi_out_u64(ctx, (UINT64)i);
            efi_out_str(ctx, "] ");
            if (got_dd) {
                efi_out_str(ctx, "VID:PID=");
                efi_out_hex16(ctx, dd.IdVendor);
                efi_out_str(ctx, ":");
                efi_out_hex16(ctx, dd.IdProduct);
                efi_out_str(ctx, " | Spec=USB ");
                out_bcd16(ctx, dd.BcdUSB);
                efi_out_str(ctx, " | Rev=");
                out_bcd16(ctx, dd.BcdDevice);
            } else {
                efi_out_str(ctx, "DeviceDescriptor FAILED");
            }
            efi_out_str(ctx, "\n");

            efi_out_str(ctx, "      Path           : ");
            out_usb_device_path(ctx, bs, io_handles[i]);
            efi_out_str(ctx, "\n");

            if (got_dd && (dd.StrManufacturer != 0U || dd.StrProduct != 0U)) {
                lang_id = usb_select_lang_id(uio);
                efi_out_str(ctx, "      Strings        : Mfg=");
                usb_out_str_desc(ctx, bs, uio, lang_id, dd.StrManufacturer);
                efi_out_str(ctx, ", Prod=");
                usb_out_str_desc(ctx, bs, uio, lang_id, dd.StrProduct);
                if (dd.StrSerialNumber != 0U) {
                    efi_out_str(ctx, ", SN=");
                    usb_out_str_desc(ctx, bs, uio, lang_id, dd.StrSerialNumber);
                }
                efi_out_str(ctx, "\n");
            }

            if (got_id) {
                efi_out_str(ctx, "      Interface      : If#");
                efi_out_u64(ctx, (UINT64)id.InterfaceNumber);
                if (got_cd) {
                    efi_out_str(ctx, " (Cfg ");
                    efi_out_u64(ctx, (UINT64)cd.ConfigurationValue);
                    efi_out_str(ctx, ", TotalIf=");
                    efi_out_u64(ctx, (UINT64)cd.NumInterfaces);
                    efi_out_str(ctx, ")");
                }
                efi_out_str(ctx, " | Class=");
                efi_out_hex8(ctx, id.InterfaceClass);
                efi_out_str(ctx, "/");
                efi_out_hex8(ctx, id.InterfaceSubClass);
                efi_out_str(ctx, "/");
                efi_out_hex8(ctx, id.InterfaceProtocol);
                efi_out_str(ctx, " [");
                efi_out_str(ctx, usb_if_class_label(id.InterfaceClass,
                                                    id.InterfaceSubClass,
                                                    id.InterfaceProtocol));
                efi_out_str(ctx, "]\n");

                /* 분류 통계 */
                if (id.InterfaceClass == 0x03U) {
                    if (id.InterfaceSubClass == 0x01U && id.InterfaceProtocol == 0x01U) dev_hid_kbd++;
                    else if (id.InterfaceSubClass == 0x01U && id.InterfaceProtocol == 0x02U) dev_hid_mse++;
                    else dev_hid_oth++;
                } else if (id.InterfaceClass == 0x08U) {
                    dev_msc++;
                    if (has_bio) dev_msc_bio++;
                } else if (id.InterfaceClass == 0x09U) {
                    dev_hub++;
                } else if (id.InterfaceClass == 0x01U) {
                    dev_audio++;
                } else {
                    dev_other++;
                }

                /* 엔드포인트 요약 */
                if (id.NumEndpoints > 0U && uio->UsbGetEndpointDescriptor != NULL) {
                    UINT8 ep_lim = (id.NumEndpoints > USB_MAX_ENDPOINTS)
                                   ? USB_MAX_ENDPOINTS : id.NumEndpoints;
                    UINT8 e;
                    efi_out_str(ctx, "      Endpoints (");
                    efi_out_u64(ctx, (UINT64)id.NumEndpoints);
                    efi_out_str(ctx, ")  : ");
                    for (e = 0U; e < ep_lim; e++) {
                        bc_usb_ep_desc_t ep;
                        SetMem(&ep, sizeof(ep), 0);
                        if (!EFI_ERROR(uefi_call_wrapper(uio->UsbGetEndpointDescriptor, 3,
                                                         uio, e, &ep))) {
                            if (e > 0U) efi_out_str(ctx, ", ");
                            efi_out_str(ctx, "0x");
                            efi_out_hex8(ctx, ep.EndpointAddress);
                            efi_out_str(ctx, "(");
                            efi_out_str(ctx, (ep.EndpointAddress & 0x80U) ? "IN/" : "OUT/");
                            efi_out_str(ctx, usb_ep_type_str(ep.Attributes));
                            efi_out_str(ctx, "/");
                            efi_out_u64(ctx, (UINT64)(ep.MaxPacketSize & 0x07FFU));
                            efi_out_str(ctx, "B)");
                        }
                    }
                    efi_out_str(ctx, "\n");
                }
            }

            /* 동일 핸들 상위 UEFI 프로토콜 바인딩 상태 */
            efi_out_str(ctx, "      Upper Protocols: ");
            if (has_bio || has_tin || has_ptr || has_abs) {
                BOOLEAN first = TRUE;
                if (has_bio) { efi_out_str(ctx, "BlockIo"); first = FALSE; }
                if (has_tin) { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "SimpleTextIn"); first = FALSE; }
                if (has_ptr) { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "SimplePointer"); first = FALSE; }
                if (has_abs) { if (!first) efi_out_str(ctx, ", "); efi_out_str(ctx, "AbsPointer"); }
                efi_out_str(ctx, " (");
                efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
                efi_out_str(ctx, ")\n");
            } else {
                efi_out_str(ctx, "NONE on handle (Raw USB_IO only)\n");
            }

            /* 외부 USB 허브인 경우 하위 포트 실제 협상 속도 조회 */
            if (got_id && id.InterfaceClass == 0x09U) {
                probe_hub_downstream_ports(ctx, uio, got_dd ? dd.BcdUSB : 0x0200U);
            }
        }
        uefi_call_wrapper(bs->FreePool, 1, io_handles);
    }

    /* ---------------------------------------------------------------------
     * [4] USB 종합 요약 및 레거시 OS 호환성 관점
     * --------------------------------------------------------------------- */
    efi_out_str(ctx, "\n  [4] USB Stack Summary & Legacy OS Outlook\n");
    efi_out_str(ctx, "    PCI HC Mix       : UHCI="); efi_out_u64(ctx, (UINT64)pci_uhci);
    efi_out_str(ctx, ", OHCI="); efi_out_u64(ctx, (UINT64)pci_ohci);
    efi_out_str(ctx, ", EHCI="); efi_out_u64(ctx, (UINT64)pci_ehci);
    efi_out_str(ctx, ", xHCI="); efi_out_u64(ctx, (UINT64)pci_xhci);
    if (pci_other > 0U) {
        efi_out_str(ctx, ", Other="); efi_out_u64(ctx, (UINT64)pci_other);
    }
    efi_out_str(ctx, " (USB2_HC bound=");
    efi_out_u64(ctx, (UINT64)pci_usb_bound_hc2);
    efi_out_str(ctx, "/");
    efi_out_u64(ctx, (UINT64)pci_usb_total);
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "    Root Port Links  : Super="); efi_out_u64(ctx, (UINT64)root_conn_super);
    efi_out_str(ctx, ", High="); efi_out_u64(ctx, (UINT64)root_conn_high);
    efi_out_str(ctx, ", Full="); efi_out_u64(ctx, (UINT64)root_conn_full);
    efi_out_str(ctx, ", Low="); efi_out_u64(ctx, (UINT64)root_conn_low);
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "    USB Interfaces   : HID(Kbd="); efi_out_u64(ctx, (UINT64)dev_hid_kbd);
    efi_out_str(ctx, ",Mse="); efi_out_u64(ctx, (UINT64)dev_hid_mse);
    efi_out_str(ctx, ",Oth="); efi_out_u64(ctx, (UINT64)dev_hid_oth);
    efi_out_str(ctx, "), Storage="); efi_out_u64(ctx, (UINT64)dev_msc);
    efi_out_str(ctx, "(BlockIo="); efi_out_u64(ctx, (UINT64)dev_msc_bio);
    efi_out_str(ctx, "), Hub="); efi_out_u64(ctx, (UINT64)dev_hub);
    if (dev_audio > 0U) {
        efi_out_str(ctx, ", Audio="); efi_out_u64(ctx, (UINT64)dev_audio);
    }
    if (dev_other > 0U) {
        efi_out_str(ctx, ", Other="); efi_out_u64(ctx, (UINT64)dev_other);
    }
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "    Legacy OS Note   : ");
    if (pci_xhci > 0U && (pci_uhci + pci_ohci + pci_ehci) == 0U) {
        efi_out_str(ctx, "xHCI-ONLY platform. UEFI USB enumeration does NOT imply DOS/WinXP USB support; extra xHCI driver or PS/2 is required.\n\n");
    } else if ((pci_uhci + pci_ohci + pci_ehci) > 0U) {
        efi_out_str(ctx, "Legacy USB HC (UHCI/OHCI/EHCI) present alongside xHCI.\n\n");
    } else {
        efi_out_str(ctx, "No PCI USB Host Controller detected.\n\n");
    }

    efilog_flush(ctx);
}