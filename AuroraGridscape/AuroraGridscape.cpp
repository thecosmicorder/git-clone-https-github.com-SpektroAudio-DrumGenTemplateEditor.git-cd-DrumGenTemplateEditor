/* Aurora Gridscape v1.0
   Topographic rhythm instrument for Qu-Bit Aurora.

   Rhythm-map data is generated at build time from Mutable Instruments Grids
   open-source resources (GPL-3.0). This derivative firmware is distributed
   under GPL-3.0 and deliberately uses a distinct product name.

   Primary controls:
     TIME       = Map X
     REFLECT    = Map Y
     MIX        = Chaos / Euclidean length 3
     ATMOSPHERE = Kick fill
     BLUR       = Snare fill
     WARP       = Hat/Perc fill

   Gate inputs:
     FREEZE gate = external clock
     REVERSE gate = reset

   Buttons:
     FREEZE tap = tap tempo; hold = reset
     REVERSE = cycle external clock resolution 4/8/24 PPQN
     SHIFT+FREEZE = internal clock run/stop
     SHIFT+REVERSE = Grids / Euclidean mode
*/

#include "aurora.h"
#include "GridResources.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace daisy;
using namespace aurora;

namespace
{
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr int kStepsPerPattern = 32;
constexpr int kParts = 3;

Hardware hw;
float sample_rate = 48000.0f;

uint32_t rng_state = 0xA17C9E31u;
uint8_t step_index = 0;
uint8_t external_subpulse = 0;
uint8_t ext_resolution = 0; // 0=4ppqn, 1=8ppqn, 2=24ppqn
uint8_t perturb[kParts] = {0, 0, 0};

bool euclidean_mode = false;
bool internal_running = true;
bool prev_clock_gate = false;
bool prev_reset_gate = false;

float bpm = 120.0f;
float internal_phase = 0.0f;
float external_recent = 0.0f;
float tap_elapsed = 10.0f;
float freeze_hold = 0.0f;
bool freeze_long_action = false;

float kick_env = 0.0f;
float kick_pitch_env = 0.0f;
float kick_phase = 0.0f;
float snare_env = 0.0f;
float snare_phase = 0.0f;
float snare_noise_lp = 0.0f;
float hat_env = 0.0f;
float hat_noise_lp = 0.0f;
float hat_pan = 0.5f;

volatile float ui_x = 0.5f;
volatile float ui_y = 0.5f;
volatile float ui_chaos = 0.0f;
volatile float ui_fill[3] = {0.55f, 0.48f, 0.58f};
volatile float ui_hit[3] = {0.0f, 0.0f, 0.0f};
volatile float ui_beat = 0.0f;
volatile bool ui_external = false;
volatile bool ui_euclidean = false;
volatile bool ui_running = true;
volatile uint8_t ui_resolution = 0;

inline float Clamp01(float x)
{
    if(!std::isfinite(x)) return 0.0f;
    if(x < 0.0f) return 0.0f;
    if(x > 1.0f) return 1.0f;
    return x;
}

inline float ClampRange(float x, float lo, float hi)
{
    if(!std::isfinite(x)) return lo;
    if(x < lo) return lo;
    if(x > hi) return hi;
    return x;
}

inline float Saturate(float x)
{
    if(!std::isfinite(x)) return 0.0f;
    return tanhf(x * 0.92f);
}

inline float KnobCv(int knob, int cv)
{
    return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv));
}

inline float WarpFill()
{
    // Aurora's WARP CV is calibrated as V/oct. Convert its musical range
    // into a gentle bipolar density modulation.
    return Clamp01(hw.GetKnobValue(KNOB_WARP) + hw.GetWarpVoct() / 60.0f);
}

uint32_t Rand32()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

uint8_t RandByte()
{
    return static_cast<uint8_t>(Rand32() >> 24);
}

inline uint8_t MixU8(uint8_t a, uint8_t b, uint8_t amount)
{
    const int32_t diff = static_cast<int32_t>(b) - static_cast<int32_t>(a);
    int32_t v = static_cast<int32_t>(a) + ((diff * amount) >> 8);
    if(v < 0) v = 0;
    if(v > 255) v = 255;
    return static_cast<uint8_t>(v);
}

const uint8_t* const drum_map[5][5] = {
    {gridscape::node_10, gridscape::node_8,  gridscape::node_0,  gridscape::node_9,  gridscape::node_11},
    {gridscape::node_15, gridscape::node_7,  gridscape::node_13, gridscape::node_12, gridscape::node_6 },
    {gridscape::node_18, gridscape::node_14, gridscape::node_4,  gridscape::node_5,  gridscape::node_3 },
    {gridscape::node_23, gridscape::node_16, gridscape::node_21, gridscape::node_1,  gridscape::node_2 },
    {gridscape::node_24, gridscape::node_19, gridscape::node_17, gridscape::node_20, gridscape::node_22},
};

uint8_t ReadMap(uint8_t step, uint8_t instrument, uint8_t x, uint8_t y)
{
    uint8_t i = x >> 6;
    uint8_t j = y >> 6;
    if(i > 3) i = 3;
    if(j > 3) j = 3;

    const uint8_t* a_map = drum_map[i][j];
    const uint8_t* b_map = drum_map[i + 1][j];
    const uint8_t* c_map = drum_map[i][j + 1];
    const uint8_t* d_map = drum_map[i + 1][j + 1];
    const uint8_t offset = static_cast<uint8_t>(instrument * kStepsPerPattern + step);

    const uint8_t fx = static_cast<uint8_t>((x & 0x3f) << 2);
    const uint8_t fy = static_cast<uint8_t>((y & 0x3f) << 2);
    const uint8_t ab = MixU8(a_map[offset], b_map[offset], fx);
    const uint8_t cd = MixU8(c_map[offset], d_map[offset], fx);
    return MixU8(ab, cd, fy);
}

void TriggerVoice(int part, bool accent)
{
    const float level = accent ? 1.0f : 0.72f;
    if(part == 0)
    {
        kick_env = level;
        kick_pitch_env = 1.0f;
        kick_phase = 0.0f;
    }
    else if(part == 1)
    {
        snare_env = level;
        snare_phase = 0.0f;
    }
    else
    {
        hat_env = level;
        hat_pan = (step_index & 0x04) ? 0.78f : 0.22f;
    }
    ui_hit[part] = 1.0f;
}

bool EuclidHit(uint8_t step, int length, int pulses)
{
    if(length <= 0 || pulses <= 0) return false;
    if(pulses >= length) return true;
    const int s = static_cast<int>(step) % length;
    return ((s * pulses) % length) < pulses;
}

void RefreshPerturbation(uint8_t chaos)
{
    const uint8_t amount = chaos >> 2;
    for(int p = 0; p < kParts; ++p)
        perturb[p] = static_cast<uint8_t>((static_cast<uint16_t>(RandByte()) * amount) >> 8);
}

void FireStep(uint8_t step)
{
    const float x_f = KnobCv(KNOB_TIME, CV_TIME);
    const float y_f = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float chaos_f = KnobCv(KNOB_MIX, CV_MIX);
    const float fill0 = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float fill1 = KnobCv(KNOB_BLUR, CV_BLUR);
    const float fill2 = WarpFill();

    ui_x = x_f;
    ui_y = y_f;
    ui_chaos = chaos_f;
    ui_fill[0] = fill0;
    ui_fill[1] = fill1;
    ui_fill[2] = fill2;

    const uint8_t x = static_cast<uint8_t>(x_f * 255.0f + 0.5f);
    const uint8_t y = static_cast<uint8_t>(y_f * 255.0f + 0.5f);
    const uint8_t chaos = static_cast<uint8_t>(chaos_f * 255.0f + 0.5f);
    const uint8_t density[3] = {
        static_cast<uint8_t>(fill0 * 255.0f + 0.5f),
        static_cast<uint8_t>(fill1 * 255.0f + 0.5f),
        static_cast<uint8_t>(fill2 * 255.0f + 0.5f)
    };

    if(step == 0)
        RefreshPerturbation(chaos);

    if((step & 0x07) == 0)
        ui_beat = 1.0f;

    if(euclidean_mode)
    {
        const int lengths[3] = {
            1 + static_cast<int>(x_f * 31.999f),
            1 + static_cast<int>(y_f * 31.999f),
            1 + static_cast<int>(chaos_f * 31.999f)
        };
        for(int p = 0; p < 3; ++p)
        {
            const int pulses = static_cast<int>(std::round(ui_fill[p] * lengths[p]));
            if(EuclidHit(step, lengths[p], pulses))
                TriggerVoice(p, ui_fill[p] > 0.72f);
        }
        return;
    }

    for(uint8_t p = 0; p < 3; ++p)
    {
        uint16_t level = ReadMap(step, p, x, y);
        level += perturb[p];
        if(level > 255) level = 255;
        const uint8_t threshold = static_cast<uint8_t>(255 - density[p]);
        if(level > threshold)
            TriggerVoice(p, level > 192);
    }
}

void ResetPattern()
{
    step_index = 0;
    external_subpulse = 0;
    internal_phase = 0.0f;
    RefreshPerturbation(static_cast<uint8_t>(ui_chaos * 255.0f));
    ui_beat = 1.0f;
}

void AdvanceInternalStep()
{
    FireStep(step_index);
    step_index = static_cast<uint8_t>((step_index + 1) & 31);
}

void ExternalClockTick()
{
    static const uint8_t pulse_increment[3] = {6, 3, 1};

    if(external_subpulse == 0)
        FireStep(step_index);

    external_subpulse = static_cast<uint8_t>(external_subpulse + pulse_increment[ext_resolution]);
    while(external_subpulse >= 3)
    {
        external_subpulse = static_cast<uint8_t>(external_subpulse - 3);
        step_index = static_cast<uint8_t>((step_index + 1) & 31);
    }
}

void ProcessButtonsAndClock(size_t size)
{
    const float block_seconds = static_cast<float>(size) / sample_rate;
    tap_elapsed += block_seconds;
    external_recent -= block_seconds;
    if(external_recent < 0.0f) external_recent = 0.0f;

    const bool shift = hw.GetButton(SW_SHIFT).Pressed();
    const bool freeze_edge = hw.GetButton(SW_FREEZE).RisingEdge();
    const bool reverse_edge = hw.GetButton(SW_REVERSE).RisingEdge();

    if(freeze_edge)
    {
        freeze_hold = 0.0f;
        freeze_long_action = false;
        if(shift)
        {
            internal_running = !internal_running;
            tap_elapsed = 10.0f;
        }
        else
        {
            if(tap_elapsed > 0.20f && tap_elapsed < 1.60f)
                bpm = ClampRange(60.0f / tap_elapsed, 40.0f, 240.0f);
            tap_elapsed = 0.0f;
        }
    }

    if(hw.GetButton(SW_FREEZE).Pressed() && !shift)
    {
        freeze_hold += block_seconds;
        if(freeze_hold > 0.72f && !freeze_long_action)
        {
            ResetPattern();
            freeze_long_action = true;
        }
    }

    if(reverse_edge)
    {
        if(shift)
            euclidean_mode = !euclidean_mode;
        else
            ext_resolution = static_cast<uint8_t>((ext_resolution + 1) % 3);
    }

    const bool clock_gate = hw.GetGateState(GATE_FREEZE);
    const bool reset_gate = hw.GetGateState(GATE_REVERSE);
    const bool clock_rise = clock_gate && !prev_clock_gate;
    const bool reset_rise = reset_gate && !prev_reset_gate;
    prev_clock_gate = clock_gate;
    prev_reset_gate = reset_gate;

    if(reset_rise)
        ResetPattern();

    if(clock_rise)
    {
        external_recent = 1.75f;
        ExternalClockTick();
    }

    ui_external = external_recent > 0.0f;
    ui_euclidean = euclidean_mode;
    ui_running = internal_running;
    ui_resolution = ext_resolution;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    (void)in;
    hw.ProcessAllControls();
    ProcessButtonsAndClock(size);

    const bool external_mode = external_recent > 0.0f;
    const float steps_per_sample = (bpm * 8.0f / 60.0f) / sample_rate;

    const float kick_decay = expf(-1.0f / (0.205f * sample_rate));
    const float kick_pitch_decay = expf(-1.0f / (0.036f * sample_rate));
    const float snare_decay = expf(-1.0f / (0.145f * sample_rate));
    const float hat_decay = expf(-1.0f / (0.058f * sample_rate));

    for(size_t i = 0; i < size; ++i)
    {
        if(!external_mode && internal_running)
        {
            internal_phase += steps_per_sample;
            if(internal_phase >= 1.0f)
            {
                internal_phase -= 1.0f;
                AdvanceInternalStep();
            }
        }

        const float kick_freq = 48.0f + 118.0f * kick_pitch_env;
        kick_phase += kick_freq / sample_rate;
        if(kick_phase >= 1.0f) kick_phase -= 1.0f;
        const float kick_click = kick_pitch_env * kick_pitch_env * 0.12f;
        const float kick = (sinf(kTwoPi * kick_phase) * 0.94f + kick_click) * kick_env;
        kick_env *= kick_decay;
        kick_pitch_env *= kick_pitch_decay;

        const float noise = static_cast<float>(static_cast<int32_t>(Rand32() >> 8)) / 8388608.0f;

        snare_noise_lp += 0.18f * (noise - snare_noise_lp);
        const float snare_hp = noise - snare_noise_lp;
        snare_phase += 185.0f / sample_rate;
        if(snare_phase >= 1.0f) snare_phase -= 1.0f;
        const float snare = (0.76f * snare_hp + 0.24f * sinf(kTwoPi * snare_phase)) * snare_env;
        snare_env *= snare_decay;

        hat_noise_lp += 0.055f * (noise - hat_noise_lp);
        const float hat_hp = noise - hat_noise_lp;
        const float hat = hat_hp * hat_env * 0.72f;
        hat_env *= hat_decay;

        const float hat_l = hat * sqrtf(fmaxf(0.0f, 1.0f - hat_pan));
        const float hat_r = hat * sqrtf(fmaxf(0.0f, hat_pan));

        const float left = kick * 0.82f + snare * 0.52f + hat_l * 0.60f;
        const float right = kick * 0.82f + snare * 0.60f + hat_r * 0.60f;
        out[0][i] = Saturate(left * 1.12f);
        out[1][i] = Saturate(right * 1.12f);
    }
}

void UpdateLeds()
{
    static float phase = 0.0f;
    phase += 0.13f;
    if(phase > kTwoPi) phase -= kTwoPi;

    for(int p = 0; p < 3; ++p)
    {
        ui_hit[p] *= 0.82f;
        if(ui_hit[p] < 0.01f) ui_hit[p] = 0.0f;
    }
    ui_beat *= 0.84f;
    if(ui_beat < 0.01f) ui_beat = 0.0f;

    const float x = Clamp01(ui_x);
    const float y = Clamp01(ui_y);
    const float c = Clamp01(ui_chaos);
    const float f0 = Clamp01(ui_fill[0]);
    const float f1 = Clamp01(ui_fill[1]);
    const float f2 = Clamp01(ui_fill[2]);

    hw.SetLed(LED_1, 0.0f, 0.22f * x, 0.12f + 0.88f * x);
    hw.SetLed(LED_2, 0.12f + 0.88f * y, 0.04f * y, 0.0f);
    hw.SetLed(LED_3, c, c * 0.68f, 0.0f);
    hw.SetLed(LED_4, ui_hit[0], f0 * (0.18f + 0.82f * ui_hit[0]), 0.0f);
    hw.SetLed(LED_5, 0.0f, f1 * (0.35f + 0.65f * ui_hit[1]), f1);
    hw.SetLed(LED_6, f2, 0.0f, f2 * (0.35f + 0.65f * ui_hit[2]));

    hw.SetLed(LED_BOT_1, ui_hit[0], 0.22f * ui_hit[0], 0.0f);
    hw.SetLed(LED_BOT_2, ui_hit[1], ui_hit[1], ui_hit[1]);
    hw.SetLed(LED_BOT_3, 0.45f * ui_hit[2], 0.0f, ui_hit[2]);

    if(ui_external)
        hw.SetLed(LED_FREEZE, 0.0f, ui_beat, 0.10f * ui_beat);
    else if(ui_running)
        hw.SetLed(LED_FREEZE, 0.0f, 0.18f * ui_beat, ui_beat);
    else
        hw.SetLed(LED_FREEZE, 0.18f, 0.02f, 0.02f);

    if(ui_euclidean)
    {
        const float blink = 0.45f + 0.55f * (0.5f + 0.5f * sinf(phase));
        hw.SetLed(LED_REVERSE, blink, blink, blink);
    }
    else if(ui_resolution == 0)
        hw.SetLed(LED_REVERSE, 0.85f, 0.45f, 0.0f);
    else if(ui_resolution == 1)
        hw.SetLed(LED_REVERSE, 0.0f, 0.85f, 0.85f);
    else
        hw.SetLed(LED_REVERSE, 0.85f, 0.0f, 0.85f);

    hw.WriteLeds();
}

} // namespace

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();
    ResetPattern();
    hw.StartAudio(AudioCallback);

    while(1)
    {
        UpdateLeds();
        System::Delay(10);
    }
}
