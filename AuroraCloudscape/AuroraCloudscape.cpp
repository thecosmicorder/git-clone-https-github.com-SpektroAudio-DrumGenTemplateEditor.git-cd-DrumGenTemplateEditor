/*
 * Aurora Cloudscape v1.0
 * Qu-Bit Aurora port using the open-source Mutable Instruments Clouds DSP
 * as maintained in Electrosmith DaisyExamples/Nimbus.
 *
 * Original Clouds DSP: Copyright 2014 Emilie Gillet, MIT License.
 * Nimbus Daisy port: Electrosmith contributors, MIT License.
 * Aurora hardware adaptation: 2026.
 *
 * This is an independent derivative firmware and is not an official
 * Mutable Instruments or Qu-Bit firmware release.
 */

#include "aurora.h"
#include "daisysp.h"
#include "granular_processor.h"
#include "resources.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace daisy;
using namespace daisysp;
using namespace aurora;

namespace
{
constexpr size_t kCloudsBlockSize = 32;
constexpr size_t kLargeMemSize = 118784;
constexpr size_t kSmallMemSize = 65536 - 128;
constexpr float kEditThreshold = 0.004f;
constexpr uint32_t kEditHoldCallbacks = 1200; // ~0.8s at 48k/32

Hardware hw;
D2RAM GranularProcessorClouds processor;
DSY_SDRAM_BSS uint8_t block_mem[kLargeMemSize];
DTCMRAM uint8_t block_ccm[kSmallMemSize];
Parameters* params = nullptr;

int blend_target = 0;   // 0 dry/wet, 1 spread, 2 feedback, 3 reverb
int playback_mode = 0;  // granular, stretch, looping delay, spectral
bool freeze_latched = false;

volatile int ui_blend_target = 0;
volatile int ui_playback_mode = 0;
volatile bool ui_freeze = false;
volatile float ui_position = 0.5f;
volatile float ui_size = 0.5f;
volatile float ui_density = 0.55f;
volatile float ui_texture = 0.5f;
volatile float ui_pitch_norm = 0.5f;
volatile float ui_blend_value = 0.5f;
volatile int ui_edit_param = -1;
volatile float ui_edit_value = 0.0f;
volatile uint32_t ui_edit_hold = 0;

float last_knob[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
bool knob_tracking_ready = false;

struct Rgb { float r, g, b; };

inline float Clamp01(float x) { return fclamp(x, 0.0f, 1.0f); }
inline float KnobCv(int knob, int cv)
{
    return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv));
}

inline float PitchFrom01(float x)
{
    // Same response used by Electrosmith Nimbus: approximately +/-24 semitones.
    float p = 9.798f * (x - 0.5f);
    p *= p;
    return x < 0.5f ? -p : p;
}

Rgb ModeColor(int mode)
{
    switch(mode)
    {
        case 0: return {0.0f, 1.0f, 0.0f}; // granular: green
        case 1: return {0.0f, 0.0f, 1.0f}; // stretch: blue
        case 2: return {1.0f, 1.0f, 0.0f}; // looping delay: yellow
        case 3: return {1.0f, 0.0f, 0.0f}; // spectral: red
        default:return {0.3f, 0.3f, 0.3f};
    }
}

Rgb BlendColor(int target)
{
    switch(target)
    {
        case 0: return {1.0f, 1.0f, 0.0f}; // dry/wet: yellow
        case 1: return {0.0f, 1.0f, 0.0f}; // spread: green
        case 2: return {1.0f, 0.0f, 0.0f}; // feedback: red
        case 3: return {0.0f, 0.0f, 1.0f}; // reverb: blue
        default:return {0.4f, 0.4f, 0.4f};
    }
}

Rgb EditColor(int param)
{
    // 0 position, 1 size, 2 blend, 3 density, 4 texture, 5 pitch
    switch(param)
    {
        case 0: return {0.0f, 0.0f, 1.0f}; // position: blue
        case 1: return {0.0f, 1.0f, 0.0f}; // size: green
        case 2: return BlendColor(ui_blend_target);
        case 3: return {1.0f, 1.0f, 0.0f}; // density: yellow
        case 4: return {1.0f, 0.0f, 0.0f}; // texture: red
        case 5: return {0.0f, 0.55f, 1.0f}; // pitch: blue/cyan
        default:return {0.3f, 0.3f, 0.3f};
    }
}

void TrackKnobEdits(const float raw[6], const float values[6])
{
    if(!knob_tracking_ready)
    {
        for(int i = 0; i < 6; ++i) last_knob[i] = raw[i];
        knob_tracking_ready = true;
        return;
    }

    int changed = -1;
    float max_delta = 0.0f;
    for(int i = 0; i < 6; ++i)
    {
        const float d = fabsf(raw[i] - last_knob[i]);
        if(d > max_delta)
        {
            max_delta = d;
            changed = i;
        }
    }

    if(changed >= 0 && max_delta >= kEditThreshold)
    {
        for(int i = 0; i < 6; ++i) last_knob[i] = raw[i];
        ui_edit_param = changed;
        ui_edit_value = Clamp01(values[changed]);
        ui_edit_hold = kEditHoldCallbacks;
    }
    else if(ui_edit_hold > 0)
    {
        --ui_edit_hold;
    }
}

void ProcessControls()
{
    hw.ProcessAllControls();

    if(hw.GetButton(SW_FREEZE).RisingEdge())
        freeze_latched = !freeze_latched;

    if(hw.GetButton(SW_REVERSE).RisingEdge())
    {
        playback_mode = (playback_mode + 1) & 3;
        processor.set_playback_mode(static_cast<PlaybackMode>(playback_mode));
    }

    if(hw.GetButton(SW_SHIFT).RisingEdge())
        blend_target = (blend_target + 1) & 3;

    const float position = KnobCv(KNOB_TIME, CV_TIME);
    const float size = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float blend = KnobCv(KNOB_MIX, CV_MIX);
    const float density = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float texture = KnobCv(KNOB_BLUR, CV_BLUR);

    const float pitch_knob = hw.GetKnobValue(KNOB_WARP);
    const float pitch_semitones = fclamp(PitchFrom01(pitch_knob) + hw.GetWarpVoct(), -48.0f, 48.0f);

    params->position = position;
    params->size = size;
    params->density = density;
    params->texture = texture;
    params->pitch = pitch_semitones;

    switch(blend_target)
    {
        case 0: params->dry_wet = blend; break;
        case 1: params->stereo_spread = blend; break;
        case 2: params->feedback = blend; break;
        case 3: params->reverb = blend; break;
    }

    const bool gate_freeze = hw.GetGateState(GATE_FREEZE);
    params->freeze = freeze_latched || gate_freeze;
    params->trigger = hw.GetGateTrig(GATE_REVERSE);
    params->gate = hw.GetGateState(GATE_REVERSE);

    const float raw[6] = {
        hw.GetKnobValue(KNOB_TIME),
        hw.GetKnobValue(KNOB_REFLECT),
        hw.GetKnobValue(KNOB_MIX),
        hw.GetKnobValue(KNOB_ATMOSPHERE),
        hw.GetKnobValue(KNOB_BLUR),
        hw.GetKnobValue(KNOB_WARP)
    };
    const float edit_values[6] = {position, size, blend, density, texture, pitch_knob};
    TrackKnobEdits(raw, edit_values);

    ui_blend_target = blend_target;
    ui_playback_mode = playback_mode;
    ui_freeze = params->freeze;
    ui_position = position;
    ui_size = size;
    ui_density = density;
    ui_texture = texture;
    ui_pitch_norm = pitch_knob;
    ui_blend_value = blend;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    ProcessControls();

    FloatFrame input[kCloudsBlockSize];
    FloatFrame output[kCloudsBlockSize];
    const size_t n = size > kCloudsBlockSize ? kCloudsBlockSize : size;

    for(size_t i = 0; i < n; ++i)
    {
        input[i].l = in[0][i];
        input[i].r = in[1][i];
        output[i].l = 0.0f;
        output[i].r = 0.0f;
    }

    processor.Process(input, output, n);

    for(size_t i = 0; i < n; ++i)
    {
        out[0][i] = output[i].l;
        out[1][i] = output[i].r;
    }
    for(size_t i = n; i < size; ++i)
    {
        out[0][i] = 0.0f;
        out[1][i] = 0.0f;
    }
}

void DrawEditMeter()
{
    const float value = Clamp01(ui_edit_value);
    const Rgb c = EditColor(ui_edit_param);
    const float fill = value * 6.0f;
    for(int i = 0; i < 6; ++i)
    {
        float seg = Clamp01(fill - static_cast<float>(i));
        if(i == 0) seg = fmaxf(seg, 0.18f);
        const float bright = fclamp(0.18f + 0.82f * seg, 0.0f, 1.0f);
        hw.SetLed(static_cast<Leds>(LED_1 + i), c.r * bright, c.g * bright, c.b * bright);
    }
}

void DrawBlendTarget()
{
    const float dim = 0.08f;
    if(ui_blend_target == 0)
    {
        hw.SetLed(LED_BOT_1, 1.0f, 1.0f, 0.0f);
        hw.SetLed(LED_BOT_2, dim, dim, 0.0f);
        hw.SetLed(LED_BOT_3, dim, dim, 0.0f);
    }
    else if(ui_blend_target == 1)
    {
        hw.SetLed(LED_BOT_1, 0.0f, dim, 0.0f);
        hw.SetLed(LED_BOT_2, 0.0f, 1.0f, 0.0f);
        hw.SetLed(LED_BOT_3, 0.0f, dim, 0.0f);
    }
    else if(ui_blend_target == 2)
    {
        hw.SetLed(LED_BOT_1, dim, 0.0f, 0.0f);
        hw.SetLed(LED_BOT_2, dim, 0.0f, 0.0f);
        hw.SetLed(LED_BOT_3, 1.0f, 0.0f, 0.0f);
    }
    else
    {
        hw.SetLed(LED_BOT_1, 0.0f, 0.0f, 0.70f);
        hw.SetLed(LED_BOT_2, 0.0f, 0.0f, 0.70f);
        hw.SetLed(LED_BOT_3, 0.0f, 0.0f, 0.70f);
    }
}

void UpdateLeds()
{
    hw.ClearLeds();

    if(ui_edit_hold > 0 && ui_edit_param >= 0)
    {
        DrawEditMeter();
    }
    else
    {
        hw.SetLed(LED_1, 0.0f, 0.0f, 0.15f + 0.85f * ui_position);
        hw.SetLed(LED_2, 0.0f, 0.15f + 0.85f * ui_size, 0.0f);
        hw.SetLed(LED_3, 0.15f + 0.85f * ui_density, 0.15f + 0.85f * ui_density, 0.0f);
        hw.SetLed(LED_4, 0.15f + 0.85f * ui_texture, 0.0f, 0.0f);
        hw.SetLed(LED_5, 0.0f, 0.25f + 0.75f * fabsf(ui_pitch_norm - 0.5f) * 2.0f, 1.0f);
        const Rgb bc = BlendColor(ui_blend_target);
        const float bv = 0.18f + 0.82f * ui_blend_value;
        hw.SetLed(LED_6, bc.r * bv, bc.g * bv, bc.b * bv);
    }

    DrawBlendTarget();

    const Rgb mc = ModeColor(ui_playback_mode);
    hw.SetLed(LED_REVERSE, mc.r, mc.g, mc.b);
    hw.SetLed(LED_FREEZE,
              ui_freeze ? 1.0f : 0.03f,
              ui_freeze ? 1.0f : 0.03f,
              ui_freeze ? 1.0f : 0.03f);
    hw.WriteLeds();
}
} // namespace

int main(void)
{
    hw.Init(true);
    hw.SetAudioBlockSize(kCloudsBlockSize);

    const float sample_rate = hw.AudioSampleRate();
    InitResources(sample_rate);

    processor.Init(sample_rate,
                   block_mem,
                   sizeof(block_mem),
                   block_ccm,
                   sizeof(block_ccm));
    processor.set_quality(0); // 16-bit stereo
    processor.set_playback_mode(PLAYBACK_MODE_GRANULAR);

    params = processor.mutable_parameters();
    params->position = 0.5f;
    params->size = 0.5f;
    params->pitch = 0.0f;
    params->density = 0.55f;
    params->texture = 0.5f;
    params->dry_wet = 0.5f;
    params->stereo_spread = 0.5f;
    params->feedback = 0.20f;
    params->reverb = 0.25f;
    params->freeze = false;
    params->trigger = false;
    params->gate = false;

    hw.StartAudio(AudioCallback);

    uint32_t last_led_ms = 0;
    while(1)
    {
        processor.Prepare();
        const uint32_t now = System::GetNow();
        if(now - last_led_ms >= 12)
        {
            UpdateLeds();
            last_led_ms = now;
        }
    }
}
