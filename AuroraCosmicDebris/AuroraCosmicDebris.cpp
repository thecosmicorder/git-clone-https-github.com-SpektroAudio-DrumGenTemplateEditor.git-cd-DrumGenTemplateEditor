/* Aurora Cosmic Debris v1.0
   Aurora-native 16-line stereo delay/reverb network inspired by WMD Cosmic Debris.
   Core architecture: 8 delay lines per side, per-line band-pass filtering,
   Spray, Scatter, Blur/Ratio/Warp processing modes, Freeze, Blast and Glitch.
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
constexpr int kLinesPerSide = 8;
constexpr int kTotalLines = 16;
constexpr size_t kMaxDelaySamples = 96000; // 2 s at 48 kHz
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kBypass = 0.0035f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS lines[kTotalLines];
float sample_rate = 48000.0f;

struct SvfState { float ic1 = 0.0f; float ic2 = 0.0f; };
struct SvfOut { float lp, bp, hp, notch; };
SvfState line_filter[kTotalLines];

float smooth_delay[kTotalLines] = {0.0f};
float mod_phase[kTotalLines] = {0.0f};
float warp_phase[kTotalLines] = {0.0f};
float feedback_tone[kTotalLines] = {0.0f};
uint32_t rng_state = 0xC05D3B51u;

bool freeze_latched = false;
bool blast_latched = false;
bool glitch_latched = false;
uint8_t mode = 0; // 0 Blur, 1 Ratio, 2 Warp
bool prev_reverse_gate = false;

// primary knob memories: TIME, FEEDBACK, MIX, SPRAY, SCATTER, MODE AMOUNT
float primary[6] = {0.42f, 0.36f, 0.50f, 0.24f, 0.18f, 0.45f};
bool controls_seeded = false;
// SHIFT layer: SLEW, FILTER Q, FILTER CENTER, WIDTH, MOD RATE, WARP INTERVAL
float secondary[6] = {0.28f, 0.32f, 0.66f, 0.78f, 0.28f, 0.58f};

volatile float ui_primary[6] = {0.0f};
volatile bool ui_shift = false;
volatile bool ui_freeze = false;
volatile bool ui_blast = false;
volatile bool ui_glitch = false;
volatile uint8_t ui_mode = 0;

inline float Safe(float x) { return std::isfinite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float Sat(float x) { return tanhf(Bound(x, 6.0f)); }

float Rand01()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return static_cast<float>(rng_state & 0x00ffffffu) / 16777215.0f;
}

SvfOut ProcessSvf(SvfState& s, float input, float cutoff, float resonance)
{
    cutoff = fclamp(cutoff, 35.0f, sample_rate * 0.42f);
    resonance = Clamp01(resonance);
    const float g = tanf(kPi * cutoff / sample_rate);
    const float k = 2.0f - 1.82f * resonance;
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = input - s.ic2;
    const float v1 = a1 * s.ic1 + a2 * v3;
    const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
    s.ic1 = Bound(2.0f * v1 - s.ic1, 3.0f);
    s.ic2 = Bound(2.0f * v2 - s.ic2, 3.0f);
    SvfOut o{v2, v1, input - k * v1 - v2, 0.0f};
    o.notch = o.lp + o.hp;
    return o;
}

inline void IncPhase(float& p, float hz)
{
    p += hz / sample_rate;
    if(p >= 1.0f) p -= floorf(p);
}

float PitchRead(int index, float base_delay, float semitones, float amount)
{
    if(amount < 0.015f || fabsf(semitones) < 0.05f)
        return Safe(lines[index].ReadHermite(base_delay));

    const float ratio = powf(2.0f, (semitones * amount) / 12.0f);
    const float window = 420.0f + 1650.0f * amount;
    const float rate = fabsf(ratio - 1.0f) / window;
    warp_phase[index] += rate;
    if(warp_phase[index] >= 1.0f) warp_phase[index] -= floorf(warp_phase[index]);

    const float p1 = warp_phase[index];
    float p2 = p1 + 0.5f;
    if(p2 >= 1.0f) p2 -= 1.0f;
    const float o1 = ratio >= 1.0f ? (1.0f - p1) * window : p1 * window;
    const float o2 = ratio >= 1.0f ? (1.0f - p2) * window : p2 * window;
    const float d1 = fclamp(base_delay + o1, 3.0f, static_cast<float>(kMaxDelaySamples - 4));
    const float d2 = fclamp(base_delay + o2, 3.0f, static_cast<float>(kMaxDelaySamples - 4));
    const float w1 = 0.5f - 0.5f * cosf(kTwoPi * p1);
    const float w2 = 1.0f - w1;
    return Safe(lines[index].ReadHermite(d1)) * w1 + Safe(lines[index].ReadHermite(d2)) * w2;
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
        default: return Clamp01(hw.GetKnobValue(KNOB_WARP));
    }
}

void ReadControls(bool shift)
{
    if(!controls_seeded)
    {
        for(int i = 0; i < 6; ++i) primary[i] = ReadKnob(i);
        controls_seeded = true;
    }
    if(shift)
    {
        for(int i = 0; i < 6; ++i) secondary[i] = ReadKnob(i);
    }
    else
    {
        for(int i = 0; i < 6; ++i) primary[i] = ReadKnob(i);
    }

    for(int i = 0; i < 6; ++i) ui_primary[i] = primary[i];
    ui_shift = shift;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();

    static float shift_grace = 0.0f;
    const bool shift = hw.GetButton(SW_SHIFT).Pressed();
    if(shift) shift_grace = 0.18f;
    else
    {
        shift_grace -= static_cast<float>(size) / sample_rate;
        if(shift_grace < 0.0f) shift_grace = 0.0f;
    }
    const bool modifier = shift || shift_grace > 0.0f;

    const bool fedge = hw.GetButton(SW_FREEZE).RisingEdge();
    const bool redge = hw.GetButton(SW_REVERSE).RisingEdge();
    if(fedge)
    {
        if(modifier) { blast_latched = !blast_latched; shift_grace = 0.0f; }
        else freeze_latched = !freeze_latched;
    }
    if(redge)
    {
        if(modifier) { glitch_latched = !glitch_latched; shift_grace = 0.0f; }
        else mode = static_cast<uint8_t>((mode + 1) % 3);
    }

    const bool rev_gate = hw.GetGateState(GATE_REVERSE);
    if(rev_gate && !prev_reverse_gate)
        glitch_latched = !glitch_latched;
    prev_reverse_gate = rev_gate;

    ReadControls(shift);

    const float time_knob = Clamp01(primary[0] + hw.GetCvValue(CV_TIME));
    const float feedback_knob = Clamp01(primary[1] + hw.GetCvValue(CV_REFLECT));
    const float mix = Clamp01(primary[2] + hw.GetCvValue(CV_MIX));
    const float spray = Clamp01(primary[3] + hw.GetCvValue(CV_ATMOSPHERE));
    const float scatter = Clamp01(primary[4] + hw.GetCvValue(CV_BLUR));
    const float amount = Clamp01(primary[5] + hw.GetWarpVoct() / 60.0f);

    const bool freeze = freeze_latched != hw.GetGateState(GATE_FREEZE);
    const bool blast = blast_latched;
    const bool glitch = glitch_latched;
    ui_freeze = freeze; ui_blast = blast; ui_glitch = glitch; ui_mode = mode;

    if(mix <= kBypass)
    {
        for(size_t i = 0; i < size; ++i) { out[0][i] = in[0][i]; out[1][i] = in[1][i]; }
        return;
    }

    // 400 us to ~1.65 s; musical log scaling.
    const float base_seconds = 0.0004f * powf(4125.0f, time_knob);
    const float base_samples = fclamp(base_seconds * sample_rate, 20.0f, static_cast<float>(kMaxDelaySamples - 2600));

    // Feedback reaches near-infinite around the upper half without uncontrolled >1 regeneration.
    float feedback = feedback_knob < 0.5f
        ? 1.88f * feedback_knob
        : 0.94f + 0.054f * powf((feedback_knob - 0.5f) * 2.0f, 1.8f);
    if(freeze) feedback = 0.9985f;

    const float slew = secondary[0];
    const float slew_coeff = 0.00010f + (1.0f - slew) * (1.0f - slew) * 0.0065f;
    const float filter_res = 0.18f + 0.74f * secondary[1];
    const float filter_center = 70.0f * powf(210.0f, secondary[2]);
    const float width = 0.18f + 0.82f * secondary[3];
    const float mod_rate = 0.015f + 1.35f * secondary[4] * secondary[4];
    static const float intervals[6] = {3.0f, 5.0f, 7.0f, 12.0f, 19.0f, 24.0f};
    int int_index = static_cast<int>(secondary[5] * 5.999f);
    if(int_index > 5) int_index = 5;
    const float warp_interval = intervals[int_index];

    const float dry_gain = cosf(mix * kPi * 0.5f);
    const float wet_gain = sinf(mix * kPi * 0.5f) * 1.18f;
    const float spray2 = spray * spray;
    const float scatter2 = scatter * scatter;
    const float blast_gain = blast ? 2.35f : 1.0f;
    const float glitch_depth = glitch ? (0.35f + 0.65f * scatter) : 0.0f;

    static const float tap_shape[kLinesPerSide] = {0.54f, 0.67f, 0.79f, 0.91f, 1.00f, 1.13f, 1.31f, 1.56f};
    static const float filter_offsets[kLinesPerSide] = {-9.0f, -6.0f, -3.0f, -1.0f, 1.0f, 3.0f, 6.0f, 9.0f};

    // Update target timing and modulation increments once per block.
    float target[kTotalLines];
    float cutoff[kTotalLines];
    float phase_inc[kTotalLines];
    for(int side = 0; side < 2; ++side)
    {
        for(int j = 0; j < kLinesPerSide; ++j)
        {
            const int idx = side * kLinesPerSide + j;
            float ratio = 1.0f + spray * (tap_shape[j] - 1.0f);
            if(mode == 1 && ((j + side) & 1))
                ratio *= 1.0f + amount; // alternates toward exact 2x at maximum
            const float stereo_skew = 1.0f + (side ? 1.0f : -1.0f) * width * (0.006f + 0.026f * spray);
            target[idx] = fclamp(base_samples * ratio * stereo_skew, 8.0f, static_cast<float>(kMaxDelaySamples - 2600));
            cutoff[idx] = fclamp(filter_center * powf(2.0f, filter_offsets[j] / 12.0f), 45.0f, 18000.0f);
            phase_inc[idx] = (mod_rate * (1.0f + 0.113f * static_cast<float>(j) + 0.071f * side)) / sample_rate;
        }
    }

    for(size_t n = 0; n < size; ++n)
    {
        const float input_l = Bound(in[0][n], 2.0f);
        const float input_r = Bound(in[1][n], 2.0f);
        float tap[kTotalLines];
        float filt[kTotalLines];

        for(int idx = 0; idx < kTotalLines; ++idx)
        {
            smooth_delay[idx] += slew_coeff * (target[idx] - smooth_delay[idx]);
            mod_phase[idx] += phase_inc[idx];
            if(mod_phase[idx] >= 1.0f) mod_phase[idx] -= 1.0f;

            const int j = idx & 7;
            const float smear = spray2 * sample_rate * (0.0005f + 0.0036f * static_cast<float>(j) / 7.0f);
            const float wobble = sinf(kTwoPi * mod_phase[idx]) * smear * (0.35f + 0.65f * amount);
            float read_delay = fclamp(smooth_delay[idx] + wobble, 4.0f, static_cast<float>(kMaxDelaySamples - 2300));

            if(mode == 2)
            {
                // Alternate pitch-shifted and unshifted lines for shimmer without destroying transients.
                float semis = 0.0f;
                if((j % 3) == 0) semis = warp_interval;
                else if((j % 3) == 1) semis = warp_interval * 0.583333f;
                if((idx & 8) && (j & 1)) semis *= 0.5f;
                tap[idx] = PitchRead(idx, read_delay, semis, amount);
            }
            else
            {
                tap[idx] = Safe(lines[idx].ReadHermite(read_delay));
            }

            const SvfOut fo = ProcessSvf(line_filter[idx], tap[idx], cutoff[idx], filter_res);
            // Keep enough broadband content for clarity while every line still has an independent BPF.
            filt[idx] = Bound(tap[idx] * 0.34f + fo.bp * (1.20f + 0.32f * filter_res), 1.65f);
        }

        float wet_l = 0.0f, wet_r = 0.0f;
        for(int j = 0; j < kLinesPerSide; ++j)
        {
            wet_l += filt[j];
            wet_r += filt[8 + j];
        }
        wet_l *= 0.205f;
        wet_r *= 0.205f;

        // BLUR mode turns the network into a denser diffuse reverb by exchanging neighboring energy.
        if(mode == 0)
        {
            const float diffuse = amount * (0.24f + 0.46f * spray);
            const float a = wet_l, b = wet_r;
            wet_l = Sat(a + b * diffuse);
            wet_r = Sat(b + a * diffuse * 0.83f);
        }

        int route = 1 + static_cast<int>(scatter * 6.999f);
        if(route > 7) route = 7;
        const float route_mix = scatter * 7.0f - floorf(scatter * 7.0f);

        for(int side = 0; side < 2; ++side)
        {
            const float live = side == 0 ? input_l : input_r;
            const float other_live = side == 0 ? input_r : input_l;
            for(int j = 0; j < kLinesPerSide; ++j)
            {
                const int idx = side * kLinesPerSide + j;
                const int r1j = (j + route) & 7;
                const int r2j = (j + route + 1) & 7;
                const int same1 = side * kLinesPerSide + r1j;
                const int same2 = side * kLinesPerSide + r2j;
                const int cross1 = (1 - side) * kLinesPerSide + ((7 - j + route) & 7);

                float routed = filt[idx] * (1.0f - scatter);
                routed += (filt[same1] * (1.0f - route_mix) + filt[same2] * route_mix) * scatter * (1.0f - 0.42f * scatter2);
                routed += filt[cross1] * scatter2 * (0.20f + 0.38f * width);

                if(mode == 0)
                {
                    const int neighbor = side * kLinesPerSide + ((j + 1) & 7);
                    routed += filt[neighbor] * amount * (0.12f + 0.34f * spray);
                }

                if(glitch)
                {
                    const int gj = static_cast<int>((Rand01() * 7.999f));
                    const int gidx = ((j + gj) & 7) + (Rand01() > 0.58f ? (1 - side) * 8 : side * 8);
                    routed = routed * (1.0f - glitch_depth * 0.48f) + filt[gidx] * glitch_depth * 0.62f;
                }

                feedback_tone[idx] += (0.07f + 0.22f * (1.0f - secondary[2])) * (routed - feedback_tone[idx]);
                const float fb_signal = Sat(feedback_tone[idx] * (1.0f + 1.35f * feedback_knob));
                const float injection = freeze ? 0.0f : Bound((live + other_live * width * 0.12f) * 0.36f * blast_gain, 1.5f);
                lines[idx].Write(Bound(injection + fb_signal * feedback, 1.48f));
            }
        }

        // BLAST makes the performance gesture obvious without permanently boosting the output tail.
        const float wet_comp = freeze ? 0.82f : 1.0f;
        out[0][n] = Sat(input_l * dry_gain + wet_l * wet_gain * wet_comp);
        out[1][n] = Sat(input_r * dry_gain + wet_r * wet_gain * wet_comp);
    }
}

float Pulse(float value, float phase, float offset)
{
    const float v = Clamp01(value);
    const float speed = 0.55f + 4.2f * v;
    const float wave = 0.5f + 0.5f * sinf(phase * speed + offset);
    return fclamp(0.05f + v * (0.28f + 0.72f * wave), 0.0f, 1.0f);
}

void UpdateLeds()
{
    static float phase = 0.0f;
    static uint32_t blink = 0;
    phase += 0.072f;
    if(phase > 1000.0f) phase = 0.0f;
    ++blink;
    const bool alt = ((blink / 18u) & 1u) != 0u;

    hw.ClearLeds();
    const float p0 = Pulse(ui_primary[0], phase, 0.0f);
    const float p1 = Pulse(ui_primary[1], phase, 0.6f);
    const float p2 = Pulse(ui_primary[2], phase, 1.2f);
    const float p3 = Pulse(ui_primary[3], phase, 1.8f);
    const float p4 = Pulse(ui_primary[4], phase, 2.4f);
    const float p5 = Pulse(ui_primary[5], phase, 3.0f);

    if(ui_shift)
    {
        // SHIFT page = white/ice indication while editing secondary controls.
        hw.SetLed(LED_1,p0,p0,p0); hw.SetLed(LED_2,p1,p1,p1);
        hw.SetLed(LED_3,p2,p2,p2); hw.SetLed(LED_4,p3,p3,p3);
        hw.SetLed(LED_5,p4,p4,p4); hw.SetLed(LED_6,p5,p5,p5);
    }
    else
    {
        hw.SetLed(LED_1,0.0f,0.0f,p0);          // Time blue
        hw.SetLed(LED_2,p1,0.0f,0.0f);          // Feedback red
        hw.SetLed(LED_3,p2,p2*0.70f,0.0f);      // Mix amber
        hw.SetLed(LED_4,0.0f,p3,0.30f*p3);      // Spray green
        hw.SetLed(LED_5,0.0f,0.70f*p4,p4);      // Scatter cyan
        hw.SetLed(LED_6,p5,0.0f,p5);            // Mode amount magenta
    }

    if(ui_mode == 0) // Blur
    {
        hw.SetLed(LED_BOT_1,0.0f,0.15f,1.0f);
        hw.SetLed(LED_BOT_2,0.03f,0.03f,0.03f);
        hw.SetLed(LED_BOT_3,0.03f,0.03f,0.03f);
    }
    else if(ui_mode == 1) // Ratio
    {
        hw.SetLed(LED_BOT_1,0.03f,0.03f,0.03f);
        hw.SetLed(LED_BOT_2,0.10f,1.0f,0.25f);
        hw.SetLed(LED_BOT_3,0.03f,0.03f,0.03f);
    }
    else // Warp
    {
        hw.SetLed(LED_BOT_1,0.03f,0.03f,0.03f);
        hw.SetLed(LED_BOT_2,0.03f,0.03f,0.03f);
        hw.SetLed(LED_BOT_3,1.0f,0.0f,1.0f);
    }

    if(ui_freeze && ui_blast)
    {
        if(alt) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
        else hw.SetLed(LED_FREEZE,0.0f,0.25f,1.0f);
    }
    else if(ui_freeze) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else if(ui_blast) hw.SetLed(LED_FREEZE,0.0f,0.25f,1.0f);
    else hw.SetLed(LED_FREEZE,0.025f,0.025f,0.025f);

    if(ui_glitch)
    {
        if(alt) hw.SetLed(LED_REVERSE,1.0f,1.0f,0.0f);
        else hw.SetLed(LED_REVERSE,1.0f,0.0f,0.25f);
    }
    else if(ui_mode == 0) hw.SetLed(LED_REVERSE,0.0f,0.25f,1.0f);
    else if(ui_mode == 1) hw.SetLed(LED_REVERSE,0.10f,1.0f,0.25f);
    else hw.SetLed(LED_REVERSE,1.0f,0.0f,1.0f);

    hw.WriteLeds();
}

} // namespace

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();
    for(int i = 0; i < kTotalLines; ++i)
    {
        lines[i].Init();
        smooth_delay[i] = 1200.0f + 170.0f * static_cast<float>(i);
        lines[i].SetDelay(smooth_delay[i]);
        mod_phase[i] = static_cast<float>(i) / static_cast<float>(kTotalLines);
        warp_phase[i] = fmodf(0.137f * static_cast<float>(i), 1.0f);
    }
    hw.SetAudioBlockSize(4);
    hw.StartAudio(AudioCallback);
    while(1)
    {
        UpdateLeds();
        System::Delay(10);
    }
}
