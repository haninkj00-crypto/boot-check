#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

#define MEM_KNOWN_TYPES 15U
#define MEM_BUCKET_CNT  16U /* 0..14 표준 타입 + 15 기타/OEM 타입 */

static const char *mem_type_name(UINT32 type)
{
    switch (type) {
        case 0:  return "Reserved           ";
        case 1:  return "LoaderCode         ";
        case 2:  return "LoaderData         ";
        case 3:  return "BootServicesCode   ";
        case 4:  return "BootServicesData   ";
        case 5:  return "RuntimeServicesCode";
        case 6:  return "RuntimeServicesData";
        case 7:  return "ConventionalMemory ";
        case 8:  return "UnusableMemory     ";
        case 9:  return "ACPIReclaimMemory  ";
        case 10: return "ACPIMemoryNVS      ";
        case 11: return "MemoryMappedIO     ";
        case 12: return "MemoryMappedIOPort ";
        case 13: return "PalCode            ";
        case 14: return "PersistentMemory   ";
        default: return "OEM/Other          ";
    }
}

static const char *mem_type_short(UINT32 type)
{
    switch (type) {
        case 0:  return "Reserved";
        case 1:  return "LoaderCode";
        case 2:  return "LoaderData";
        case 3:  return "BootSvcCode";
        case 4:  return "BootSvcData";
        case 5:  return "RuntimeCode";
        case 6:  return "RuntimeData";
        case 7:  return "Conventional";
        case 8:  return "Unusable";
        case 9:  return "ACPIReclaim";
        case 10: return "ACPINVS";
        case 11: return "MMIO";
        case 12: return "MMIOPort";
        case 13: return "PalCode";
        case 14: return "Persistent";
        default: return "OEM/Other";
    }
}

static BOOLEAN is_os_usable_ram(UINT32 type)
{
    /* ExitBootServices 이후 OS가 회수해 사용할 수 있는 일반 RAM 계통 */
    return (type == 1U || type == 2U || type == 3U || type == 4U || type == 7U)
           ? TRUE : FALSE;
}

static BOOLEAN is_firmware_ram(UINT32 type)
{
    return (type == 0U || type == 5U || type == 6U || type == 9U || type == 10U)
           ? TRUE : FALSE;
}

/* -------------------------------------------------------------------------
 * 2-pass GetMemoryMap() 안전 수집 (할당 시 디스크립터 증가 대비 재시도)
 * ------------------------------------------------------------------------- */
static EFI_STATUS fetch_memory_map(EFI_BOOT_SERVICES *bs,
                                   UINT8 **buf_out,
                                   UINTN *map_size_out,
                                   UINTN *map_key_out,
                                   UINTN *desc_size_out,
                                   UINT32 *desc_ver_out)
{
    UINT8     *buf = NULL;
    UINTN      map_size = 0;
    UINTN      alloc_size = 0;
    UINTN      map_key = 0;
    UINTN      desc_size = 0;
    UINT32     desc_ver = 0;
    EFI_STATUS st;
    UINTN      attempt;

    *buf_out       = NULL;
    *map_size_out  = 0;
    *map_key_out   = 0;
    *desc_size_out = 0;
    *desc_ver_out  = 0;

    if (bs == NULL || bs->GetMemoryMap == NULL ||
        bs->AllocatePool == NULL || bs->FreePool == NULL) {
        return EFI_UNSUPPORTED;
    }

    /* 1차 조회: 필요 버퍼 크기 파악 */
    st = uefi_call_wrapper(bs->GetMemoryMap, 5,
                           &map_size, NULL, &map_key, &desc_size, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL) {
        return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;
    }
    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    }

    for (attempt = 0; attempt < 4U; attempt++) {
        alloc_size = map_size + (desc_size * 8U);
        st = uefi_call_wrapper(bs->AllocatePool, 3,
                               EfiLoaderData, alloc_size, (VOID **)&buf);
        if (EFI_ERROR(st) || buf == NULL) {
            return EFI_ERROR(st) ? st : EFI_OUT_OF_RESOURCES;
        }

        map_size = alloc_size;
        st = uefi_call_wrapper(bs->GetMemoryMap, 5,
                               &map_size, (EFI_MEMORY_DESCRIPTOR *)buf,
                               &map_key, &desc_size, &desc_ver);
        if (!EFI_ERROR(st)) {
            *buf_out       = buf;
            *map_size_out  = map_size;
            *map_key_out   = map_key;
            *desc_size_out = desc_size;
            *desc_ver_out  = desc_ver;
            return EFI_SUCCESS;
        }

        uefi_call_wrapper(bs->FreePool, 1, buf);
        buf = NULL;

        if (st != EFI_BUFFER_TOO_SMALL) {
            return st;
        }
    }

    return EFI_BUFFER_TOO_SMALL;
}

/* -------------------------------------------------------------------------
 * Step 1 메인 엔트리: UEFI Memory Map & Low-1MB 요약
 * ------------------------------------------------------------------------- */
void efi_run_step1_memory_map(efi_diag_ctx_t *ctx)
{
    EFI_BOOT_SERVICES *bs = ctx->st->BootServices;
    UINT8             *map_buf = NULL;
    UINTN              map_size = 0;
    UINTN              map_key = 0;
    UINTN              desc_size = 0;
    UINT32             desc_ver = 0;
    EFI_STATUS         st;

    UINT64 type_cnt[MEM_BUCKET_CNT];
    UINT64 type_bytes[MEM_BUCKET_CNT];
    UINT64 type_below4g[MEM_BUCKET_CNT];
    UINT64 type_above4g[MEM_BUCKET_CNT];

    UINT64 total_entries = 0;
    UINT64 usable_below4g = 0, usable_above4g = 0;
    UINT64 fw_below4g = 0, fw_above4g = 0;
    UINT64 mmio_below4g = 0, mmio_above4g = 0;
    UINT64 highest_ram_end = 0;

    UINT64 low640_conv = 0;
    UINT64 low640_reclaim = 0;
    UINTN  low_printed = 0;
    UINTN  low_total = 0;
    UINTN  off;
    UINT32 t;

    SetMem(type_cnt,     sizeof(type_cnt),     0);
    SetMem(type_bytes,   sizeof(type_bytes),   0);
    SetMem(type_below4g, sizeof(type_below4g), 0);
    SetMem(type_above4g, sizeof(type_above4g), 0);

    efi_out_str(ctx, "=== [Step 1] UEFI Memory Map & Low-1MB Summary ===\n");

    st = fetch_memory_map(bs, &map_buf, &map_size, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(st) || map_buf == NULL || desc_size == 0) {
        efi_out_str(ctx, "  GetMemoryMap() : FAILED - ");
        efi_out_status(ctx, st);
        efi_out_str(ctx, "\n\n");
        efilog_flush(ctx);
        return;
    }

    /* 1차 순회: 타입별 / 4GB 경계별 집계 */
    for (off = 0; off + desc_size <= map_size; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map_buf + off);
        UINT32 bucket = (d->Type < MEM_KNOWN_TYPES) ? d->Type : MEM_KNOWN_TYPES;
        UINT64 start  = (UINT64)d->PhysicalStart;
        UINT64 pages  = d->NumberOfPages;
        UINT64 bytes;
        UINT64 end_excl;
        UINT64 b4 = 0, a4 = 0;

        total_entries++;
        if (pages == 0 || pages > (0xFFFFFFFFFFFFFFFFULL >> 12)) {
            continue;
        }
        bytes = pages << 12;
        end_excl = (start + bytes < start) ? 0xFFFFFFFFFFFFFFFFULL : (start + bytes);

        if (start >= 0x100000000ULL) {
            a4 = bytes;
        } else if (end_excl <= 0x100000000ULL) {
            b4 = bytes;
        } else {
            b4 = 0x100000000ULL - start;
            a4 = bytes - b4;
        }

        type_cnt[bucket]++;
        type_bytes[bucket]   += bytes;
        type_below4g[bucket] += b4;
        type_above4g[bucket] += a4;

        if (is_os_usable_ram(d->Type)) {
            usable_below4g += b4;
            usable_above4g += a4;
            if (end_excl > 0 && (end_excl - 1ULL) > highest_ram_end) {
                highest_ram_end = end_excl - 1ULL;
            }
        } else if (is_firmware_ram(d->Type)) {
            fw_below4g += b4;
            fw_above4g += a4;
        } else if (d->Type == 11U || d->Type == 12U) {
            mmio_below4g += b4;
            mmio_above4g += a4;
        }

        /* 640KB 미만(0x00000 - 0x9FFFF) 가용 페이지 집계 (AP SIPI 등 참고) */
        if (start < 0xA0000ULL) {
            UINT64 ov_end = (end_excl > 0xA0000ULL) ? 0xA0000ULL : end_excl;
            UINT64 ov_sz  = ov_end - start;
            if (d->Type == 7U) {
                low640_conv += ov_sz;
            } else if (is_os_usable_ram(d->Type)) {
                low640_reclaim += ov_sz;
            }
        }
    }

    /* [1] 메타데이터 */
    efi_out_str(ctx, "  [1] Memory Map Metadata\n");
    efi_out_str(ctx, "    Entries / DescSz : ");
    efi_out_u64(ctx, total_entries);
    efi_out_str(ctx, " entries, DescSize=");
    efi_out_u64(ctx, (UINT64)desc_size);
    efi_out_str(ctx, " B, DescVer=");
    efi_out_u64(ctx, (UINT64)desc_ver);
    efi_out_str(ctx, "\n");

    /* [2] 타입별 용량 요약 */
    efi_out_str(ctx, "  [2] Memory Summary by UEFI Type\n");
    for (t = 0; t < MEM_BUCKET_CNT; t++) {
        if (type_cnt[t] == 0) {
            continue;
        }
        efi_out_str(ctx, "    ");
        efi_out_str(ctx, mem_type_name(t));
        efi_out_str(ctx, " : ");
        efi_out_size_human(ctx, type_bytes[t]);
        efi_out_str(ctx, " (");
        efi_out_u64(ctx, type_cnt[t]);
        efi_out_str(ctx, " ent");
        if (type_above4g[t] > 0) {
            efi_out_str(ctx, ", <4G=");
            efi_out_size_human(ctx, type_below4g[t]);
            efi_out_str(ctx, ", >=4G=");
            efi_out_size_human(ctx, type_above4g[t]);
        }
        efi_out_str(ctx, ")\n");
    }

    /* [3] 4GB 경계 기준 전체 분포 */
    efi_out_str(ctx, "  [3] Address Space Distribution (<4GB vs >=4GB)\n");
    efi_out_str(ctx, "    OS-Usable RAM    : ");
    efi_out_size_human(ctx, usable_below4g + usable_above4g);
    efi_out_str(ctx, " (<4GB=");
    efi_out_size_human(ctx, usable_below4g);
    efi_out_str(ctx, ", >=4GB=");
    efi_out_size_human(ctx, usable_above4g);
    efi_out_str(ctx, ")\n");

    efi_out_str(ctx, "    Firmware/ACPI    : ");
    efi_out_size_human(ctx, fw_below4g + fw_above4g);
    efi_out_str(ctx, " (<4GB=");
    efi_out_size_human(ctx, fw_below4g);
    efi_out_str(ctx, ", >=4GB=");
    efi_out_size_human(ctx, fw_above4g);
    efi_out_str(ctx, ")\n");

    if (mmio_below4g + mmio_above4g > 0) {
        efi_out_str(ctx, "    Reported MMIO    : ");
        efi_out_size_human(ctx, mmio_below4g + mmio_above4g);
        efi_out_str(ctx, " (<4GB=");
        efi_out_size_human(ctx, mmio_below4g);
        efi_out_str(ctx, ", >=4GB=");
        efi_out_size_human(ctx, mmio_above4g);
        efi_out_str(ctx, ")\n");
    }

    efi_out_str(ctx, "    Highest RAM Addr : 0x");
    efi_out_hex64(ctx, highest_ram_end);
    efi_out_str(ctx, "\n");

    /* [4] 1MB 미만(0x00000 - 0xFFFFF) 영역 스냅샷 */
    efi_out_str(ctx, "  [4] Low-1MB Snapshot (0x00000-0xFFFFF; UEFI state, NOT DOS ConvMem)\n");
    efi_out_str(ctx, "    <640KB Free Conv : ");
    efi_out_size_human(ctx, low640_conv);
    efi_out_str(ctx, " | Reclaimable Boot/Loader: ");
    efi_out_size_human(ctx, low640_reclaim);
    efi_out_str(ctx, "\n");

    for (off = 0; off + desc_size <= map_size; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map_buf + off);
        UINT64 start = (UINT64)d->PhysicalStart;
        UINT64 pages = d->NumberOfPages;
        UINT64 bytes, end_excl, clip_end;

        if (start >= 0x100000ULL || pages == 0 || pages > (0xFFFFFFFFFFFFFFFFULL >> 12)) {
            continue;
        }
        bytes    = pages << 12;
        end_excl = (start + bytes < start) ? 0xFFFFFFFFFFFFFFFFULL : (start + bytes);
        clip_end = (end_excl > 0x100000ULL) ? 0x100000ULL : end_excl;

        low_total++;
        if (low_printed < 24U) {
            efi_out_str(ctx, "    - 0x");
            efi_out_hex32(ctx, (UINT32)start);
            efi_out_str(ctx, " - 0x");
            efi_out_hex32(ctx, (UINT32)(clip_end - 1ULL));
            efi_out_str(ctx, " : ");
            efi_out_str(ctx, mem_type_short(d->Type));
            efi_out_str(ctx, " (");
            efi_out_size_human(ctx, clip_end - start);
            if (end_excl > 0x100000ULL) {
                efi_out_str(ctx, ", extends >1MB");
            }
            efi_out_str(ctx, ")\n");
            low_printed++;
        }
    }
    if (low_total == 0) {
        efi_out_str(ctx, "    (No descriptors below 1MB reported)\n");
    } else if (low_total > low_printed) {
        efi_out_str(ctx, "    ... (");
        efi_out_u64(ctx, (UINT64)(low_total - low_printed));
        efi_out_str(ctx, " more low-1MB entries omitted)\n");
    }

    uefi_call_wrapper(bs->FreePool, 1, map_buf);
    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}