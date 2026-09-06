/* Aurora Cloudscape v1.5 - INTENSE FX + MULTIMODE FILTER DELAY + PULSE LEDs */
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
constexpr size_t kFilterDelaySamples = 96000;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kBypassThreshold = 0.004f;
constexpr float kMaxFeedback = 0.93f;
constexpr float kFreezeFeedback = 0.986f;

enum FilterMode { FILTER_LP = 0, FILTER_BP = 1, FILTER_HP = 2, FILTER_NOTCH = 3 };

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delays[kNumLines];
DelayLine<float, kFilterDelaySamples> DSY_SDRAM_BSS filter_delay_l;
DelayLine<float, kFilterDelaySamples> DSY_SDRAM_BSS filter_delay_r;
ReverbSc DSY_SDRAM_BSS reverb;

float sample_rate = 48000.0f;
float smooth_delay[kNumLines] = {2400.0f, 3600.0f, 4800.0f, 6000.0f};
float feedback_lp[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f};
float phase[kNumLines] = {0.0f, 1.7f, 3.4f, 5.1f};
float filter_delay_smooth_l = 7200.0f;
float filter_delay_smooth_r = 7600.0f;
float filter_lfo_phase = 0.0f;

bool freeze_latched = false;
bool rotate_latched = false;
bool lush_latched = false;
bool filter_latched = false;

volatile float ui_time = 0.0f;
volatile float ui_feedback = 0.0f;
volatile float ui_mix = 0.0f;
volatile float ui_atmosphere = 0.0f;
volatile float ui_blur = 0.0f;
volatile float ui_warp = 0.0f;
volatile int ui_lines = 1;
volatile int ui_filter_mode = FILTER_LP;
volatile bool ui_freeze = false;
volatile bool ui_rotate = false;
volatile bool ui_lush = false;
volatile bool ui_filter = false;

inline bool Finite(float x) { return std::isfinite(x); }
inline float Safe(float x) { return Finite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float Saturate(float x) { return tanhf(Bound(x, 6.0f)); }
inline float KnobCv(int knob, int cv) { return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv)); }

inline float Driven(float x, float amount)
{
    const float drive = 1.0f + amount * 2.6f;
    const float denom = fmaxf(0.25f, tanhf(drive));
    return tanhf(Bound(x, 1.4f) * drive) / denom;
}

struct BiquadCoeffs { float b0, b1, b2, a1, a2; };
struct BiquadState
{
    float z1 = 0.0f;
    float z2 = 0.0f;
    float Process(float x, const BiquadCoeffs& c)
    {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        if(!Finite(y) || !Finite(z1) || !Finite(z2))
        {
            z1 = 0.0f;
            z2 = 0.0f;
            return 0.0f;
        }
        return y;
    }
};

BiquadState filter_l;
BiquadState filter_r;

BiquadCoeffs MakeFilter(int mode, float cutoff, float q)
{
    cutoff = fclamp(cutoff, 70.0f, sample_rate * 0.42f);
    q = fclamp(q, 0.55f, 8.0f);
    const float w0 = kTwoPi * cutoff / sample_rate;
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float alpha = sw / (2.0f * q);
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cw;
    const float a2 = 1.0f - alpha;
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    switch(mode)
    {
        default:
        case FILTER_LP:    b0=(1.0f-cw)*0.5f; b1=1.0f-cw; b2=b0; break;
        case FILTER_BP:    b0=alpha; b1=0.0f; b2=-alpha; break;
        case FILTER_HP:    b0=(1.0f+cw)*0.5f; b1=-(1.0f+cw); b2=b0; break;
        case FILTER_NOTCH: b0=1.0f; b1=-2.0f*cw; b2=1.0f; break;
    }
    BiquadCoeffs c;
    c.b0=b0/a0; c.b1=b1/a0; c.b2=b2/a0; c.a1=a1/a0; c.a2=a2/a0;
    return c;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();
    const bool shift_pressed = hw.GetButton(SW_SHIFT).Pressed();

    if(hw.GetButton(SW_FREEZE).RisingEdge())
    {
        if(shift_pressed) lush_latched = !lush_latched;
        else freeze_latched = !freeze_latched;
    }
    if(hw.GetButton(SW_REVERSE).RisingEdge())
    {
        if(shift_pressed) filter_latched = !filter_latched;
        else rotate_latched = !rotate_latched;
    }

    const bool freeze = freeze_latched != hw.GetGateState(GATE_FREEZE);
    const bool rotate = rotate_latched != hw.GetGateState(GATE_REVERSE);
    const bool lush = lush_latched;
    const bool filter = filter_latched;

    const float time_ctl = KnobCv(KNOB_TIME, CV_TIME);
    const float reflect = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float mix = KnobCv(KNOB_MIX, CV_MIX);
    const float atmosphere = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float blur = KnobCv(KNOB_BLUR, CV_BLUR);
    const float warp = Clamp01(hw.GetKnobValue(KNOB_WARP) + hw.GetWarpVoct() / 60.0f);

    const int active_lines = 1 + static_cast<int>(atmosphere * 3.999f);
    int filter_mode = static_cast<int>(atmosphere * 4.0f);
    if(filter_mode > FILTER_NOTCH) filter_mode = FILTER_NOTCH;

    ui_time=time_ctl; ui_feedback=reflect; ui_mix=mix; ui_atmosphere=atmosphere;
    ui_blur=blur; ui_warp=warp; ui_lines=active_lines; ui_filter_mode=filter_mode;
    ui_freeze=freeze; ui_rotate=rotate; ui_lush=lush; ui_filter=filter;

    if(mix <= kBypassThreshold)
    {
        for(size_t i=0;i<size;++i) { out[0][i]=in[0][i]; out[1][i]=in[1][i]; }
        return;
    }

    const float base_seconds = fmap(time_ctl, 0.010f, 1.05f, Mapping::LOG);
    constexpr float ratio_max[kNumLines] = {1.0f,1.27f,1.58f,2.05f};
    float centre_delay[kNumLines];
    for(size_t j=0;j<kNumLines;++j)
    {
        const float density_spread = 0.55f + atmosphere * 0.75f;
        const float ratio = 1.0f + warp * density_spread * (ratio_max[j]-1.0f);
        centre_delay[j] = fclamp(base_seconds * ratio * sample_rate, 2.0f,
                                  static_cast<float>(kMaxDelaySamples-256));
    }

    const float atmo2 = atmosphere * atmosphere;
    const float mod_depth_samples = (0.004f + 0.031f*warp*warp + 0.010f*atmo2) * sample_rate;
    const float lfo_base_hz = 0.035f + warp*0.52f + atmo2*0.10f;
    float phase_inc[kNumLines];
    for(size_t j=0;j<kNumLines;++j)
        phase_inc[j] = kTwoPi * lfo_base_hz * (1.0f + 0.41f*static_cast<float>(j)) / sample_rate;

    const float feedback = freeze ? kFreezeFeedback : fmap(reflect,0.0f,kMaxFeedback,Mapping::LINEAR);
    const float feedback_alpha = 0.30f - 0.20f * blur;

    float reverb_amount = powf(blur,0.62f);
    if(filter && !lush) reverb_amount = 0.07f + 0.08f*blur;
    if(lush) reverb_amount = 0.96f;
    const float verb_feedback = lush ? 0.92f : (0.76f + reverb_amount*0.14f);
    const float verb_lpf = lush ? 15500.0f : (14500.0f - reverb_amount*7200.0f);
    reverb.SetFeedback(verb_feedback);
    reverb.SetLpFreq(verb_lpf);

    const float filter_delay_seconds = fmap(time_ctl,0.018f,0.78f,Mapping::LOG);
    const float filter_delay_target_l = fclamp(filter_delay_seconds*sample_rate,2.0f,
                                               static_cast<float>(kFilterDelaySamples-4));
    const float filter_delay_target_r = fclamp(filter_delay_target_l*(1.015f+0.055f*warp),2.0f,
                                               static_cast<float>(kFilterDelaySamples-4));
    const float filter_delay_feedback = 0.86f * reflect;

    filter_lfo_phase += kTwoPi * (0.035f + 0.70f*warp) * static_cast<float>(size) / sample_rate;
    if(filter_lfo_phase >= kTwoPi) filter_lfo_phase = fmodf(filter_lfo_phase,kTwoPi);
    const float cutoff_base = 85.0f * powf(18000.0f/85.0f, blur);
    const float cutoff_motion = powf(2.0f, sinf(filter_lfo_phase) * warp * 1.15f);
    const float filter_cutoff = fclamp(cutoff_base*cutoff_motion,70.0f,18000.0f);
    const float filter_q = 0.68f + 7.0f*warp*warp;
    const BiquadCoeffs filter_coeffs = MakeFilter(filter_mode,filter_cutoff,filter_q);

    const float dry_gain = cosf(mix*kPi*0.5f);
    const float wet_gain = sinf(mix*kPi*0.5f) * 1.28f;
    constexpr float tap_norm[4] = {1.25f,1.06f,0.94f,0.86f};
    const float tap_gain = tap_norm[active_lines-1];

    for(size_t i=0;i<size;++i)
    {
        const float input_l = Bound(in[0][i],2.0f);
        const float input_r = Bound(in[1][i],2.0f);
        float tap[kNumLines];

        for(size_t j=0;j<kNumLines;++j)
        {
            fonepole(smooth_delay[j],centre_delay[j],0.00072f);
            phase[j] += phase_inc[j];
            if(phase[j]>=kTwoPi) phase[j]-=kTwoPi;
            const float mod = sinf(phase[j]) * mod_depth_samples * (0.55f+0.13f*static_cast<float>(j));
            const float d = fclamp(smooth_delay[j]+mod,2.0f,static_cast<float>(kMaxDelaySamples-2));
            delays[j].SetDelay(d);
            tap[j] = Safe(delays[j].Read());
        }

        float cloud_l=0.0f, cloud_r=0.0f;
        for(int j=0;j<active_lines;++j)
        {
            float p = active_lines<=1 ? 0.5f : static_cast<float>(j)/static_cast<float>(active_lines-1);
            p = 0.5f + (p-0.5f)*(0.42f+0.58f*warp+0.18f*atmosphere);
            p = fclamp(p,0.0f,1.0f);
            cloud_l += tap[j]*sqrtf(fmaxf(0.0f,1.0f-p))*tap_gain;
            cloud_r += tap[j]*sqrtf(fmaxf(0.0f,p))*tap_gain;
        }

        const float pre_l=cloud_l, pre_r=cloud_r;
        const float atmo_cross=0.30f*atmo2;
        cloud_l = pre_l + pre_r*atmo_cross;
        cloud_r = pre_r - pre_l*atmo_cross*0.72f;

        for(size_t j=0;j<kNumLines;++j)
        {
            const bool enabled = static_cast<int>(j)<active_lines;
            const float p = active_lines<=1 ? 0.5f : static_cast<float>(j)/static_cast<float>(active_lines-1);
            const float pan_l=sqrtf(fmaxf(0.0f,1.0f-p));
            const float pan_r=sqrtf(fmaxf(0.0f,p));
            const float injection = Bound(input_l*pan_l+input_r*pan_r,1.6f)*0.90f;
            int src=static_cast<int>(j);
            if(rotate && enabled) src=(src+active_lines-1)%active_lines;
            const float feedback_source = enabled ? Bound(tap[src],1.35f) : 0.0f;
            feedback_lp[j] += feedback_alpha*(feedback_source-feedback_lp[j]);
            feedback_lp[j] = Bound(feedback_lp[j],1.35f);
            const float driven_feedback = enabled ? Driven(feedback_lp[j],reflect) : 0.0f;
            const float new_input = freeze ? 0.0f : injection;
            const float regen = enabled ? feedback*driven_feedback*0.94f : 0.0f;
            delays[j].Write(Bound(new_input+regen,1.42f));
        }

        const float blur_diff = 0.38f*blur*blur;
        const float diff_l = cloud_l + cloud_r*blur_diff;
        const float diff_r = cloud_r + cloud_l*blur_diff;
        cloud_l = Saturate(diff_l*(1.42f+0.18f*atmosphere));
        cloud_r = Saturate(diff_r*(1.42f+0.18f*atmosphere));

        float verb_l=0.0f, verb_r=0.0f;
        if(reverb_amount>0.001f)
        {
            const float cloud_feed = lush ? 0.86f : (0.69f+0.13f*blur);
            const float live_feed = lush ? 0.28f : (0.12f+0.12f*blur);
            const float vin_l=Bound((cloud_l*cloud_feed+input_l*live_feed)*reverb_amount,0.84f);
            const float vin_r=Bound((cloud_r*cloud_feed+input_r*live_feed)*reverb_amount,0.84f);
            reverb.Process(vin_l,vin_r,&verb_l,&verb_r);
            verb_l=Saturate(verb_l); verb_r=Saturate(verb_r);
        }

        const float reverb_return = lush ? 1.12f : (0.55f+0.52f*reverb_amount);
        float wet_l=Saturate(cloud_l+verb_l*reverb_amount*reverb_return);
        float wet_r=Saturate(cloud_r+verb_r*reverb_amount*reverb_return);

        if(filter)
        {
            fonepole(filter_delay_smooth_l,filter_delay_target_l,0.0010f);
            fonepole(filter_delay_smooth_r,filter_delay_target_r,0.0010f);
            filter_delay_l.SetDelay(filter_delay_smooth_l);
            filter_delay_r.SetDelay(filter_delay_smooth_r);
            const float fd_l=Safe(filter_delay_l.Read());
            const float fd_r=Safe(filter_delay_r.Read());
            const float fb_l=rotate?fd_r:fd_l;
            const float fb_r=rotate?fd_l:fd_r;
            const float fin_l=Bound(wet_l+fb_l*filter_delay_feedback,1.35f);
            const float fin_r=Bound(wet_r+fb_r*filter_delay_feedback,1.35f);
            const float filtered_l=Saturate(filter_l.Process(fin_l,filter_coeffs)*1.25f);
            const float filtered_r=Saturate(filter_r.Process(fin_r,filter_coeffs)*1.25f);
            filter_delay_l.Write(Bound(filtered_l,1.25f));
            filter_delay_r.Write(Bound(filtered_r,1.25f));
            wet_l=Saturate(filtered_l*0.94f+fd_l*0.88f);
            wet_r=Saturate(filtered_r*0.94f+fd_r*0.88f);
        }

        out[0][i]=Saturate(input_l*dry_gain+wet_l*wet_gain);
        out[1][i]=Saturate(input_r*dry_gain+wet_r*wet_gain);
    }
}

float Pulse(float value, float phase, float offset)
{
    const float v=Clamp01(value);
    const float speed=0.55f+4.5f*v;
    const float s=0.5f+0.5f*sinf(phase*speed+offset);
    return fclamp(0.06f + v*(0.30f+0.70f*s),0.0f,1.0f);
}

void UpdateLeds()
{
    static float led_phase=0.0f;
    static uint32_t blink_counter=0;
    led_phase += 0.075f;
    if(led_phase>kTwoPi*12.0f) led_phase=0.0f;
    ++blink_counter;
    const bool alt=((blink_counter/20u)&1u)!=0u;

    hw.ClearLeds();

    const float p_time=Pulse(ui_time,led_phase,0.0f);
    const float p_ref=Pulse(ui_feedback,led_phase,0.7f);
    const float p_mix=Pulse(ui_mix,led_phase,1.4f);
    const float p_atm=Pulse(ui_atmosphere,led_phase,2.1f);
    const float p_blur=Pulse(ui_blur,led_phase,2.8f);
    const float p_warp=Pulse(ui_warp,led_phase,3.5f);

    hw.SetLed(LED_1,0.0f,0.0f,p_time);
    hw.SetLed(LED_2,p_ref,0.0f,0.0f);
    hw.SetLed(LED_3,p_mix,p_mix,0.0f);

    if(ui_filter)
    {
        const float b=fmaxf(0.30f,p_atm);
        switch(ui_filter_mode)
        {
            default:
            case FILTER_LP: hw.SetLed(LED_4,0.0f,0.15f*b,b); break;
            case FILTER_BP: hw.SetLed(LED_4,0.0f,b,0.10f*b); break;
            case FILTER_HP: hw.SetLed(LED_4,b,0.05f*b,0.0f); break;
            case FILTER_NOTCH: hw.SetLed(LED_4,b,0.0f,b); break;
        }
        hw.SetLed(LED_5,p_blur,p_blur,0.0f);
        hw.SetLed(LED_6,p_warp,0.0f,p_warp);
    }
    else
    {
        hw.SetLed(LED_4,0.0f,p_atm,0.0f);
        hw.SetLed(LED_5,0.0f,p_blur*0.72f,p_blur);
        hw.SetLed(LED_6,p_warp,0.0f,p_warp);
    }

    if(ui_lush && ui_filter)
    {
        hw.SetLed(LED_BOT_1,0.0f,0.0f,1.0f);
        hw.SetLed(LED_BOT_2,1.0f,1.0f,0.0f);
        hw.SetLed(LED_BOT_3,0.0f,0.0f,1.0f);
    }
    else if(ui_lush)
    {
        hw.SetLed(LED_BOT_1,0.0f,0.0f,1.0f);
        hw.SetLed(LED_BOT_2,0.0f,0.0f,1.0f);
        hw.SetLed(LED_BOT_3,0.0f,0.0f,1.0f);
    }
    else if(ui_filter)
    {
        hw.SetLed(LED_BOT_1,1.0f,1.0f,0.0f);
        hw.SetLed(LED_BOT_2,1.0f,1.0f,0.0f);
        hw.SetLed(LED_BOT_3,1.0f,1.0f,0.0f);
    }
    else
    {
        for(int j=0;j<3;++j)
        {
            const float on=j<(ui_lines-1)?0.85f:0.06f;
            hw.SetLed(static_cast<Leds>(LED_BOT_1+j),0.0f,on,0.0f);
        }
    }

    if(ui_freeze && ui_lush)
    {
        if(alt) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
        else hw.SetLed(LED_FREEZE,0.0f,0.0f,1.0f);
    }
    else if(ui_lush) hw.SetLed(LED_FREEZE,0.0f,0.0f,1.0f);
    else if(ui_freeze) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else hw.SetLed(LED_FREEZE,0.03f,0.03f,0.03f);

    if(ui_rotate && ui_filter)
    {
        if(alt) hw.SetLed(LED_REVERSE,0.0f,1.0f,1.0f);
        else hw.SetLed(LED_REVERSE,1.0f,1.0f,0.0f);
    }
    else if(ui_filter) hw.SetLed(LED_REVERSE,1.0f,1.0f,0.0f);
    else if(ui_rotate) hw.SetLed(LED_REVERSE,0.0f,1.0f,1.0f);
    else hw.SetLed(LED_REVERSE,0.03f,0.03f,0.03f);

    hw.WriteLeds();
}

} // namespace

int main(void)
{
    hw.Init();
    sample_rate=hw.AudioSampleRate();
    for(size_t j=0;j<kNumLines;++j)
    {
        delays[j].Init();
        delays[j].SetDelay(smooth_delay[j]);
    }
    filter_delay_l.Init();
    filter_delay_r.Init();
    filter_delay_l.SetDelay(filter_delay_smooth_l);
    filter_delay_r.SetDelay(filter_delay_smooth_r);
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
