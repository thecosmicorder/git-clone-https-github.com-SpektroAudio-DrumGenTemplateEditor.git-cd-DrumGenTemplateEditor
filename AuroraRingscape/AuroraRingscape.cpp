/* Aurora Ringscape v1.0
   Uses Mutable Instruments Rings DSP (MIT license) with Aurora-native control/FX layer. */
#include "aurora.h"
#include "daisysp.h"
#include "rings/dsp/part.h"
#include "rings/dsp/strummer.h"
#include "stmlib/utils/random.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace daisy;
using namespace daisysp;
using namespace aurora;

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kBypassThreshold = 0.004f;
constexpr size_t kRingsBlock = 24;

Hardware hw;
D2RAM rings::Part part;
D2RAM rings::Strummer strummer;
DSY_SDRAM_BSS uint16_t rings_reverb_buffer[32768];
ReverbSc DSY_SDRAM_BSS lush_reverb;

float sample_rate = 48000.0f;
int model_index = 0;
bool sustain_latched = false;
bool lush_latched = false;
bool swarm_latched = false;

volatile float ui_pitch = 0.5f;
volatile float ui_structure = 0.5f;
volatile float ui_mix = 0.0f;
volatile float ui_brightness = 0.5f;
volatile float ui_damping = 0.5f;
volatile float ui_position = 0.5f;
volatile int ui_model = 0;
volatile bool ui_sustain = false;
volatile bool ui_lush = false;
volatile bool ui_swarm = false;

inline bool Finite(float x) { return std::isfinite(x); }
inline float Safe(float x) { return Finite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float Saturate(float x) { return tanhf(Bound(x, 5.0f)); }
inline float KnobCv(int knob, int cv) { return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv)); }

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();

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
        if(modifier)
        {
            lush_latched = !lush_latched;     // SHIFT + FREEZE = Lush Space
            shift_grace = 0.0f;
        }
        else
        {
            sustain_latched = !sustain_latched; // FREEZE = Sustain
        }
    }

    if(reverse_edge)
    {
        if(modifier)
        {
            swarm_latched = !swarm_latched;   // SHIFT + REVERSE = 4-voice Chord Swarm
            shift_grace = 0.0f;
        }
        else
        {
            model_index = (model_index + 1) % static_cast<int>(rings::RESONATOR_MODEL_LAST);
            part.set_model(static_cast<rings::ResonatorModel>(model_index));
        }
    }

    const bool sustain = sustain_latched != hw.GetGateState(GATE_FREEZE);
    const bool lush = lush_latched;
    const bool swarm = swarm_latched;

    const float pitch_ctl = KnobCv(KNOB_TIME, CV_TIME);
    const float structure = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float mix = KnobCv(KNOB_MIX, CV_MIX);
    const float brightness = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float damping = KnobCv(KNOB_BLUR, CV_BLUR);
    const float position = Clamp01(hw.GetKnobValue(KNOB_WARP));

    ui_pitch = pitch_ctl;
    ui_structure = structure;
    ui_mix = mix;
    ui_brightness = brightness;
    ui_damping = damping;
    ui_position = position;
    ui_model = model_index;
    ui_sustain = sustain;
    ui_lush = lush;
    ui_swarm = swarm;

    if(mix <= kBypassThreshold)
    {
        for(size_t i = 0; i < size; ++i)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        return;
    }

    // Rings DSP is fixed at 48 kHz / max block 24; Aurora is configured likewise.
    float input_mono[kRingsBlock] = {0.0f};
    float rings_out[kRingsBlock] = {0.0f};
    float rings_aux[kRingsBlock] = {0.0f};
    const size_t n = size > kRingsBlock ? kRingsBlock : size;
    for(size_t i = 0; i < n; ++i)
        input_mono[i] = Bound((in[0][i] + in[1][i]) * 0.5f, 1.5f);

    const int polyphony = swarm ? 4 : 1;
    if(part.polyphony() != polyphony)
        part.set_polyphony(polyphony);

    rings::Patch patch;
    patch.structure = fclamp(structure, 0.0f, 0.9995f);
    patch.brightness = brightness;
    // High damping values in Rings produce the long, resonant end of the range.
    patch.damping = sustain ? 0.998f : fclamp(0.08f + 0.90f * damping, 0.0f, 0.9995f);
    patch.position = fclamp(position, 0.0f, 0.9995f);

    rings::PerformanceState ps;
    ps.strum = hw.GetGateTrig(GATE_REVERSE);
    ps.internal_exciter = false;  // Aurora audio input excites the resonator.
    ps.internal_strum = !ps.strum; // When no trigger gate, auto-detect transients.
    ps.internal_note = true;
    ps.tonic = 12.0f + pitch_ctl * 60.0f;
    ps.note = hw.GetWarpVoct();
    ps.fm = 0.0f;
    ps.chord = static_cast<int32_t>(fclamp(structure * static_cast<float>(rings::kNumChords),
                                           0.0f, static_cast<float>(rings::kNumChords - 1)));

    strummer.Process(input_mono, n, &ps);
    part.Process(ps, patch, input_mono, rings_out, rings_aux, n);

    const float dry_gain = cosf(mix * kPi * 0.5f);
    const float wet_gain = sinf(mix * kPi * 0.5f) * 1.22f;
    const float verb_amount = lush ? 0.95f : (0.04f + 0.30f * brightness * damping);
    lush_reverb.SetFeedback(lush ? 0.94f : (0.78f + 0.10f * damping));
    lush_reverb.SetLpFreq(lush ? 15800.0f : (13500.0f - 4500.0f * damping));

    for(size_t i = 0; i < n; ++i)
    {
        float wet_l = Saturate(rings_out[i] * 1.30f);
        float wet_r = Saturate(rings_aux[i] * 1.30f);

        float rv_l = 0.0f, rv_r = 0.0f;
        lush_reverb.Process(Bound(wet_l * verb_amount, 0.85f),
                            Bound(wet_r * verb_amount, 0.85f), &rv_l, &rv_r);
        wet_l = Saturate(wet_l + rv_l * verb_amount * (lush ? 1.18f : 0.55f));
        wet_r = Saturate(wet_r + rv_r * verb_amount * (lush ? 1.18f : 0.55f));

        out[0][i] = Saturate(in[0][i] * dry_gain + wet_l * wet_gain);
        out[1][i] = Saturate(in[1][i] * dry_gain + wet_r * wet_gain);
    }
    for(size_t i = n; i < size; ++i)
    {
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }
}

float Pulse(float value, float phase, float offset)
{
    const float v = Clamp01(value);
    const float speed = 0.55f + 4.7f * v;
    const float s = 0.5f + 0.5f * sinf(phase * speed + offset);
    return fclamp(0.05f + v * (0.30f + 0.70f * s), 0.0f, 1.0f);
}

void SetModelColor(float level)
{
    switch(ui_model)
    {
        default:
        case rings::RESONATOR_MODEL_MODAL:
            hw.SetLed(LED_REVERSE, 0.0f, level, 0.0f); break;       // green
        case rings::RESONATOR_MODEL_SYMPATHETIC_STRING:
            hw.SetLed(LED_REVERSE, 0.0f, level, level); break;      // cyan
        case rings::RESONATOR_MODEL_STRING:
            hw.SetLed(LED_REVERSE, level, level, 0.0f); break;      // yellow
        case rings::RESONATOR_MODEL_FM_VOICE:
            hw.SetLed(LED_REVERSE, level, 0.0f, level); break;      // magenta
        case rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED:
            hw.SetLed(LED_REVERSE, 0.0f, 0.0f, level); break;       // blue
        case rings::RESONATOR_MODEL_STRING_AND_REVERB:
            hw.SetLed(LED_REVERSE, level, 0.10f * level, 0.0f); break; // red/orange
    }
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
    const float p1 = Pulse(ui_pitch, led_phase, 0.0f);
    const float p2 = Pulse(ui_structure, led_phase, 0.7f);
    const float p3 = Pulse(ui_mix, led_phase, 1.4f);
    const float p4 = Pulse(ui_brightness, led_phase, 2.1f);
    const float p5 = Pulse(ui_damping, led_phase, 2.8f);
    const float p6 = Pulse(ui_position, led_phase, 3.5f);

    hw.SetLed(LED_1, 0.0f, 0.15f * p1, p1);   // Pitch blue
    hw.SetLed(LED_2, p2, 0.0f, 0.0f);         // Structure red
    hw.SetLed(LED_3, p3, p3, 0.0f);           // Mix yellow
    hw.SetLed(LED_4, 0.0f, p4, 0.0f);         // Brightness green
    hw.SetLed(LED_5, 0.0f, 0.70f * p5, p5);   // Damping cyan
    hw.SetLed(LED_6, p6, 0.0f, p6);           // Position magenta

    if(ui_lush && ui_sustain)
    {
        if(alt) hw.SetLed(LED_FREEZE, 1.0f, 1.0f, 1.0f);
        else hw.SetLed(LED_FREEZE, 0.0f, 0.0f, 1.0f);
    }
    else if(ui_lush) hw.SetLed(LED_FREEZE, 0.0f, 0.0f, 1.0f);
    else if(ui_sustain) hw.SetLed(LED_FREEZE, 1.0f, 1.0f, 1.0f);
    else hw.SetLed(LED_FREEZE, 0.03f, 0.03f, 0.03f);

    if(ui_swarm && alt)
        hw.SetLed(LED_REVERSE, 1.0f, 0.32f, 0.0f); // orange = Chord Swarm
    else
        SetModelColor(0.95f);

    if(ui_lush)
    {
        hw.SetLed(LED_BOT_1, 0.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_2, 0.0f, 0.0f, 1.0f);
        hw.SetLed(LED_BOT_3, 0.0f, 0.0f, 1.0f);
    }
    else if(ui_swarm)
    {
        hw.SetLed(LED_BOT_1, 1.0f, 0.32f, 0.0f);
        hw.SetLed(LED_BOT_2, 1.0f, 0.32f, 0.0f);
        hw.SetLed(LED_BOT_3, 1.0f, 0.32f, 0.0f);
    }
    else
    {
        hw.SetLed(LED_BOT_1, ui_model == 0 ? 0.7f : 0.06f, 0.0f, 0.0f);
        hw.SetLed(LED_BOT_2, ui_model == 1 || ui_model == 4 ? 0.7f : 0.06f,
                              ui_model == 2 ? 0.7f : 0.0f, 0.0f);
        hw.SetLed(LED_BOT_3, ui_model >= 3 ? 0.6f : 0.06f, 0.0f,
                              ui_model >= 3 ? 0.6f : 0.0f);
    }

    hw.WriteLeds();
}

} // namespace

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(kRingsBlock);
    sample_rate = hw.AudioSampleRate();

    stmlib::Random::Seed(0x52A17E5Du);
    part.Init(rings_reverb_buffer);
    part.set_model(rings::RESONATOR_MODEL_MODAL);
    part.set_polyphony(1);
    strummer.Init(0.01f, rings::kSampleRate / static_cast<float>(rings::kMaxBlockSize));

    lush_reverb.Init(sample_rate);
    lush_reverb.SetFeedback(0.82f);
    lush_reverb.SetLpFreq(11000.0f);

    hw.StartAudio(AudioCallback);
    while(1)
    {
        UpdateLeds();
        System::Delay(12);
    }
}
