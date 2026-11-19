typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void (*put_ch)(char c);
extern void put_str(const char *s);
extern void log_str(const char *s);
extern void log_ulong_only(uint32_t val);
extern uint16_t log_get_pos(void);
extern void log_rewind_to(uint16_t saved_pos);
extern void out_str(const char *s);
extern void out_hex16(uint16_t val);
extern void out_ulong(uint32_t val);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void delay_ms_pit(uint16_t ms);

static void out_hex32(uint32_t val)
{
    out_hex16((uint16_t)(val >> 16));
    out_hex16((uint16_t)(val & 0xFFFFU));
}

/* ---------------------------------------------------------
 * 1. 선(先) 디스크 실패 기록 -> 후(後) 성공 덮어쓰기 헬퍼
 * --------------------------------------------------------- */
static uint16_t g_test_mark = 0;
static const char *g_cur_tag = "";
static uint8_t  g_cur_step = 0;
static const char *g_cur_desc = "";

/* 서브스텝 진입 직전 호출: 화면 출력 없이 디스크(RESULT.TXT)에만 FAIL 선기록 */
static void prep_substep(uint8_t step_no, const char *step_desc)
{
    g_cur_step = step_no;
    g_cur_desc = step_desc;

    log_rewind_to(g_test_mark);
    log_str("  ");
    log_str(g_cur_tag);
    log_str(" : FAIL [CRASH/HANG at Substep ");
    log_ulong_only(step_no);
    log_str(": ");
    log_str(step_desc);
    log_str("]\n");
}

/* 테스트 성공 완료 시 호출: 디스크의 임시 FAIL을 지우고 화면+디스크에 PASS 기록 */
static void finish_pass(const char *detail)
{
    log_rewind_to(g_test_mark);
    out_str("  ");
    out_str(g_cur_tag);
    out_str(" : PASS (");
    out_str(detail);
    out_str(")\n");
}

/* 예외 포착 또는 값 검증 실패 시 호출: 디스크의 임시 FAIL을 구체적 실패 원인으로 교체 + 화면 출력 */
static void finish_fail(const char *reason, uint8_t exc_vec)
{
    log_rewind_to(g_test_mark);
    out_str("  ");
    out_str(g_cur_tag);
    out_str(" : FAIL at Substep ");
    out_ulong(g_cur_step);
    out_str(" (");
    out_str(g_cur_desc);
    out_str(") -> ");
    out_str(reason);
    if (exc_vec != 0xFF) {
        out_str(" [INT 0x");
        out_hex16(exc_vec);
        if (exc_vec == 0x06)      out_str(" #UD Invalid Opcode");
        else if (exc_vec == 0x07) out_str(" #NM Math/SIMD Unavailable");
        else if (exc_vec == 0x0D) out_str(" #GP General Protection");
        out_str("]");
    }
    out_str("\n");
}

/* ---------------------------------------------------------
 * 2. 하드웨어 예외(#UD / #NM / #GP) 안전 트랩 및 복구 가드
 * --------------------------------------------------------- */
struct trap_ctx {
    uint16_t save_sp;       /* +0 */
    uint16_t save_bp;       /* +2 */
    uint16_t save_cs;       /* +4 */
    uint16_t resume_ip;     /* +6 */
    uint8_t  trap_vec;      /* +8 */
};

static struct trap_ctx g_tctx;
static void (far *g_old_int06)(void);
static void (far *g_old_int07)(void);
static void (far *g_old_int0d)(void);

static void far isr_trap06(void)
{
    __emit__(0x0E, 0x1F);       /* PUSH CS; POP DS (?꽭洹몃㉫?듃 ?옱諛곗튂 ?뾾?씠 DS 蹂듦뎄) */
    g_tctx.trap_vec = 0x06;
    _SI = (uint16_t)&g_tctx;
    __emit__(
        0x8E, 0x54, 0x04,       /* MOV SS, [SI+4] */
        0x8B, 0x24,             /* MOV SP, [SI]   */
        0x8B, 0x6C, 0x02,       /* MOV BP, [SI+2] */
        0xB8, 0x02, 0x02,       /* MOV AX, 0202h  */
        0x50,                   /* PUSH AX        */
        0xFF, 0x74, 0x04,       /* PUSH [SI+4]    */
        0xFF, 0x74, 0x06,       /* PUSH [SI+6]    */
        0xCF                    /* IRET           */
    );
}

static void far isr_trap07(void)
{
    __emit__(0x0E, 0x1F);       /* PUSH CS; POP DS */
    g_tctx.trap_vec = 0x07;
    _SI = (uint16_t)&g_tctx;
    __emit__(
        0x8E, 0x54, 0x04,
        0x8B, 0x24,
        0x8B, 0x6C, 0x02,
        0xB8, 0x02, 0x02,
        0x50,
        0xFF, 0x74, 0x04,
        0xFF, 0x74, 0x06,
        0xCF
    );
}

static void far isr_trap0d(void)
{
    __emit__(0x0E, 0x1F);       /* PUSH CS; POP DS */
    g_tctx.trap_vec = 0x0D;
    _SI = (uint16_t)&g_tctx;
    __emit__(
        0x8E, 0x54, 0x04,
        0x8B, 0x24,
        0x8B, 0x6C, 0x02,
        0xB8, 0x02, 0x02,
        0x50,
        0xFF, 0x74, 0x04,
        0xFF, 0x74, 0x06,
        0xCF
    );
}

static void install_trap_vectors(uint8_t on)
{
    void (far * far *v06)(void) =
        (void (far * far *)(void))MK_FP(0, 0x06 * 4);

    void (far * far *v07)(void) =
        (void (far * far *)(void))MK_FP(0, 0x07 * 4);

    void (far * far *v0d)(void) =
        (void (far * far *)(void))MK_FP(0, 0x0D * 4);

    uint16_t far *w06 = (uint16_t far *)v06;
    uint16_t far *w07 = (uint16_t far *)v07;
    uint16_t far *w0d = (uint16_t far *)v0d;

    uint16_t handler_cs;

    __emit__(0xFA); /* CLI */
    handler_cs = _CS;

    if (on) {
        g_old_int06 = *v06;
        g_old_int07 = *v07;
        g_old_int0d = *v0d;

        w06[0] = (uint16_t)(void near *)isr_trap06;
        w06[1] = handler_cs;

        w07[0] = (uint16_t)(void near *)isr_trap07;
        w07[1] = handler_cs;

        w0d[0] = (uint16_t)(void near *)isr_trap0d;
        w0d[1] = handler_cs;

    } else {
        *v06 = g_old_int06;
        *v07 = g_old_int07;
        *v0d = g_old_int0d;
    }

    __emit__(0xFB); /* STI */
}

/* 단일 마이크로 스텝 함수를 예외 보호 상태에서 실행 (0=성공, 그 외=발생한 예외 벡터) */
static uint8_t exec_guarded(void (*step_fn)(void))
{
    g_tctx.trap_vec = 0xFF;
    install_trap_vectors(1);

    _SI = (uint16_t)&g_tctx;
    _BX = (uint16_t)step_fn;
    __emit__(
        0x89, 0x24,             /* MOV [SI], SP      */
        0x89, 0x6C, 0x02,       /* MOV [SI+2], BP    */
        0x8C, 0x4C, 0x04,       /* MOV [SI+4], CS    */
        0xE8, 0x00, 0x00,       /* CALL $+3          */
        0x58,                   /* POP AX            */
        0x05, 0x09, 0x00,       /* ADD AX, 9         */
        0x89, 0x44, 0x06,       /* MOV [SI+6], AX    */
        0xFF, 0xD3,             /* CALL BX           */
        0x90                    /* NOP (resume_ip)   */
    );

    install_trap_vectors(0);
    return g_tctx.trap_vec;
}

/* ---------------------------------------------------------
 * [Test 01] FLAGS / EFLAGS 비트 단계별 판별 (8086 -> 286 -> 386 -> 486 -> CPUID)
 * --------------------------------------------------------- */
static uint16_t swap_flags16(uint16_t v)
{
    _AX = v;
    __emit__(0x9C, 0x50, 0x9D, 0x9C, 0x58, 0x9D);
    return _AX;
}

static uint8_t toggle_eflags_bit(uint32_t mask)
{
    uint32_t orig_f, read_f;
    _SI = (uint16_t)&orig_f;
    _DI = (uint16_t)&mask;
    _BX = (uint16_t)&read_f;
    __emit__(
        0x66, 0x9C,             /* PUSHFD */
        0x66, 0x58,             /* POP EAX */
        0x66, 0x89, 0x04,       /* MOV [SI], EAX */
        0x66, 0x33, 0x05,       /* XOR EAX, [DI] */
        0x66, 0x50,             /* PUSH EAX */
        0x66, 0x9D,             /* POPFD */
        0x66, 0x9C,             /* PUSHFD */
        0x66, 0x58,             /* POP EAX */
        0x66, 0x89, 0x07,       /* MOV [BX], EAX */
        0x66, 0xFF, 0x34,       /* PUSH DWORD PTR [SI] */
        0x66, 0x9D              /* POPFD */
    );
    return (uint8_t)(((orig_f ^ read_f) & mask) != 0);
}

static void test01_flags(void)
{
    uint16_t f;
    uint8_t has_ac, has_id;

    g_cur_tag = "[01] FLAGS/EFLAGS Arch";
    g_test_mark = log_get_pos();

    prep_substep(1, "16-bit FLAGS IOPL/NT bits 12..15");
    f = swap_flags16(0x0202U);
    if ((f & 0xF000U) == 0xF000U) {
        finish_pass("8086/80186 detected (Bits 12..15 fixed 1)");
        return;
    }

    prep_substep(2, "16-bit FLAGS 80286 vs 80386+");
    f = swap_flags16(0x7202U);
    if ((f & 0xF000U) == 0x0000U) {
        finish_pass("80286 detected (Bits 12..14 fixed 0 in Real Mode)");
        return;
    }

    prep_substep(3, "32-bit EFLAGS.AC (Bit 18) toggle");
    has_ac = toggle_eflags_bit(0x00040000UL);

    prep_substep(4, "32-bit EFLAGS.ID (Bit 21) toggle");
    has_id = toggle_eflags_bit(0x00200000UL);

    if (has_id)      finish_pass("486+/Pentium+ with EFLAGS.AC=1, EFLAGS.ID(CPUID)=1");
    else if (has_ac) finish_pass("80486 (EFLAGS.AC=1, no CPUID ID bit)");
    else             finish_pass("80386 (32-bit EFLAGS OK, no AC/ID bits)");
}

/* ---------------------------------------------------------
 * [Test 02] 286 Protected-Mode Status Instructions (SMSW, SGDT, SIDT)
 * --------------------------------------------------------- */
static uint16_t g_msw_val;
static uint8_t  g_gdtr_buf[6];
static uint8_t  g_idtr_buf[6];

static void sub02_smsw(void)
{
    __emit__(0x0F, 0x01, 0xE0); /* SMSW AX */
    g_msw_val = _AX;
}

static void sub02_sgdt(void)
{
    _SI = (uint16_t)g_gdtr_buf;
    __emit__(0x0F, 0x01, 0x04); /* SGDT [SI] */
}

static void sub02_sidt(void)
{
    _SI = (uint16_t)g_idtr_buf;
    __emit__(0x0F, 0x01, 0x0C); /* SIDT [SI] */
}

static void test02_286_status(void)
{
    uint8_t v;

    g_cur_tag = "[02] 286 Status (SMSW/SGDT/SIDT)";
    g_test_mark = log_get_pos();

    prep_substep(1, "SMSW AX instruction");
    v = exec_guarded(sub02_smsw);
    if (v != 0xFF) { finish_fail("SMSW trapped", v); return; }

    prep_substep(2, "SGDT [mem] instruction");
    v = exec_guarded(sub02_sgdt);
    if (v != 0xFF) { finish_fail("SGDT trapped", v); return; }

    prep_substep(3, "SIDT [mem] instruction");
    v = exec_guarded(sub02_sidt);
    if (v != 0xFF) { finish_fail("SIDT trapped", v); return; }

    if ((g_msw_val & 0x0001U) != 0) {
        finish_pass("OK (Note: MSW.PE=1 -> CPU is already in V86 mode!)");
    } else {
        finish_pass("OK (MSW.PE=0 Pure Real Mode, SGDT/SIDT readable)");
    }
}

/* ---------------------------------------------------------
 * [Test 03] 386 Control & Debug Registers (CR0, CR2, CR3, DR7)
 * --------------------------------------------------------- */
static uint32_t g_reg32_out;

static void sub03_cr0(void)
{
    _SI = (uint16_t)&g_reg32_out;
    __emit__(0x0F, 0x20, 0xC0, 0x66, 0x89, 0x04); /* MOV EAX, CR0; MOV [SI], EAX */
}

static void sub03_cr2(void)
{
    _SI = (uint16_t)&g_reg32_out;
    __emit__(0x0F, 0x20, 0xD0, 0x66, 0x89, 0x04); /* MOV EAX, CR2 */
}

static void sub03_cr3(void)
{
    _SI = (uint16_t)&g_reg32_out;
    __emit__(0x0F, 0x20, 0xD8, 0x66, 0x89, 0x04); /* MOV EAX, CR3 */
}

static void sub03_dr7(void)
{
    _SI = (uint16_t)&g_reg32_out;
    __emit__(0x0F, 0x21, 0xF8, 0x66, 0x89, 0x04); /* MOV EAX, DR7 */
}

static void test03_386_cregs(void)
{
    uint8_t v;

    g_cur_tag = "[03] 386 Control/Debug Regs";
    g_test_mark = log_get_pos();

    prep_substep(1, "MOV EAX, CR0");
    v = exec_guarded(sub03_cr0);
    if (v != 0xFF) { finish_fail("MOV EAX, CR0 trapped (V86/286?)", v); return; }

    prep_substep(2, "MOV EAX, CR2");
    v = exec_guarded(sub03_cr2);
    if (v != 0xFF) { finish_fail("MOV EAX, CR2 trapped", v); return; }

    prep_substep(3, "MOV EAX, CR3");
    v = exec_guarded(sub03_cr3);
    if (v != 0xFF) { finish_fail("MOV EAX, CR3 trapped", v); return; }

    prep_substep(4, "MOV EAX, DR7");
    v = exec_guarded(sub03_dr7);
    if (v != 0xFF) { finish_fail("MOV EAX, DR7 trapped", v); return; }

    finish_pass("CR0, CR2, CR3, DR7 all accessible in Ring 0 Real Mode");
}

/* ---------------------------------------------------------
 * [Test 04] x87 FPU Step-by-Step Probe
 * --------------------------------------------------------- */
static volatile uint16_t g_fpu_sw = 0xFFFFU;
static volatile uint16_t g_fpu_cw = 0xFFFFU;
static volatile uint16_t g_fpu_res = 0;

static void sub04_clear_em_ts(void)
{
    __emit__(
        0x0F, 0x20, 0xC0,               /* MOV EAX, CR0 */
        0x66, 0x83, 0xE0, 0xF3,         /* AND EAX, ~0Ch (Clear EM bit2, TS bit3) */
        0x0F, 0x22, 0xC0                /* MOV CR0, EAX */
    );
}

static void sub04_fninit(void)
{
    __emit__(0xDB, 0xE3);               /* FNINIT */
}

static void sub04_fnstsw(void)
{
    g_fpu_sw = 0x55AAU;
    _SI = (uint16_t)&g_fpu_sw;
    __emit__(0xDF, 0x3C);               /* FNSTSW [SI] */
}

static void sub04_fnstcw(void)
{
    g_fpu_cw = 0x0000U;
    _SI = (uint16_t)&g_fpu_cw;
    __emit__(0xD9, 0x3C);               /* FNSTCW [SI] */
}

static void sub04_fadd(void)
{
    g_fpu_res = 0;
    _SI = (uint16_t)&g_fpu_res;
    __emit__(
        0xD9, 0xE8,                     /* FLD1 */
        0xD9, 0xE8,                     /* FLD1 */
        0xDE, 0xC1,                     /* FADDP ST(1), ST */
        0xDF, 0x1C                      /* FISTP WORD PTR [SI] */
    );
}

static void test04_x87_fpu(void)
{
    uint8_t v;

    g_cur_tag = "[04] x87 FPU Coprocessor";
    g_test_mark = log_get_pos();

    prep_substep(1, "Clear CR0.EM and CR0.TS");
    (void)exec_guarded(sub04_clear_em_ts);

    prep_substep(2, "FNINIT instruction");
    v = exec_guarded(sub04_fninit);
    if (v != 0xFF) { finish_fail("FNINIT trapped", v); return; }

    prep_substep(3, "FNSTSW status word == 0");
    v = exec_guarded(sub04_fnstsw);
    if (v != 0xFF || (g_fpu_sw & 0xFFU) != 0) {
        finish_fail("No FPU present (FNSTSW != 0)", v);
        return;
    }

    prep_substep(4, "FNSTCW control word == 0x037F");
    v = exec_guarded(sub04_fnstcw);
    if (v != 0xFF || (g_fpu_cw & 0x103FU) != 0x003FU) {
        finish_fail("Invalid FPU control word", v);
        return;
    }

    prep_substep(5, "IEEE-754 FLD1+FLD1 FADDP == 2");
    v = exec_guarded(sub04_fadd);
    if (v != 0xFF || g_fpu_res != 2U) {
        finish_fail("FPU arithmetic mismatch", v);
        return;
    }

    finish_pass("FNINIT, FNSTSW, FNSTCW & 1.0+1.0=2.0 verified");
}

/* ---------------------------------------------------------
 * [Test 05] CPUID Instruction & Feature Leaves
 * --------------------------------------------------------- */
struct cpuid_regs {
    uint32_t eax, ebx, ecx, edx;
};
static struct cpuid_regs g_cid;

static void sub05_cpuid_call(void)
{
    _SI = (uint16_t)&g_cid;
    __emit__(
        0x66, 0x8B, 0x04,       /* MOV EAX, [SI] */
        0x0F, 0xA2,             /* CPUID */
        0x66, 0x89, 0x04,       /* MOV [SI+0], EAX */
        0x66, 0x89, 0x5C, 0x04, /* MOV [SI+4], EBX */
        0x66, 0x89, 0x4C, 0x08, /* MOV [SI+8], ECX */
        0x66, 0x89, 0x54, 0x0C  /* MOV [SI+12], EDX */
    );
}

static void test05_cpuid(void)
{
    uint8_t  v;
    char     vendor[13];
    uint32_t f1_edx, f1_ecx, ext_max, ext_edx = 0;

    g_cur_tag = "[05] CPUID Leaves & Features";
    g_test_mark = log_get_pos();

    prep_substep(1, "Execute CPUID Leaf 0x00000000");
    g_cid.eax = 0;
    v = exec_guarded(sub05_cpuid_call);
    if (v != 0xFF) { finish_fail("CPUID opcode 0F A2 trapped", v); return; }

    *(uint32_t *)(vendor + 0) = g_cid.ebx;
    *(uint32_t *)(vendor + 4) = g_cid.edx;
    *(uint32_t *)(vendor + 8) = g_cid.ecx;
    vendor[12] = '\0';

    prep_substep(2, "Execute CPUID Leaf 0x00000001");
    g_cid.eax = 1UL;
    v = exec_guarded(sub05_cpuid_call);
    if (v != 0xFF) { finish_fail("CPUID Leaf 1 trapped", v); return; }
    f1_edx = g_cid.edx;
    f1_ecx = g_cid.ecx;

    prep_substep(3, "Execute CPUID Extended Leaf 0x80000000/1");
    g_cid.eax = 0x80000000UL;
    v = exec_guarded(sub05_cpuid_call);
    ext_max = g_cid.eax;
    if (v == 0xFF && ext_max >= 0x80000001UL) {
        g_cid.eax = 0x80000001UL;
        (void)exec_guarded(sub05_cpuid_call);
        ext_edx = g_cid.edx;
    }

    log_rewind_to(g_test_mark);
    out_str("  [05] CPUID Leaves & Features : PASS (Vendor=\"");
    out_str(vendor);
    out_str("\"");
    if (f1_edx & (1UL << 23)) out_str(" MMX");
    if (f1_edx & (1UL << 25)) out_str(" SSE");
    if (f1_edx & (1UL << 26)) out_str(" SSE2");
    if (f1_ecx & (1UL << 0))  out_str(" SSE3");
    if (ext_edx & (1UL << 20)) out_str(" NX");
    if (ext_edx & (1UL << 29)) out_str(" LM64");
    out_str(")\n");
}

/* ---------------------------------------------------------
 * [Test 06] Time Stamp Counter (CR4.TSD & RDTSC)
 * --------------------------------------------------------- */
static uint32_t g_tsc_lo, g_tsc_hi;

static void sub06_read_cr4(void)
{
    _SI = (uint16_t)&g_reg32_out;
    __emit__(0x0F, 0x20, 0xE0, 0x66, 0x89, 0x04); /* MOV EAX, CR4 */
}

static void sub06_rdtsc(void)
{
    _SI = (uint16_t)&g_tsc_lo;
    _DI = (uint16_t)&g_tsc_hi;
    __emit__(
        0x0F, 0x31,             /* RDTSC */
        0x66, 0x89, 0x04,       /* MOV [SI], EAX */
        0x66, 0x89, 0x15        /* MOV [DI], EDX */
    );
}

static void test06_rdtsc(void)
{
    uint8_t  v;
    uint32_t t1_lo, t1_hi;

    g_cur_tag = "[06] RDTSC & CR4 Register";
    g_test_mark = log_get_pos();

    prep_substep(1, "MOV EAX, CR4 (0F 20 E0)");
    v = exec_guarded(sub06_read_cr4);
    if (v != 0xFF) { finish_fail("CR4 read trapped (Pre-Pentium/486)", v); return; }

    prep_substep(2, "First RDTSC (0F 31) execution");
    v = exec_guarded(sub06_rdtsc);
    if (v != 0xFF) { finish_fail("RDTSC trapped", v); return; }
    t1_lo = g_tsc_lo;
    t1_hi = g_tsc_hi;

    prep_substep(3, "Second RDTSC after 2ms PIT delay");
    delay_ms_pit(2);
    v = exec_guarded(sub06_rdtsc);
    if (v != 0xFF) { finish_fail("Second RDTSC trapped", v); return; }

    if (g_tsc_hi == t1_hi && g_tsc_lo == t1_lo) {
        finish_fail("TSC counter stuck (delta == 0)", 0xFF);
        return;
    }

    finish_pass("CR4 readable & 64-bit TSC monotonically advancing");
}

/* ---------------------------------------------------------
 * [Test 07] Pentium+ Atomic CMPXCHG8B & RDMSR
 * --------------------------------------------------------- */
static uint32_t g_cmp64[2];
static uint8_t  g_cmp_zf;

static void sub07_cmpxchg8b(void)
{
    g_cmp64[0] = 0x11223344UL;
    g_cmp64[1] = 0x55667788UL;
    _SI = (uint16_t)g_cmp64;
    __emit__(
        0x66, 0xB8, 0x44, 0x33, 0x22, 0x11, /* MOV EAX, 11223344h */
        0x66, 0xBA, 0x88, 0x77, 0x66, 0x55, /* MOV EDX, 55667788h */
        0x66, 0xBB, 0xEF, 0xBE, 0xAD, 0xDE, /* MOV EBX, DEADBEEFh */
        0x66, 0xB9, 0xFE, 0xCA, 0xFE, 0xCA, /* MOV ECX, CAFEBABEh (or CAFECAFEh) */
        0x0F, 0xC7, 0x0C                    /* CMPXCHG8B QWORD PTR [SI] */
    );
    g_cmp_zf = (uint8_t)((_FLAGS & 0x0040U) != 0);
}

static void sub07_rdmsr_tsc(void)
{
    __emit__(
        0x66, 0xB9, 0x10, 0x00, 0x00, 0x00, /* MOV ECX, 00000010h (IA32_TIME_STAMP_COUNTER) */
        0x0F, 0x32                          /* RDMSR */
    );
}

static void test07_cmpxchg8b_msr(void)
{
    uint8_t v;

    g_cur_tag = "[07] CMPXCHG8B & RDMSR";
    g_test_mark = log_get_pos();

    prep_substep(1, "CMPXCHG8B (0F C7) 64-bit atomic compare-exchange");
    v = exec_guarded(sub07_cmpxchg8b);
    if (v != 0xFF) { finish_fail("CMPXCHG8B trapped", v); return; }
    if (!g_cmp_zf || g_cmp64[0] != 0xDEADBEEFUL) {
        finish_fail("CMPXCHG8B memory result mismatch", 0xFF);
        return;
    }

    prep_substep(2, "RDMSR (0F 32) on IA32_TSC (ECX=0x10)");
    v = exec_guarded(sub07_rdmsr_tsc);
    if (v != 0xFF) { finish_fail("RDMSR ECX=0x10 trapped", v); return; }

    finish_pass("64-bit CMPXCHG8B atomic swap & RDMSR(0x10) verified");
}

/* ---------------------------------------------------------
 * [Test 08] MMX 64-bit SIMD Instruction Set
 * --------------------------------------------------------- */
static uint8_t g_mmx_a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static uint8_t g_mmx_b[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
static uint8_t g_mmx_r[8];

static void sub08_movq_load(void)
{
    _SI = (uint16_t)g_mmx_a;
    __emit__(0x0F, 0x6F, 0x04);             /* MOVQ MM0, [SI] */
}

static void sub08_paddb_store_emms(void)
{
    _SI = (uint16_t)g_mmx_b;
    _DI = (uint16_t)g_mmx_r;
    __emit__(
        0x0F, 0xFC, 0x04,                   /* PADDB MM0, [SI] */
        0x0F, 0x7F, 0x05,                   /* MOVQ [DI], MM0  */
        0x0F, 0x77                          /* EMMS            */
    );
}

static void test08_mmx(void)
{
    uint8_t v, i;

    g_cur_tag = "[08] MMX 64-bit SIMD";
    g_test_mark = log_get_pos();

    prep_substep(1, "Clear CR0.EM/TS for MMX");
    (void)exec_guarded(sub04_clear_em_ts);

    prep_substep(2, "MOVQ MM0, [mem] (0F 6F)");
    v = exec_guarded(sub08_movq_load);
    if (v != 0xFF) { finish_fail("MOVQ load trapped", v); return; }

    prep_substep(3, "PADDB MM0 + MOVQ store + EMMS");
    v = exec_guarded(sub08_paddb_store_emms);
    if (v != 0xFF) { finish_fail("PADDB/EMMS trapped", v); return; }

    for (i = 0; i < 8; i++) {
        if (g_mmx_r[i] != (uint8_t)(g_mmx_a[i] + g_mmx_b[i])) {
            finish_fail("MMX PADDB byte vector mismatch", 0xFF);
            return;
        }
    }

    finish_pass("MOVQ, PADDB 8-byte vector add & EMMS verified");
}

/* ---------------------------------------------------------
 * [Test 09] SSE 128-bit SIMD & CR4.OSFXSR Handshake
 * --------------------------------------------------------- */
static uint32_t g_saved_cr4;

static void sub09_enable_osfxsr(void)
{
    _SI = (uint16_t)&g_saved_cr4;
    __emit__(
        0x0F, 0x20, 0xE0,                   /* MOV EAX, CR4 */
        0x66, 0x89, 0x04,                   /* MOV [SI], EAX */
        0x66, 0x0D, 0x00, 0x02, 0x00, 0x00, /* OR EAX, 00000200h (OSFXSR Bit 9) */
        0x0F, 0x22, 0xE0                    /* MOV CR4, EAX */
    );
}

static void sub09_xorps(void)
{
    __emit__(0x0F, 0x57, 0xC0);             /* XORPS XMM0, XMM0 */
}

static void sub09_restore_cr4(void)
{
    _SI = (uint16_t)&g_saved_cr4;
    __emit__(
        0x66, 0x8B, 0x04,                   /* MOV EAX, [SI] */
        0x0F, 0x22, 0xE0                    /* MOV CR4, EAX  */
    );
}

static void test09_sse(void)
{
    uint8_t v;

    g_cur_tag = "[09] SSE & CR4.OSFXSR";
    g_test_mark = log_get_pos();

    prep_substep(1, "Clear CR0.EM and set CR4.OSFXSR (Bit 9)");
    (void)exec_guarded(sub04_clear_em_ts);
    v = exec_guarded(sub09_enable_osfxsr);
    if (v != 0xFF) { finish_fail("Setting CR4.OSFXSR trapped", v); return; }

    prep_substep(2, "Execute XORPS XMM0, XMM0 (0F 57 C0)");
    v = exec_guarded(sub09_xorps);
    if (v != 0xFF) {
        (void)exec_guarded(sub09_restore_cr4);
        finish_fail("XORPS XMM0 trapped", v);
        return;
    }

    prep_substep(3, "Restore original CR4");
    (void)exec_guarded(sub09_restore_cr4);

    finish_pass("CR4.OSFXSR enabled & 128-bit XORPS XMM0 executed");
}

/* ---------------------------------------------------------
 * 공통 GDT 구성 헬퍼 (보호모드 / 언리얼모드 / 페이징 공용)
 * --------------------------------------------------------- */
struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_hi;
};

struct gdtr_ptr {
    uint16_t limit;
    uint32_t base;
};

static struct gdt_entry g_gdt[4];
static struct gdtr_ptr  g_gdtr;
static uint8_t          g_bios_gdtr[6];

static void set_gdt_desc(uint8_t idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    g_gdt[idx].limit_lo = (uint16_t)(limit & 0xFFFFU);
    g_gdt[idx].base_lo  = (uint16_t)(base & 0xFFFFU);
    g_gdt[idx].base_mid = (uint8_t)((base >> 16) & 0xFFU);
    g_gdt[idx].access   = access;
    g_gdt[idx].gran     = (uint8_t)(((limit >> 16) & 0x0FU) | (gran & 0xF0U));
    g_gdt[idx].base_hi  = (uint8_t)((base >> 24) & 0xFFU);
}

static void build_test_gdt(void)
{
    uint32_t cs_phys = ((uint32_t)_CS) << 4;

    /* 0x00: Null Descriptor */
    set_gdt_desc(0, 0, 0, 0, 0);
    /* 0x08: 16-bit Code Segment (Base = _CS<<4, Limit = 64KB, Exec/Read) */
    set_gdt_desc(1, cs_phys, 0x0000FFFFUL, 0x9AU, 0x00U);
    /* 0x10: 16-bit Data Segment (Base = _CS<<4, Limit = 64KB, Read/Write) */
    set_gdt_desc(2, cs_phys, 0x0000FFFFUL, 0x92U, 0x00U);
    /* 0x18: 32-bit Flat Data Segment (Base = 0, Limit = 4GB, G=1, D/B=1) */
    set_gdt_desc(3, 0UL,     0x000FFFFFUL, 0x92U, 0xCFU);

    g_gdtr.limit = sizeof(g_gdt) - 1U;
    g_gdtr.base  = cs_phys + (uint32_t)(uint16_t)g_gdt;
}

/* ---------------------------------------------------------
 * [Test 10] GDT / IDT Register Load Dry-Run in Real Mode
 * --------------------------------------------------------- */
static void test10_lgdt_dryrun(void)
{
    uint8_t readback[6];

    g_cur_tag = "[10] LGDT/SGDT Table Dry-Run";
    g_test_mark = log_get_pos();

    prep_substep(1, "Save original BIOS GDTR via SGDT");
    build_test_gdt();
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x04);             /* SGDT [SI] */

    prep_substep(2, "Load custom GDTR via LGDT [mem]");
    _SI = (uint16_t)&g_gdtr;
    __emit__(0x0F, 0x01, 0x14);             /* LGDT [SI] */

    prep_substep(3, "Read back GDTR via SGDT and verify base/limit");
    _SI = (uint16_t)readback;
    __emit__(0x0F, 0x01, 0x04);             /* SGDT [SI] */

    prep_substep(4, "Restore original BIOS GDTR");
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14);             /* LGDT [SI] */

    if (*(uint16_t *)(readback + 0) != g_gdtr.limit ||
        *(uint32_t *)(readback + 2) != g_gdtr.base) {
        finish_fail("SGDT read-back did not match LGDT value", 0xFF);
        return;
    }

    finish_pass("SGDT backup -> LGDT load -> SGDT verify -> BIOS GDTR restored");
}

/* ---------------------------------------------------------
 * [Test 11] 16-bit Protected Mode (Incremental Micro-Step Round-Trips)
 * --------------------------------------------------------- */
static volatile uint16_t g_pm_scratch = 0;

static void test11_pm16_microsteps(void)
{
    uint16_t far_ptr[2];
    uint16_t real_cs = _CS;

    g_cur_tag = "[11] 16-bit Protected Mode";
    g_test_mark = log_get_pos();

    /* V86 모드에서 진입하면 CR0/LGDT가 트랩되므로 사전 점검 */
    sub02_smsw();
    if ((g_msw_val & 0x0001U) != 0) {
        finish_fail("CPU is already in V86 mode (MSW.PE=1); cannot enter raw PM", 0xFF);
        return;
    }

    build_test_gdt();

    /* Substep 1: CR0.PE=1 켰다가 분기 없이 즉시 CR0.PE=0 복귀 */
    prep_substep(1, "Toggle CR0.PE=1 and immediately back to CR0.PE=0");
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x04);             /* SGDT [SI] */
    _SI = (uint16_t)&g_gdtr;
    __emit__(
        0xFA,                               /* CLI */
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x0C, 0x01,                         /* OR AL, 01h (PE=1) */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xEB, 0x00,                         /* JMP SHORT $+2 */
        0x24, 0xFE,                         /* AND AL, 0FEh (PE=0) */
        0x0F, 0x22, 0xC0                    /* MOV CR0, EAX */
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);       /* LGDT [SI]; STI */

    /* Substep 2: PE=1 진입 + Far JMP 0008h:ip (CS 보호모드 셀렉터 로드) 후 복귀 */
    prep_substep(2, "PE=1 + Far JMP 0008h:ip (CS selector load) + return");
    _SI = (uint16_t)&g_gdtr;
    _DI = (uint16_t)far_ptr;
    _BX = real_cs;
    __emit__(
        0xFA,                               /* CLI */
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x0C, 0x01,                         /* OR AL, 01h */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        /* 간접 Far JMP 0008h:pm_cs_ok */
        0xE8, 0x00, 0x00,                   /* CALL $+3 */
        0x58,                               /* POP AX (1B) */
        0x05, 0x0E, 0x00,                   /* ADD AX, 14 (3B) */
        0x89, 0x05,                         /* MOV [DI], AX (2B) */
        0xC7, 0x45, 0x02, 0x08, 0x00,       /* MOV WORD PTR [DI+2], 0008h (5B) */
        0xFF, 0x2D,                         /* JMP FAR DWORD PTR [DI] (2B) */
        0x90,                               /* NOP (pm_cs_ok: +14B) */
        /* PE=0 복귀 및 Real Mode CS 복원 */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x24, 0xFE,                         /* AND AL, 0FEh */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xE8, 0x00, 0x00,                   /* CALL $+3 */
        0x58,                               /* POP AX (1B) */
        0x05, 0x0B, 0x00,                   /* ADD AX, 11 (3B) */
        0x89, 0x05,                         /* MOV [DI], AX (2B) */
        0x89, 0x5D, 0x02,                   /* MOV [DI+2], BX (2B) */
        0xFF, 0x2D,                         /* JMP FAR DWORD PTR [DI] (2B) */
        0x90                                /* NOP (rm_cs_ok: +11B) */
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);       /* LGDT [SI]; STI */

    /* Substep 3: PE=1 + DS/ES 보호모드 데이터 셀렉터(0x10) 로드 + 메모리 쓰기/역읽기 */
    prep_substep(3, "PE=1 + Load DS/ES=0x10 + Protected-Mode RAM R/W");
    g_pm_scratch = 0x1234U;
    _SI = (uint16_t)&g_gdtr;
    _DI = (uint16_t)&g_pm_scratch;
    _BX = real_cs;
    __emit__(
        0xFA,                               /* CLI */
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x0C, 0x01,                         /* OR AL, 01h */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xEB, 0x00,                         /* JMP SHORT $+2 */
        0xB8, 0x10, 0x00,                   /* MOV AX, 0010h */
        0x8E, 0xD8,                         /* MOV DS, AX */
        0x8E, 0xC0,                         /* MOV ES, AX */
        0xC7, 0x05, 0xA5, 0x5A,             /* MOV WORD PTR [DI], 5AA5h */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x24, 0xFE,                         /* AND AL, 0FEh */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xEB, 0x00,                         /* JMP SHORT $+2 */
        0x8E, 0xDB,                         /* MOV DS, BX */
        0x8E, 0xC3                          /* MOV ES, BX */
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);       /* LGDT [SI]; STI */

    if (g_pm_scratch != 0x5AA5U) {
        finish_fail("PM selector 0x10 memory write failed", 0xFF);
        return;
    }

    finish_pass("PE=1 -> Far JMP 0x08 -> DS/ES=0x10 RAM R/W -> PE=0 verified");
}

/* ---------------------------------------------------------
 * [Test 12] Unreal Mode (32-bit 4GB Data Limit in Real Mode)
 * --------------------------------------------------------- */
static uint8_t is_a20_on(void)
{
    uint8_t far *lo = (uint8_t far *)MK_FP(0x0000, 0x0000);
    uint8_t far *hi = (uint8_t far *)MK_FP(0xFFFF, 0x0010);
    uint8_t i;
    for (i = 0; i < 16; i++) {
        if (lo[i] != hi[i]) return 1;
    }
    return 0;
}

static void test12_unreal_mode(void)
{
    uint32_t orig_val, read_val;
    uint16_t real_cs = _CS;

    g_cur_tag = "[12] Unreal Mode (4GB Limit)";
    g_test_mark = log_get_pos();

    sub02_smsw();
    if ((g_msw_val & 0x0001U) != 0) {
        finish_fail("Blocked by active V86 monitor (MSW.PE=1)", 0xFF);
        return;
    }

    /* Substep 1: A20 게이트 확인 및 활성화 */
    prep_substep(1, "Verify/Enable A20 Gate via Port 0x92");
    if (!is_a20_on()) {
        uint8_t p92 = inb(0x92);
        if (p92 != 0xFF) {
            outb(0x92, (uint8_t)((p92 | 0x02U) & 0xFEU));
            delay_ms_pit(2);
        }
        if (!is_a20_on()) {
            finish_fail("A20 Gate remains disabled (1MB wrap-around)", 0xFF);
            return;
        }
    }

    /* Substep 2: FS에 4GB 플랫 디스크립터(0x18) 적재 후 리얼모드 복귀 */
    prep_substep(2, "Load 4GB descriptor (0x18) into FS cache and return to RM");
    build_test_gdt();
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x04);             /* SGDT [SI] */
    _SI = (uint16_t)&g_gdtr;
    __emit__(
        0xFA,                               /* CLI */
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x0C, 0x01,                         /* OR AL, 01h */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xEB, 0x00,                         /* JMP SHORT $+2 */
        0xB8, 0x18, 0x00,                   /* MOV AX, 0018h (4GB Flat) */
        0x8E, 0xE0,                         /* MOV FS, AX */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x24, 0xFE,                         /* AND AL, 0FEh */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xEB, 0x00,                         /* JMP SHORT $+2 */
        0x31, 0xC0,                         /* XOR AX, AX */
        0x8E, 0xE0                          /* MOV FS, AX (Base=0, Limit=4GB retained) */
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);       /* LGDT [SI]; STI */

    /* Substep 3: 리얼 모드에서 32비트 주소(FS:[0x00108000]) 직접 읽기/쓰기 */
    prep_substep(3, "Real-Mode 32-bit offset R/W at physical 0x00108000");
    _SI = (uint16_t)&orig_val;
    _DI = (uint16_t)&read_val;
    __emit__(
        0x66, 0xBB, 0x00, 0x80, 0x10, 0x00, /* MOV EBX, 00108000h */
        0x64, 0x67, 0x66, 0x8B, 0x03,       /* MOV EAX, FS:[EBX] */
        0x66, 0x89, 0x04,                   /* MOV [SI], EAX */
        0x66, 0xB8, 0xBE, 0xBA, 0xFE, 0xCA, /* MOV EAX, CAFEBABEh */
        0x64, 0x67, 0x66, 0x89, 0x03,       /* MOV FS:[EBX], EAX */
        0x64, 0x67, 0x66, 0x8B, 0x03,       /* MOV EAX, FS:[EBX] */
        0x66, 0x89, 0x05,                   /* MOV [DI], EAX */
        0x66, 0x8B, 0x04,                   /* MOV EAX, [SI] */
        0x64, 0x67, 0x66, 0x89, 0x03        /* MOV FS:[EBX], EAX (원상 복구) */
    );
    (void)real_cs;

    if (read_val != 0xCAFEBABEUL) {
        finish_fail("High memory 0x00108000 read-back mismatch", 0xFF);
        return;
    }

    finish_pass("FS 4GB limit cached & physical 0x00108000 R/W in Real Mode OK");
}

/* ---------------------------------------------------------
 * [Test 13] 32-bit Protected Mode (4GB Flat Selector & High Address R/W)
 * --------------------------------------------------------- */
static void test13_pm32_flat(void)
{
    uint32_t orig_v = 0, read_v = 0;

    g_cur_tag = "[13] 32-bit Protected Mode";
    g_test_mark = log_get_pos();

    sub02_smsw();
    if ((g_msw_val & 0x0001U) != 0) {
        finish_fail("Blocked by active V86 monitor (MSW.PE=1)", 0xFF);
        return;
    }
    if (!is_a20_on()) {
        uint8_t p92 = inb(0x92);
        if (p92 != 0xFF) outb(0x92, (uint8_t)((p92 | 0x02U) & 0xFEU));
    }

    build_test_gdt();

    /* Substep 1: PE=1 상태에서 32비트 플랫 셀렉터(0x18) 로드 후 16비트 셀렉터(0x10)로 정규화 복귀 */
    prep_substep(1, "Enter PE=1, load 32-bit flat selector 0x18, normalize to 0x10 & exit");
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x04);
    _SI = (uint16_t)&g_gdtr;
    __emit__(
        0xFA,
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,
        0x0C, 0x01,
        0x0F, 0x22, 0xC0,                   /* PE=1 */
        0xEB, 0x00,
        0xB8, 0x18, 0x00,                   /* MOV AX, 0018h (32-bit Flat) */
        0x8E, 0xE0,                         /* MOV FS, AX */
        0xB8, 0x10, 0x00,                   /* MOV AX, 0010h (16-bit 64KB) */
        0x8E, 0xE0,                         /* MOV FS, AX (Restore 64KB limit) */
        0x0F, 0x20, 0xC0,
        0x24, 0xFE,
        0x0F, 0x22, 0xC0,                   /* PE=0 */
        0xEB, 0x00,
        0x31, 0xC0,
        0x8E, 0xE0
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);

    /* Substep 2: PE=1 상태에서 32비트 선형 주소(0x00108000) 직접 R/W 검증 */
    prep_substep(2, "PE=1 32-bit linear address R/W at 0x00108000 via selector 0x18");
    _SI = (uint16_t)&g_gdtr;
    _DI = (uint16_t)&read_v;
    _BX = (uint16_t)&orig_v;
    __emit__(
        0xFA,
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,
        0x0C, 0x01,
        0x0F, 0x22, 0xC0,                   /* PE=1 */
        0xEB, 0x00,
        0xB8, 0x18, 0x00,
        0x8E, 0xE0,                         /* MOV FS, 0018h */
        0x66, 0xBE, 0x00, 0x80, 0x10, 0x00, /* MOV ESI, 00108000h */
        0x64, 0x67, 0x66, 0x8B, 0x06,       /* MOV EAX, FS:[ESI] */
        0x66, 0x89, 0x07,                   /* MOV [BX], EAX */
        0x66, 0xB8, 0x37, 0x13, 0xAD, 0xDE, /* MOV EAX, DEAD1337h */
        0x64, 0x67, 0x66, 0x89, 0x06,       /* MOV FS:[ESI], EAX */
        0x64, 0x67, 0x66, 0x8B, 0x06,       /* MOV EAX, FS:[ESI] */
        0x66, 0x89, 0x05,                   /* MOV [DI], EAX */
        0x66, 0x8B, 0x07,                   /* MOV EAX, [BX] */
        0x64, 0x67, 0x66, 0x89, 0x06,       /* MOV FS:[ESI], EAX (복원) */
        0xB8, 0x10, 0x00,
        0x8E, 0xE0,                         /* MOV FS, 0010h */
        0x0F, 0x20, 0xC0,
        0x24, 0xFE,
        0x0F, 0x22, 0xC0,                   /* PE=0 */
        0xEB, 0x00,
        0x31, 0xC0,
        0x8E, 0xE0
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);

    if (read_v != 0xDEAD1337UL) {
        finish_fail("32-bit PM linear R/W mismatch at 0x00108000", 0xFF);
        return;
    }

    finish_pass("32-bit Flat Selector 0x18 & Linear 0x00108000 R/W under PE=1 OK");
}

/* ---------------------------------------------------------
 * [Test 14] 32-bit Protected Mode Paging (CR3 + CR0.PG=1 + Virtual Remap)
 * --------------------------------------------------------- */
static void test14_pm_paging(void)
{
    uint16_t far *bda_mem = (uint16_t far *)MK_FP(0x0040, 0x0013);
    uint32_t far *pd_ptr  = (uint32_t far *)MK_FP(0x2000, 0x0000); /* 물리 0x00020000 (4KB 정렬) */
    uint32_t far *pt_ptr  = (uint32_t far *)MK_FP(0x2100, 0x0000); /* 물리 0x00021000 (4KB 정렬) */
    uint32_t far *pg_tgt  = (uint32_t far *)MK_FP(0x2200, 0x0000); /* 물리 0x00022000 (매핑 타겟 프레임) */
    uint32_t pd_phys = 0x00020000UL;
    uint32_t cr3_rb  = 0;
    uint32_t mapped_read = 0;
    uint16_t i;

    g_cur_tag = "[14] 32-bit Paging (CR3/PG=1)";
    g_test_mark = log_get_pos();

    sub02_smsw();
    if ((g_msw_val & 0x0001U) != 0) {
        finish_fail("Blocked by active V86 monitor (MSW.PE=1)", 0xFF);
        return;
    }
    if (*bda_mem < 192U) {
        finish_fail("Requires >=192KB conventional RAM for page tables at 0x20000", 0xFF);
        return;
    }

    /* Substep 1: 4KB 정렬 Page Directory 및 0~4MB 1:1 Identity Page Table 생성 */
    prep_substep(1, "Build 4KB-aligned Page Directory (0x20000) & Page Table (0x21000)");
    for (i = 0; i < 1024U; i++) {
        pd_ptr[i] = 0x00000002UL;           /* Not Present, R/W */
        pt_ptr[i] = ((uint32_t)i << 12) | 0x00000003UL; /* 0~4MB 1:1 Present + R/W */
    }
    /* 가상 주소 0x003FF000 (1023번 페이지)을 물리 프레임 0x00022000으로 리매핑! */
    pt_ptr[1023] = 0x00022003UL;
    pd_ptr[0]    = 0x00021003UL;            /* PDE[0] -> PT at 0x21000 */
    pg_tgt[0]    = 0x50414745UL;            /* 'PAGE' 매직 값 기록 */

    /* Substep 2: CR3 레지스터에 Page Directory 물리 주소(0x00020000) 로드 및 역읽기 */
    prep_substep(2, "Load CR3 with 0x00020000 and read back CR3");
    _SI = (uint16_t)&pd_phys;
    _DI = (uint16_t)&cr3_rb;
    __emit__(
        0x66, 0x8B, 0x04,                   /* MOV EAX, [SI] */
        0x0F, 0x22, 0xD8,                   /* MOV CR3, EAX  */
        0x0F, 0x20, 0xD8,                   /* MOV EAX, CR3  */
        0x66, 0x89, 0x05                    /* MOV [DI], EAX */
    );
    if ((cr3_rb & 0xFFFFF000UL) != 0x00020000UL) {
        finish_fail("CR3 register did not latch 0x00020000", 0xFF);
        return;
    }

    /* Substep 3: CR0.PE=1 및 CR0.PG=1 (Bit 31) 토글 생존 확인 */
    prep_substep(3, "Enable Paging (CR0.PE=1 | CR0.PG=1) and immediately disable");
    build_test_gdt();
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x04);
    _SI = (uint16_t)&g_gdtr;
    __emit__(
        0xFA,
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,                   /* MOV EAX, CR0 */
        0x66, 0x0D, 0x01, 0x00, 0x00, 0x80, /* OR EAX, 80000001h (PG=1, PE=1) */
        0x0F, 0x22, 0xC0,                   /* MOV CR0, EAX */
        0xEB, 0x00,                         /* JMP SHORT $+2 (TLB/Prefetch sync) */
        0x66, 0x25, 0xFE, 0xFF, 0xFF, 0x7F, /* AND EAX, 7FFFFFFEh (PG=0, PE=0) */
        0x0F, 0x22, 0xC0                    /* MOV CR0, EAX */
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);

    /* Substep 4: PG=1 상태에서 리매핑된 가상 주소(0x003FF000 -> 물리 0x00022000) 읽기 검증 */
    prep_substep(4, "Translate virtual 0x003FF000 -> physical 0x00022000 under PG=1");
    _SI = (uint16_t)&g_gdtr;
    _DI = (uint16_t)&mapped_read;
    __emit__(
        0xFA,
        0x0F, 0x01, 0x14,                   /* LGDT [SI] */
        0x0F, 0x20, 0xC0,
        0x66, 0x0D, 0x01, 0x00, 0x00, 0x80, /* PG=1, PE=1 */
        0x0F, 0x22, 0xC0,
        0xEB, 0x00,
        0xB8, 0x18, 0x00,
        0x8E, 0xE0,                         /* MOV FS, 0018h (4GB Flat) */
        0x66, 0xBB, 0x00, 0xF0, 0x3F, 0x00, /* MOV EBX, 003FF000h (Virtual Page 1023) */
        0x64, 0x67, 0x66, 0x8B, 0x03,       /* MOV EAX, FS:[EBX] */
        0x66, 0x89, 0x05,                   /* MOV [DI], EAX */
        0xB8, 0x10, 0x00,
        0x8E, 0xE0,                         /* MOV FS, 0010h (64KB limit) */
        0x0F, 0x20, 0xC0,
        0x66, 0x25, 0xFE, 0xFF, 0xFF, 0x7F, /* PG=0, PE=0 */
        0x0F, 0x22, 0xC0,
        0xEB, 0x00,
        0x31, 0xC0,
        0x8E, 0xE0,
        0x66, 0x31, 0xC0,
        0x0F, 0x22, 0xD8                    /* MOV CR3, EAX (TLB Flush & Reset) */
    );
    _SI = (uint16_t)g_bios_gdtr;
    __emit__(0x0F, 0x01, 0x14, 0xFB);

    if (mapped_read != 0x50414745UL) {
        finish_fail("MMU virtual-to-physical translation mismatch", 0xFF);
        return;
    }

    finish_pass("CR3 loaded, CR0.PG=1 enabled & MMU virtual remap (0x3FF000->0x22000) OK");
}

/* ---------------------------------------------------------
 * 인터랙티브 메뉴 출력 및 사용자 번호 선택 실행기
 * --------------------------------------------------------- */
static void print_cpu_menu(void)
{
    put_str("\n--- Select CPU Instruction / Mode Micro-Test ---\n");
    put_str("  [1]  FLAGS/EFLAGS Arch (8086/286/386/486/ID)   [8]  MMX 64-bit SIMD (MOVQ/PADDB/EMMS)\n");
    put_str("  [2]  286 Protected Status (SMSW/SGDT/SIDT)     [9]  SSE 128-bit & CR4.OSFXSR\n");
    put_str("  [3]  386 Control/Debug Regs (CR0/CR2/CR3/DR7)  [10] LGDT/SGDT Table Dry-Run\n");
    put_str("  [4]  x87 FPU Coprocessor (FNINIT/FADDP)        [11] 16-bit Protected Mode (4 Substeps)\n");
    put_str("  [5]  CPUID Leaves & Feature Flags              [12] Unreal Mode 4GB Limit (3 Substeps)\n");
    put_str("  [6]  RDTSC & CR4 Monotonic Check               [13] 32-bit Protected Mode (2 Substeps)\n");
    put_str("  [7]  CMPXCHG8B & RDMSR(0x10)                   [14] 32-bit Paging CR3/PG=1 (4 Substeps)\n");
    put_str("  [A]  Run ALL sequentially (1..14)              [L]  Show list   [0/Q] Exit CPU Test\n");
}

static void dispatch_cpu_test(uint8_t num)
{
    if      (num == 1)  test01_flags();
    else if (num == 2)  test02_286_status();
    else if (num == 3)  test03_386_cregs();
    else if (num == 4)  test04_x87_fpu();
    else if (num == 5)  test05_cpuid();
    else if (num == 6)  test06_rdtsc();
    else if (num == 7)  test07_cmpxchg8b_msr();
    else if (num == 8)  test08_mmx();
    else if (num == 9)  test09_sse();
    else if (num == 10) test10_lgdt_dryrun();
    else if (num == 11) test11_pm16_microsteps();
    else if (num == 12) test12_unreal_mode();
    else if (num == 13) test13_pm32_flat();
    else if (num == 14) test14_pm_paging();
}

void run_cpu_test(void)
{
    uint8_t i;

    out_str("=== [Step 9] Granular CPU ISA & Mode Switch Diagnostics ===\n");
    out_str("Pre-writes FAIL to RESULT.TXT before each micro-step; overwrites with PASS on survival.\n");

    print_cpu_menu();

    while (1) {
        char    buf[8];
        uint8_t len = 0;
        uint8_t num = 0;

        put_str("\nSelect test [1-14, A=All, L=List, 0/Q=Exit] > ");

        while (1) {
            uint8_t ch;
            _AH = 0x00;
            __emit__(0xCD, 0x16);
            ch = _AL;

            if (ch == '\r') {
                put_ch('\n');
                break;
            }
            if (ch == 0x08) {
                if (len > 0) {
                    len--;
                }
                continue;
            }
            if (len < 6 && ch >= 0x20 && ch <= 0x7E) {
                buf[len++] = (char)ch;
                put_ch((char)ch);
            }
        }
        buf[len] = '\0';

        if (len == 0) {
            continue;
        }
        if (buf[0] == '0' || buf[0] == 'q' || buf[0] == 'Q') {
            out_str("CPU diagnostic session closed by user.\n\n");
            break;
        }
        if (buf[0] == 'l' || buf[0] == 'L') {
            print_cpu_menu();
            continue;
        }
        if (buf[0] == 'a' || buf[0] == 'A') {
            for (i = 1; i <= 14; i++) {
                dispatch_cpu_test(i);
            }
            continue;
        }

        for (i = 0; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                num = (uint8_t)(num * 10U + (uint8_t)(buf[i] - '0'));
            } else {
                num = 0;
                break;
            }
        }

        if (num >= 1 && num <= 14) {
            dispatch_cpu_test(num);
        } else {
            put_str("  Invalid selection. Enter 1..14, A, L, or Q.\n");
        }
    }
}
