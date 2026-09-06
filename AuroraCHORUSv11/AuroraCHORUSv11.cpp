/* Aurora CHORUS v1.1 - musical multi-delay chorus / ensemble
   Aurora-native DSP for Qu-Bit Aurora.

   Normal controls:
   TIME       = chorus base delay / spacing
   REFLECT    = chorus feedback
   MIX        = dry/wet (hard dry at minimum)
   ATMOSPHERE = modulation rate
   BLUR       = modulation depth
   WARP       = stereo width / voice spread

   Buttons:
   FREEZE            = latch HOLD (delay memory recirculates, input reduced)
   REVERSE           = cycle 4 chorus characters
   SHIFT + FREEZE    = latch AIR ENSEMBLE (extra voices + bright widening)
   SHIFT + REVERSE   = latch ECHO CHORUS (long stereo delay pair)

   ECHO CHORUS layer:
   TIME       = echo time
   REFLECT    = echo feedback
   ATMOSPHERE = echo tone / brightness
   BLUR       = echo modulation depth
   WARP       = stereo offset / ping-pong amount
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
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr size_t kShortMax = 12000;   // 250 ms @48k, plenty for chorus/doubler
constexpr size_t kLongMax  = 96000;   // 2 sec stereo echo
constexpr float kBypass = 0.0035f;

Hardware hw;
float sample_rate = 48000.0f;

// Four short modulated delay voices + two long echo lines.
DelayLine<float, kShortMax> DSY_SDRAM_BSS c1_l;
DelayLine<float, kShortMax> DSY_SDRAM_BSS c1_r;
DelayLine<float, kShortMax> DSY_SDRAM_BSS c2_l;
DelayLine<float, kShortMax> DSY_SDRAM_BSS c2_r;
DelayLine<float, kLongMax>  DSY_SDRAM_BSS echo_l;
DelayLine<float, kLongMax>  DSY_SDRAM_BSS echo_r;

bool hold_latched = false;
bool air_latched = false;
bool echo_latched = false;
uint8_t character = 0;
float phase_a = 0.0f;
float phase_b = 0.37f;
float echo_phase = 0.0f;
bool prev_reverse_gate = false;

volatile float ui_p[6] = {0.0f};
volatile bool ui_hold = false;
volatile bool ui_air = false;
volatile bool ui_echo = false;
volatile uint8_t ui_character = 0;

inline float Safe(float x){ return std::isfinite(x) ? x : 0.0f; }
inline float Clamp01(float x){ return fclamp(Safe(x),0.0f,1.0f); }
inline float Bound(float x,float lim){ return fclamp(Safe(x),-lim,lim); }
inline float Sat(float x){ return tanhf(Bound(x,5.0f)); }
inline float KnobCv(int knob,int cv){ return Clamp01(hw.GetKnobValue(knob)+hw.GetCvValue(cv)); }
inline void IncPhase(float& p,float hz){ p += hz/sample_rate; if(p>=1.0f) p-=floorf(p); }

void HandleButtons(size_t size)
{
    static float shift_grace=0.0f;
    const bool shift=hw.GetButton(SW_SHIFT).Pressed();
    if(shift) shift_grace=0.18f;
    else { shift_grace -= static_cast<float>(size)/sample_rate; if(shift_grace<0.0f) shift_grace=0.0f; }
    const bool modifier = shift || shift_grace>0.0f;
    const bool fedge = hw.GetButton(SW_FREEZE).RisingEdge();
    const bool redge = hw.GetButton(SW_REVERSE).RisingEdge();

    if(fedge)
    {
        if(modifier){ air_latched=!air_latched; shift_grace=0.0f; }
        else hold_latched=!hold_latched;
    }
    if(redge)
    {
        if(modifier){ echo_latched=!echo_latched; shift_grace=0.0f; }
        else character=static_cast<uint8_t>((character+1u)&3u);
    }

    ui_hold = hold_latched != hw.GetGateState(GATE_FREEZE);
    const bool rg=hw.GetGateState(GATE_REVERSE);
    if(rg && !prev_reverse_gate) character=static_cast<uint8_t>((character+1u)&3u);
    prev_reverse_gate=rg;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();
    HandleButtons(size);

    const float time = KnobCv(KNOB_TIME,CV_TIME);
    const float reflect = KnobCv(KNOB_REFLECT,CV_REFLECT);
    const float mix = KnobCv(KNOB_MIX,CV_MIX);
    const float rate_ctl = KnobCv(KNOB_ATMOSPHERE,CV_ATMOSPHERE);
    const float depth_ctl = KnobCv(KNOB_BLUR,CV_BLUR);
    const float warp = Clamp01(hw.GetKnobValue(KNOB_WARP)+hw.GetWarpVoct()/60.0f);
    ui_p[0]=time; ui_p[1]=reflect; ui_p[2]=mix; ui_p[3]=rate_ctl; ui_p[4]=depth_ctl; ui_p[5]=warp;
    ui_air=air_latched; ui_echo=echo_latched; ui_character=character;

    if(mix<=kBypass)
    {
        for(size_t i=0;i<size;++i){ out[0][i]=in[0][i]; out[1][i]=in[1][i]; }
        return;
    }

    // Four musical characters: chorus / flange / doubler / tape-chorus.
    float min_ms=4.0f,max_ms=24.0f,rate_scale=1.0f,depth_scale=1.0f,fb_sign=1.0f,tone_hz=14500.0f;
    switch(character&3u)
    {
        case 0: min_ms=5.0f; max_ms=28.0f; rate_scale=1.0f; depth_scale=1.0f; fb_sign=1.0f; tone_hz=15500.0f; break; // Chorus
        case 1: min_ms=0.7f; max_ms=8.0f;  rate_scale=0.65f; depth_scale=0.55f; fb_sign=-1.0f; tone_hz=12000.0f; break; // Flange
        case 2: min_ms=16.0f;max_ms=52.0f; rate_scale=0.30f; depth_scale=0.35f; fb_sign=0.45f; tone_hz=16500.0f; break; // Doubler
        default:min_ms=7.0f; max_ms=36.0f; rate_scale=0.48f; depth_scale=1.25f; fb_sign=0.72f; tone_hz=9800.0f; break; // Tape chorus
    }

    const float base_ms = min_ms + (max_ms-min_ms)*time*time;
    const float rate = (0.045f + 2.8f*rate_ctl*rate_ctl)*rate_scale;
    const float depth_ms = (0.18f + 7.5f*depth_ctl*depth_ctl)*depth_scale;
    const float feedback = fclamp(fb_sign*(0.02f + 0.54f*reflect*reflect),-0.58f,0.58f);
    const float voice_spread = 0.18f + 0.72f*warp;
    const float dry_gain=cosf(mix*kPi*0.5f);
    float wet_gain=sinf(mix*kPi*0.5f)*(1.02f+0.16f*warp);
    if(ui_hold) wet_gain*=0.88f;

    static float lp1_l=0.0f,lp1_r=0.0f,lp2_l=0.0f,lp2_r=0.0f;
    static float low_l=0.0f,low_r=0.0f;
    const float lpa=fclamp(1.0f-expf(-kTwoPi*tone_hz/sample_rate),0.001f,0.97f);
    const float hpa=fclamp(1.0f-expf(-kTwoPi*120.0f/sample_rate),0.0001f,0.25f); // low-cut keeps feedback clean

    // Echo chorus parameters (only active when SHIFT+REVERSE is latched).
    const float echo_ms = 55.0f + 395.0f*time*time;
    const float echo_fb = 0.08f + 0.68f*reflect*reflect;
    const float echo_tone = 3800.0f + 12500.0f*rate_ctl;
    const float echo_mod_ms = 0.25f + 5.0f*depth_ctl*depth_ctl;
    const float echo_ping = 0.20f + 0.72f*warp;
    static float echo_lp_l=0.0f,echo_lp_r=0.0f;
    const float echo_lpa=fclamp(1.0f-expf(-kTwoPi*echo_tone/sample_rate),0.001f,0.97f);

    for(size_t i=0;i<size;++i)
    {
        const float il=Bound(in[0][i],2.0f);
        const float ir=Bound(in[1][i],2.0f);

        IncPhase(phase_a,rate);
        IncPhase(phase_b,rate*(0.73f+0.31f*warp));
        const float a=sinf(kTwoPi*phase_a);
        const float b=sinf(kTwoPi*(phase_b+0.25f));
        const float c=sinf(kTwoPi*(phase_a+0.50f+0.17f*warp));
        const float d=sinf(kTwoPi*(phase_b+0.75f));

        // Two independent short delay voices per channel = four actual chorus delay lines.
        const float d1l=(base_ms + depth_ms*a) * 0.001f*sample_rate;
        const float d1r=(base_ms + depth_ms*b) * 0.001f*sample_rate;
        const float d2l=(base_ms*(1.28f+0.22f*voice_spread) + depth_ms*0.83f*c) * 0.001f*sample_rate;
        const float d2r=(base_ms*(1.46f+0.26f*voice_spread) + depth_ms*0.71f*d) * 0.001f*sample_rate;
        c1_l.SetDelay(fclamp(d1l,2.0f,static_cast<float>(kShortMax-4)));
        c1_r.SetDelay(fclamp(d1r,2.0f,static_cast<float>(kShortMax-4)));
        c2_l.SetDelay(fclamp(d2l,2.0f,static_cast<float>(kShortMax-4)));
        c2_r.SetDelay(fclamp(d2r,2.0f,static_cast<float>(kShortMax-4)));

        const float t1l=Safe(c1_l.Read()), t1r=Safe(c1_r.Read());
        const float t2l=Safe(c2_l.Read()), t2r=Safe(c2_r.Read());

        // Filter each voice before feedback; prevents muddy buildup.
        lp1_l += lpa*(t1l-lp1_l); lp1_r += lpa*(t1r-lp1_r);
        lp2_l += lpa*(t2l-lp2_l); lp2_r += lpa*(t2r-lp2_r);
        low_l += hpa*((lp1_l+lp2_l)*0.5f-low_l);
        low_r += hpa*((lp1_r+lp2_r)*0.5f-low_r);
        const float f1l=lp1_l-low_l*0.72f;
        const float f1r=lp1_r-low_r*0.72f;
        const float f2l=lp2_l-low_l*0.55f;
        const float f2r=lp2_r-low_r*0.55f;

        const float hold_in = ui_hold ? 0.0f : 1.0f;
        c1_l.Write(Bound(il*0.90f*hold_in + f1r*feedback*0.64f,1.25f));
        c1_r.Write(Bound(ir*0.90f*hold_in + f1l*feedback*0.64f,1.25f));
        c2_l.Write(Bound(il*0.80f*hold_in + f2r*feedback*0.52f,1.25f));
        c2_r.Write(Bound(ir*0.80f*hold_in + f2l*feedback*0.52f,1.25f));

        float wet_l = f1l*0.56f + f2l*0.44f;
        float wet_r = f1r*0.56f + f2r*0.44f;

        // AIR ENSEMBLE uses cross-voice sum/difference to create a more open six-voice impression.
        if(air_latched)
        {
            const float side=(wet_l-wet_r)*0.5f;
            const float mid=(wet_l+wet_r)*0.5f;
            wet_l=Sat(mid + side*(1.45f+0.55f*warp) + (f2r-f1r)*0.16f);
            wet_r=Sat(mid - side*(1.45f+0.55f*warp) + (f1l-f2l)*0.16f);
            wet_gain*=1.04f;
        }

        // ECHO CHORUS: separate long stereo delay pair layered behind the chorus.
        if(echo_latched)
        {
            IncPhase(echo_phase,0.05f+0.32f*depth_ctl);
            const float em=sinf(kTwoPi*echo_phase)*echo_mod_ms*0.001f*sample_rate;
            const float base_echo=echo_ms*0.001f*sample_rate;
            echo_l.SetDelay(fclamp(base_echo+em,8.0f,static_cast<float>(kLongMax-4)));
            echo_r.SetDelay(fclamp(base_echo*(1.0f+0.18f*warp)-em*0.63f,8.0f,static_cast<float>(kLongMax-4)));
            const float el=Safe(echo_l.Read()), er=Safe(echo_r.Read());
            echo_lp_l += echo_lpa*(el-echo_lp_l);
            echo_lp_r += echo_lpa*(er-echo_lp_r);
            const float ew_l=Sat(echo_lp_l), ew_r=Sat(echo_lp_r);
            echo_l.Write(Bound(wet_l*0.72f + ew_r*echo_fb*echo_ping,1.25f));
            echo_r.Write(Bound(wet_r*0.72f + ew_l*echo_fb*echo_ping,1.25f));
            const float echo_mix=0.26f+0.42f*reflect;
            wet_l=Sat(wet_l + ew_l*echo_mix);
            wet_r=Sat(wet_r + ew_r*echo_mix);
        }

        out[0][i]=Sat(il*dry_gain + wet_l*wet_gain);
        out[1][i]=Sat(ir*dry_gain + wet_r*wet_gain);
    }
}

float Pulse(float value,float phase,float offset)
{
    const float v=Clamp01(value);
    const float speed=0.65f+5.0f*v;
    const float s=0.5f+0.5f*sinf(phase*speed+offset);
    return fclamp(0.05f+v*(0.24f+0.76f*s),0.0f,1.0f);
}

void UpdateLeds()
{
    static float phase=0.0f; static uint32_t ticks=0;
    phase+=0.07f; if(phase>kTwoPi*12.0f) phase=0.0f; ++ticks;
    const bool alt=((ticks/20u)&1u)!=0u;
    hw.ClearLeds();
    const float p1=Pulse(ui_p[0],phase,0.0f),p2=Pulse(ui_p[1],phase,0.7f),p3=Pulse(ui_p[2],phase,1.4f);
    const float p4=Pulse(ui_p[3],phase,2.1f),p5=Pulse(ui_p[4],phase,2.8f),p6=Pulse(ui_p[5],phase,3.5f);
    hw.SetLed(LED_1,0.0f,0.25f*p1,p1);       // TIME blue
    hw.SetLed(LED_2,p2,0.0f,0.0f);           // REFLECT red
    hw.SetLed(LED_3,p3,p3*0.7f,0.0f);        // MIX amber
    hw.SetLed(LED_4,0.0f,p4,0.22f*p4);       // RATE green
    hw.SetLed(LED_5,0.0f,0.72f*p5,p5);       // DEPTH cyan
    hw.SetLed(LED_6,p6,0.0f,p6);              // WIDTH magenta

    if(ui_hold && ui_air)
    {
        if(alt) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
        else hw.SetLed(LED_FREEZE,0.0f,0.0f,1.0f);
    }
    else if(ui_air) hw.SetLed(LED_FREEZE,0.0f,0.0f,1.0f);
    else if(ui_hold) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else hw.SetLed(LED_FREEZE,0.03f,0.03f,0.03f);

    if(ui_echo) hw.SetLed(LED_REVERSE,1.0f,0.85f,0.0f); // yellow = Echo Chorus
    else
    {
        switch(ui_character&3u)
        {
            case 0: hw.SetLed(LED_REVERSE,0.0f,1.0f,0.35f); break; // chorus green
            case 1: hw.SetLed(LED_REVERSE,0.0f,0.75f,1.0f); break; // flange cyan
            case 2: hw.SetLed(LED_REVERSE,0.45f,0.0f,1.0f); break; // doubler violet
            default:hw.SetLed(LED_REVERSE,1.0f,0.28f,0.0f); break; // tape orange
        }
    }

    hw.SetLed(LED_BOT_1,ui_air?0.0f:0.0f,0.0f,ui_air?1.0f:0.0f);
    hw.SetLed(LED_BOT_2,ui_echo?1.0f:0.0f,ui_echo?0.85f:0.0f,0.0f);
    hw.SetLed(LED_BOT_3,ui_hold?1.0f:0.0f,ui_hold?1.0f:0.0f,ui_hold?1.0f:0.0f);
    hw.WriteLeds();
}
}

int main(void)
{
    hw.Init(); sample_rate=hw.AudioSampleRate();
    c1_l.Init(); c1_r.Init(); c2_l.Init(); c2_r.Init(); echo_l.Init(); echo_r.Init();
    hw.SetAudioBlockSize(4);
    hw.StartAudio(AudioCallback);
    while(1){ UpdateLeds(); System::Delay(10); }
}
