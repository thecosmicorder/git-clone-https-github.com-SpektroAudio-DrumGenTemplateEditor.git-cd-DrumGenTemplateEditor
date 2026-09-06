/*
 * Aurora Cloudscape v1.2 - NATIVE DSP
 * Qu-Bit Aurora custom firmware.
 *
 * Re-coded after the Nimbus/Clouds wet engine proved silent on hardware.
 * This revision deliberately uses only Aurora SDK + DaisySP primitives that
 * are known to run on the Aurora platform: DelayLine and ReverbSc.
 *
 * Behaviour:
 *   MIX fully CCW = hard stereo dry passthrough.
 *   Turning MIX clockwise introduces an audible four-tap modulated cloud.
 *
 * Controls:
 *   TIME       base cloud/echo time 12 ms .. 900 ms
 *   REFLECT    feedback / regeneration
 *   MIX        dry/wet (hard bypass at minimum)
 *   ATMOSPHERE active cloud taps 1 .. 4
 *   BLUR       diffusion/reverb + darker feedback
 *   WARP       stereo spread + delay modulation depth/rate
 *
 * Buttons:
 *   FREEZE     latch buffer hold / near-infinite regeneration
 *   REVERSE    latch rotating cross-feedback (ping-pong cloud)
 *   SHIFT      hold for BLOOM: larger, wetter reverb
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
constexpr size_t kNumLines = 4;
constexpr size_t kMaxDelaySamples = 192000;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kBypassThreshold = 0.004f;
constexpr float kMaxFeedback = 0.90f;
constexpr float kFreezeFeedback = 0.985f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delays[kNumLines];
ReverbSc DSY_SDRAM_BSS reverb;

float sample_rate = 48000.0f;
float smooth_delay[kNumLines] = {2400.0f, 3600.0f, 4800.0f, 6000.0f};
float feedback_lp[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f};
float phase[kNumLines] = {0.0f, 1.7f, 3.4f, 5.1f};

bool freeze_latched = false;
bool rotate_latched = false;

volatile float ui_time = 0.0f;
volatile float ui_feedback = 0.0f;
volatile float ui_mix = 0.0f;
volatile float ui_atmosphere = 0.0f;
volatile float ui_blur = 0.0f;
volatile float ui_warp = 0.0f;
volatile int ui_lines = 1;
volatile bool ui_freeze = false;
volatile bool ui_rotate = false;
volatile bool ui_bloom = false;

inline bool Finite(float x)
{
    return std::isfinite(x);
}

inline float Safe(float x)
{
    return Finite(x) ? x : 0.0f;
}

inline float Clamp01(float x)
{
    return fclamp(Safe(x), 0.0f, 1.0f);
}

inline float Bound(float x, float lim)
{
    return fclamp(Safe(x), -lim, lim);
}

inline float Saturate(float x)
{
    return tanhf(Bound(x, 6.0f));
}

inline float KnobCv(int knob, int cv)
{
    return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv));
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    hw.ProcessAllControls();

    if(hw.GetButton(SW_FREEZE).RisingEdge())
        freeze_latched = !freeze_latched;
    if(hw.GetButton(SW_REVERSE).RisingEdge())
        rotate_latched = !rotate_latched;

    const bool freeze = freeze_latched != hw.GetGateState(GATE_FREEZE);
    const bool rotate = rotate_latched != hw.GetGateState(GATE_REVERSE);
    const bool bloom = hw.GetButton(SW_SHIFT).Pressed();

    const float time_ctl = KnobCv(KNOB_TIME, CV_TIME);
    const float reflect = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float mix = KnobCv(KNOB_MIX, CV_MIX);
    const float atmosphere = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float blur = KnobCv(KNOB_BLUR, CV_BLUR);
    const float warp = Clamp01(hw.GetKnobValue(KNOB_WARP)
                               + hw.GetWarpVoct() / 60.0f);

    const int active_lines = 1 + static_cast<int>(atmosphere * 3.999f);

    ui_time = time_ctl;
    ui_feedback = reflect;
    ui_mix = mix;
    ui_atmosphere = atmosphere;
    ui_blur = blur;
    ui_warp = warp;
    ui_lines = active_lines;
    ui_freeze = freeze;
    ui_rotate = rotate;
    ui_bloom = bloom;

    // Absolute guarantee: at minimum MIX nothing in the effect engine can
    // alter or mute the dry signal.
    if(mix <= kBypassThreshold)
    {
        for(size_t i = 0; i < size; ++i)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        return;
    }

    const float base_seconds = fmap(time_ctl, 0.012f, 0.90f, Mapping::LOG);
    constexpr float ratio_max[kNumLines] = {1.0f, 1.21f, 1.47f, 1.83f};
    float centre_delay[kNumLines];
    for(size_t j = 0; j < kNumLines; ++j)
    {
        // WARP progressively separates the tap times.
        const float ratio = 1.0f + warp * (ratio_max[j] - 1.0f);
        centre_delay[j] = fclamp(base_seconds * ratio * sample_rate,
                                 2.0f,
                                 static_cast<float>(kMaxDelaySamples - 256));
    }

    // WARP also adds gentle continuously moving delay modulation. This is what
    // turns the multi-tap echo into a smeared, animated cloud rather than four
    // static repeats.
    const float mod_depth_samples = warp * warp * sample_rate * 0.022f;
    const float lfo_base_hz = 0.045f + warp * 0.40f;
    float phase_inc[kNumLines];
    for(size_t j = 0; j < kNumLines; ++j)
        phase_inc[j] = kTwoPi * lfo_base_hz * (1.0f + 0.37f * j) / sample_rate;

    const float feedback = freeze
        ? kFreezeFeedback
        : fmap(reflect, 0.0f, kMaxFeedback, Mapping::LINEAR);

    // More BLUR = darker repeated material and more diffusion.
    const float feedback_alpha = 0.30f - 0.235f * blur;
    const float effective_blur = bloom ? fmaxf(blur, 0.72f) : blur;
    const float verb_feedback = bloom
        ? 0.91f
        : (0.76f + effective_blur * 0.13f);
    const float verb_lpf = 13500.0f - blur * 6500.0f;
    reverb.SetFeedback(verb_feedback);
    reverb.SetLpFreq(verb_lpf);

    // True dry/wet crossfade. MIX=0 is handled above as a hard bypass.
    const float dry_gain = cosf(mix * kPi * 0.5f);
    const float wet_gain = sinf(mix * kPi * 0.5f) * 1.18f;

    constexpr float tap_norm[4] = {1.18f, 0.96f, 0.82f, 0.74f};
    const float tap_gain = tap_norm[active_lines - 1];

    for(size_t i = 0; i < size; ++i)
    {
        const float input_l = Bound(in[0][i], 2.0f);
        const float input_r = Bound(in[1][i], 2.0f);

        float tap[kNumLines];
        for(size_t j = 0; j < kNumLines; ++j)
        {
            fonepole(smooth_delay[j], centre_delay[j], 0.00065f);
            phase[j] += phase_inc[j];
            if(phase[j] >= kTwoPi)
                phase[j] -= kTwoPi;

            const float mod = sinf(phase[j])
                              * mod_depth_samples
                              * (0.62f + 0.12f * static_cast<float>(j));
            const float d = fclamp(smooth_delay[j] + mod,
                                   2.0f,
                                   static_cast<float>(kMaxDelaySamples - 2));
            delays[j].SetDelay(d);
            tap[j] = Safe(delays[j].Read());
        }

        float cloud_l = 0.0f;
        float cloud_r = 0.0f;
        for(int j = 0; j < active_lines; ++j)
        {
            // Wider panning as WARP increases; still centred with one tap.
            float p = active_lines <= 1
                ? 0.5f
                : static_cast<float>(j) / static_cast<float>(active_lines - 1);
            p = 0.5f + (p - 0.5f) * (0.35f + 0.65f * warp);
            const float pan_l = sqrtf(fmaxf(0.0f, 1.0f - p));
            const float pan_r = sqrtf(fmaxf(0.0f, p));
            cloud_l += tap[j] * pan_l * tap_gain;
            cloud_r += tap[j] * pan_r * tap_gain;
        }

        // Write each line after reading. In rotate mode each line regenerates
        // from its neighbour, creating a clearly audible moving stereo cloud.
        for(size_t j = 0; j < kNumLines; ++j)
        {
            const bool enabled = static_cast<int>(j) < active_lines;
            const float p = active_lines <= 1
                ? 0.5f
                : static_cast<float>(j) / static_cast<float>(active_lines - 1);
            const float pan_l = sqrtf(fmaxf(0.0f, 1.0f - p));
            const float pan_r = sqrtf(fmaxf(0.0f, p));
            const float injection = Bound(input_l * pan_l + input_r * pan_r,
                                          1.5f) * 0.82f;

            int src = static_cast<int>(j);
            if(rotate && enabled)
                src = (src + active_lines - 1) % active_lines;

            const float feedback_source = enabled ? Bound(tap[src], 1.2f) : 0.0f;
            feedback_lp[j] += feedback_alpha * (feedback_source - feedback_lp[j]);
            feedback_lp[j] = Bound(feedback_lp[j], 1.2f);

            const float new_input = freeze ? 0.0f : injection;
            const float regen = enabled ? feedback * feedback_lp[j] : 0.0f;
            delays[j].Write(Bound(new_input + regen, 1.35f));
        }

        cloud_l = Saturate(cloud_l * 1.36f);
        cloud_r = Saturate(cloud_r * 1.36f);

        // Feed both the cloud and a little live input to the reverb so BLUR is
        // audible immediately instead of waiting for the first long delay tap.
        float verb_l = 0.0f;
        float verb_r = 0.0f;
        if(effective_blur > 0.001f)
        {
            const float verb_in_l = Bound((cloud_l * 0.70f + input_l * 0.18f)
                                              * effective_blur,
                                          0.8f);
            const float verb_in_r = Bound((cloud_r * 0.70f + input_r * 0.18f)
                                              * effective_blur,
                                          0.8f);
            reverb.Process(verb_in_l, verb_in_r, &verb_l, &verb_r);
            verb_l = Saturate(verb_l);
            verb_r = Saturate(verb_r);
        }

        const float wet_l = Saturate(cloud_l + verb_l * effective_blur * 0.78f);
        const float wet_r = Saturate(cloud_r + verb_r * effective_blur * 0.78f);

        out[0][i] = Saturate(input_l * dry_gain + wet_l * wet_gain);
        out[1][i] = Saturate(input_r * dry_gain + wet_r * wet_gain);
    }
}

void UpdateLeds()
{
    hw.ClearLeds();

    // Six top LEDs mirror the six knob values so control movement is visible.
    hw.SetLed(LED_1, 0.0f, 0.0f, 0.12f + 0.88f * ui_time);              // TIME blue
    hw.SetLed(LED_2, 0.12f + 0.88f * ui_feedback, 0.0f, 0.0f);          // REFLECT red
    hw.SetLed(LED_3, 0.12f + 0.88f * ui_mix, 0.12f + 0.88f * ui_mix, 0.0f); // MIX yellow
    hw.SetLed(LED_4, 0.0f, 0.12f + 0.88f * ui_atmosphere, 0.0f);        // ATMOS green
    hw.SetLed(LED_5, 0.0f, 0.18f + 0.55f * ui_blur, 0.18f + 0.82f * ui_blur); // BLUR cyan
    hw.SetLed(LED_6, 0.45f + 0.55f * ui_warp, 0.0f, 0.45f + 0.55f * ui_warp); // WARP magenta

    for(int j = 0; j < 3; ++j)
    {
        const float on = j < (ui_lines - 1) ? 0.85f : 0.06f;
        hw.SetLed(static_cast<Leds>(LED_BOT_1 + j), 0.0f, on, 0.0f);
    }

    hw.SetLed(LED_FREEZE,
              ui_freeze ? 1.0f : 0.03f,
              ui_freeze ? 1.0f : 0.03f,
              ui_freeze ? 1.0f : 0.03f);
    hw.SetLed(LED_REVERSE,
              ui_rotate ? 0.0f : 0.03f,
              ui_rotate ? 1.0f : 0.03f,
              ui_rotate ? 1.0f : 0.03f);

    if(ui_bloom)
    {
        hw.SetLed(LED_BOT_1, 1.0f, 1.0f, 1.0f);
        hw.SetLed(LED_BOT_2, 1.0f, 1.0f, 1.0f);
        hw.SetLed(LED_BOT_3, 1.0f, 1.0f, 1.0f);
    }

    hw.WriteLeds();
}

} // namespace

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();

    for(size_t j = 0; j < kNumLines; ++j)
    {
        delays[j].Init();
        delays[j].SetDelay(smooth_delay[j]);
    }

    reverb.Init(sample_rate);
    reverb.SetFeedback(0.80f);
    reverb.SetLpFreq(10000.0f);

    hw.StartAudio(AudioCallback);

    while(1)
    {
        UpdateLeds();
        System::Delay(12);
    }
}
