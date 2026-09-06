/* Aurora Beadscape v1.1 - Beads-inspired musical granular texture processor
   Independent Aurora-native implementation. No Mutable Instruments Beads code used.
   v1.1: balanced Freeze level, gentler feedback, musical Scatter/Reverse/Lush gain staging. */
#include "aurora.h"
#include "daisysp.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace daisy;
using namespace daisysp;
using namespace aurora;

namespace
{
constexpr size_t kNumGrains = 6;
constexpr size_t kMaxDelaySamples = 120000; // 2.5 s at 48 kHz
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kBypassThreshold = 0.004f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS grain_line[kNumGrains];
ReverbSc DSY_SDRAM_BSS reverb;

float sample_rate = 48000.0f;
float grain_phase[kNumGrains] = {0.00f, 0.17f, 0.34f, 0.51f, 0.68f, 0.85f};
float grain_jitter[kNumGrains] = {0.0f};
float grain_pan[kNumGrains] = {0.12f, 0.82f, 0.28f, 0.72f, 0.42f, 0.62f};
float grain_feedback_lp[kNumGrains] = {0.0f};
uint32_t rng_state = 0x6D2B79F5u;

bool freeze_latched = false;
bool reverse_latched = false;
bool lush_latched = false;
bool scatter_latched = false;

volatile float ui_position = 0.0f;
volatile float ui_feedback = 0.0f;
volatile float ui_mix = 0.0f;
volatile float ui_density = 0.0f;
volatile float ui_size = 0.0f;
volatile float ui_pitch = 0.5f;
volatile int ui_grains = 2;
volatile bool ui_freeze = false;
volatile bool ui_reverse = false;
volatile bool ui_lush = false;
volatile bool ui_scatter = false;

inline bool Finite(float x) { return std::isfinite(x); }
inline float Safe(float x) { return Finite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float Saturate(float x) { return tanhf(Bound(x, 5.0f)); }
inline float KnobCv(int knob, int cv) { return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv)); }

float Rand01()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return static_cast<float>(rng_state & 0x00FFFFFFu) / 16777215.0f;
}

float Hann(float phase)
{
    return 0.5f - 0.5f * cosf(kTwoPi * Clamp01(phase));
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();

    // Robust SHIFT chord router: SHIFT only modifies FREEZE / REVERSE.
    static float shift_grace = 0.0f;
    const bool shift_pressed = hw.GetButton(SW_SHIFT).Pressed();
    if(shift_pressed)
        shift_grace = 0.18f;
    else
    {
        shift_grace -= static_cast<float>(size) / sample_rate;
        if(shift_grace < 0.0f) shift_grace = 0.0f;
    }
    const bool modifier = shift_pressed || shift_grace > 0.0f;

    const bool freeze_edge = hw.GetButton(SW_FREEZE).RisingEdge();
    const bool reverse_edge = hw.GetButton(SW_REVERSE).RisingEdge();
    if(freeze_edge)
    {
        if(modifier) { lush_latched = !lush_latched; shift_grace = 0.0f; }
        else freeze_latched = !freeze_latched;
    }
    if(reverse_edge)
    {
        if(modifier) { scatter_latched = !scatter_latched; shift_grace = 0.0f; }
        else reverse_latched = !reverse_latched;
    }

    const bool freeze = freeze_latched != hw.GetGateState(GATE_FREEZE);
    const bool reverse = reverse_latched != hw.GetGateState(GATE_REVERSE);
    const bool lush = lush_latched;
    const bool scatter = scatter_latched;

    const float position = KnobCv(KNOB_TIME, CV_TIME);
    const float feedback = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float mix = KnobCv(KNOB_MIX, CV_MIX);
    const float density = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float size_ctl = KnobCv(KNOB_BLUR, CV_BLUR);
    const float pitch_ctl = Clamp01(hw.GetKnobValue(KNOB_WARP) + hw.GetWarpVoct() / 60.0f);

    const int active_grains = 2 + static_cast<int>(density * 4.999f);
    const float grain_seconds = 0.020f * powf(18.0f, size_ctl); // ~20 ms to ~360 ms
    const float grain_samples = fclamp(grain_seconds * sample_rate, 96.0f, 17500.0f);
    const float base_delay = fclamp((0.020f + 2.20f * position * position) * sample_rate,
                                    64.0f, static_cast<float>(kMaxDelaySamples - 2048));
    const float semitones = (pitch_ctl - 0.5f) * 48.0f;
    const float pitch_ratio = powf(2.0f, semitones / 12.0f);
    const float reverse_ratio = fclamp(pitch_ratio, 0.50f, 2.0f);
    const float playback_ratio = reverse ? -reverse_ratio : pitch_ratio;
    const float phase_rate = (0.50f + 1.45f * density) / grain_samples;

    // v1.1: gentler, more musical random motion.
    const float jitter_depth = (0.010f + 0.115f * density * density) * sample_rate;
    const float scatter_gain = scatter ? 1.45f : 1.0f;

    // v1.1: Freeze now holds with slow decay instead of runaway regeneration.
    const float fb = freeze ? 0.972f : (0.025f + 0.80f * feedback * feedback);
    const float fb_smooth = 0.27f - 0.16f * size_ctl;

    ui_position = position;
    ui_feedback = feedback;
    ui_mix = mix;
    ui_density = density;
    ui_size = size_ctl;
    ui_pitch = pitch_ctl;
    ui_grains = active_grains;
    ui_freeze = freeze;
    ui_reverse = reverse;
    ui_lush = lush;
    ui_scatter = scatter;

    if(mix <= kBypassThreshold)
    {
        for(size_t i = 0; i < size; ++i)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        return;
    }

    const float dry_gain = cosf(mix * kPi * 0.5f);

    // Effects are level-compensated so toggling an effect changes colour, not volume.
    float fx_trim = 1.0f;
    if(freeze) fx_trim *= 0.72f;
    if(reverse) fx_trim *= 0.94f;
    if(lush) fx_trim *= 0.90f;
    if(scatter) fx_trim *= 0.86f;
    const float wet_gain = sinf(mix * kPi * 0.5f) * 0.98f * fx_trim;

    const float verb_amount = lush ? 0.78f : (0.04f + 0.46f * size_ctl * size_ctl);
    reverb.SetFeedback(lush ? 0.885f : (0.73f + 0.11f * size_ctl));
    reverb.SetLpFreq(lush ? 12500.0f : (13500.0f - 5200.0f * size_ctl));

    for(size_t i = 0; i < size; ++i)
    {
        const float input_l = Bound(in[0][i], 2.0f);
        const float input_r = Bound(in[1][i], 2.0f);
        const float mono = Bound((input_l + input_r) * 0.5f, 1.5f);

        float wet_l = 0.0f;
        float wet_r = 0.0f;
        float norm = 0.0f;

        for(int g = 0; g < active_grains; ++g)
        {
            const float p = grain_phase[g];
            const float window = Hann(p);
            const float scan = (p - 0.5f) * (1.0f - playback_ratio) * grain_samples;
            const float stereo_offset = (static_cast<float>(g) - 2.5f) * (28.0f + 105.0f * density);
            float d = base_delay + grain_jitter[g] * scatter_gain + scan + stereo_offset;
            d = fclamp(d, 2.0f, static_cast<float>(kMaxDelaySamples - 4));
            grain_line[g].SetDelay(d);
            const float tap = Safe(grain_line[g].Read());

            float pan = grain_pan[g];
            if(scatter)
                pan = fclamp(0.5f + (pan - 0.5f) * 1.28f, 0.08f, 0.92f);
            const float gl = sqrtf(fmaxf(0.0f, 1.0f - pan));
            const float gr = sqrtf(fmaxf(0.0f, pan));
            const float v = tap * window;
            wet_l += v * gl;
            wet_r += v * gr;
            norm += window;

            grain_feedback_lp[g] += fb_smooth * (tap - grain_feedback_lp[g]);
            const float fb_drive = 1.0f + feedback * 1.15f;
            const float fb_signal = Saturate(grain_feedback_lp[g] * fb_drive);
            const float write_in = freeze ? 0.0f : mono;
            grain_line[g].Write(Bound(write_in * 0.82f + fb_signal * fb, 1.16f));

            grain_phase[g] += phase_rate * (1.0f + 0.045f * static_cast<float>(g));
            if(grain_phase[g] >= 1.0f)
            {
                grain_phase[g] -= 1.0f;
                const float r = Rand01() * 2.0f - 1.0f;
                grain_jitter[g] = r * jitter_depth * (scatter ? 1.55f : 1.0f);
                grain_pan[g] = scatter
                    ? (0.12f + 0.76f * Rand01())
                    : fclamp(0.15f + 0.14f * static_cast<float>(g), 0.0f, 1.0f);
            }
        }

        if(norm > 0.05f)
        {
            const float scale = (1.00f + 0.28f * density) / norm;
            wet_l *= scale;
            wet_r *= scale;
        }

        // Musical smear: moderate stereo cross-spread instead of aggressive crossfeed.
        const float cross = (0.04f + 0.22f * density) * size_ctl;
        const float pre_l = wet_l;
        const float pre_r = wet_r;
        wet_l = Saturate(pre_l + pre_r * cross);
        wet_r = Saturate(pre_r - pre_l * cross * 0.62f);

        float rv_l = 0.0f, rv_r = 0.0f;
        reverb.Process(Bound(wet_l * verb_amount, 0.68f), Bound(wet_r * verb_amount, 0.68f), &rv_l, &rv_r);
        const float verb_return = lush ? 0.80f : 0.55f;
        wet_l = Saturate(wet_l + rv_l * verb_amount * verb_return);
        wet_r = Saturate(wet_r + rv_r * verb_amount * verb_return);

        out[0][i] = Saturate(input_l * dry_gain + wet_l * wet_gain);
        out[1][i] = Saturate(input_r * dry_gain + wet_r * wet_gain);
    }
}

float Pulse(float value, float phase, float offset)
{
    const float v = Clamp01(value);
    const float speed = 0.55f + 5.0f * v;
    const float s = 0.5f + 0.5f * sinf(phase * speed + offset);
    return fclamp(0.05f + v * (0.28f + 0.72f * s), 0.0f, 1.0f);
}

void UpdateLeds()
{
    static float led_phase = 0.0f;
    static uint32_t blink_counter = 0;
    led_phase += 0.075f;
    if(led_phase > kTwoPi * 12.0f) led_phase = 0.0f;
    ++blink_counter;
    const bool alt = ((blink_counter / 20u) & 1u) != 0u;

    hw.ClearLeds();
    const float p1 = Pulse(ui_position, led_phase, 0.0f);
    const float p2 = Pulse(ui_feedback, led_phase, 0.7f);
    const float p3 = Pulse(ui_mix, led_phase, 1.4f);
    const float p4 = Pulse(ui_density, led_phase, 2.1f);
    const float p5 = Pulse(ui_size, led_phase, 2.8f);
    const float pitch_amount = fabsf(ui_pitch - 0.5f) * 2.0f;
    const float p6 = Pulse(pitch_amount, led_phase, 3.5f);

    hw.SetLed(LED_1, 0.0f, 0.20f * p1, p1);
    hw.SetLed(LED_2, p2, 0.0f, 0.0f);
    hw.SetLed(LED_3, p3, p3, 0.0f);
    hw.SetLed(LED_4, 0.0f, p4, 0.18f * p4);
    hw.SetLed(LED_5, 0.0f, 0.65f * p5, p5);
    hw.SetLed(LED_6, p6, 0.0f, p6);

    if(ui_lush && ui_scatter)
    {
        hw.SetLed(LED_BOT_1, 0.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_2, 1.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_3, 0.0f, 0.0f, 1.0f);
    }
    else if(ui_lush)
    {
        hw.SetLed(LED_BOT_1, 0.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_2, 0.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_3, 0.0f, 0.0f, 1.0f);
    }
    else if(ui_scatter)
    {
        hw.SetLed(LED_BOT_1, 1.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_2, 1.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_3, 1.0f, 0.0f, 1.0f);
    }
    else
    {
        for(int j = 0; j < 3; ++j)
        {
            const float on = j < (ui_grains - 2) ? 0.80f : 0.05f;
            hw.SetLed(static_cast<Leds>(LED_BOT_1 + j), 0.0f, on, 0.15f * on);
        }
    }

    if(ui_freeze && ui_lush)
    {
        if(alt) hw.SetLed(LED_FREEZE, 1.0f, 1.0f, 1.0f);
        else hw.SetLed(LED_FREEZE, 0.0f, 0.0f, 1.0f);
    }
    else if(ui_lush) hw.SetLed(LED_FREEZE, 0.0f, 0.0f, 1.0f);
    else if(ui_freeze) hw.SetLed(LED_FREEZE, 1.0f, 1.0f, 1.0f);
    else hw.SetLed(LED_FREEZE, 0.03f, 0.03f, 0.03f);

    if(ui_reverse && ui_scatter)
    {
        if(alt) hw.SetLed(LED_REVERSE, 0.0f, 1.0f, 1.0f);
        else hw.SetLed(LED_REVERSE, 1.0f, 0.0f, 1.0f);
    }
    else if(ui_scatter) hw.SetLed(LED_REVERSE, 1.0f, 0.0f, 1.0f);
    else if(ui_reverse) hw.SetLed(LED_REVERSE, 0.0f, 1.0f, 1.0f);
    else hw.SetLed(LED_REVERSE, 0.03f, 0.03f, 0.03f);

    hw.WriteLeds();
}

} // namespace

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();
    for(size_t g = 0; g < kNumGrains; ++g)
    {
        grain_line[g].Init();
        grain_line[g].SetDelay(2400.0f + 300.0f * static_cast<float>(g));
    }
    reverb.Init(sample_rate);
    reverb.SetFeedback(0.78f);
    reverb.SetLpFreq(10500.0f);
    hw.StartAudio(AudioCallback);
    while(1)
    {
        UpdateLeds();
        System::Delay(12);
    }
}