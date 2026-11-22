#include "efictx.h"
#include "efilog.h"
#include "efitxt.h"

/* -------------------------------------------------------------------------
 * 저수준 x86/x64 CPUID / RDTSC / RDMSR 헬퍼 (IA32 -fpic EBX 보존 대응)
 * ------------------------------------------------------------------------- */
static VOID do_cpuid(UINT32 leaf, UINT32 subleaf,
                     UINT32 *eax, UINT32 *ebx, UINT32 *ecx, UINT32 *edx)
{
    UINT32 a = leaf, b = 0U, c = subleaf, d = 0U;

#if EFI_IS_X64
    __asm__ __volatile__ (
        "cpuid"
        : "+a"(a), "=b"(b), "+c"(c), "=d"(d)
    );
#else
    __asm__ __volatile__ (
        "xchgl %%ebx, %1\n\t"
        "cpuid\n\t"
        "xchgl %%ebx, %1"
        : "+a"(a), "=&r"(b), "+c"(c), "=d"(d)
    );
#endif

    if (eax != NULL) *eax = a;
    if (ebx != NULL) *ebx = b;
    if (ecx != NULL) *ecx = c;
    if (edx != NULL) *edx = d;
}

static UINT64 do_rdtsc(void)
{
    UINT32 lo = 0U, hi = 0U;

    __asm__ __volatile__ (
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );
    return ((UINT64)hi << 32) | (UINT64)lo;
}

static UINT64 do_rdmsr(UINT32 msr)
{
    UINT32 lo = 0U, hi = 0U;

    __asm__ __volatile__ (
        "rdmsr"
        : "=a"(lo), "=d"(hi)
        : "c"(msr)
    );
    return ((UINT64)hi << 32) | (UINT64)lo;
}

static VOID put_u32_chars(char *dst, UINT32 v)
{
    dst[0] = (char)(v & 0xFFU);
    dst[1] = (char)((v >> 8)  & 0xFFU);
    dst[2] = (char)((v >> 16) & 0xFFU);
    dst[3] = (char)((v >> 24) & 0xFFU);
}

static VOID out_flag_if(efi_diag_ctx_t *ctx, BOOLEAN cond, const char *name, BOOLEAN *first)
{
    if (!cond) {
        return;
    }
    if (!(*first)) {
        efi_out_str(ctx, " ");
    }
    efi_out_str(ctx, name);
    *first = FALSE;
}

/* -------------------------------------------------------------------------
 * Step 8 메인 엔트리: CPU 식별 · 기능 플래그 · RDTSC · 표준 MSR
 * ------------------------------------------------------------------------- */
void efi_run_step8_cpu_features(efi_diag_ctx_t *ctx)
{
    UINT32 max_basic = 0U, max_ext = 0U;
    UINT32 a = 0U, b = 0U, c = 0U, d = 0U;
    UINT32 f1_eax = 0U, f1_ebx = 0U, f1_ecx = 0U, f1_edx = 0U;
    UINT32 f7_ebx = 0U, f7_ecx = 0U;
    UINT32 ext1_ecx = 0U, ext1_edx = 0U;
    UINT32 ext7_edx = 0U, ext8_eax = 0U;
    char   vendor[13];
    char   brand[49];
    UINT32 base_fam, base_mod, step, ext_fam, ext_mod, disp_fam, disp_mod;
    BOOLEAN has_tsc, has_msr, has_apic, has_lm, has_nx, has_syscall;
    BOOLEAN first;

    efi_out_str(ctx, "=== [Step 8] CPU Identification & Safe Feature Check ===\n");
    efi_out_str(ctx, "  [1] Execution Environment\n");
    efi_out_str(ctx, "    Current Mode     : ");
    efi_out_str(ctx, EFI_EXEC_MODE_STR);
    efi_out_str(ctx, "\n    Safety Policy    : Read-only CPUID/RDTSC/Arch-MSR (No GDT/IDT/Paging changes)\n");

    /* Leaf 0: Vendor & Max Basic Leaf */
    do_cpuid(0x00000000U, 0U, &max_basic, &b, &c, &d);
    put_u32_chars(vendor + 0, b);
    put_u32_chars(vendor + 4, d);
    put_u32_chars(vendor + 8, c);
    vendor[12] = '\0';

    /* Extended Leaf 0x80000000: Max Extended Leaf */
    do_cpuid(0x80000000U, 0U, &max_ext, &b, &c, &d);

    efi_out_str(ctx, "\n  [2] CPUID Identification\n");
    efi_out_str(ctx, "    Vendor String    : \"");
    efi_out_str(ctx, vendor);
    efi_out_str(ctx, "\" (MaxBasic=0x");
    efi_out_hex32(ctx, max_basic);
    efi_out_str(ctx, ", MaxExt=0x");
    efi_out_hex32(ctx, max_ext);
    efi_out_str(ctx, ")\n");

    /* Brand String (0x80000002..0x80000004) */
    if (max_ext >= 0x80000004U) {
        UINT32 leaf;
        UINTN  pos = 0U, start = 0U;

        for (leaf = 0x80000002U; leaf <= 0x80000004U; leaf++) {
            do_cpuid(leaf, 0U, &a, &b, &c, &d);
            put_u32_chars(brand + pos + 0U,  a);
            put_u32_chars(brand + pos + 4U,  b);
            put_u32_chars(brand + pos + 8U,  c);
            put_u32_chars(brand + pos + 12U, d);
            pos += 16U;
        }
        brand[48] = '\0';
        while (brand[start] == ' ') {
            start++;
        }
        efi_out_str(ctx, "    Brand String     : \"");
        efi_out_str(ctx, brand + start);
        efi_out_str(ctx, "\"\n");
    }

    /* Leaf 1: Family / Model / Stepping & Basic Features */
    if (max_basic >= 1U) {
        do_cpuid(0x00000001U, 0U, &f1_eax, &f1_ebx, &f1_ecx, &f1_edx);

        step     = f1_eax & 0x0FU;
        base_mod = (f1_eax >> 4)  & 0x0FU;
        base_fam = (f1_eax >> 8)  & 0x0FU;
        ext_mod  = (f1_eax >> 16) & 0x0FU;
        ext_fam  = (f1_eax >> 20) & 0xFFU;

        disp_fam = (base_fam == 0x0FU) ? (base_fam + ext_fam) : base_fam;
        disp_mod = (base_fam == 0x06U || base_fam == 0x0FU)
                   ? ((ext_mod << 4) | base_mod) : base_mod;

        efi_out_str(ctx, "    Signature        : Family=0x");
        efi_out_hex8(ctx, (UINT8)disp_fam);
        efi_out_str(ctx, " (");
        efi_out_u64(ctx, (UINT64)disp_fam);
        efi_out_str(ctx, "), Model=0x");
        efi_out_hex8(ctx, (UINT8)disp_mod);
        efi_out_str(ctx, " (");
        efi_out_u64(ctx, (UINT64)disp_mod);
        efi_out_str(ctx, "), Stepping=");
        efi_out_u64(ctx, (UINT64)step);
        efi_out_str(ctx, " (Raw=0x");
        efi_out_hex32(ctx, f1_eax);
        efi_out_str(ctx, ")\n");

        efi_out_str(ctx, "    Initial APIC ID  : ");
        efi_out_u64(ctx, (UINT64)((f1_ebx >> 24) & 0xFFU));
        efi_out_str(ctx, " | Hypervisor Bit=");
        efi_out_str(ctx, ((f1_ecx & (1U << 31)) != 0U) ? "1 (VM/Hypervisor detected)\n"
                                                       : "0 (Bare-metal or hidden)\n");
    }

    if (max_basic >= 7U) {
        do_cpuid(0x00000007U, 0U, &a, &f7_ebx, &f7_ecx, &d);
    }
    if (max_ext >= 0x80000001U) {
        do_cpuid(0x80000001U, 0U, &a, &b, &ext1_ecx, &ext1_edx);
    }
    if (max_ext >= 0x80000007U) {
        do_cpuid(0x80000007U, 0U, &a, &b, &c, &ext7_edx);
    }
    if (max_ext >= 0x80000008U) {
        do_cpuid(0x80000008U, 0U, &ext8_eax, &b, &c, &d);
    }

    has_tsc     = ((f1_edx & (1U << 4))  != 0U) ? TRUE : FALSE;
    has_msr     = ((f1_edx & (1U << 5))  != 0U) ? TRUE : FALSE;
    has_apic    = ((f1_edx & (1U << 9))  != 0U) ? TRUE : FALSE;
    has_syscall = ((ext1_edx & (1U << 11)) != 0U) ? TRUE : FALSE;
    has_nx      = ((ext1_edx & (1U << 20)) != 0U) ? TRUE : FALSE;
    has_lm      = ((ext1_edx & (1U << 29)) != 0U) ? TRUE : FALSE;

    /* [3] 명령어 집합 및 기능 플래그 */
    efi_out_str(ctx, "\n  [3] ISA & Hardware Feature Flags\n");

    efi_out_str(ctx, "    Core / Legacy    : ");
    first = TRUE;
    out_flag_if(ctx, (f1_edx & (1U << 0))  != 0U, "x87",       &first);
    out_flag_if(ctx, has_tsc,                     "TSC",       &first);
    out_flag_if(ctx, has_msr,                     "MSR",       &first);
    out_flag_if(ctx, (f1_edx & (1U << 6))  != 0U, "PAE",       &first);
    out_flag_if(ctx, (f1_edx & (1U << 8))  != 0U, "CX8",       &first);
    out_flag_if(ctx, has_apic,                    "APIC",      &first);
    out_flag_if(ctx, (f1_edx & (1U << 15)) != 0U, "CMOV",      &first);
    out_flag_if(ctx, (f1_edx & (1U << 23)) != 0U, "MMX",       &first);
    out_flag_if(ctx, (f1_ecx & (1U << 13)) != 0U, "CX16",      &first);
    if (first) efi_out_str(ctx, "(none)");
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "    SIMD / Crypto    : ");
    first = TRUE;
    out_flag_if(ctx, (f1_edx & (1U << 25)) != 0U, "SSE",       &first);
    out_flag_if(ctx, (f1_edx & (1U << 26)) != 0U, "SSE2",      &first);
    out_flag_if(ctx, (f1_ecx & (1U << 0))  != 0U, "SSE3",      &first);
    out_flag_if(ctx, (f1_ecx & (1U << 9))  != 0U, "SSSE3",     &first);
    out_flag_if(ctx, (f1_ecx & (1U << 19)) != 0U, "SSE4.1",    &first);
    out_flag_if(ctx, (f1_ecx & (1U << 20)) != 0U, "SSE4.2",    &first);
    out_flag_if(ctx, (f1_ecx & (1U << 23)) != 0U, "POPCNT",    &first);
    out_flag_if(ctx, (f1_ecx & (1U << 25)) != 0U, "AES-NI",    &first);
    out_flag_if(ctx, (f1_ecx & (1U << 28)) != 0U, "AVX",       &first);
    out_flag_if(ctx, (f7_ebx & (1U << 5))  != 0U, "AVX2",      &first);
    out_flag_if(ctx, (f1_ecx & (1U << 30)) != 0U, "RDRAND",    &first);
    out_flag_if(ctx, (f7_ebx & (1U << 18)) != 0U, "RDSEED",    &first);
    out_flag_if(ctx, (f7_ebx & (1U << 29)) != 0U, "SHA",       &first);
    if (first) efi_out_str(ctx, "(none)");
    efi_out_str(ctx, "\n");

    efi_out_str(ctx, "    64-bit / Virt    : ");
    first = TRUE;
    out_flag_if(ctx, has_lm,                      "LM(x64)",   &first);
    out_flag_if(ctx, has_nx,                      "NX/XD",     &first);
    out_flag_if(ctx, (ext1_edx & (1U << 26)) != 0U, "PDPE1GB", &first);
    out_flag_if(ctx, (ext1_edx & (1U << 27)) != 0U, "RDTSCP",  &first);
    out_flag_if(ctx, (ext7_edx & (1U << 8))  != 0U, "InvTSC",  &first);
    out_flag_if(ctx, (f1_ecx & (1U << 5))  != 0U, "VMX(Intel)",&first);
    out_flag_if(ctx, (ext1_ecx & (1U << 2)) != 0U, "SVM(AMD)", &first);
    out_flag_if(ctx, (f7_ebx & (1U << 7))  != 0U, "SMEP",      &first);
    out_flag_if(ctx, (f7_ebx & (1U << 20)) != 0U, "SMAP",      &first);
    if (first) efi_out_str(ctx, "(none)");
    efi_out_str(ctx, "\n");

    if (max_ext >= 0x80000008U) {
        efi_out_str(ctx, "    Address Width    : Phys=");
        efi_out_u64(ctx, (UINT64)(ext8_eax & 0xFFU));
        efi_out_str(ctx, "-bit, Linear=");
        efi_out_u64(ctx, (UINT64)((ext8_eax >> 8) & 0xFFU));
        efi_out_str(ctx, "-bit\n");
    }

    /* [4] RDTSC 단조 증가 확인 */
    efi_out_str(ctx, "\n  [4] RDTSC & Safe Architectural MSRs\n");
    efi_out_str(ctx, "    RDTSC Monotonic  : ");
    if (has_tsc) {
        UINT64 t0 = do_rdtsc();
        UINT64 t1;
        if (ctx->st->BootServices != NULL && ctx->st->BootServices->Stall != NULL) {
            uefi_call_wrapper(ctx->st->BootServices->Stall, 1, (UINTN)1000U); /* 1ms */
        }
        t1 = do_rdtsc();
        if (t1 > t0) {
            efi_out_str(ctx, "PASS (delta=");
            efi_out_u64(ctx, t1 - t0);
            efi_out_str(ctx, " ticks/1ms)\n");
        } else {
            efi_out_str(ctx, "STALLED / NON-MONOTONIC\n");
        }
    } else {
        efi_out_str(ctx, "NOT SUPPORTED\n");
    }

    /* 표준 아키텍처 MSR 제한 조회 (CPUID 선검증 + 실행 직전 Flush) */
    if (!has_msr) {
        efi_out_str(ctx, "    Arch MSRs        : NOT SUPPORTED (CPUID MSR=0)\n");
    } else {
        efilog_flush(ctx);

        if (has_tsc) {
            UINT64 msr_tsc = do_rdmsr(0x00000010U);
            efi_out_str(ctx, "    IA32_TSC (0x10)  : 0x");
            efi_out_hex64(ctx, msr_tsc);
            efi_out_str(ctx, "\n");
        }

        if (has_apic) {
            UINT64 msr_apic = do_rdmsr(0x0000001BU);
            efi_out_str(ctx, "    APIC_BASE (0x1B) : 0x");
            efi_out_hex64(ctx, msr_apic);
            efi_out_str(ctx, " [BSP=");
            efi_out_str(ctx, (msr_apic & (1ULL << 8))  ? "1" : "0");
            efi_out_str(ctx, ", x2APIC=");
            efi_out_str(ctx, (msr_apic & (1ULL << 10)) ? "1" : "0");
            efi_out_str(ctx, ", EN=");
            efi_out_str(ctx, (msr_apic & (1ULL << 11)) ? "1" : "0");
            efi_out_str(ctx, "]\n");
        }

        if (has_lm || has_nx || has_syscall) {
            UINT64 msr_efer = do_rdmsr(0xC0000080U);
            efi_out_str(ctx, "    IA32_EFER(C0..80): 0x");
            efi_out_hex64(ctx, msr_efer);
            efi_out_str(ctx, " [SCE=");
            efi_out_str(ctx, (msr_efer & (1ULL << 0))  ? "1" : "0");
            efi_out_str(ctx, ", LME=");
            efi_out_str(ctx, (msr_efer & (1ULL << 8))  ? "1" : "0");
            efi_out_str(ctx, ", LMA=");
            efi_out_str(ctx, (msr_efer & (1ULL << 10)) ? "1" : "0");
            efi_out_str(ctx, ", NXE=");
            efi_out_str(ctx, (msr_efer & (1ULL << 11)) ? "1" : "0");
            efi_out_str(ctx, "]\n");
        }
    }

    efi_out_str(ctx, "\n");
    efilog_flush(ctx);
}