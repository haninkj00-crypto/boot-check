#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#define DISK_MAX_BLOCK_READ_SZ  65536U

/* -------------------------------------------------------------------------
 * UEFI 2.11 Block I/O, DevicePath, SimpleFileSystem, FileSystemInfo GUID
 * ------------------------------------------------------------------------- */
static EFI_GUID kGuidBlockIo =
    { 0x964E5B21, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidDevicePath =
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidSimpleFs =
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_GUID kGuidFsInfo =
    { 0x09576E93, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

/* -------------------------------------------------------------------------
 * UEFI 2.11 EFI_BLOCK_IO_PROTOCOL / EFI_FILE_SYSTEM_INFO (X64/IA32 정렬 일치)
 * ------------------------------------------------------------------------- */
typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT8   _pad_bool[3];
    UINT32  BlockSize;
    UINT32  IoAlign;
    UINT32  _pad_u64;           /* UEFI ABI: LastBlock 8-byte alignment on IA32 & X64 */
    UINT64  LastBlock;
    /* EFI_BLOCK_IO_PROTOCOL_REVISION2 (0x00020001) */
    UINT64  LowestAlignedLba;
    UINT32  LogicalBlocksPerPhysicalBlock;
    /* EFI_BLOCK_IO_PROTOCOL_REVISION3 (0x00020031) */
    UINT32  OptimalTransferLengthGranularity;
} bc_block_io_media_t;

typedef struct _BC_BLOCK_IO bc_block_io_t;

typedef EFI_STATUS (EFIAPI *BC_BLOCK_READ_FN)(
    bc_block_io_t *This,
    UINT32         MediaId,
    UINT64         Lba,
    UINTN          BufferSize,
    VOID          *Buffer
);

struct _BC_BLOCK_IO {
    UINT64               Revision;
    bc_block_io_media_t *Media;
    VOID                *Reset;
    BC_BLOCK_READ_FN     ReadBlocks;
    VOID                *WriteBlocks;
    VOID                *FlushBlocks;
};

typedef struct {
    UINT64  Size;
    BOOLEAN ReadOnly;
    UINT8   _pad_u64[7];        /* UEFI ABI: VolumeSize at offset 16 on IA32 & X64 */
    UINT64  VolumeSize;
    UINT64  FreeSpace;
    UINT32  BlockSize;
    CHAR16  VolumeLabel[1];
} bc_fs_info_t;

/* -------------------------------------------------------------------------
 * DevicePath 파싱 결과 구조체
 * ------------------------------------------------------------------------- */
typedef enum {
    XPORT_UNKNOWN = 0,
    XPORT_NVME    = 1,
    XPORT_SATA    = 2,
    XPORT_USB     = 3,
    XPORT_ATAPI   = 4,
    XPORT_SCSI    = 5,
    XPORT_SD_MMC  = 6,
    XPORT_RAMDISK = 7
} disk_xport_t;

typedef struct {
    disk_xport_t xport;
    BOOLEAN      has_hd_node;
    UINT32       part_num;
    UINT64       part_start;
    UINT64       part_size;
    UINT8        mbr_type;      /* 0x01=MBR, 0x02=GPT */
    UINT8        sig_type;      /* 0x01=MBR32, 0x02=GUID */
    UINT8        sig[16];
    BOOLEAN      is_cdrom_node;
    UINT32       cd_boot_entry;
} disk_dp_info_t;

/* -------------------------------------------------------------------------
 * 내부 유틸리티 (리틀엔디안 읽기, GUID 출력, DevicePath 해석)
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

static const char *xport_name(disk_xport_t x)
{
    switch (x) {
        case XPORT_NVME:    return "NVMe";
        case XPORT_SATA:    return "SATA";
        case XPORT_USB:     return "USB";
        case XPORT_ATAPI:   return "ATAPI/IDE";
        case XPORT_SCSI:    return "SCSI";
        case XPORT_SD_MMC:  return "SD/eMMC";
        case XPORT_RAMDISK: return "RAMDisk/Media";
        default:            return "Other/Virtual";
    }
}

static VOID out_guid_bytes(efi_diag_ctx_t *ctx, const UINT8 *g)
{
    UINT32 d1 = rd_u32(g);
    UINT16 d2 = rd_u16(g + 4);
    UINT16 d3 = rd_u16(g + 6);
    UINTN  k;

    efi_out_hex32(ctx, d1); efi_out_str(ctx, "-");
    efi_out_hex16(ctx, d2); efi_out_str(ctx, "-");
    efi_out_hex16(ctx, d3); efi_out_str(ctx, "-");
    efi_out_hex8(ctx, g[8]); efi_out_hex8(ctx, g[9]); efi_out_str(ctx, "-");
    for (k = 10U; k < 16U; k++) {
        efi_out_hex8(ctx, g[k]);
    }
}

static VOID parse_and_print_disk_dp(efi_diag_ctx_t *ctx,
                                    EFI_BOOT_SERVICES *bs,
                                    EFI_HANDLE h,
                                    disk_dp_info_t *info)
{
    const UINT8 *dp = NULL;
    UINTN        guard = 0;
    BOOLEAN      printed = FALSE;
    EFI_STATUS   st;

    SetMem(info, sizeof(*info), 0);

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
        } else if (type == 0x03U && sub == 0x01U && len >= 8U) {
            info->xport = XPORT_ATAPI;
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "ATAPI(");
            efi_out_str(ctx, dp[4] ? "Sec/" : "Pri/");
            efi_out_str(ctx, dp[5] ? "Slv)" : "Mst)");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x02U && len >= 8U) {
            info->xport = XPORT_SCSI;
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "SCSI(Pun=");
            efi_out_u64(ctx, (UINT64)rd_u16(dp + 4));
            efi_out_str(ctx, ",Lun=");
            efi_out_u64(ctx, (UINT64)rd_u16(dp + 6));
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x05U && len >= 6U) {
            info->xport = XPORT_USB;
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "USB(P");
            efi_out_u64(ctx, (UINT64)dp[4]);
            efi_out_str(ctx, ",If");
            efi_out_u64(ctx, (UINT64)dp[5]);
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x12U && len >= 10U) {
            info->xport = XPORT_SATA;
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "SATA(Port=");
            efi_out_u64(ctx, (UINT64)rd_u16(dp + 4));
            efi_out_str(ctx, ",Lun=");
            efi_out_u64(ctx, (UINT64)rd_u16(dp + 8));
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && sub == 0x17U && len >= 16U) {
            info->xport = XPORT_NVME;
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "NVMe(NS=");
            efi_out_u64(ctx, (UINT64)rd_u32(dp + 4));
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x03U && (sub == 0x1AU || sub == 0x1DU) && len >= 5U) {
            info->xport = XPORT_SD_MMC;
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, (sub == 0x1AU) ? "SD(Slot=" : "eMMC(Slot=");
            efi_out_u64(ctx, (UINT64)dp[4]);
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x04U && sub == 0x01U && len >= 42U) {
            UINTN k;
            info->has_hd_node = TRUE;
            info->part_num    = rd_u32(dp + 4);
            info->part_start  = rd_u64(dp + 8);
            info->part_size   = rd_u64(dp + 16);
            info->mbr_type    = dp[40];
            info->sig_type    = dp[41];
            for (k = 0U; k < 16U; k++) {
                info->sig[k] = dp[24U + k];
            }
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "HD(Part");
            efi_out_u64(ctx, (UINT64)info->part_num);
            efi_out_str(ctx, ",");
            efi_out_str(ctx, (info->mbr_type == 0x02U) ? "GPT" :
                             ((info->mbr_type == 0x01U) ? "MBR" : "Fmt?"));
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x04U && sub == 0x02U && len >= 24U) {
            info->is_cdrom_node = TRUE;
            info->cd_boot_entry = rd_u32(dp + 4);
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "CDROM(Entry=");
            efi_out_u64(ctx, (UINT64)info->cd_boot_entry);
            efi_out_str(ctx, ")");
            printed = TRUE;
        } else if (type == 0x04U && sub == 0x09U) {
            if (info->xport == XPORT_UNKNOWN) {
                info->xport = XPORT_RAMDISK;
            }
            if (printed) efi_out_str(ctx, " -> ");
            efi_out_str(ctx, "RAMDisk");
            printed = TRUE;
        }

        dp += len;
    }

    if (!printed) {
        efi_out_str(ctx, "(unparsed DevicePath)");
    }
}

/* -------------------------------------------------------------------------
 * 물리 디스크 LBA 0 읽기 전용 검증 (IoAlign 준수, TRANSFER SUCCESS 판정)
 * ------------------------------------------------------------------------- */
static VOID probe_whole_disk_lba0(efi_diag_ctx_t *ctx,
                                  EFI_BOOT_SERVICES *bs,
                                  bc_block_io_t *bio)
{
    bc_block_io_media_t *m = bio->Media;
    UINTN      bsz, align, alloc_sz;
    UINT8     *raw_buf = NULL;
    UINT8     *buf = NULL;
    EFI_STATUS st;

    efi_out_str(ctx, "      LBA 0 Probe    : ");

    if (bio->ReadBlocks == NULL || !m->MediaPresent) {
        efi_out_str(ctx, "SKIPPED (No media or ReadBlocks unavailable)\n");
        return;
    }
    if (m->BlockSize < 512U || m->BlockSize > DISK_MAX_BLOCK_READ_SZ) {
        efi_out_str(ctx, "SKIPPED (Unsupported BlockSize=");
        efi_out_u64(ctx, (UINT64)m->BlockSize);
        efi_out_str(ctx, " B)\n");
        return;
    }

    bsz   = (UINTN)m->BlockSize;
    align = (m->IoAlign > 1U) ? (UINTN)m->IoAlign : 1U;
    if (align > 4096U) {
        align = 4096U;
    }
    alloc_sz = bsz + align;

    st = uefi_call_wrapper(bs->AllocatePool, 3, EfiLoaderData, alloc_sz, (VOID **)&raw_buf);
    if (EFI_ERROR(st) || raw_buf == NULL) {
        efi_out_str(ctx, "SKIPPED (AllocatePool failed)\n");
        return;
    }

    {
        UINTN addr = (UINTN)raw_buf;
        UINTN rem  = addr % align;
        buf = (rem == 0U) ? raw_buf : (raw_buf + (align - rem));
    }

    efilog_flush(ctx);

    st = uefi_call_wrapper(bio->ReadBlocks, 5, bio, m->MediaId, 0ULL, bsz, buf);
    if (EFI_ERROR(st)) {
        efi_out_str(ctx, "READ FAILED - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n");
    } else {
        BOOLEAN has_55aa = (buf[510] == 0x55U && buf[511] == 0xAAU) ? TRUE : FALSE;
        BOOLEAN has_gpt_ee = FALSE;
        UINTN   mbr_parts = 0U;
        UINTN   p;

        if (has_55aa) {
            for (p = 0U; p < 4U; p++) {
                UINT8 ptype = buf[446U + (p * 16U) + 4U];
                if (ptype != 0x00U) {
                    mbr_parts++;
                    if (ptype == 0xEEU) {
                        has_gpt_ee = TRUE;
                    }
                }
            }
        }

        efi_out_str(ctx, efi_diag_state_str(DIAG_XFER_OK));
        if (has_gpt_ee) {
            efi_out_str(ctx, " | Sig=0x55AA (Protective MBR / GPT)");
        } else if (has_55aa) {
            efi_out_str(ctx, " | Sig=0x55AA (MBR/VBR, ");
            efi_out_u64(ctx, (UINT64)mbr_parts);
            efi_out_str(ctx, " MBR entries)");
        } else {
            efi_out_str(ctx, " | No 0x55AA signature (Raw / ISO / Unformatted)");
        }
        efi_out_str(ctx, "\n");
    }

    uefi_call_wrapper(bs->FreePool, 1, raw_buf);
}

/* -------------------------------------------------------------------------
 * SimpleFileSystem 볼륨 정보 조회 (미디어 장착 시에만 읽기 전용 OpenVolume)
 * ------------------------------------------------------------------------- */
static BOOLEAN inspect_simple_fs(efi_diag_ctx_t *ctx,
                                 EFI_BOOT_SERVICES *bs,
                                 EFI_HANDLE h,
                                 BOOLEAN media_present)
{
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_FILE_HANDLE        root = NULL;
    UINT8                 *info_buf = NULL;
    UINTN                  info_sz = 0U;
    EFI_STATUS             st;

    st = uefi_call_wrapper(bs->HandleProtocol, 3, h, &kGuidSimpleFs, (VOID **)&fs);
    if (EFI_ERROR(st) || fs == NULL) {
        return FALSE;
    }

    efi_out_str(ctx, "      FileSystem     : ");
    if (!media_present) {
        efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
        efi_out_str(ctx, " (Skipped OpenVolume: No media)\n");
        return TRUE;
    }

    efilog_flush(ctx);
    st = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(st) || root == NULL) {
        efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
        efi_out_str(ctx, " (OpenVolume failed: ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
        return TRUE;
    }

    st = uefi_call_wrapper(root->GetInfo, 4, root, &kGuidFsInfo, &info_sz, NULL);
    if (st == EFI_BUFFER_TOO_SMALL && info_sz >= 38U && info_sz <= 4096U) {
        UINTN alloc_sz = (info_sz < sizeof(bc_fs_info_t)) ? sizeof(bc_fs_info_t) : info_sz;
        alloc_sz += sizeof(CHAR16); /* 볼륨 레이블 NUL 종단 안전 여유 */

        st = uefi_call_wrapper(bs->AllocatePool, 3, EfiLoaderData, alloc_sz, (VOID **)&info_buf);
        if (!EFI_ERROR(st) && info_buf != NULL) {
            UINTN req_sz = alloc_sz - sizeof(CHAR16);
            bc_zero_mem(info_buf, alloc_sz);
            st = uefi_call_wrapper(root->GetInfo, 4, root, &kGuidFsInfo, &req_sz, info_buf);
            if (!EFI_ERROR(st)) {
                const bc_fs_info_t *fi = (const bc_fs_info_t *)info_buf;
                efi_out_str(ctx, "MOUNTABLE (SimpleFS) | Label=\"");
                efi_out_wstr(ctx, fi->VolumeLabel);
                efi_out_str(ctx, "\" | Vol=");
                efi_out_size_human(ctx, fi->VolumeSize);
                efi_out_str(ctx, ", Free=");
                efi_out_size_human(ctx, fi->FreeSpace);
                efi_out_str(ctx, ", Clus=");
                efi_out_u64(ctx, (UINT64)fi->BlockSize);
                efi_out_str(ctx, "B");
                if (fi->ReadOnly) {
                    efi_out_str(ctx, " [RO]");
                }
                efi_out_str(ctx, "\n");
            } else {
                efi_out_str(ctx, "MOUNTABLE (GetInfo failed: ");
                efi_out_status(ctx, st);
                efi_out_str(ctx, ")\n");
            }
            uefi_call_wrapper(bs->FreePool, 1, info_buf);
        } else {
            efi_out_str(ctx, "MOUNTABLE (Pool alloc failed)\n");
        }
    } else {
        efi_out_str(ctx, "MOUNTABLE (FS Info unavailable: ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n");
    }

    uefi_call_wrapper(root->Close, 1, root);
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Step 6 메인 엔트리: Block I/O · 파티션 · 파일시스템 볼륨
 * ------------------------------------------------------------------------- */
void efi_run_step6_storage_fs(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    EFI_HANDLE        *handles = NULL;
    UINTN              cnt = 0U;
    UINTN              i;
    EFI_STATUS         st;

    UINTN whole_cnt = 0U, part_cnt = 0U, fs_cnt = 0U;
    UINTN nvme_cnt = 0U, sata_cnt = 0U, usb_cnt = 0U, other_cnt = 0U;
    UINTN gpt_parts = 0U, mbr_parts = 0U;

    efi_out_str(ctx, "=== [Step 6] Block I/O, Partitions & File Systems ===\n");
    efi_out_str(ctx, "  Policy : UEFI Block I/O + SimpleFileSystem (No legacy INT 13h)\n");
    efilog_flush(ctx);

    if (bs == NULL || bs->LocateHandleBuffer == NULL) {
        efi_out_str(ctx, "  LocateHandleBuffer() : NOT EXPOSED\n\n");
        efilog_flush(ctx);
        return;
    }

    st = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
                           ByProtocol, &kGuidBlockIo, NULL, &cnt, &handles);
    if (EFI_ERROR(st) || handles == NULL || cnt == 0U) {
        efi_out_str(ctx, "  EFI_BLOCK_IO_PROTOCOL : ");
        efi_out_str(ctx, efi_diag_state_str(DIAG_NOT_EXPOSED));
        efi_out_str(ctx, " (");
        efi_out_status(ctx, st);
        efi_out_str(ctx, ")\n\n");
        efilog_flush(ctx);
        return;
    }

    efi_out_str(ctx, "  Total Block I/O Handles : ");
    efi_out_u64(ctx, (UINT64)cnt);
    efi_out_str(ctx, " (");
    efi_out_str(ctx, efi_diag_state_str(DIAG_PROTO_AVAIL));
    efi_out_str(ctx, ")\n");

    for (i = 0U; i < cnt; i++) {
        bc_block_io_t       *bio = NULL;
        bc_block_io_media_t *m;
        disk_dp_info_t       dpi;
        BOOLEAN              is_boot;
        UINT64               total_blocks = 0ULL;
        UINT64               total_bytes  = 0ULL;
        UINT32               phys_bsz     = 0U;

        st = uefi_call_wrapper(bs->HandleProtocol, 3,
                               handles[i], &kGuidBlockIo, (VOID **)&bio);
        if (EFI_ERROR(st) || bio == NULL || bio->Media == NULL) {
            continue;
        }

        m       = bio->Media;
        is_boot = (handles[i] == ctx->boot_dev_handle) ? TRUE : FALSE;

        efi_out_str(ctx, "\n    [BLK #");
        efi_out_u64(ctx, (UINT64)i);
        efi_out_str(ctx, "] ");
        efi_out_str(ctx, m->LogicalPartition ? "PARTITION " : "WHOLE DISK");
        if (is_boot) {
            efi_out_str(ctx, " [BOOT VOLUME]");
        }
        efi_out_str(ctx, "\n");

        efi_out_str(ctx, "      Path           : ");
        parse_and_print_disk_dp(ctx, bs, handles[i], &dpi);
        efi_out_str(ctx, "\n");

        if (m->LogicalPartition) {
            part_cnt++;
            if (dpi.has_hd_node) {
                if (dpi.mbr_type == 0x02U) gpt_parts++;
                else if (dpi.mbr_type == 0x01U) mbr_parts++;
            }
        } else {
            whole_cnt++;
            if      (dpi.xport == XPORT_NVME) nvme_cnt++;
            else if (dpi.xport == XPORT_SATA) sata_cnt++;
            else if (dpi.xport == XPORT_USB)  usb_cnt++;
            else                              other_cnt++;
        }

        /* 64비트 나눗셈 없이 오버플로 검사 (X64/IA32 공용) */
        if (m->MediaPresent && m->BlockSize > 0U && m->LastBlock < 0xFFFFFFFFFFFFFFFFULL) {
            total_blocks = m->LastBlock + 1ULL;
            if (total_blocks <= (0xFFFFFFFFFFFFFFFFULL >> 16)) {
                total_bytes = total_blocks * (UINT64)m->BlockSize;
            }
        }

        /* Rev 2+ 물리 섹터 크기 산출 */
        phys_bsz = m->BlockSize;
        if (bio->Revision >= 0x00020001ULL && m->LogicalBlocksPerPhysicalBlock > 1U) {
            phys_bsz = m->BlockSize * m->LogicalBlocksPerPhysicalBlock;
        }

        efi_out_str(ctx, "      Transport/Flags: ");
        efi_out_str(ctx, xport_name(dpi.xport));
        efi_out_str(ctx, " | MediaId=");
        efi_out_u64(ctx, (UINT64)m->MediaId);
        efi_out_str(ctx, " [");
        efi_out_str(ctx, m->MediaPresent ? "PRESENT" : "NO_MEDIA");
        if (m->RemovableMedia) efi_out_str(ctx, ",REMOVABLE");
        if (m->ReadOnly)       efi_out_str(ctx, ",RO");
        if (m->WriteCaching)   efi_out_str(ctx, ",WC");
        efi_out_str(ctx, "]\n");

        efi_out_str(ctx, "      Geometry       : ");
        if (!m->MediaPresent) {
            efi_out_str(ctx, "No media inserted (BlockSize=");
            efi_out_u64(ctx, (UINT64)m->BlockSize);
            efi_out_str(ctx, " B)\n");
        } else {
            efi_out_size_human(ctx, total_bytes);
            efi_out_str(ctx, " | LBA 0..");
            efi_out_u64(ctx, m->LastBlock);
            efi_out_str(ctx, " | LogSec=");
            efi_out_u64(ctx, (UINT64)m->BlockSize);
            efi_out_str(ctx, "B, PhysSec=");
            efi_out_u64(ctx, (UINT64)phys_bsz);
            efi_out_str(ctx, "B, Align=");
            efi_out_u64(ctx, (UINT64)m->IoAlign);
            efi_out_str(ctx, "\n");
        }

        /* 파티션 상세 (HardDrive Media DevicePath 노드) */
        if (dpi.has_hd_node) {
            efi_out_str(ctx, "      Partition Info : #");
            efi_out_u64(ctx, (UINT64)dpi.part_num);
            efi_out_str(ctx, " (");
            efi_out_str(ctx, (dpi.mbr_type == 0x02U) ? "GPT" :
                             ((dpi.mbr_type == 0x01U) ? "MBR" : "Other"));
            efi_out_str(ctx, ") | StartLBA=");
            efi_out_u64(ctx, dpi.part_start);
            efi_out_str(ctx, ", Sectors=");
            efi_out_u64(ctx, dpi.part_size);
            if (dpi.sig_type == 0x02U) {
                efi_out_str(ctx, "\n      Partition GUID : ");
                out_guid_bytes(ctx, dpi.sig);
            } else if (dpi.sig_type == 0x01U) {
                efi_out_str(ctx, " | DiskSig=0x");
                efi_out_hex32(ctx, rd_u32(dpi.sig));
            }
            efi_out_str(ctx, "\n");
        }

        /* 물리 디스크 LBA 0 읽기 검증 */
        if (!m->LogicalPartition) {
            probe_whole_disk_lba0(ctx, bs, bio);
        }

        /* SimpleFileSystem 마운트 볼륨 조회 */
        if (inspect_simple_fs(ctx, bs, handles[i], m->MediaPresent)) {
            fs_cnt++;
        }
    }

    uefi_call_wrapper(bs->FreePool, 1, handles);

    /* 종합 요약 */
    efi_out_str(ctx, "\n  [Storage Summary]\n");
    efi_out_str(ctx, "    Whole Disks      : ");
    efi_out_u64(ctx, (UINT64)whole_cnt);
    efi_out_str(ctx, " (NVMe="); efi_out_u64(ctx, (UINT64)nvme_cnt);
    efi_out_str(ctx, ", SATA="); efi_out_u64(ctx, (UINT64)sata_cnt);
    efi_out_str(ctx, ", USB=");  efi_out_u64(ctx, (UINT64)usb_cnt);
    if (other_cnt > 0U) {
        efi_out_str(ctx, ", Other="); efi_out_u64(ctx, (UINT64)other_cnt);
    }
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "    Partitions       : ");
    efi_out_u64(ctx, (UINT64)part_cnt);
    efi_out_str(ctx, " (GPT="); efi_out_u64(ctx, (UINT64)gpt_parts);
    efi_out_str(ctx, ", MBR="); efi_out_u64(ctx, (UINT64)mbr_parts);
    efi_out_str(ctx, ") | SimpleFileSystem Volumes: ");
    efi_out_u64(ctx, (UINT64)fs_cnt);
    efi_out_str(ctx, "\n\n");

    efilog_flush(ctx);
}