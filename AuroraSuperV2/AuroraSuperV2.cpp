/* Aurora Super Synthesis V2 - POWER / CLEAN rebuild
   Seven standalone Aurora-native musical adaptations:
   0 2OPFM, 1 ROOM, 2 CHORUS, 3 TVCA, 4 EG, 5 SVFs, 6 SCANNER.
   Rewritten for stronger knob response, cleaner gain staging and useful SHIFT FX.
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
#define SUPER_ENGINE 5
#endif

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr size_t kDelayMax = 96000; // 2 seconds at 48kHz
constexpr float kBypass = 0.003f;

Hardware hw;
float sample_rate = 48000.0f;

DelayLine<float, kDelayMax> DSY_SDRAM_BSS d0;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d1;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d2;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d3;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d4;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d5;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d6;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS d7;
ReverbSc DSY_SDRAM_BSS reverb;

uint32_t rng_state = 0x5A17C3E9u;
float lfo_phase = 0.0f;
float lfo_phase2 = 0.37f;

// shared UI / mode state
volatile float ui_p[6] = {0.0f};
volatile bool ui_primary = false;
volatile bool ui_secondary = false;
volatile uint8_t ui_mode = 0;
volatile float ui_activity = 0.0f;

bool state_a = false;
bool state_b = false;
uint8_t mode = 0;
bool prev_gate_f = false;
bool prev_gate_r = false;

// FM state
float fm_car_phase = 0.0f;
float fm_mod_phase = 0.0f;
float fm_env = 0.0f;
float fm_mod_env = 0.0f;

// EG state
float eg_value = 0.0f;
uint8_t eg_stage = 0;
bool eg_retrigger = true;

// filter states
struct SvfState { float ic1 = 0.0f; float ic2 = 0.0f; };
SvfState svf1_l, svf1_r, svf2_l, svf2_r;
SvfState aux_l, aux_r;

// feedback filter states
float fb_lp[8] = {0.0f};
float fb_hp_lp[8] = {0.0f};
float env_follow = 0.0f;

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
    return static_cast<float>(rng_state & 0x00FFFFFFu) / 16777215.0f;
}

inline void IncPhase(float& p, float hz)
{
    p += hz / sample_rate;
    if(p >= 1.0f) p -= floorf(p);
    if(p < 0.0f) p += 1.0f;
}

struct SvfOut { float lp, bp, hp, notch; };
SvfOut ProcessSvf(SvfState& s, float input, float cutoff, float resonance)
{
    const float f = fclamp(cutoff, 8.0f, sample_rate * 0.43f);
    const float r = Clamp01(resonance);
    // TPT SVF; very low damping near full CW for obvious resonance.
    const float g = tanf(kPi * f / sample_rate);
    const float k = 2.0f - 1.985f * r;
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = input - s.ic2;
    const float v1 = a1 * s.ic1 + a2 * v3;
    const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
    s.ic1 = Bound(2.0f * v1 - s.ic1, 8.0f);
    s.ic2 = Bound(2.0f * v2 - s.ic2, 8.0f);
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

void TriggerFm()
{
    fm_env = 1.0f;
    fm_mod_env = 1.0f;
}

void TriggerEg()
{
    if(eg_retrigger || eg_stage == 0) eg_stage = 1;
}

void HandleButtons(size_t size)
{
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
        if(SUPER_ENGINE == 0)
        {
            if(modifier) state_a = !state_a; // drone
            else TriggerFm();
        }
        else if(SUPER_ENGINE == 1)
        {
            if(modifier) state_a = !state_a; // air bloom
            else state_b = !state_b; // freeze
        }
        else if(SUPER_ENGINE == 2)
        {
            if(modifier) state_a = !state_a; // air ensemble
            else state_b = !state_b; // hold
        }
        else if(SUPER_ENGINE == 3)
        {
            if(modifier) state_a = !state_a; // auto dynamics
            else state_b = !state_b; // manual enable
        }
        else if(SUPER_ENGINE == 4)
        {
            if(modifier) state_a = !state_a; // loop
            else TriggerEg();
        }
        else if(SUPER_ENGINE == 5)
        {
            if(modifier) state_a = !state_a; // animate
            else state_b = !state_b; // serial/parallel
        }
        else if(SUPER_ENGINE == 6)
        {
            if(modifier) state_a = !state_a; // auto scan
            else state_b = !state_b; // hold scan motion
        }
        if(modifier) shift_grace = 0.0f;
    }

    if(redge)
    {
        if(modifier)
        {
            // secondary performance effect for every engine
            if(SUPER_ENGINE == 4) eg_retrigger = !eg_retrigger;
            else ui_secondary = !ui_secondary;
            shift_grace = 0.0f;
        }
        else
        {
            mode = static_cast<uint8_t>((mode + 1) & 3u);
        }
    }

    const bool gf = hw.GetGateState(GATE_FREEZE);
    const bool gr = hw.GetGateState(GATE_REVERSE);
    const bool rise_f = gf && !prev_gate_f;
    const bool rise_r = gr && !prev_gate_r;
    prev_gate_f = gf;
    prev_gate_r = gr;

    if(rise_f)
    {
        if(SUPER_ENGINE == 0) TriggerFm();
        if(SUPER_ENGINE == 4) TriggerEg();
    }
    if(rise_r)
    {
        if(SUPER_ENGINE == 6) lfo_phase = 0.0f;
        if(SUPER_ENGINE == 0 && !state_a) TriggerFm();
    }
}

float OnePoleAlpha(float hz)
{
    return fclamp(1.0f - expf(-kTwoPi * fclamp(hz, 5.0f, sample_rate * 0.44f) / sample_rate), 0.0001f, 0.999f);
}

void Process2OPFM(float* l, float* r, size_t size, const float p[6])
{
    const float hz = 28.0f * powf(235.0f, p[0]);
    static const float ratios[8] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
    float ratio = 0.25f * powf(24.0f, p[1]);
    if(mode == 1) ratio = ratios[static_cast<int>(p[1] * 7.999f)];
    if(mode == 2) ratio = 1.0f / fmaxf(1.0f, floorf(1.0f + p[1] * 7.0f));
    if(mode == 3) ratio = 1.37f + p[1] * 6.71f;

    const float index = 0.05f + 18.0f * p[2] * p[2];
    const float decay = 0.008f * powf(500.0f, p[3]);
    const float mod_decay = decay * (0.20f + 0.72f * p[4]);
    const float drive = 1.0f + 7.0f * p[4] * p[4];
    const float detune = (p[5] - 0.5f) * 0.018f;
    const float env_coef = expf(-1.0f / (fmaxf(0.004f, decay) * sample_rate));
    const float mod_coef = expf(-1.0f / (fmaxf(0.003f, mod_decay) * sample_rate));
    const bool space = ui_secondary;
    const float echo_samp = (0.055f + 0.38f * p[5] * p[5]) * sample_rate;
    d0.SetDelay(echo_samp);
    d1.SetDelay(fclamp(echo_samp * 1.33f, 4.0f, static_cast<float>(kDelayMax - 4)));

    for(size_t i = 0; i < size; ++i)
    {
        if(state_a) { fm_env = 1.0f; fm_mod_env = 1.0f; }
        const float modsig = sinf(kTwoPi * fm_mod_phase) * index * fm_mod_env;
        const float carrier = sinf(kTwoPi * fm_car_phase + modsig);
        float tone = tanhf(carrier * drive) * fm_env;
        IncPhase(fm_mod_phase, hz * ratio);
        IncPhase(fm_car_phase, hz * (1.0f + detune));
        fm_env *= env_coef;
        fm_mod_env *= mod_coef;
        if(fm_env < 0.00002f) fm_env = 0.0f;

        float out_l = tone * (0.84f - detune * 5.0f);
        float out_r = tone * (0.84f + detune * 5.0f);
        if(space)
        {
            const float el = Safe(d0.Read());
            const float er = Safe(d1.Read());
            d0.Write(Bound(tone * 0.55f + er * (0.18f + 0.42f * p[2]), 1.2f));
            d1.Write(Bound(tone * 0.48f + el * (0.18f + 0.42f * p[2]), 1.2f));
            out_l += el * 0.55f;
            out_r += er * 0.55f;
        }
        l[i] = Sat(out_l);
        r[i] = Sat(out_r);
        ui_activity = fmaxf(ui_activity * 0.995f, fabsf(tone));
    }
    ui_primary = state_a;
    ui_mode = mode;
}

void ProcessRoom(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float mix = p[2];
    if(mix <= kBypass)
    {
        for(size_t i=0;i<size;++i){ l[i]=inl[i]; r[i]=inr[i]; }
        return;
    }
    static const float size_mul[4] = {0.72f, 0.92f, 1.18f, 1.42f};
    static const float fb_add[4]   = {-0.035f, 0.0f, 0.018f, 0.030f};
    const float predelay_ms = (2.0f + 58.0f * p[0] * p[0]) * size_mul[mode];
    const bool freeze = state_b != hw.GetGateState(GATE_FREEZE);
    const float fb = freeze ? 0.988f : fclamp(0.69f + 0.255f * p[1] + fb_add[mode], 0.62f, 0.965f);
    float lp_hz = 4300.0f * powf(4.55f, p[3]);
    if(state_a) lp_hz = fmaxf(lp_hz, 11800.0f + 6500.0f * p[3]);
    const float hp_hz = 22.0f * powf(65.0f, p[4]);
    const float mod = p[5];
    const float dry = cosf(mix * kPi * 0.5f);
    const float wet = sinf(mix * kPi * 0.5f) * 1.12f;
    const float lpa = OnePoleAlpha(lp_hz);
    const float hpa = OnePoleAlpha(hp_hz);
    reverb.SetFeedback(fb);
    reverb.SetLpFreq(lp_hz);

    static float wet_lp_l=0, wet_lp_r=0, wet_hp_l=0, wet_hp_r=0;
    const bool filter_delay = ui_secondary;
    for(size_t i=0;i<size;++i)
    {
        if(!freeze) IncPhase(lfo_phase, 0.035f + 0.42f * mod);
        const float wob = sinf(kTwoPi * lfo_phase) * (0.12f + 2.8f * mod * mod) * 0.001f * sample_rate;
        d0.SetDelay(fclamp(predelay_ms * 0.001f * sample_rate + wob, 3.0f, static_cast<float>(kDelayMax - 4)));
        d1.SetDelay(fclamp(predelay_ms * 1.17f * 0.001f * sample_rate - wob * 0.71f, 3.0f, static_cast<float>(kDelayMax - 4)));
        const float pl = Safe(d0.Read());
        const float pr = Safe(d1.Read());
        d0.Write(freeze ? pl * 0.9992f : Bound(inl[i], 1.3f));
        d1.Write(freeze ? pr * 0.9992f : Bound(inr[i], 1.3f));
        float rv_l=0, rv_r=0;
        reverb.Process(pl, pr, &rv_l, &rv_r);
        wet_lp_l += lpa * (rv_l - wet_lp_l);
        wet_lp_r += lpa * (rv_r - wet_lp_r);
        wet_hp_l += hpa * (wet_lp_l - wet_hp_l);
        wet_hp_r += hpa * (wet_lp_r - wet_hp_r);
        float fx_l = wet_lp_l - wet_hp_l;
        float fx_r = wet_lp_r - wet_hp_r;

        if(state_a)
        {
            // Air/Bloom: retain bright transient energy without harsh gain jump.
            const float air_l = rv_l - wet_lp_l;
            const float air_r = rv_r - wet_lp_r;
            fx_l += air_l * 0.38f;
            fx_r += air_r * 0.38f;
        }

        if(filter_delay)
        {
            const float delay_ms = 42.0f + 430.0f * p[0] * p[0];
            d2.SetDelay(delay_ms * 0.001f * sample_rate);
            d3.SetDelay(fclamp(delay_ms * (1.18f + 0.18f * p[5]) * 0.001f * sample_rate, 3.0f, static_cast<float>(kDelayMax-4)));
            const float dl = Safe(d2.Read());
            const float dr = Safe(d3.Read());
            const float cutoff = 90.0f * powf(180.0f, p[4]);
            const float resonance = 0.18f + 0.78f * p[5];
            const SvfOut fo_l = ProcessSvf(aux_l, dl + fx_l * 0.42f, cutoff, resonance);
            const SvfOut fo_r = ProcessSvf(aux_r, dr + fx_r * 0.42f, cutoff, resonance);
            float fl, fr;
            const int ft = static_cast<int>(p[3] * 3.999f);
            if(ft==0){fl=fo_l.lp;fr=fo_r.lp;} else if(ft==1){fl=fo_l.bp;fr=fo_r.bp;} else if(ft==2){fl=fo_l.hp;fr=fo_r.hp;} else {fl=fo_l.notch;fr=fo_r.notch;}
            const float dfb = 0.10f + 0.74f * p[1];
            d2.Write(Bound(fx_l * 0.40f + fr * dfb, 1.25f));
            d3.Write(Bound(fx_r * 0.40f + fl * dfb, 1.25f));
            fx_l = Sat(fx_l + fl * 0.78f);
            fx_r = Sat(fx_r + fr * 0.78f);
        }

        l[i] = Sat(inl[i] * dry + fx_l * wet);
        r[i] = Sat(inr[i] * dry + fx_r * wet);
        ui_activity = fmaxf(ui_activity * 0.996f, 0.5f * (fabsf(fx_l)+fabsf(fx_r)));
    }
    ui_primary = freeze || state_a;
    ui_mode = mode;
}

void ProcessChorus(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float mix = p[2];
    if(mix <= kBypass)
    {
        for(size_t i=0;i<size;++i){ l[i]=inl[i]; r[i]=inr[i]; }
        return;
    }
    static const float base_mul[4] = {1.0f, 0.36f, 1.75f, 0.72f}; // chorus/flange/doubler/tape
    static const float depth_mul[4] = {1.0f, 0.55f, 0.72f, 1.45f};
    static const float rate_mul[4] = {1.0f, 1.45f, 0.63f, 0.43f};
    const float base_ms = (0.75f + 24.0f * p[0] * p[0]) * base_mul[mode];
    const float rate = (0.035f + 5.8f * p[3] * p[3]) * rate_mul[mode];
    const float depth_ms = (0.08f + 8.8f * p[4] * p[4]) * depth_mul[mode];
    float feedback = (p[1] * 1.55f - 0.55f) * 0.72f;
    if(mode == 1) feedback *= 1.14f;
    const float width = p[5];
    const float dry = cosf(mix * kPi * 0.5f);
    const float wet = sinf(mix * kPi * 0.5f) * 1.16f;
    const bool hold = state_b != hw.GetGateState(GATE_FREEZE);
    const bool air = state_a;
    const bool echo = ui_secondary;
    const float lp_a = OnePoleAlpha(6500.0f + 9000.0f * p[4]);
    const float hp_a = OnePoleAlpha(55.0f + 420.0f * (1.0f - p[4]));
    DelayLine<float,kDelayMax>* lines[4] = {&d0,&d1,&d2,&d3};
    static const float ph[4] = {0.0f,0.25f,0.51f,0.76f};

    for(size_t i=0;i<size;++i)
    {
        if(!hold) IncPhase(lfo_phase, rate);
        float sum_l=0.0f, sum_r=0.0f;
        for(int v=0; v<4; ++v)
        {
            const float phase = lfo_phase + ph[v] + (v&1 ? width*0.08f : -width*0.05f);
            const float m = sinf(kTwoPi * phase);
            const float delay_ms = fmaxf(0.25f, base_ms * (0.84f + 0.11f*v) + depth_ms * m);
            lines[v]->SetDelay(fclamp(delay_ms * 0.001f * sample_rate, 2.0f, static_cast<float>(kDelayMax-4)));
            const float rd = Safe(lines[v]->Read());
            fb_lp[v] += lp_a * (rd - fb_lp[v]);
            fb_hp_lp[v] += hp_a * (fb_lp[v] - fb_hp_lp[v]);
            const float band = fb_lp[v] - fb_hp_lp[v];
            const float src = (v&1) ? inr[i] : inl[i];
            lines[v]->Write(hold ? rd * 0.9990f : Bound(src * 0.82f + band * feedback, 1.20f));
            const float pan = (static_cast<float>(v)/3.0f - 0.5f) * (0.52f + 0.48f*width);
            const float gl = sqrtf(fclamp(0.5f - pan,0.0f,1.0f));
            const float gr = sqrtf(fclamp(0.5f + pan,0.0f,1.0f));
            sum_l += rd * gl;
            sum_r += rd * gr;
        }
        sum_l *= 0.44f;
        sum_r *= 0.44f;
        if(air)
        {
            const float bright_l = sum_l - fb_lp[6];
            const float bright_r = sum_r - fb_lp[7];
            fb_lp[6] += OnePoleAlpha(7200.0f) * (sum_l - fb_lp[6]);
            fb_lp[7] += OnePoleAlpha(7200.0f) * (sum_r - fb_lp[7]);
            sum_l += bright_l * 0.28f + sum_r * 0.10f;
            sum_r += bright_r * 0.28f + sum_l * 0.08f;
        }
        if(echo)
        {
            const float echo_ms = 58.0f + 540.0f * p[0] * p[0];
            d4.SetDelay(echo_ms * 0.001f * sample_rate);
            d5.SetDelay(fclamp(echo_ms * (1.18f + 0.26f*width) * 0.001f * sample_rate,3.0f,static_cast<float>(kDelayMax-4)));
            const float el = Safe(d4.Read());
            const float er = Safe(d5.Read());
            const float ea = OnePoleAlpha(4200.0f + 9200.0f * p[3]);
            fb_lp[4] += ea * (er - fb_lp[4]);
            fb_lp[5] += ea * (el - fb_lp[5]);
            const float efb = 0.08f + 0.74f * p[1];
            d4.Write(Bound(sum_l * 0.48f + fb_lp[4] * efb, 1.20f));
            d5.Write(Bound(sum_r * 0.48f + fb_lp[5] * efb, 1.20f));
            sum_l = Sat(sum_l + el * 0.70f);
            sum_r = Sat(sum_r + er * 0.70f);
        }
        l[i] = Sat(inl[i]*dry + sum_l*wet);
        r[i] = Sat(inr[i]*dry + sum_r*wet);
        ui_activity = fmaxf(ui_activity*0.996f,0.5f*(fabsf(sum_l)+fabsf(sum_r)));
    }
    ui_primary = hold || air;
    ui_mode = mode;
}

void ProcessTVCA(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float lev_l = p[0] * 1.85f;
    const float lev_r = p[1] * 1.85f;
    const float init = p[2];
    const float cv_amt = p[3];
    const float drive = 1.0f + 17.0f * p[4] * p[4];
    const float cross = (p[5] - 0.5f) * 1.30f;
    const bool enabled = !state_b;
    const bool auto_dyn = state_a;
    const bool wide = ui_secondary;
    const bool gate = hw.GetGateState(GATE_FREEZE);

    for(size_t i=0;i<size;++i)
    {
        const float a = Bound(inl[i]*lev_l,2.0f);
        const float b = Bound(inr[i]*lev_r,2.0f);
        const float mag = 0.5f*(fabsf(a)+fabsf(b));
        const float atk = OnePoleAlpha(40.0f);
        const float rel = OnePoleAlpha(4.0f);
        const float coeff = mag > env_follow ? atk : rel;
        env_follow += coeff * (mag - env_follow);
        float cv = Clamp01(init + (gate ? cv_amt : 0.0f));
        if(auto_dyn) cv = Clamp01(cv + env_follow * cv_amt * 1.35f);
        if(!enabled) cv = 0.0f;
        float ml = a + b * fmaxf(0.0f,cross);
        float mr = b + a * fmaxf(0.0f,-cross);
        float ol, orr;
        if(mode == 0){ ol = tanhf(ml*(1.0f+0.32f*p[4])); orr=tanhf(mr*(1.0f+0.32f*p[4])); }
        else if(mode == 1){ ol = tanhf(ml*drive)*0.92f; orr=tanhf(mr*drive)*0.92f; }
        else if(mode == 2){ ol = Sat(ml*drive*1.55f); orr=Sat(mr*drive*1.55f); }
        else { ol = tanhf((ml + 0.23f*fabsf(ml))*drive); orr=tanhf((mr + 0.23f*fabsf(mr))*drive); }
        if(wide)
        {
            const float mid = 0.5f*(ol+orr);
            const float side = 0.5f*(ol-orr)*(1.0f+1.6f*p[5]);
            ol = mid + side;
            orr = mid - side;
        }
        l[i] = Bound(ol*cv,1.0f);
        r[i] = Bound(orr*cv,1.0f);
        ui_activity = fmaxf(ui_activity*0.995f,cv);
    }
    ui_primary = auto_dyn || !enabled;
    ui_mode = mode;
}

void ProcessEG(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float attack = 0.00035f * powf(12000.0f, p[0]);
    const float decay  = 0.0015f * powf(6500.0f, p[1]);
    const float depth = p[2];
    const float curve = p[3]*2.0f - 1.0f;
    const float level = 0.35f + 1.30f*p[4];
    const float time_scale = 0.45f + 3.5f*p[5];
    const bool loop = state_a;
    const bool pump = ui_secondary;

    for(size_t i=0;i<size;++i)
    {
        if(eg_stage == 1)
        {
            const float a = 1.0f - expf(-1.0f/(fmaxf(0.00015f,attack/time_scale)*sample_rate));
            eg_value += a*(1.0f-eg_value);
            if(eg_value >= 0.997f){ eg_value=1.0f; eg_stage=2; }
        }
        else if(eg_stage == 2)
        {
            const float a = 1.0f - expf(-1.0f/(fmaxf(0.00035f,decay/time_scale)*sample_rate));
            eg_value += a*(0.0f-eg_value);
            if(eg_value <= 0.0015f)
            {
                eg_value=0.0f;
                if(loop) eg_stage=1; else eg_stage=0;
            }
        }
        float shaped = eg_value;
        if(curve > 0.0f) shaped = powf(shaped,1.0f+curve*4.5f);
        else shaped = 1.0f - powf(1.0f-shaped,1.0f-curve*4.5f);
        const float env = pump ? (1.0f - depth*shaped) : ((1.0f-depth)+depth*shaped);
        l[i] = Sat(inl[i]*env*level);
        r[i] = Sat(inr[i]*env*level);
        ui_activity = shaped;
    }
    ui_primary = loop;
    ui_mode = eg_retrigger ? 1 : 0;
}

void ProcessSVFs(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float mix = p[2];
    if(mix <= kBypass && !ui_secondary)
    {
        for(size_t i=0;i<size;++i){ l[i]=inl[i]; r[i]=inr[i]; }
        return;
    }

    const bool animate = state_a;
    const bool serial = state_b;
    const bool filter_delay = ui_secondary;
    const float dry = cosf(mix*kPi*0.5f);
    const float wet = sinf(mix*kPi*0.5f)*1.28f;

    for(size_t i=0;i<size;++i)
    {
        float c1 = 24.0f * powf(760.0f,p[0]);
        float c2 = 24.0f * powf(760.0f,p[3]);
        if(animate)
        {
            IncPhase(lfo_phase,0.035f+3.5f*p[5]*p[5]);
            const float m1 = sinf(kTwoPi*lfo_phase);
            const float m2 = sinf(kTwoPi*(lfo_phase+0.31f));
            c1 *= powf(2.0f,m1*(0.35f+1.9f*p[1]));
            c2 *= powf(2.0f,m2*(0.35f+1.9f*p[4]));
        }
        c1 = fclamp(c1,18.0f,19000.0f);
        c2 = fclamp(c2,18.0f,19000.0f);
        const float excite1 = p[1] > 0.94f ? (Rand01()-0.5f)*0.00002f*(p[1]-0.94f)*16.0f : 0.0f;
        const float excite2 = p[4] > 0.94f ? (Rand01()-0.5f)*0.00002f*(p[4]-0.94f)*16.0f : 0.0f;
        const SvfOut a_l = ProcessSvf(svf1_l,inl[i]+excite1,c1,p[1]);
        const SvfOut a_r = ProcessSvf(svf1_r,inr[i]+excite1,c1,p[1]);
        float f1l = MorphSvf(a_l,p[5]);
        float f1r = MorphSvf(a_r,p[5]);
        const float src_l = serial ? f1l : inl[i];
        const float src_r = serial ? f1r : inr[i];
        const SvfOut b_l = ProcessSvf(svf2_l,src_l+excite2,c2,p[4]);
        const SvfOut b_r = ProcessSvf(svf2_r,src_r+excite2,c2,p[4]);
        float f2l = MorphSvf(b_l,1.0f-p[5]);
        float f2r = MorphSvf(b_r,1.0f-p[5]);
        float fx_l, fx_r;
        if(mode==0) { fx_l=0.58f*f1l+0.58f*f2l; fx_r=0.58f*f1r+0.58f*f2r; }
        else if(mode==1) { fx_l=serial?f2l:ProcessSvf(aux_l,f1l,c2,p[4]).lp; fx_r=serial?f2r:ProcessSvf(aux_r,f1r,c2,p[4]).lp; }
        else if(mode==2) { fx_l=f1l; fx_r=f2r; }
        else { fx_l=0.70f*a_l.bp+0.55f*b_l.bp; fx_r=0.55f*a_r.bp+0.70f*b_r.bp; }

        if(filter_delay)
        {
            const float delay_ms = 28.0f + 620.0f*p[0]*p[0];
            d0.SetDelay(delay_ms*0.001f*sample_rate);
            d1.SetDelay(fclamp(delay_ms*(1.12f+0.24f*p[5])*0.001f*sample_rate,3.0f,static_cast<float>(kDelayMax-4)));
            const float dl = Safe(d0.Read());
            const float dr = Safe(d1.Read());
            const float fc = 75.0f * powf(220.0f,p[3]);
            const SvfOut fd_l = ProcessSvf(aux_l,dl + fx_l*0.32f,fc,p[4]);
            const SvfOut fd_r = ProcessSvf(aux_r,dr + fx_r*0.32f,fc,p[4]);
            const float fl = MorphSvf(fd_l,p[5]);
            const float fr = MorphSvf(fd_r,p[5]);
            const float dfb = 0.05f + 0.82f*p[1];
            d0.Write(Bound(fx_l*0.40f + fr*dfb,1.20f));
            d1.Write(Bound(fx_r*0.40f + fl*dfb,1.20f));
            fx_l = Sat(fx_l + fl*0.82f);
            fx_r = Sat(fx_r + fr*0.82f);
        }

        l[i] = Sat(inl[i]*dry + fx_l*wet);
        r[i] = Sat(inr[i]*dry + fx_r*wet);
        ui_activity = fmaxf(ui_activity*0.995f,0.5f*(fabsf(fx_l)+fabsf(fx_r)));
    }
    ui_primary = animate || serial;
    ui_mode = mode;
}

float TriWeight(float x, float centre, float width)
{
    return fmaxf(0.0f,1.0f-fabsf(x-centre)/fmaxf(0.05f,width));
}

void ProcessScanner(const float* inl, const float* inr, float* l, float* r, size_t size, const float p[6])
{
    const float mix=p[2];
    const float dry=cosf(mix*kPi*0.5f);
    const float wet= sinf(mix*kPi*0.5f)*1.22f;
    const float overlap=0.16f+0.42f*p[1];
    const bool auto_scan=state_a;
    const bool hold=state_b;
    const bool orbit=ui_secondary;
    const float rate=0.025f+5.0f*p[3]*p[3];
    const float color=p[4];
    const float width=p[5];

    for(size_t i=0;i<size;++i)
    {
        if(auto_scan && !hold) IncPhase(lfo_phase,rate);
        float scan=p[0];
        if(auto_scan) scan=Clamp01(p[0]*0.42f + 0.29f + 0.29f*sinf(kTwoPi*lfo_phase));
        const float w0=TriWeight(scan,0.00f,overlap);
        const float w1=TriWeight(scan,0.333f,overlap);
        const float w2=TriWeight(scan,0.667f,overlap);
        const float w3=TriWeight(scan,1.00f,overlap);
        const float norm=fmaxf(0.001f,w0+w1+w2+w3);

        const float fc1=120.0f+1500.0f*color*color;
        const float fc2=450.0f+5200.0f*color*color;
        const float fc3=1600.0f+13000.0f*color*color;
        const SvfOut aL=ProcessSvf(svf1_l,inl[i],fc1,0.25f+0.55f*color);
        const SvfOut aR=ProcessSvf(svf1_r,inr[i],fc1,0.25f+0.55f*color);
        const SvfOut bL=ProcessSvf(svf2_l,inl[i],fc2,0.35f+0.55f*color);
        const SvfOut bR=ProcessSvf(svf2_r,inr[i],fc2,0.35f+0.55f*color);
        const SvfOut cL=ProcessSvf(aux_l,inl[i],fc3,0.18f+0.48f*color);
        const SvfOut cR=ProcessSvf(aux_r,inr[i],fc3,0.18f+0.48f*color);
        float A_l=aL.lp, A_r=aR.lp;
        float B_l=bL.bp, B_r=bR.bp;
        float C_l=cL.hp, C_r=cR.hp;
        float D_l=inl[i], D_r=inr[i];
        if(mode==1){ A_l=aL.bp;A_r=aR.bp;B_l=bL.notch;B_r=bR.notch;C_l=cL.bp;C_r=cR.bp; }
        if(mode==2){ A_l=Sat(aL.lp*1.8f);A_r=Sat(aR.lp*1.8f);B_l=Sat(bL.bp*2.2f);B_r=Sat(bR.bp*2.2f);C_l=Sat(cL.hp*1.7f);C_r=Sat(cR.hp*1.7f); }
        if(mode==3)
        {
            d0.SetDelay((12.0f+210.0f*color*color)*0.001f*sample_rate);
            d1.SetDelay((19.0f+310.0f*color*color)*0.001f*sample_rate);
            D_l=Safe(d0.Read());D_r=Safe(d1.Read());
            d0.Write(Bound(inl[i]+D_r*(0.10f+0.50f*width),1.2f));
            d1.Write(Bound(inr[i]+D_l*(0.10f+0.50f*width),1.2f));
        }
        float fx_l=(A_l*w0+B_l*w1+C_l*w2+D_l*w3)/norm;
        float fx_r=(A_r*w0+B_r*w1+C_r*w2+D_r*w3)/norm;
        const float mid=0.5f*(fx_l+fx_r);
        const float side=0.5f*(fx_l-fx_r)*(0.65f+1.75f*width);
        fx_l=mid+side; fx_r=mid-side;
        if(orbit)
        {
            d2.SetDelay((35.0f+280.0f*p[1])*0.001f*sample_rate);
            d3.SetDelay((55.0f+410.0f*p[1])*0.001f*sample_rate);
            const float ol=Safe(d2.Read()), orr=Safe(d3.Read());
            d2.Write(Bound(fx_l*0.42f+orr*(0.16f+0.56f*color),1.2f));
            d3.Write(Bound(fx_r*0.42f+ol*(0.16f+0.56f*color),1.2f));
            fx_l=Sat(fx_l+ol*0.68f); fx_r=Sat(fx_r+orr*0.68f);
        }
        l[i]=Sat(inl[i]*dry+fx_l*wet);
        r[i]=Sat(inr[i]*dry+fx_r*wet);
        ui_activity=fmaxf(ui_activity*0.995f,0.5f*(fabsf(fx_l)+fabsf(fx_r)));
    }
    ui_primary=auto_scan||hold;
    ui_mode=mode;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();
    HandleButtons(size);
    float p[6]; ReadParams(p);
    ui_activity *= 0.96f;

    if(SUPER_ENGINE==0) Process2OPFM(out[0],out[1],size,p);
    else if(SUPER_ENGINE==1) ProcessRoom(in[0],in[1],out[0],out[1],size,p);
    else if(SUPER_ENGINE==2) ProcessChorus(in[0],in[1],out[0],out[1],size,p);
    else if(SUPER_ENGINE==3) ProcessTVCA(in[0],in[1],out[0],out[1],size,p);
    else if(SUPER_ENGINE==4) ProcessEG(in[0],in[1],out[0],out[1],size,p);
    else if(SUPER_ENGINE==5) ProcessSVFs(in[0],in[1],out[0],out[1],size,p);
    else ProcessScanner(in[0],in[1],out[0],out[1],size,p);
}

void EngineColor(float& r,float& g,float& b)
{
    if(SUPER_ENGINE==0){r=1.0f;g=0.22f;b=0.02f;}
    else if(SUPER_ENGINE==1){r=0.10f;g=0.55f;b=1.0f;}
    else if(SUPER_ENGINE==2){r=0.75f;g=0.12f;b=1.0f;}
    else if(SUPER_ENGINE==3){r=1.0f;g=0.16f;b=0.18f;}
    else if(SUPER_ENGINE==4){r=0.15f;g=1.0f;b=0.20f;}
    else if(SUPER_ENGINE==5){r=0.0f;g=0.90f;b=0.80f;}
    else {r=1.0f;g=0.82f;b=0.05f;}
}

float Pulse(float v,float phase,float off)
{
    v=Clamp01(v);
    const float s=0.5f+0.5f*sinf(phase*(0.55f+5.8f*v)+off);
    return fclamp(0.04f+v*(0.24f+0.76f*s),0.0f,1.0f);
}

void UpdateLeds()
{
    static float phase=0.0f;
    phase+=0.075f;
    if(phase>1000.0f) phase=0.0f;
    hw.ClearLeds();
    float er,eg,eb; EngineColor(er,eg,eb);
    const Leds leds[6]={LED_1,LED_2,LED_3,LED_4,LED_5,LED_6};
    for(int i=0;i<6;++i)
    {
        const float p=Pulse(ui_p[i],phase,0.63f*i);
        hw.SetLed(leds[i],er*p,eg*p,eb*p);
    }
    // White primary, yellow secondary; mode encoded on bottom LEDs.
    if(ui_primary) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else hw.SetLed(LED_FREEZE,0.03f,0.03f,0.03f);
    if(ui_secondary) hw.SetLed(LED_REVERSE,1.0f,0.85f,0.0f);
    else hw.SetLed(LED_REVERSE,er*0.20f,eg*0.20f,eb*0.20f);
    const uint8_t code=static_cast<uint8_t>(ui_mode+1u);
    hw.SetLed(LED_BOT_1,(code&1)?er:0.0f,(code&1)?eg:0.0f,(code&1)?eb:0.0f);
    hw.SetLed(LED_BOT_2,(code&2)?er:0.0f,(code&2)?eg:0.0f,(code&2)?eb:0.0f);
    hw.SetLed(LED_BOT_3,(code&4)?er:0.0f,(code&4)?eg:0.0f,(code&4)?eb:0.0f);
    hw.WriteLeds();
}
}

int main(void)
{
    hw.Init();
    sample_rate=hw.AudioSampleRate();
    d0.Init();d1.Init();d2.Init();d3.Init();d4.Init();d5.Init();d6.Init();d7.Init();
    reverb.Init(sample_rate);
    // TVCA starts enabled; other engines default neutral.
    state_b = false;
    hw.SetAudioBlockSize(4);
    hw.StartAudio(AudioCallback);
    while(1)
    {
        UpdateLeds();
        System::Delay(10);
    }
}
