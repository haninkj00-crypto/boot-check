typedef unsigned char  uint8_t;
typedef unsigned int   uint16_t;
typedef unsigned long  uint32_t;

#define MK_FP(seg, ofs) ((void far *)(((uint32_t)(seg) << 16) | (uint16_t)(ofs)))

extern void out_str(const char *s);
extern void out_ulong(uint32_t val);

static volatile uint32_t far *bios_ticks =
    (volatile uint32_t far *)MK_FP(0x0040, 0x006C);

static uint8_t flame_src[375] = { 1 };
static uint8_t flame_dst[375] = { 0 };

static uint32_t wait_next_tick(void)
{
    uint32_t t0 = *bios_ticks;
    while (*bios_ticks == t0) {
    }
    return *bios_ticks;
}

static uint32_t bench_nibbles_10ticks(void)
{
    uint32_t t_start;
    uint32_t units = 0;
    volatile uint16_t i;

    t_start = wait_next_tick();
    while (1) {
        uint32_t now = *bios_ticks;
        if (now < t_start) {
            t_start = now;
            units = 0;
        }
        if ((now - t_start) >= 10UL) {
            break;
        }
        for (i = 0; i < 1000; i++) {
        }
        units++;
    }
    return units;
}

static uint32_t bench_cake_10ticks(void)
{
    uint32_t t_start;
    uint32_t frames = 0;
    const short cCandles = 20;
    const short pt1_x = 40, pt1_y = 125;
    const short pt2_x = 600;
    volatile short sink = 0;
    short i, px;

    t_start = wait_next_tick();
    while (1) {
        uint32_t now = *bios_ticks;
        if (now < t_start) {
            t_start = now;
            frames = 0;
        }
        if ((now - t_start) >= 10UL) {
            break;
        }
        for (i = 0; i < cCandles; i++) {
            short x = (short)(((long)(cCandles - i) * pt1_x +
                               (long)(i + 1) * pt2_x) / (cCandles + 1));
            short y = pt1_y;

            if (i % 2 == 1) y -= 5;
            else            y += 5;

            for (px = 0; px < 375; px++) {
                flame_dst[px] = flame_src[px];
            }
            sink += (x + y);
        }
        frames++;
    }
    return frames;
}

void run_timer_test(void)
{
    uint32_t nib_10, nib_per_tick;
    uint32_t cake_10, cake_per_tick;

    out_str("=== [Step 3] Timer & DIV 0 Compatibility Test ===\n");
    out_str("Measuring over 10 BIOS ticks (~549 ms)...\n\n");

    nib_10 = bench_nibbles_10ticks();
    nib_per_tick = nib_10 / 10UL;

    out_str("[1] NIBBLES Calibration Loop (1000 iters/unit)\n");
    out_str("    10-tick total : "); out_ulong(nib_10); out_str(" units\n");
    out_str("    Speed (avg)   : "); out_ulong(nib_per_tick); out_str(" units/tick\n");
    out_str("    Verdict       : ");
    if (nib_per_tick == 0) {
        out_str("SAFE (stopTime - startTime >= 1 tick, no DIV 0)\n\n");
    } else {
        out_str("DIV 0 RISK! (1000-loop finishes within 0 ticks -> stopTime == startTime)\n\n");
    }

    cake_10 = bench_cake_10ticks();
    cake_per_tick = cake_10 / 10UL;

    out_str("[2] CAKE WndProc Render + Timer Frame (20 candles)\n");
    out_str("    10-tick total : "); out_ulong(cake_10); out_str(" frames\n");
    out_str("    Speed (avg)   : "); out_ulong(cake_per_tick); out_str(" frames/tick\n");
    out_str("    Verdict       : ");
    if (cake_per_tick == 0) {
        out_str("SLOW CPU (1 frame > 1 tick -> SetTimer 1ms queue saturation)\n\n");
    } else if (cake_per_tick < 10) {
        out_str("OPTIMAL (1~9 frames/tick -> target 386/486 timing window)\n\n");
    } else {
        out_str("FAST CPU / DIV 0 RISK (>=10 frames/tick -> dt == 0 if tick-divided)\n\n");
    }
}