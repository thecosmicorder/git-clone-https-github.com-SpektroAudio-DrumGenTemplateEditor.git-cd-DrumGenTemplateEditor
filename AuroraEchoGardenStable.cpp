/**
 * Aurora EchoGarden v0.3.1 Stable
 * Four-line feedback echo + lush stereo reverb for Qu-Bit Aurora.
 * Stability revision: bounded delay/reverb drive, safer feedback ranges,
 * finite-value guards, and automatic reverb recovery.
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
constexpr size_t   kNumLines             = 4;
constexpr size_t   kMaxDelaySamples      = 192000;
constexpr float    kPi                   = 3.14159265358979323846f;
constexpr float    kVisualMoveThreshold  = 0.004f;
constexpr uint32_t kVisualHoldCallbacks  = 425;
constexpr float    kMaxNormalFeedback    = 0.91f;
constexpr float    kFreezeFeedback       = 0.985f;
constexpr float    kMaxReverbFeedback    = 0.88f;
constexpr float    kBloomReverbFeedback  = 0.90f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delay_lines[kNumLines];
ReverbSc DSY_SDRAM_BSS reverb;

float smooth_delay[kNumLines] = {24000.0f, 32000.0f, 40000.0f, 48000.0f};
float feedback_lp[kNumLines]  = {0.0f, 0.0f, 0.0f, 0.0f};

bool freeze_latched   = false;
bool pingpong_latched = false;

volatile bool reverb_fault = false;
volatile uint32_t reverb_recoveries = 0;

volatile int   ui_active_lines = 4;
volatile float ui_feedback     = 0.45f;
volatile float ui_mix          = 0.50f;
volatile float ui_reverb       = 0.55f;
volatile float ui_spread       = 0.45f;
volatile bool  ui_freeze       = false;
volatile bool  ui_pingpong     = false;
volatile bool  ui_bloom        = false;

enum VisualParam
{
    VIS_TIME = 0,
    VIS_FEEDBACK,
    VIS_MIX,
    VIS_LINES,
    VIS_REVERB,
    VIS_WARP,
    VIS_NONE
};

volatile int      ui_visual_param = VIS_NONE;
volatile float    ui_visual_value = 0.0f;
volatile uint32_t ui_visual_hold  = 0;
float last_visual_knob[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
bool visual_tracking_ready = false;

struct Rgb
{
    float r;
    float g;
    float b;
};

inline bool IsFiniteSample(float x)
{
    return (x == x) && x > -100000.0f && x < 100000.0f;
}

inline float SafeSample(float x)
{
    return IsFiniteSample(x) ? x : 0.0f;
}

inline float Clamp01(float x)
{
    x = SafeSample(x);
    return fclamp(x, 0.0f, 1.0f);
}

inline float HardBound(float x, float limit)
{
    x = SafeSample(x);
    return fclamp(x, -limit, limit);
}

inline float SoftLimit(float x)
{
    return tanhf(HardBound(x, 6.0f));
}

inline float KnobCv(int knob, int cv)
{
    return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv));
}

Rgb GetVisualColor(int param)
{
    switch(param)
    {
        case VIS_TIME:     return {0.0f, 0.0f, 1.0f}; // BLUE
        case VIS_FEEDBACK: return {1.0f, 0.0f, 0.0f}; // RED
        case VIS_MIX:      return {1.0f, 1.0f, 0.0f}; // YELLOW
        case VIS_LINES:    return {0.0f, 1.0f, 0.0f}; // GREEN
        case VIS_REVERB:   return {0.0f, 0.0f, 1.0f}; // BLUE
        case VIS_WARP:     return {1.0f, 1.0f, 0.0f}; // YELLOW
        default:           return {0.25f, 0.25f, 0.25f};
    }
}

void TrackVisualChange(const float raw_knob[6], const float effective_value[6])
{
    if(!visual_tracking_ready)
    {
        for(int i = 0; i < 6; ++i)
            last_visual_knob[i] = raw_knob[i];
        visual_tracking_ready = true;
        return;
    }

    int changed = -1;
    float largest_delta = 0.0f;
    for(int i = 0; i < 6; ++i)
    {
        const float delta = fabsf(raw_knob[i] - last_visual_knob[i]);
        if(delta > largest_delta)
        {
            largest_delta = delta;
            changed = i;
        }
    }

    if(changed >= 0 && largest_delta >= kVisualMoveThreshold)
    {
        for(int i = 0; i < 6; ++i)
            last_visual_knob[i] = raw_knob[i];
        ui_visual_param = changed;
        ui_visual_value = Clamp01(effective_value[changed]);
        ui_visual_hold  = kVisualHoldCallbacks;
    }
    else if(ui_visual_hold > 0)
    {
        --ui_visual_hold;
    }
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();

    if(hw.GetButton(SW_FREEZE).RisingEdge())
        freeze_latched = !freeze_latched;
    if(hw.GetButton(SW_REVERSE).RisingEdge())
        pingpong_latched = !pingpong_latched;

    const bool freeze   = freeze_latched != hw.GetGateState(GATE_FREEZE);
    const bool pingpong = pingpong_latched != hw.GetGateState(GATE_REVERSE);
    const bool bloom    = hw.GetButton(SW_SHIFT).Pressed();

    const float time_control     = KnobCv(KNOB_TIME, CV_TIME);
    const float feedback_control = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float mix              = KnobCv(KNOB_MIX, CV_MIX);
    const float line_control     = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float reverb_send      = KnobCv(KNOB_BLUR, CV_BLUR);
    const float spread = Clamp01(hw.GetKnobValue(KNOB_WARP)
                                 + (hw.GetWarpVoct() / 60.0f));

    const int active_lines = 1 + static_cast<int>(line_control * 3.999f);

    const float raw_knob[6] = {
        hw.GetKnobValue(KNOB_TIME),
        hw.GetKnobValue(KNOB_REFLECT),
        hw.GetKnobValue(KNOB_MIX),
        hw.GetKnobValue(KNOB_ATMOSPHERE),
        hw.GetKnobValue(KNOB_BLUR),
        hw.GetKnobValue(KNOB_WARP)
    };
    const float effective_visual[6] = {
        time_control,
        feedback_control,
        mix,
        static_cast<float>(active_lines - 1) / 3.0f,
        reverb_send,
        spread
    };
    TrackVisualChange(raw_knob, effective_visual);

    const float base_seconds = fmap(time_control, 0.015f, 1.8f, Mapping::LOG);
    const float sample_rate  = hw.AudioSampleRate();

    constexpr float max_ratio[kNumLines] = {1.0f, 1.333333f, 1.666667f, 2.0f};
    float target_delay[kNumLines];
    for(size_t j = 0; j < kNumLines; ++j)
    {
        const float ratio = 1.0f + spread * (max_ratio[j] - 1.0f);
        target_delay[j]
            = fclamp(base_seconds * ratio * sample_rate,
                     1.0f,
                     static_cast<float>(kMaxDelaySamples - 2));
    }

    const float feedback = freeze
                               ? kFreezeFeedback
                               : fmap(feedback_control,
                                      0.0f,
                                      kMaxNormalFeedback,
                                      Mapping::LINEAR);

    const float effective_reverb_send
        = bloom ? fmaxf(reverb_send, 0.72f) : reverb_send;
    const float reverb_feedback
        = bloom ? kBloomReverbFeedback
                : (0.78f + effective_reverb_send
                              * (kMaxReverbFeedback - 0.78f));
    const float reverb_lpf
        = bloom ? 13000.0f : (7000.0f + (1.0f - spread) * 5500.0f);

    if(!reverb_fault)
    {
        reverb.SetFeedback(reverb_feedback);
        reverb.SetLpFreq(reverb_lpf);
    }

    const float dry_gain = cosf(mix * kPi * 0.5f);
    const float wet_gain = sinf(mix * kPi * 0.5f) * 1.26f;

    // Fixed lookup avoids powf in the audio callback and keeps multiple taps loud.
    constexpr float tap_norm[4] = {1.22f, 1.00f, 0.88f, 0.80f};
    const float tap_gain = tap_norm[active_lines - 1];

    for(size_t i = 0; i < size; ++i)
    {
        const float input_l = HardBound(in[0][i], 2.0f);
        const float input_r = HardBound(in[1][i], 2.0f);
        float tap[kNumLines];

        for(size_t j = 0; j < kNumLines; ++j)
        {
            fonepole(smooth_delay[j], target_delay[j], 0.00045f);
            delay_lines[j].SetDelay(smooth_delay[j]);
            tap[j] = SafeSample(delay_lines[j].Read());
        }

        float echo_l = 0.0f;
        float echo_r = 0.0f;
        for(int j = 0; j < active_lines; ++j)
        {
            const float pan = active_lines <= 1
                                  ? 0.5f
                                  : static_cast<float>(j)
                                        / static_cast<float>(active_lines - 1);
            const float pan_l = sqrtf(fmaxf(0.0f, 1.0f - pan));
            const float pan_r = sqrtf(fmaxf(0.0f, pan));
            echo_l += tap[j] * pan_l * tap_gain;
            echo_r += tap[j] * pan_r * tap_gain;
        }

        for(size_t j = 0; j < kNumLines; ++j)
        {
            const bool line_active = static_cast<int>(j) < active_lines;
            const float pan = active_lines <= 1
                                  ? 0.5f
                                  : static_cast<float>(j)
                                        / static_cast<float>(active_lines - 1);
            const float pan_l = sqrtf(fmaxf(0.0f, 1.0f - pan));
            const float pan_r = sqrtf(fmaxf(0.0f, pan));
            const float injection
                = HardBound((input_l * pan_l + input_r * pan_r) * 0.84f, 1.20f);

            float feedback_source = 0.0f;
            if(line_active)
            {
                int src = static_cast<int>(j);
                if(pingpong)
                    src = (src + active_lines - 1) % active_lines;
                feedback_source = HardBound(tap[src], 1.10f);
            }

            if(!IsFiniteSample(feedback_lp[j]))
                feedback_lp[j] = 0.0f;
            feedback_lp[j] += 0.16f * (feedback_source - feedback_lp[j]);
            feedback_lp[j] = HardBound(feedback_lp[j], 1.10f);

            const float input_term = freeze ? 0.0f : injection;
            const float regen_term
                = line_active ? feedback * feedback_lp[j] : 0.0f;
            const float write_sample
                = HardBound(input_term + regen_term, 1.20f);
            delay_lines[j].Write(write_sample);
        }

        // Keep the audible echo strong, independently of internal regeneration.
        const float audible_echo_l = SoftLimit(echo_l * 1.48f);
        const float audible_echo_r = SoftLimit(echo_r * 1.48f);

        float verb_l = 0.0f;
        float verb_r = 0.0f;
        if(!reverb_fault && effective_reverb_send > 0.001f)
        {
            // ReverbSc is linear internally, so bound its input explicitly.
            const float verb_in_l
                = HardBound(audible_echo_l * effective_reverb_send * 0.58f,
                            0.75f);
            const float verb_in_r
                = HardBound(audible_echo_r * effective_reverb_send * 0.58f,
                            0.75f);

            reverb.Process(verb_in_l, verb_in_r, &verb_l, &verb_r);

            if(!IsFiniteSample(verb_l) || !IsFiniteSample(verb_r)
               || fabsf(verb_l) > 8.0f || fabsf(verb_r) > 8.0f)
            {
                reverb_fault = true;
                verb_l = 0.0f;
                verb_r = 0.0f;
            }
            else
            {
                verb_l = SoftLimit(verb_l);
                verb_r = SoftLimit(verb_r);
            }
        }

        const float wet_l
            = SoftLimit(audible_echo_l
                        + verb_l * effective_reverb_send * 0.52f);
        const float wet_r
            = SoftLimit(audible_echo_r
                        + verb_r * effective_reverb_send * 0.52f);

        out[0][i] = SoftLimit(input_l * dry_gain + wet_l * wet_gain);
        out[1][i] = SoftLimit(input_r * dry_gain + wet_r * wet_gain);
    }

    ui_active_lines = active_lines;
    ui_feedback     = feedback_control;
    ui_mix          = mix;
    ui_reverb       = effective_reverb_send;
    ui_spread       = spread;
    ui_freeze       = freeze;
    ui_pingpong     = pingpong;
    ui_bloom        = bloom;
}

void DrawEditMeter()
{
    static float pulse_phase = 0.0f;
    pulse_phase += 0.30f;
    if(pulse_phase > 2.0f * kPi)
        pulse_phase -= 2.0f * kPi;

    const int param = ui_visual_param;
    const float value = Clamp01(ui_visual_value);
    const Rgb color = GetVisualColor(param);
    const float pulse
        = 0.88f + 0.12f * (0.5f + 0.5f * sinf(pulse_phase));
    const float level = value * 6.0f;
    const float value_brightness = 0.35f + 0.65f * value;

    for(int i = 0; i < 6; ++i)
    {
        float segment = Clamp01(level - static_cast<float>(i));
        if(i == 0)
            segment = fmaxf(segment, 0.25f);
        const float intensity
            = fclamp(value_brightness * (0.32f + 0.68f * segment) * pulse,
                     0.0f,
                     1.0f);
        hw.SetLed(static_cast<Leds>(LED_1 + i),
                  color.r * intensity,
                  color.g * intensity,
                  color.b * intensity);
    }

    const float underline
        = fclamp((0.28f + 0.58f * value) * pulse, 0.0f, 1.0f);
    hw.SetLed(LED_BOT_1,
              color.r * underline,
              color.g * underline,
              color.b * underline);
    hw.SetLed(LED_BOT_2,
              color.r * underline,
              color.g * underline,
              color.b * underline);
    hw.SetLed(LED_BOT_3,
              color.r * underline,
              color.g * underline,
              color.b * underline);
}

void DrawNormalStatus()
{
    for(int j = 0; j < 4; ++j)
    {
        const float on = j < ui_active_lines ? 1.0f : 0.08f;
        hw.SetLed(static_cast<Leds>(LED_1 + j), 0.0f, on, 0.0f);
    }

    const float fb = 0.20f + 0.80f * ui_feedback;
    const float rv = 0.20f + 0.80f * ui_reverb;
    hw.SetLed(LED_5, fb, 0.0f, 0.0f);
    hw.SetLed(LED_6, 0.0f, 0.0f, rv);

    const float mix_level
        = ui_mix > 0.02f ? 0.25f + 0.75f * ui_mix : 0.08f;
    hw.SetLed(LED_BOT_1, mix_level, mix_level, 0.0f);
    hw.SetLed(LED_BOT_2, 0.0f, 0.0f, 0.18f + 0.82f * ui_spread);
    hw.SetLed(LED_BOT_3,
              ui_bloom ? 1.0f : 0.0f,
              ui_bloom ? 1.0f : 0.0f,
              0.0f);
}

void UpdateLeds()
{
    hw.ClearLeds();

    if(ui_visual_hold > 0 && ui_visual_param != VIS_NONE)
        DrawEditMeter();
    else
        DrawNormalStatus();

    hw.SetLed(LED_FREEZE,
              ui_freeze ? 1.0f : 0.0f,
              ui_freeze ? 1.0f : 0.0f,
              0.0f);
    hw.SetLed(LED_REVERSE,
              0.0f,
              ui_pingpong ? 1.0f : 0.0f,
              0.0f);
    hw.WriteLeds();
}

void RecoverReverbIfNeeded()
{
    if(!reverb_fault)
        return;

    // Reinitialize outside the audio callback so a corrupt reverb state can
    // never leave the whole module permanently silent.
    hw.StopAudio();
    reverb.Init(hw.AudioSampleRate());
    reverb.SetFeedback(0.82f);
    reverb.SetLpFreq(11000.0f);
    reverb_fault = false;
    ++reverb_recoveries;
    hw.StartAudio(AudioCallback);
}

} // namespace

int main(void)
{
    hw.Init();

    for(size_t j = 0; j < kNumLines; ++j)
    {
        delay_lines[j].Init();
        delay_lines[j].SetDelay(smooth_delay[j]);
    }

    reverb.Init(hw.AudioSampleRate());
    reverb.SetFeedback(0.82f);
    reverb.SetLpFreq(11000.0f);

    hw.StartAudio(AudioCallback);

    while(1)
    {
        RecoverReverbIfNeeded();
        UpdateLeds();
        System::Delay(12);
    }
}
