typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern uint8_t g_spk_latch_ok;
extern void spk_tone(uint16_t freq_hz, uint16_t duration_ms);
extern void spk_tone_vol(uint16_t freq_hz, uint16_t duration_ms, uint8_t vol_pct);
extern void spk_rest(uint16_t duration_ms);
extern void out_str(const char *s);
uint8_t g_spk_audible_or_partial = 0;

static volatile uint32_t far *spk_ticks =
    (volatile uint32_t far *)MK_FP(0x0040, 0x006C);

void run_speaker_test(void)
{
    /* 반짝반짝 작은별 / ABCD송 앞 7음절: 도 도 솔 솔 라 라 솔 (C5, C5, G5, G5, A5, A5, G5) */
    const uint16_t notes[7] = { 523, 523, 784, 784, 880, 880, 784 };
    const uint16_t durs[7]  = { 260, 260, 260, 260, 260, 260, 520 };
    uint8_t  i, ans = 0;
    uint32_t t0;

    out_str("=== [Step 2] PC Speaker Melody & Audible Check ===\n");
    out_str("  Playing first 7 notes of Twinkle Star / ABCD (C-C-G-G-A-A-G)...\n");

    for (i = 0; i < 7; i++) {
        spk_tone(notes[i], durs[i]);
        spk_rest(55);
    }

    /* PWM 볼륨 감쇠 확인용 짧은 에코 1음 (솔 35% 볼륨) */
    spk_tone_vol(784, 220, 35);

    out_str("  Port 0x61 Latch Status : ");
    out_str(g_spk_latch_ok ? "LATCH DETECTED\n" : "NO LATCH (0xFF/Static)\n");

    out_str("  Did you hear the 7-note melody? Press [Y] or [N] (10s timeout): ");

    /* 키 버퍼 비우고 Y/N 응답 대기 */
    while (1) {
        _AH = 0x01;
        __emit__(0xCD, 0x16);
        if ((_FLAGS & 0x0040) != 0) {
            break;
        }
        _AH = 0x00;
        __emit__(0xCD, 0x16);
    }

    t0 = *spk_ticks;
    while ((*spk_ticks - t0) < 182UL) {
        _AH = 0x01;
        __emit__(0xCD, 0x16);
        if ((_FLAGS & 0x0040) == 0) {
            uint8_t ch;
            _AH = 0x00;
            __emit__(0xCD, 0x16);
            ch = _AL;
            if (ch == 'y' || ch == 'Y') {
                ans = 'Y';
                break;
            }
            if (ch == 'n' || ch == 'N') {
                ans = 'N';
                break;
            }
        }
    }

    if (ans == 'Y') {
        g_spk_audible_or_partial = 1;
        out_str("YES (Audible Confirmed -> Radio Time Signal Armed)\n\n");
    } else if (ans == 'N') {
        g_spk_audible_or_partial = (g_spk_latch_ok != 0) ? 1 : 0;
        if (g_spk_audible_or_partial) {
            out_str("NO (Muted, but 0x61 Latch OK -> Partial Pass)\n\n");
        } else {
            out_str("NO (Muted / Piezo Absent)\n\n");
        }
    } else {
        g_spk_audible_or_partial = (g_spk_latch_ok != 0) ? 1 : 0;
        if (g_spk_audible_or_partial) {
            out_str("TIMEOUT (Port 0x61 Latch OK -> Partial Pass)\n\n");
        } else {
            out_str("TIMEOUT (No response & No latch)\n\n");
        }
    }
}