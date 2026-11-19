#ifndef SPKIO_H
#define SPKIO_H

#include "txtio.h"

/* Port 0x61 하드웨어 게이트 래치 응답 상태 (1: 응답 정상, 0: 미응답) */
uint8_t g_spk_latch_ok = 0;

/* ---------------------------------------------------------
 * PIT Channel 0 (1.193182 MHz) 기반 CPU 클록 독립 밀리초 지연
 * --------------------------------------------------------- */
void delay_ms_pit(uint16_t ms)
{
    uint32_t target_ticks;
    uint32_t elapsed = 0;
    uint32_t guard = 0;
    uint16_t prev, curr;

    target_ticks = (uint32_t)ms * 1193UL;

    outb(0x43, 0x00);
    prev = (uint16_t)inb(0x40);
    prev |= ((uint16_t)inb(0x40) << 8);

    while (elapsed < target_ticks && guard < 3000000UL) {
        outb(0x43, 0x00);
        curr = (uint16_t)inb(0x40);
        curr |= ((uint16_t)inb(0x40) << 8);

        elapsed += (uint16_t)(prev - curr);
        prev = curr;
        guard++;
    }
}

/* ---------------------------------------------------------
 * 기본 하드웨어 구형파 제어 (PIT Ch2 Mode 3)
 * --------------------------------------------------------- */
void spk_set_freq(uint16_t freq_hz)
{
    uint16_t divisor;
    if (freq_hz < 20U) {
        freq_hz = 20U;
    }
    divisor = (uint16_t)(1193182UL / (uint32_t)freq_hz);
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)(divisor >> 8));
}

void spk_on(uint16_t freq_hz)
{
    uint8_t p61, rb;
    spk_set_freq(freq_hz);
    p61 = inb(0x61);
    outb(0x61, (uint8_t)(p61 | 0x03));
    rb = inb(0x61);
    if (rb != 0xFF && (rb & 0x03) == 0x03) {
        g_spk_latch_ok = 1;
    }
}

void spk_off(void)
{
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)(p61 & 0xFC));
}

/* 1) 표준 음높이 재생 (최대 출력 100%) */
void spk_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    spk_on(freq_hz);
    delay_ms_pit(duration_ms);
    spk_off();
}

/* 2) 고전 1비트 PWM 듀티비 볼륨 제어 (vol_pct: 1~100%) */
void spk_tone_vol(uint16_t freq_hz, uint16_t duration_ms, uint8_t vol_pct)
{
    uint32_t total_ticks, elapsed = 0;
    uint16_t period_ticks, high_ticks;
    uint16_t prev, curr, phase = 0;
    uint8_t  base_61, hi_61, lo_61;

    if (vol_pct == 0) {
        delay_ms_pit(duration_ms);
        return;
    }
    if (vol_pct >= 100) {
        spk_tone(freq_hz, duration_ms);
        return;
    }
    if (freq_hz < 40U) {
        freq_hz = 40U;
    }

    period_ticks = (uint16_t)(1193182UL / (uint32_t)freq_hz);
    high_ticks   = (uint16_t)(((uint32_t)(period_ticks >> 1) * (uint32_t)vol_pct) / 100UL);
    if (high_ticks == 0) {
        high_ticks = 1;
    }
    total_ticks = (uint32_t)duration_ms * 1193UL;

    /* Bit 0(PIT Ch2 게이트) 끄고 Bit 1(스피커 콘)을 직접 PWM 구동 */
    base_61 = (uint8_t)(inb(0x61) & 0xFC);
    hi_61   = (uint8_t)(base_61 | 0x02);
    lo_61   = base_61;

    outb(0x43, 0x00);
    prev = (uint16_t)inb(0x40);
    prev |= ((uint16_t)inb(0x40) << 8);

    while (elapsed < total_ticks) {
        uint16_t dt;
        outb(0x43, 0x00);
        curr = (uint16_t)inb(0x40);
        curr |= ((uint16_t)inb(0x40) << 8);

        dt = (uint16_t)(prev - curr);
        prev = curr;
        elapsed += dt;
        phase   += dt;
        while (phase >= period_ticks) {
            phase -= period_ticks;
        }
        outb(0x61, (phase < high_ticks) ? hi_61 : lo_61);
    }
    outb(0x61, lo_61);
}

/* 3) 연속 주파수 슬라이드 / 글리산도 (사이렌·레이저 효과) */
void spk_sweep(uint16_t start_hz, uint16_t end_hz, uint16_t duration_ms)
{
    uint16_t steps, s, step_ms;
    int32_t  diff;

    steps = duration_ms / 5U;
    if (steps == 0) {
        steps = 1;
    }
    step_ms = duration_ms / steps;
    diff = (int32_t)end_hz - (int32_t)start_hz;

    spk_on(start_hz);
    for (s = 0; s <= steps; s++) {
        uint16_t f = (uint16_t)((int32_t)start_hz + (diff * (int32_t)s) / (int32_t)steps);
        spk_set_freq(f);
        delay_ms_pit(step_ms);
    }
    spk_off();
}

/* 4) 비브라토 / 트릴 효과 (중심 주파수 상하 진동) */
void spk_vibrato(uint16_t center_hz, uint16_t depth_hz, uint16_t step_ms, uint16_t duration_ms)
{
    uint16_t elapsed = 0;
    uint8_t  dir = 0;

    spk_on(center_hz);
    while (elapsed < duration_ms) {
        spk_set_freq(dir ? (center_hz + depth_hz) : (center_hz - depth_hz));
        dir = (uint8_t)!dir;
        delay_ms_pit(step_ms);
        elapsed += step_ms;
    }
    spk_off();
}

void spk_rest(uint16_t duration_ms)
{
    spk_off();
    delay_ms_pit(duration_ms);
}

/* Step 0용 정보성 비프 (1회: 화면 활성, 2회: 화면 불가) */
void beep_code(uint8_t count)
{
    uint8_t i;
    for (i = 0; i < count; i++) {
        if (i > 0) {
            delay_ms_pit(120);
        }
        spk_tone(1000, 140);
    }
}

#endif /* SPKIO_H */