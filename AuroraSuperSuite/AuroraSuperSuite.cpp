/* Aurora SuperSuite v1.0
   Aurora-native musical adaptations inspired by Super Synthesis:
   2OPFM, ROOM, CHORUS, TVCA, EG, SVFs, SCANNER.
   Independent DSP implementation for Qu-Bit Aurora.
*/
#include "aurora.h"
#include "daisysp.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace daisy;
using namespace daisysp;
using namespace aurora;

#ifndef SUPER_ENGINE
#define SUPER_ENGINE 255
#endif
#ifndef SUPER_STANDALONE
#define SUPER_STANDALONE 0
#endif

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr size_t kDelayMax = 96000;
constexpr float kBypass = 0.003f;

enum Engine : uint8_t
{
    ENG_2OFM = 0,
    ENG_ROOM,
    ENG_CHORUS,
    ENG_TVCA,
    ENG_EG,
    ENG_SVFS,
    ENG_SCANNER,
    ENG_COUNT
};

Hardware hw;
float sample_rate = 48000.0f;

DelayLine<float, kDelayMax> DSY_SDRAM_BSS delay_l;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS delay_r;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS room_pre_l;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS room_pre_r;
ReverbSc DSY_SDRAM_BSS reverb;

uint32_t rng_state = 0xA3517E29u;
float chorus_phase = 0.0f;
float room_phase = 0.0f;
float fm_phase_c = 0.0f;
float fm_phase_m = 0.0f;
float fm_env = 0.0f;
float fm_mod_env = 0.0f;
bool fm_drone = false;
bool room_freeze = false;
bool chorus_hold = false;
bool tvca_open = true;
bool eg_loop = false;
bool eg_retrigger = true;
bool svf_series = false;
bool scanner_freeze = false;
bool scanner_reverse = false;

float eg_value = 0.0f;
uint8_t eg_stage = 0; // 0 idle, 1 attack, 2 decay
bool prev_freeze_gate = false;

struct SvfState
{
    float ic1 = 0.0f;
    float ic2 = 0.0f;
};
SvfState svf1_l, svf1_r, svf2_l, svf2_r;

float scanner_phase = 0.0f;
float scan_lp_l[4] = {0.0f};
float scan_lp_r[4] = {0.0f};

volatile uint8_t ui_engine = 0;
volatile float ui_p[6] = {0.0f};
volatile bool ui_primary = false;
volatile bool ui_secondary = false;
volatile uint8_t ui_option = 0;

inline float Safe(float x) { return std::isfinite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float Sat(float x) { return tanhf(Bound(x, 6.0f)); }
inline float KnobCv(int knob, int cv) { return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv)); }

float Rand01()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return static_cast<float>(rng_state & 0x00ffffffu) / 16777215.0f;
}

inline void IncPhase(float& p, float hz)
{
    p += hz / sample_rate;
    if(p >= 1.0f) p -= floorf(p);
    if(p < 0.0f) p += 1.0f;
}

inline float Tri(float x)
{
    x -= floorf(x);
    return 1.0f - fabsf(x * 2.0f - 1.0f);
}

struct SvfOut { float lp, bp, hp, notch; };
SvfOut ProcessSvf(SvfState& s, float input, float cutoff, float resonance)
{
    const float f = fclamp(cutoff, 8.0f, sample_rate * 0.43f);
    const float g = tanf(kPi * f / sample_rate);
    const float k = 2.0f - 1.92f * Clamp01(resonance);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = input - s.ic2;
    const float v1 = a1 * s.ic1 + a2 * v3;
    const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    SvfOut o;
    o.lp = v2;
    o.bp = v1;
    o.hp = input - k * v1 - v2;
    o.notch = o.lp + o.hp;
    return o;
}

float MorphSvf(const SvfOut& o, float morph)
{
    const float x = Clamp01(morph) * 3.0f;
    const int i = static_cast<int>(x);
    const float t = x - static_cast<float>(i);
    if(i <= 0) return o.lp + (o.bp - o.lp) * t;
    if(i == 1) return o.bp + (o.hp - o.bp) * t;
    if(i == 2) return o.hp + (o.notch - o.hp) * t;
    return o.notch;
}

void TriggerEg()
{
    if(eg_retrigger || eg_stage == 0)
        eg_stage = 1;
}

void TriggerFm()
{
    fm_env = 1.0f;
    fm_mod_env = 1.0f;
}

uint8_t CurrentEngine()
{
#if SUPER_STANDALONE
    return static_cast<uint8_t>(SUPER_ENGINE);
#else
    return ui_engine;
#endif
}

void HandleButtons(size_t size)
{
    static float shift_grace = 0.0f;
    const bool shift = hw.GetButton(SW_SHIFT).Pressed();
    if(shift) shift_grace = 0.16f;
    else
    {
        shift_grace -= static_cast<float>(size) / sample_rate;
        if(shift_grace < 0.0f) shift_grace = 0.0f;
    }
    const bool modifier = shift || shift_grace > 0.0f;
    const bool fedge = hw.GetButton(SW_FREEZE).RisingEdge();
    const bool redge = hw.GetButton(SW_REVERSE).RisingEdge();

#if SUPER_STANDALONE
    const uint8_t e = static_cast<uint8_t>(SUPER_ENGINE);
    if(fedge)
    {
        if(e == ENG_2OFM) { if(modifier) fm_drone = !fm_drone; else TriggerFm(); }
        else if(e == ENG_ROOM) room_freeze = !room_freeze;
        else if(e == ENG_CHORUS) chorus_hold = !chorus_hold;
        else if(e == ENG_TVCA) tvca_open = !tvca_open;
        else if(e == ENG_EG) { if(modifier) eg_loop = !eg_loop; else TriggerEg(); }
        else if(e == ENG_SCANNER) scanner_freeze = !scanner_freeze;
    }
    if(redge)
    {
        if(e == ENG_EG) eg_retrigger = !eg_retrigger;
        else if(e == ENG_SVFS) svf_series = !svf_series;
        else if(e == ENG_SCANNER) scanner_reverse = !scanner_reverse;
        else ui_option = static_cast<uint8_t>((ui_option + 1) & 3);
    }
#else
    if(redge)
    {
        if(modifier)
            ui_engine = static_cast<uint8_t>((ui_engine + ENG_COUNT - 1) % ENG_COUNT);
        else
            ui_engine = static_cast<uint8_t>((ui_engine + 1) % ENG_COUNT);
        shift_grace = 0.0f;
    }
    if(fedge)
    {
        const uint8_t e = ui_engine;
        if(e == ENG_2OFM) { if(modifier) fm_drone = !fm_drone; else TriggerFm(); }
        else if(e == ENG_ROOM) room_freeze = !room_freeze;
        else if(e == ENG_CHORUS) chorus_hold = !chorus_hold;
        else if(e == ENG_TVCA) tvca_open = !tvca_open;
        else if(e == ENG_EG) { if(modifier) eg_loop = !eg_loop; else TriggerEg(); }
        else if(e == ENG_SVFS) svf_series = !svf_series;
        else if(e == ENG_SCANNER) scanner_freeze = !scanner_freeze;
    }
#endif

    const bool gate = hw.GetGateState(GATE_FREEZE);
    const bool rise = gate && !prev_freeze_gate;
    prev_freeze_gate = gate;
    if(rise)
    {
        if(CurrentEngine() == ENG_2OFM) TriggerFm();
        if(CurrentEngine() == ENG_EG) TriggerEg();
    }
}

void ReadParams(float p[6])
{
    p[0] = KnobCv(KNOB_TIME, CV_TIME);
    p[1] = KnobCv(KNOB_REFLECT, CV_REFLECT);
    p[2] = KnobCv(KNOB_MIX, CV_MIX);
    p[3] = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    p[4] = KnobCv(KNOB_BLUR, CV_BLUR);
    p[5] = Clamp01(hw.GetKnobValue(KNOB_WARP) + hw.GetWarpVoct() / 60.0f);
    for(int i = 0; i < 6; ++i) ui_p[i] = p[i];
}

void Process2Ofm(float* l, float* r, size_t size, const float p[6])
{
    const float hz = 35.0f * powf(128.0f, p[0]);
    static const float ratios[8] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
    const float ratio_free = 0.25f * powf(24.0f, p[1]);
    const float ratio = (ui_option & 1) ? ratios[static_cast<int>(p[1] * 7.999f)] : ratio_free;
    const float index = p[2] * p[2] * 12.0f;
    const float decay = 0.015f * powf(180.0f, p[3]);
    const float mod_decay = decay * (0.35f + 0.45f * p[4]);
    const float drive = 1.0f + 4.0f * p[4];
    const float spread = (p[5] - 0.5f) * 0.025f;
    const float env_coef = expf(-1.0f / (fmaxf(0.005f, decay) * sample_rate));
    const float mod_coef = expf(-1.0f / (fmaxf(0.004f, mod_decay) * sample_rate));

    for(size_t i = 0; i < size; ++i)
    {
        if(fm_drone) { fm_env = 1.0f; fm_mod_env = 1.0f; }
        const float mod = sinf(kTwoPi * fm_phase_m) * index * fm_mod_env;
        const float c = sinf(kTwoPi * fm_phase_c + mod);
        const float tone = Sat(c * drive) * fm_env;
        l[i] = tone * (0.82f - spread * 5.0f);
        r[i] = tone * (0.82f + spread * 5.0f);
        IncPhase(fm_phase_m, hz * ratio);
        IncPhase(fm_phase_c, hz * (1.0f + spread));
        fm_env *= env_coef;
        fm_mod_env *= mod_coef;
        if(fm_env < 0.00002f) fm_env = 0.0f;
    }
    ui_primary = fm_drone;
    ui_secondary = (ui_option & 1) != 0;
}

void ProcessRoom(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float mix = p[2];
    const float size_s = 0.018f + p[0] * p[0] * 0.72f;
    const float fb = room_freeze ? 0.988f : (0.58f + 0.385f * p[1]);
    const float lp_hz = 1200.0f * powf(15.0f, p[3]);
    const float hp_hz = 25.0f * powf(120.0f, p[4]);
    const float mod = p[5];
    const float dry = cosf(mix * kPi * 0.5f);
    const float wet = sinf(mix * kPi * 0.5f) * 1.08f;
    static float lp_l = 0, lp_r = 0, hp_lp_l = 0, hp_lp_r = 0;
    const float lpa = 1.0f - expf(-kTwoPi * lp_hz / sample_rate);
    const float hpa = 1.0f - expf(-kTwoPi * hp_hz / sample_rate);
    reverb.SetFeedback(fb);
    reverb.SetLpFreq(lp_hz);

    for(size_t i = 0; i < size; ++i)
    {
        IncPhase(room_phase, 0.06f + 0.32f * mod);
        const float wob = sinf(kTwoPi * room_phase) * mod * 0.008f * sample_rate;
        room_pre_l.SetDelay(fclamp(size_s * sample_rate + wob, 4.0f, static_cast<float>(kDelayMax - 4)));
        room_pre_r.SetDelay(fclamp(size_s * sample_rate - wob * 0.73f, 4.0f, static_cast<float>(kDelayMax - 4)));
        const float dl = Safe(room_pre_l.Read());
        const float dr = Safe(room_pre_r.Read());
        room_pre_l.Write(room_freeze ? dl * 0.999f : inl[i]);
        room_pre_r.Write(room_freeze ? dr * 0.999f : inr[i]);
        float rv_l=0, rv_r=0;
        reverb.Process(dl, dr, &rv_l, &rv_r);
        lp_l += lpa * (rv_l - lp_l); lp_r += lpa * (rv_r - lp_r);
        hp_lp_l += hpa * (lp_l - hp_lp_l); hp_lp_r += hpa * (lp_r - hp_lp_r);
        const float fx_l = lp_l - hp_lp_l;
        const float fx_r = lp_r - hp_lp_r;
        l[i] = Sat(inl[i] * dry + fx_l * wet);
        r[i] = Sat(inr[i] * dry + fx_r * wet);
    }
    ui_primary = room_freeze;
    ui_secondary = false;
}

void ProcessChorus(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float delay_ms = 0.3f + 299.7f * p[0] * p[0];
    const float fb = (p[1] * 1.72f - 0.72f) * 0.82f;
    const float mix = p[2];
    const float rate = 0.03f + 6.0f * p[3] * p[3];
    const float depth = 0.2f + 18.0f * p[4] * p[4];
    const float spread = p[5];
    const float dry = cosf(mix * kPi * 0.5f);
    const float wet = sinf(mix * kPi * 0.5f);
    static float last_l = 0, last_r = 0;
    for(size_t i = 0; i < size; ++i)
    {
        if(!chorus_hold) IncPhase(chorus_phase, rate);
        const float m1 = sinf(kTwoPi * chorus_phase);
        const float m2 = sinf(kTwoPi * (chorus_phase + 0.25f + 0.25f * spread));
        delay_l.SetDelay(fclamp((delay_ms + depth * m1) * 0.001f * sample_rate, 2.0f, static_cast<float>(kDelayMax - 4)));
        delay_r.SetDelay(fclamp((delay_ms + depth * m2) * 0.001f * sample_rate, 2.0f, static_cast<float>(kDelayMax - 4)));
        const float dl = Safe(delay_l.Read());
        const float dr = Safe(delay_r.Read());
        const float wr_l = chorus_hold ? last_l * 0.9985f : (inl[i] + dl * fb);
        const float wr_r = chorus_hold ? last_r * 0.9985f : (inr[i] + dr * fb);
        delay_l.Write(Bound(wr_l, 1.5f)); delay_r.Write(Bound(wr_r, 1.5f));
        last_l = dl; last_r = dr;
        l[i] = Sat(inl[i] * dry + dl * wet * 1.05f);
        r[i] = Sat(inr[i] * dry + dr * wet * 1.05f);
    }
    ui_primary = chorus_hold;
    ui_secondary = fb < 0.0f;
}

void ProcessTvca(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float lev_l = p[0] * 1.5f;
    const float lev_r = p[1] * 1.5f;
    const float init = tvca_open ? p[2] : 0.0f;
    const float cv_amt = p[3];
    const float drive = 1.0f + 13.0f * p[4] * p[4];
    const float cross = p[5] * 0.8f;
    const float gate_cv = hw.GetGateState(GATE_FREEZE) ? 1.0f : 0.0f;
    const float vca = Clamp01(init + gate_cv * cv_amt);
    for(size_t i = 0; i < size; ++i)
    {
        const float a = inl[i] * lev_l;
        const float b = inr[i] * lev_r;
        const float ml = a + b * cross;
        const float mr = b + a * cross;
        l[i] = tanhf(ml * drive) * vca;
        r[i] = tanhf(mr * drive) * vca;
    }
    ui_primary = !tvca_open;
    ui_secondary = p[4] > 0.65f;
}

void ProcessEg(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float attack = 0.0005f * powf(4000.0f, p[0]);
    const float decay = 0.002f * powf(2500.0f, p[1]);
    const float depth = p[2];
    const float curve = p[3] * 2.0f - 1.0f;
    const float level = 0.2f + 1.35f * p[4];
    const float loop_scale = 0.35f + 2.0f * p[5];
    for(size_t i = 0; i < size; ++i)
    {
        if(eg_stage == 1)
        {
            const float a = 1.0f - expf(-1.0f / (fmaxf(0.0002f, attack / loop_scale) * sample_rate));
            eg_value += a * (1.0f - eg_value);
            if(eg_value >= 0.995f) { eg_value = 1.0f; eg_stage = 2; }
        }
        else if(eg_stage == 2)
        {
            const float a = 1.0f - expf(-1.0f / (fmaxf(0.0005f, decay / loop_scale) * sample_rate));
            eg_value += a * (0.0f - eg_value);
            if(eg_value <= 0.002f)
            {
                eg_value = 0.0f;
                if(eg_loop) eg_stage = 1; else eg_stage = 0;
            }
        }
        float shaped = eg_value;
        if(curve > 0.0f) shaped = powf(shaped, 1.0f + curve * 3.0f);
        else shaped = 1.0f - powf(1.0f - shaped, 1.0f - curve * 3.0f);
        const float gain = ((1.0f - depth) + depth * shaped) * level;
        l[i] = Sat(inl[i] * gain);
        r[i] = Sat(inr[i] * gain);
    }
    ui_primary = eg_loop;
    ui_secondary = eg_retrigger;
}

void ProcessSvfs(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float c1 = 18.0f * powf(1000.0f, p[0]);
    const float r1 = p[1];
    const float mix = p[2];
    const float c2 = 18.0f * powf(1000.0f, p[3]);
    const float r2 = p[4];
    const float morph = p[5];
    const float dry = cosf(mix * kPi * 0.5f);
    const float wet = sinf(mix * kPi * 0.5f) * 1.08f;
    for(size_t i = 0; i < size; ++i)
    {
        const SvfOut a_l = ProcessSvf(svf1_l, inl[i], c1, r1);
        const SvfOut a_r = ProcessSvf(svf1_r, inr[i], c1, r1);
        const float f1l = MorphSvf(a_l, morph);
        const float f1r = MorphSvf(a_r, morph);
        const float src_l = svf_series ? f1l : inl[i];
        const float src_r = svf_series ? f1r : inr[i];
        const SvfOut b_l = ProcessSvf(svf2_l, src_l, c2, r2);
        const SvfOut b_r = ProcessSvf(svf2_r, src_r, c2, r2);
        const float f2l = MorphSvf(b_l, 1.0f - morph);
        const float f2r = MorphSvf(b_r, 1.0f - morph);
        const float fx_l = svf_series ? f2l : 0.5f * (f1l + f2l);
        const float fx_r = svf_series ? f2r : 0.5f * (f1r + f2r);
        l[i] = Sat(inl[i] * dry + fx_l * wet);
        r[i] = Sat(inr[i] * dry + fx_r * wet);
    }
    ui_primary = svf_series;
    ui_secondary = (morph > 0.66f);
}

void ProcessScanner(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float manual = p[0];
    const float width = 0.12f + 0.55f * p[1];
    const float mix = p[2];
    const float rate = 0.01f + 3.0f * p[3] * p[3];
    const float depth = p[4];
    const float spread = p[5];
    const float dry = cosf(mix * kPi * 0.5f);
    const float wet = sinf(mix * kPi * 0.5f) * 1.1f;
    const float cut[4] = {350.0f, 950.0f, 2800.0f, 9000.0f};
    for(size_t i = 0; i < size; ++i)
    {
        if(!scanner_freeze) IncPhase(scanner_phase, scanner_reverse ? -rate : rate);
        float pos = manual + (sinf(kTwoPi * scanner_phase) * 0.5f + 0.5f - 0.5f) * depth;
        pos -= floorf(pos);
        float wl = 0.0f, wr = 0.0f, sum = 0.0f;
        for(int s = 0; s < 4; ++s)
        {
            const float center = static_cast<float>(s) / 3.0f;
            const float d = fabsf(pos - center);
            const float w = fclamp(1.0f - d / width, 0.0f, 1.0f);
            const float a = 1.0f - expf(-kTwoPi * cut[s] / sample_rate);
            scan_lp_l[s] += a * (inl[i] - scan_lp_l[s]);
            scan_lp_r[s] += a * (inr[i] - scan_lp_r[s]);
            float vl = scan_lp_l[s];
            float vr = scan_lp_r[s];
            if(s & 1) { vl = inl[i] - vl; vr = inr[i] - vr; }
            const float pan = fclamp((static_cast<float>(s) / 3.0f - 0.5f) * spread + 0.5f, 0.0f, 1.0f);
            wl += vl * w * sqrtf(1.0f - pan);
            wr += vr * w * sqrtf(pan);
            sum += w;
        }
        if(sum > 0.001f) { wl /= sum; wr /= sum; }
        l[i] = Sat(inl[i] * dry + wl * wet * 1.3f);
        r[i] = Sat(inr[i] * dry + wr * wet * 1.3f);
    }
    ui_primary = scanner_freeze;
    ui_secondary = scanner_reverse;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();
    HandleButtons(size);
    float p[6]; ReadParams(p);
    const uint8_t e = CurrentEngine();
    ui_engine = e;

    if(e == ENG_2OFM)
    {
        Process2Ofm(out[0], out[1], size, p);
        return;
    }
    if(e == ENG_ROOM) ProcessRoom(in[0], in[1], out[0], out[1], size, p);
    else if(e == ENG_CHORUS) ProcessChorus(in[0], in[1], out[0], out[1], size, p);
    else if(e == ENG_TVCA) ProcessTvca(in[0], in[1], out[0], out[1], size, p);
    else if(e == ENG_EG) ProcessEg(in[0], in[1], out[0], out[1], size, p);
    else if(e == ENG_SVFS) ProcessSvfs(in[0], in[1], out[0], out[1], size, p);
    else ProcessScanner(in[0], in[1], out[0], out[1], size, p);
}

void EngineColor(uint8_t e, float& r, float& g, float& b)
{
    static const float c[7][3] = {
        {1.0f,0.12f,0.70f}, {0.10f,0.45f,1.0f}, {0.0f,0.90f,0.85f},
        {1.0f,0.22f,0.05f}, {0.30f,1.0f,0.18f}, {1.0f,0.82f,0.0f}, {0.62f,0.20f,1.0f}
    };
    r=c[e][0]; g=c[e][1]; b=c[e][2];
}

float Pulse(float value, float phase, float offset)
{
    value = Clamp01(value);
    const float speed = 0.7f + 5.2f * value;
    const float wave = 0.5f + 0.5f * sinf(phase * speed + offset);
    return fclamp(0.06f + value * (0.30f + 0.70f * wave), 0.0f, 1.0f);
}

void UpdateLeds()
{
    static float phase = 0.0f;
    phase += 0.065f;
    if(phase > 1000.0f) phase = 0.0f;
    hw.ClearLeds();
    float er,eg,eb; EngineColor(ui_engine,er,eg,eb);
    for(int i=0;i<6;++i)
    {
        const float p = Pulse(ui_p[i], phase, static_cast<float>(i)*0.65f);
        const int led = LED_1 + i;
        hw.SetLed(led, er*p, eg*p, eb*p);
    }
    hw.SetLed(LED_FREEZE, ui_primary ? 1.0f : 0.08f, ui_primary ? 1.0f : 0.08f, ui_primary ? 1.0f : 0.08f);
#if SUPER_STANDALONE
    hw.SetLed(LED_REVERSE, ui_secondary ? 1.0f : er*0.55f, ui_secondary ? 1.0f : eg*0.55f, ui_secondary ? 1.0f : eb*0.55f);
#else
    hw.SetLed(LED_REVERSE, er, eg, eb);
#endif
    // bottom LEDs encode the 7 engines in binary-style presence.
    const uint8_t code = static_cast<uint8_t>(ui_engine + 1);
    hw.SetLed(LED_BOT_1, (code&1)?er:0.0f, (code&1)?eg:0.0f, (code&1)?eb:0.0f);
    hw.SetLed(LED_BOT_2, (code&2)?er:0.0f, (code&2)?eg:0.0f, (code&2)?eb:0.0f);
    hw.SetLed(LED_BOT_3, (code&4)?er:0.0f, (code&4)?eg:0.0f, (code&4)?eb:0.0f);
    hw.WriteLeds();
}
}

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();
    delay_l.Init(); delay_r.Init(); room_pre_l.Init(); room_pre_r.Init();
    reverb.Init(sample_rate);
#if SUPER_STANDALONE
    ui_engine = static_cast<uint8_t>(SUPER_ENGINE);
#else
    ui_engine = ENG_2OFM;
#endif
    hw.SetAudioBlockSize(4);
    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    while(1)
    {
        UpdateLeds();
        System::Delay(10);
    }
}
