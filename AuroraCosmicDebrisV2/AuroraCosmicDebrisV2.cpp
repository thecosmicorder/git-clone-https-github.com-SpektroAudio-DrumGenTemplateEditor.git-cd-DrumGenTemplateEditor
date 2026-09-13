/* Aurora Cosmic Debris v2.0 MANUAL ARCHITECTURE
   Independent Aurora adaptation based on the published WMD Cosmic Debris manual.

   NORMAL PAGE
     TIME       = TIME
     REFLECT    = FEEDBACK
     MIX        = MIX / SEND LEVEL when SEND mode is enabled
     ATMOSPHERE = SPRAY
     BLUR       = SCATTER
     WARP       = MOD amount

   Hold SHIFT and tap REVERSE to cycle three secondary banks.

   SHIFT BANK A - TIME / MOD
     TIME       = PRE DELAY
     REFLECT    = RATE
     MIX        = WET GAIN
     ATMOSPHERE = SIDE CHAIN
     BLUR       = SLEW RATE (near CCW = crossfade mode)
     WARP       = SPREAD

   SHIFT BANK B - FILTER / FX
     TIME       = CENTER
     REFLECT    = WIDTH
     MIX        = SHAPE (sine -> triangle -> smooth random)
     ATMOSPHERE = ANOMALY
     BLUR       = WARP FACTOR (-12..+12 semitones)
     WARP       = reserved / clock division trim

   SHIFT BANK C - SYSTEM
     TIME       = GATE MODE (FREEZE / BLAST / GLITCH)
     REFLECT    = SEND MODE off/on
     MIX        = FREE / SYNC mode
     ATMOSPHERE = BLAST intensity
     BLUR       = GLITCH intensity
     WARP       = clock range (short / normal / long)

   BUTTONS
     FREEZE                 = momentary FREEZE
     REVERSE                = cycle BLUR / RATIO / WARP
     SHIFT + FREEZE         = momentary BLAST
     SHIFT + REVERSE        = cycle SHIFT bank A / B / C
     FREEZE + REVERSE       = TAP tempo
     SHIFT + both buttons   = KILL / clear delay buffers

   GATES
     GATE_FREEZE  = CLOCK
     GATE_REVERSE = configurable GATE (FREEZE / BLAST / GLITCH)
*/

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
constexpr int    kLines              = 16;
constexpr size_t kMaxDelaySamples    = 120000;
constexpr size_t kMaxPreDelaySamples = 24000;
constexpr float  kPi                 = 3.14159265358979323846f;
constexpr float  kTwoPi              = 6.28318530717958647692f;
constexpr float  kPickupThreshold    = 0.035f;
constexpr float  kCrossfadeTime      = 0.030f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delay_line[kLines];
DelayLine<float, kMaxPreDelaySamples> DSY_SDRAM_BSS pre_delay[2];
float sample_rate = 48000.0f;

float delay_now[kLines];
float delay_target[kLines];
float xfade_from[kLines];
float xfade_to[kLines];
float xfade_pos[kLines];
float filter_hp_state[kLines] = {0.0f};
float filter_lp_state[kLines] = {0.0f};
float lfo_phase[kLines] = {0.0f};
float random_prev[kLines] = {0.0f};
float random_next[kLines] = {0.0f};
uint32_t rng_state = 0x41c6ce57u;
float pitch_phase[2] = {0.13f, 0.61f};

float normal_param[6] = {0.35f, 0.38f, 0.55f, 0.20f, 0.05f, 0.12f};
bool normal_pickup[6] = {true, true, true, true, true, true};
bool controls_ready = false;

float shift_param[3][6] = {
    {0.00f, 0.25f, 0.333f, 0.00f, 0.12f, 0.35f},
    {0.50f, 0.00f, 0.00f, 0.00f, 0.50f, 0.50f},
    {0.00f, 0.00f, 0.00f, 0.60f, 0.75f, 0.50f}
};
float shift_capture[6] = {0.0f};
bool shift_was_down = false;
uint8_t shift_bank = 0;
uint8_t last_shift_bank = 0;

uint8_t mode = 0;
bool prev_clock_gate = false;
bool prev_both = false;
bool prev_shift_both = false;
uint64_t sample_counter = 0;
uint64_t last_ext_clock_sample = 0;
uint64_t ext_clock_period = 24000;
bool ext_clock_seen = false;
uint64_t last_tap_sample = 0;
uint64_t tap_period = 24000;
bool tap_seen = false;
volatile bool reset_requested = false;
volatile uint32_t kill_countdown = 0;
float input_env = 0.0f;

volatile float ui_normal[6] = {0.0f};
volatile float ui_shift[6] = {0.0f};
volatile bool ui_pickup[6] = {true, true, true, true, true, true};
volatile bool ui_shift_down = false;
volatile bool ui_freeze = false;
volatile bool ui_blast = false;
volatile bool ui_glitch = false;
volatile bool ui_send = false;
volatile bool ui_sync = false;
volatile uint8_t ui_mode = 0;
volatile uint8_t ui_shift_bank = 0;

inline bool Finite(float x)
{
    return (x == x) && x > -100000.0f && x < 100000.0f;
}
inline float Safe(float x) { return Finite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float FastSat(float x)
{
    x = Bound(x, 3.0f);
    return x / (1.0f + 0.38f * fabsf(x));
}
inline float SmoothStep(float x)
{
    x = Clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}
inline float FastSine(float phase)
{
    const float x = phase * kTwoPi - kPi;
    const float ax = fabsf(x);
    float y = (4.0f * x * (kPi - ax)) / (kPi * kPi);
    y = 0.225f * (y * fabsf(y) - y) + y;
    return fclamp(y, -1.0f, 1.0f);
}
inline float Triangle(float phase)
{
    return 1.0f - 4.0f * fabsf(phase - 0.5f);
}

float NextRandom()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    const float u = static_cast<float>(rng_state & 0x00ffffffu) / 8388607.5f;
    return u - 1.0f;
}

float ReadKnob(int i)
{
    switch(i)
    {
        case 0: return Clamp01(hw.GetKnobValue(KNOB_TIME));
        case 1: return Clamp01(hw.GetKnobValue(KNOB_REFLECT));
        case 2: return Clamp01(hw.GetKnobValue(KNOB_MIX));
        case 3: return Clamp01(hw.GetKnobValue(KNOB_ATMOSPHERE));
        case 4: return Clamp01(hw.GetKnobValue(KNOB_BLUR));
        default:return Clamp01(hw.GetKnobValue(KNOB_WARP));
    }
}

void InitializeControls()
{
    for(int i = 0; i < 6; ++i)
    {
        normal_param[i] = ReadKnob(i);
        normal_pickup[i] = true;
        shift_capture[i] = normal_param[i];
    }
    controls_ready = true;
}

void EnterShift()
{
    for(int i = 0; i < 6; ++i)
        shift_capture[i] = ReadKnob(i);
    last_shift_bank = shift_bank;
}

void UpdateControls(bool shift)
{
    if(!controls_ready)
        InitializeControls();
    if(shift && !shift_was_down)
        EnterShift();

    if(shift)
    {
        if(last_shift_bank != shift_bank)
            EnterShift();
        for(int i = 0; i < 6; ++i)
        {
            const float now = ReadKnob(i);
            const float delta = now - shift_capture[i];
            if(fabsf(delta) > 0.001f)
            {
                shift_param[shift_bank][i] = Clamp01(shift_param[shift_bank][i] + delta);
                shift_capture[i] = now;
            }
        }
    }
    else
    {
        if(shift_was_down)
            for(int i = 0; i < 6; ++i)
                normal_pickup[i] = false;
        for(int i = 0; i < 6; ++i)
        {
            const float raw = ReadKnob(i);
            if(!normal_pickup[i] && fabsf(raw - normal_param[i]) <= kPickupThreshold)
                normal_pickup[i] = true;
            if(normal_pickup[i])
                normal_param[i] = raw;
        }
    }
    shift_was_down = shift;
}

void Hadamard16(float x[16])
{
    for(int step = 1; step < 16; step <<= 1)
    {
        for(int base = 0; base < 16; base += (step << 1))
        {
            for(int j = 0; j < step; ++j)
            {
                const float a = x[base + j];
                const float b = x[base + j + step];
                x[base + j] = a + b;
                x[base + j + step] = a - b;
            }
        }
    }
    for(int i = 0; i < 16; ++i)
        x[i] *= 0.25f;
}

inline float BandPass(int i, float x, float hp_coeff, float lp_coeff)
{
    filter_hp_state[i] += hp_coeff * (x - filter_hp_state[i]);
    const float high = x - filter_hp_state[i];
    filter_lp_state[i] += lp_coeff * (high - filter_lp_state[i]);
    return Safe(filter_lp_state[i]);
}

float LfoValue(int i, float shape, float phase)
{
    const float s = FastSine(phase);
    const float t = Triangle(phase);
    const float r = random_prev[i] + (random_next[i] - random_prev[i]) * SmoothStep(phase);
    if(shape < 0.5f)
    {
        const float m = shape * 2.0f;
        return s + (t - s) * m;
    }
    const float m = (shape - 0.5f) * 2.0f;
    return t + (r - t) * m;
}

float PitchFeedbackRead(int i, float delay, float ratio, float phase_inc)
{
    const float window = 2200.0f;
    float p = pitch_phase[i];
    float p2 = p + 0.5f;
    if(p2 >= 1.0f) p2 -= 1.0f;
    const float o1 = ratio >= 1.0f ? (1.0f - p) * window : p * window;
    const float o2 = ratio >= 1.0f ? (1.0f - p2) * window : p2 * window;
    const float d1 = fclamp(delay + o1, 4.0f, static_cast<float>(kMaxDelaySamples - 4));
    const float d2 = fclamp(delay + o2, 4.0f, static_cast<float>(kMaxDelaySamples - 4));
    const float w1 = 1.0f - fabsf(2.0f * p - 1.0f);
    const float a = Safe(delay_line[i].Read(d1));
    const float b = Safe(delay_line[i].Read(d2));
    p += phase_inc;
    if(p >= 1.0f) p -= 1.0f;
    pitch_phase[i] = p;
    return a * w1 + b * (1.0f - w1);
}

float FeedbackAmount(float knob)
{
    knob = Clamp01(knob);
    if(knob <= 0.5f) return knob * 2.0f;
    const float x = (knob - 0.5f) * 2.0f;
    return 1.0f + 0.28f * x * x;
}

float ClockDivision(float time_knob, float range_knob)
{
    static const float divs[16] = {
        0.0625f, 0.083333f, 0.125f, 0.166667f,
        0.25f, 0.333333f, 0.5f, 0.666667f,
        0.75f, 1.0f, 1.333333f, 1.5f,
        2.0f, 3.0f, 4.0f, 6.0f
    };
    int idx = static_cast<int>(Clamp01(time_knob) * 15.999f);
    if(idx > 15) idx = 15;
    float range = 1.0f;
    if(range_knob < 0.333f) range = 0.5f;
    else if(range_knob > 0.666f) range = 2.0f;
    return divs[idx] * range;
}

float BaseDelaySamples(float time_knob, bool sync_mode)
{
    if(sync_mode)
    {
        uint64_t period = tap_seen ? tap_period : 24000;
        const uint64_t since_ext = sample_counter - last_ext_clock_sample;
        if(ext_clock_seen && since_ext < static_cast<uint64_t>(sample_rate * 2.5f))
            period = ext_clock_period;
        const float d = static_cast<float>(period) * ClockDivision(time_knob, shift_param[2][5]);
        return fclamp(d, 19.2f, static_cast<float>(kMaxDelaySamples - 4096));
    }
    const float seconds = 0.0004f * powf(3750.0f, Clamp01(time_knob));
    return fclamp(seconds * sample_rate, 19.2f, static_cast<float>(kMaxDelaySamples - 4096));
}

void ComputeDelayTargets(float base, float spray, uint8_t m)
{
    static const float blur_full[16] = {
        1.000f, 0.930f, 0.860f, 0.790f,
        0.720f, 0.650f, 0.580f, 0.515f,
        0.455f, 0.400f, 0.350f, 0.305f,
        0.265f, 0.225f, 0.185f, 0.145f
    };
    static const float ratio_full[16] = {
        1.000f, 2.000f, 1.500f, 2.667f,
        1.333f, 2.500f, 1.250f, 3.000f,
        1.667f, 2.250f, 1.125f, 3.500f,
        1.750f, 2.750f, 1.875f, 4.000f
    };
    for(int i = 0; i < 16; ++i)
    {
        float ratio;
        if(m == 1)
        {
            const float start = (i & 1) ? 2.0f : 1.0f;
            ratio = start + (ratio_full[i] - start) * spray;
        }
        else
        {
            ratio = 1.0f + (blur_full[i] - 1.0f) * spray;
        }
        delay_target[i] = fclamp(base * ratio, 4.0f,
                                 static_cast<float>(kMaxDelaySamples - 4096));
    }
}

void RegisterClock()
{
    const uint64_t now = sample_counter;
    if(last_ext_clock_sample != 0)
    {
        const uint64_t p = now - last_ext_clock_sample;
        if(p > static_cast<uint64_t>(sample_rate * 0.015f)
           && p < static_cast<uint64_t>(sample_rate * 8.0f))
        {
            ext_clock_period = p;
            ext_clock_seen = true;
        }
    }
    last_ext_clock_sample = now;
}

void RegisterTap()
{
    const uint64_t now = sample_counter;
    if(last_tap_sample != 0)
    {
        const uint64_t p = now - last_tap_sample;
        if(p > static_cast<uint64_t>(sample_rate * 0.12f)
           && p < static_cast<uint64_t>(sample_rate * 4.0f))
        {
            tap_period = p;
            tap_seen = true;
        }
    }
    last_tap_sample = now;
}

void HandleButtons(bool shift)
{
    const bool freeze_pressed = hw.GetButton(SW_FREEZE).Pressed();
    const bool reverse_pressed = hw.GetButton(SW_REVERSE).Pressed();
    const bool both = freeze_pressed && reverse_pressed;
    if(shift)
    {
        if(both && !prev_shift_both)
            kill_countdown = static_cast<uint32_t>(sample_rate * 0.012f);
        else if(hw.GetButton(SW_REVERSE).RisingEdge() && !freeze_pressed)
            shift_bank = static_cast<uint8_t>((shift_bank + 1) % 3);
    }
    else
    {
        if(both && !prev_both)
            RegisterTap();
        else if(hw.GetButton(SW_REVERSE).RisingEdge() && !freeze_pressed)
            mode = static_cast<uint8_t>((mode + 1) % 3);
    }
    prev_both = both && !shift;
    prev_shift_both = both && shift;
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    hw.ProcessAllControls();
    sample_counter += size;

    const bool shift = hw.GetButton(SW_SHIFT).Pressed();
    HandleButtons(shift);
    UpdateControls(shift);

    const bool clock_gate = hw.GetGateState(GATE_FREEZE);
    if(clock_gate && !prev_clock_gate)
        RegisterClock();
    prev_clock_gate = clock_gate;

    int gate_mode = static_cast<int>(shift_param[2][0] * 2.999f);
    if(gate_mode > 2) gate_mode = 2;
    const bool gate_high = hw.GetGateState(GATE_REVERSE);

    const bool button_freeze = hw.GetButton(SW_FREEZE).Pressed() && !shift
                               && !hw.GetButton(SW_REVERSE).Pressed();
    const bool button_blast = hw.GetButton(SW_FREEZE).Pressed() && shift
                              && !hw.GetButton(SW_REVERSE).Pressed();
    const bool freeze = button_freeze || (gate_high && gate_mode == 0);
    const bool blast  = button_blast || (gate_high && gate_mode == 1);
    const bool glitch = gate_high && gate_mode == 2;

    const bool send_mode = shift_param[2][1] >= 0.5f;
    const bool sync_mode = shift_param[2][2] >= 0.5f;

    const float time = Clamp01(normal_param[0] - hw.GetCvValue(CV_TIME));
    const float feedback_knob = Clamp01(normal_param[1] + hw.GetCvValue(CV_REFLECT));
    const float mix = Clamp01(normal_param[2] + hw.GetCvValue(CV_MIX));
    const float spray = Clamp01(normal_param[3] + hw.GetCvValue(CV_ATMOSPHERE));
    const float scatter = Clamp01(normal_param[4] + hw.GetCvValue(CV_BLUR));
    const float mod_amount = Clamp01(normal_param[5] + hw.GetWarpVoct() / 60.0f);

    const float pre_delay_ms = shift_param[0][0] * 300.0f;
    const float rate_hz = 0.025f * powf(320.0f, shift_param[0][1]);
    const float wet_gain = 0.50f + 1.50f * shift_param[0][2];
    const float sidechain = shift_param[0][3];
    const float slew = shift_param[0][4];
    const float spread = shift_param[0][5];

    const float center = shift_param[1][0];
    const float width = shift_param[1][1];
    const float shape = shift_param[1][2];
    float anomaly = shift_param[1][3];
    if(glitch)
        anomaly = fmaxf(anomaly, 0.55f + 0.45f * shift_param[2][4]);
    const int warp_semitones = static_cast<int>(floorf(shift_param[1][4] * 24.999f)) - 12;

    float feedback = FeedbackAmount(feedback_knob);
    const float blast_strength = 1.0f + 2.2f * shift_param[2][3];
    if(blast)
        feedback += 0.08f + 0.18f * shift_param[2][3];
    if(freeze)
        feedback = 1.0f;

    const float base = BaseDelaySamples(time, sync_mode);
    ComputeDelayTargets(base, spray, mode);

    const float center_hz = 80.0f * powf(200.0f, center);
    const float half_oct = 5.5f * (1.0f - width) + 0.18f * width;
    float hp_hz = center_hz / powf(2.0f, half_oct);
    float lp_hz = center_hz * powf(2.0f, half_oct);
    if(width < 0.015f)
    {
        hp_hz = 18.0f;
        lp_hz = 20000.0f;
    }
    hp_hz = fclamp(hp_hz, 18.0f, 17000.0f);
    lp_hz = fclamp(lp_hz, hp_hz + 20.0f, 20000.0f);
    const float hp_coeff = 1.0f - expf(-kTwoPi * hp_hz / sample_rate);
    const float lp_coeff = 1.0f - expf(-kTwoPi * lp_hz / sample_rate);

    const bool crossfade_mode = slew < 0.075f;
    const float slew_coeff = 0.00005f + 0.025f * (1.0f - slew) * (1.0f - slew);
    const float xfade_inc = 1.0f / (kCrossfadeTime * sample_rate);

    for(int i = 0; i < 16; ++i)
    {
        if(crossfade_mode && fabsf(delay_target[i] - xfade_to[i]) > 6.0f)
        {
            const float current = xfade_pos[i] < 1.0f
                                ? xfade_from[i] + (xfade_to[i] - xfade_from[i]) * SmoothStep(xfade_pos[i])
                                : xfade_to[i];
            xfade_from[i] = current;
            xfade_to[i] = delay_target[i];
            xfade_pos[i] = 0.0f;
        }
    }

    const float warp_ratio = powf(2.0f, static_cast<float>(warp_semitones) / 12.0f);
    const float pitch_inc = fabsf(warp_ratio - 1.0f) / 2200.0f;

    int anomaly_bits = 16 - static_cast<int>(anomaly * 12.0f);
    if(anomaly_bits < 4) anomaly_bits = 4;
    if(anomaly_bits > 16) anomaly_bits = 16;
    const float anomaly_levels = static_cast<float>(1u << anomaly_bits);

    const float pre_samples = fclamp(pre_delay_ms * 0.001f * sample_rate,
                                     1.0f,
                                     static_cast<float>(kMaxPreDelaySamples - 2));
    const float lfo_inc = rate_hz / sample_rate;
    const float mod_depth = 0.28f * mod_amount * mod_amount;

    const float dry_gain = send_mode ? 1.0f : cosf(mix * kPi * 0.5f);
    const float return_gain = send_mode ? wet_gain : sinf(mix * kPi * 0.5f) * wet_gain;
    float send_level = send_mode ? mix : 1.0f;
    if(blast && send_mode) send_level = 1.0f;

    for(size_t n = 0; n < size; ++n)
    {
        float in_l = Bound(in[0][n], 2.5f);
        float in_r = Bound(in[1][n], 2.5f);
        if(fabsf(in_r) < 0.00001f && fabsf(in_l) > 0.00005f)
            in_r = in_l;

        pre_delay[0].Write(in_l);
        pre_delay[1].Write(in_r);
        const float source_l = pre_delay_ms > 0.05f ? Safe(pre_delay[0].Read(pre_samples)) : in_l;
        const float source_r = pre_delay_ms > 0.05f ? Safe(pre_delay[1].Read(pre_samples)) : in_r;

        const float level = fminf(1.0f, fmaxf(fabsf(in_l), fabsf(in_r)) * 0.70f);
        if(level > input_env) input_env += 0.055f * (level - input_env);
        else input_env += 0.0012f * (level - input_env);
        const float duck = 1.0f - sidechain * 0.88f * Clamp01(input_env);

        float raw[16];
        float filtered[16];
        float matrix[16];

        for(int i = 0; i < 16; ++i)
        {
            float phase = lfo_phase[i] + spread * (static_cast<float>(i) / 16.0f);
            if(phase >= 1.0f) phase -= 1.0f;
            const float lfo = LfoValue(i, shape, phase);
            const float mod_ratio = freeze ? 1.0f : (1.0f + mod_depth * lfo);

            float read_delay = delay_now[i] * mod_ratio;
            if(crossfade_mode)
            {
                if(xfade_pos[i] < 1.0f)
                {
                    const float p = SmoothStep(xfade_pos[i]);
                    const float da = fclamp(xfade_from[i] * mod_ratio, 4.0f,
                                            static_cast<float>(kMaxDelaySamples - 4));
                    const float db = fclamp(xfade_to[i] * mod_ratio, 4.0f,
                                            static_cast<float>(kMaxDelaySamples - 4));
                    const float a = Safe(delay_line[i].Read(da));
                    const float b = Safe(delay_line[i].Read(db));
                    raw[i] = a + (b - a) * p;
                    xfade_pos[i] += xfade_inc;
                    if(xfade_pos[i] >= 1.0f)
                    {
                        xfade_pos[i] = 1.0f;
                        delay_now[i] = xfade_to[i];
                    }
                    read_delay = xfade_to[i] * mod_ratio;
                }
                else
                {
                    delay_now[i] = xfade_to[i];
                    read_delay = fclamp(delay_now[i] * mod_ratio, 4.0f,
                                        static_cast<float>(kMaxDelaySamples - 4));
                    raw[i] = Safe(delay_line[i].Read(read_delay));
                }
            }
            else
            {
                delay_now[i] += slew_coeff * (delay_target[i] - delay_now[i]);
                read_delay = fclamp(delay_now[i] * mod_ratio, 4.0f,
                                    static_cast<float>(kMaxDelaySamples - 4));
                raw[i] = Safe(delay_line[i].Read(read_delay));
            }

            filtered[i] = freeze ? raw[i] : BandPass(i, raw[i], hp_coeff, lp_coeff);
            matrix[i] = filtered[i];

            lfo_phase[i] += lfo_inc;
            if(lfo_phase[i] >= 1.0f)
            {
                lfo_phase[i] -= 1.0f;
                random_prev[i] = random_next[i];
                random_next[i] = NextRandom();
            }
        }

        float dense[16];
        for(int i = 0; i < 16; ++i) dense[i] = matrix[i];
        Hadamard16(dense);

        float wet_l = 0.0f;
        float wet_r = 0.0f;
        for(int i = 0; i < 16; ++i)
        {
            if((i & 1) == 0) wet_l += filtered[i] * 0.235f;
            else wet_r += filtered[i] * 0.235f;
        }

        float warp_focus = 0.0f;
        float warp_all = 1.0f;
        if(mode == 2 && spray < 0.5f)
        {
            warp_all = spray * 2.0f;
            warp_focus = 1.0f - warp_all;
        }

        for(int i = 0; i < 16; ++i)
        {
            const float self = matrix[i];
            float fb = self + (dense[i] - self) * scatter;

            if(!freeze && anomaly > 0.01f)
            {
                const int32_t q = static_cast<int32_t>(fb * anomaly_levels);
                const float crushed = static_cast<float>(q) / anomaly_levels;
                fb += (crushed - fb) * anomaly;
                if(anomaly > 0.48f)
                {
                    const float stutter_amt = (anomaly - 0.48f) / 0.52f;
                    const float short_d = 24.0f + (1.0f - stutter_amt) * 900.0f
                                          + 180.0f * fabsf(Triangle(lfo_phase[i]));
                    const float st = Safe(delay_line[i].Read(short_d));
                    fb += (st - fb) * (0.15f + 0.65f * stutter_amt);
                }
            }

            if(mode == 2 && !freeze && i < 2 && warp_semitones != 0)
            {
                const float pd = fclamp(delay_now[i], 4.0f,
                                        static_cast<float>(kMaxDelaySamples - 3000));
                const float shifted = PitchFeedbackRead(i, pd, warp_ratio, pitch_inc);
                fb = 0.28f * fb + 0.72f * shifted;
            }

            const float channel_source = (i & 1) == 0 ? source_l : source_r;
            float inject_weight = 1.0f;
            if(mode == 2)
            {
                if(i < 2) inject_weight = warp_all + warp_focus * 2.4f;
                else inject_weight = warp_all;
            }

            float inject = freeze ? 0.0f
                                  : channel_source * send_level * inject_weight * 0.42f;
            if(blast) inject = FastSat(inject * blast_strength);
            const float write = FastSat(inject + fb * feedback);
            delay_line[i].Write(Bound(write, 2.2f));
        }

        float kill_gain = 1.0f;
        if(kill_countdown > 0)
        {
            kill_gain = static_cast<float>(kill_countdown)
                      / fmaxf(1.0f, sample_rate * 0.012f);
            --kill_countdown;
            if(kill_countdown == 0) reset_requested = true;
        }

        wet_l *= duck * kill_gain;
        wet_r *= duck * kill_gain;
        out[0][n] = FastSat(in_l * dry_gain + wet_l * return_gain);
        out[1][n] = FastSat(in_r * dry_gain + wet_r * return_gain);
    }

    ui_shift_down = shift;
    ui_freeze = freeze;
    ui_blast = blast;
    ui_glitch = glitch;
    ui_send = send_mode;
    ui_sync = sync_mode;
    ui_mode = mode;
    ui_shift_bank = shift_bank;
    for(int i = 0; i < 6; ++i)
    {
        ui_normal[i] = normal_param[i];
        ui_shift[i] = shift_param[shift_bank][i];
        ui_pickup[i] = normal_pickup[i];
    }
}

void SetTopLed(int i, float v, float r, float g, float b)
{
    v = Clamp01(v);
    const float level = 0.08f + 0.92f * v;
    hw.SetLed(static_cast<Leds>(LED_1 + i), r * level, g * level, b * level);
}

void UpdateLeds()
{
    static uint32_t tick = 0;
    ++tick;
    hw.ClearLeds();

    if(!ui_shift_down)
    {
        SetTopLed(0, ui_normal[0], 0.0f, 0.0f, 1.0f);
        SetTopLed(1, ui_normal[1], 1.0f, 0.0f, 0.0f);
        SetTopLed(2, ui_normal[2], 1.0f, 0.72f, 0.0f);
        SetTopLed(3, ui_normal[3], 0.0f, 1.0f, 0.08f);
        SetTopLed(4, ui_normal[4], 0.0f, 0.75f, 1.0f);
        SetTopLed(5, ui_normal[5], 1.0f, 0.0f, 1.0f);
        const bool flash = ((tick / 10) & 1u) != 0;
        if(flash)
        {
            for(int i = 0; i < 6; ++i)
                if(!ui_pickup[i])
                    hw.SetLed(static_cast<Leds>(LED_1 + i), 1.0f, 0.0f, 0.0f);
        }
    }
    else
    {
        for(int i = 0; i < 6; ++i)
        {
            if(ui_shift_bank == 0) SetTopLed(i, ui_shift[i], 1.0f, 0.35f, 0.0f);
            else if(ui_shift_bank == 1) SetTopLed(i, ui_shift[i], 0.0f, 0.75f, 1.0f);
            else SetTopLed(i, ui_shift[i], 0.85f, 0.85f, 0.85f);
        }
    }

    hw.SetLed(LED_BOT_1, ui_mode == 0 ? 0.0f : 0.03f,
                          ui_mode == 0 ? 0.25f : 0.03f,
                          ui_mode == 0 ? 1.0f : 0.03f);
    hw.SetLed(LED_BOT_2, ui_mode == 1 ? 0.05f : 0.03f,
                          ui_mode == 1 ? 1.0f : 0.03f,
                          ui_mode == 1 ? 0.18f : 0.03f);
    hw.SetLed(LED_BOT_3, ui_mode == 2 ? 1.0f : 0.03f,
                          0.0f,
                          ui_mode == 2 ? 1.0f : 0.03f);

    if(ui_blast) hw.SetLed(LED_FREEZE, 1.0f, 0.22f, 0.0f);
    else if(ui_freeze) hw.SetLed(LED_FREEZE, 1.0f, 1.0f, 1.0f);
    else if(ui_send) hw.SetLed(LED_FREEZE, 0.55f, 0.0f, 0.0f);
    else hw.SetLed(LED_FREEZE, 0.02f, 0.02f, 0.02f);

    if(ui_mode == 0) hw.SetLed(LED_REVERSE, 0.0f, 0.22f, 1.0f);
    else if(ui_mode == 1) hw.SetLed(LED_REVERSE, 0.05f, 1.0f, 0.18f);
    else hw.SetLed(LED_REVERSE, 1.0f, 0.0f, 1.0f);
    if(ui_glitch && ((tick / 3) & 1u))
        hw.SetLed(LED_REVERSE, 1.0f, 1.0f, 1.0f);
    hw.WriteLeds();
}

void ResetNetwork()
{
    hw.StopAudio();
    for(int i = 0; i < 16; ++i)
    {
        delay_line[i].Reset();
        filter_hp_state[i] = 0.0f;
        filter_lp_state[i] = 0.0f;
        delay_now[i] = delay_target[i];
        xfade_from[i] = delay_now[i];
        xfade_to[i] = delay_now[i];
        xfade_pos[i] = 1.0f;
    }
    pre_delay[0].Reset();
    pre_delay[1].Reset();
    input_env = 0.0f;
    reset_requested = false;
    hw.StartAudio(AudioCallback);
}

} // namespace

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();

    hw.ClearLeds();
    hw.SetLed(LED_1, 0.0f, 0.0f, 1.0f);
    hw.SetLed(LED_2, 1.0f, 0.0f, 0.0f);
    hw.SetLed(LED_3, 1.0f, 0.7f, 0.0f);
    hw.SetLed(LED_4, 0.0f, 1.0f, 0.0f);
    hw.SetLed(LED_5, 0.0f, 0.7f, 1.0f);
    hw.SetLed(LED_6, 1.0f, 0.0f, 1.0f);
    hw.SetLed(LED_BOT_1, 0.0f, 0.25f, 1.0f);
    hw.WriteLeds();

    for(int i = 0; i < 16; ++i)
    {
        delay_line[i].Init();
        delay_now[i] = 800.0f + 40.0f * i;
        delay_target[i] = delay_now[i];
        xfade_from[i] = delay_now[i];
        xfade_to[i] = delay_now[i];
        xfade_pos[i] = 1.0f;
        lfo_phase[i] = static_cast<float>(i) / 16.0f;
        random_prev[i] = NextRandom();
        random_next[i] = NextRandom();
    }
    pre_delay[0].Init();
    pre_delay[1].Init();

    // Keep Aurora's default block size, matching the hardware-confirmed EchoGarden runtime.
    hw.StartAudio(AudioCallback);

    while(1)
    {
        if(reset_requested) ResetNetwork();
        UpdateLeds();
        System::Delay(12);
    }
}
