/* Aurora Cosmic Debris v2.1 POWER/CLEAN
   Independent Aurora adaptation based on the published WMD Cosmic Debris manual.

   NORMAL
     TIME       = TIME
     REFLECT    = FEEDBACK
     MIX        = MIX / SEND LEVEL when SEND mode is active
     ATMOSPHERE = SPRAY
     BLUR       = SCATTER
     WARP       = MOD amount

   Hold SHIFT and tap REVERSE to cycle secondary banks.
   BANK A amber: PRE DELAY / RATE / WET GAIN / SIDECHAIN / SLEW / SPREAD
   BANK B cyan:  CENTER / WIDTH / SHAPE / ANOMALY / WARP FACTOR / TAIL TONE
   BANK C white: GATE MODE / SEND / FREE-SYNC / BLAST / GLITCH / CLOCK RANGE

   FREEZE = momentary freeze
   REVERSE = BLUR -> RATIO -> WARP
   SHIFT+FREEZE = BLAST
   SHIFT+REVERSE = next SHIFT bank
   FREEZE+REVERSE = TAP
   SHIFT+both = KILL

   GATE_FREEZE = CLOCK
   GATE_REVERSE = configurable FREEZE / BLAST / GLITCH
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
constexpr int kLines = 16;
constexpr size_t kMaxDelaySamples = 120000;
constexpr size_t kMaxPreDelaySamples = 24000;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPickupThreshold = 0.035f;
constexpr float kShiftTouchThreshold = 0.008f;
constexpr float kCrossfadeSeconds = 0.026f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delay_line[kLines];
DelayLine<float, kMaxPreDelaySamples> DSY_SDRAM_BSS pre_delay[2];
float sample_rate = 48000.0f;

float delay_now[kLines] = {0};
float delay_target[kLines] = {0};
float xfade_from[kLines] = {0};
float xfade_to[kLines] = {0};
float xfade_pos[kLines] = {0};
float hp_state[kLines] = {0};
float lp_state[kLines] = {0};
float dc_state[kLines] = {0};
float tail_lp[kLines] = {0};
float lfo_phase[kLines] = {0};
float random_prev[kLines] = {0};
float random_next[kLines] = {0};
float pitch_phase[2] = {0.11f, 0.63f};
uint32_t rng_state = 0x6d2b79f5u;

float normal_param[6] = {0.35f,0.32f,0.55f,0.18f,0.05f,0.08f};
bool normal_pickup[6] = {true,true,true,true,true,true};
bool controls_ready = false;

float shift_param[3][6] = {
    {0.00f,0.30f,0.45f,0.00f,0.14f,0.50f},
    {0.55f,0.00f,0.00f,0.00f,0.50f,0.78f},
    {0.00f,0.00f,0.00f,0.72f,0.78f,0.50f}
};
float shift_capture[6] = {0};
bool shift_touched[6] = {false,false,false,false,false,false};
bool shift_was_down = false;
uint8_t shift_bank = 0;
uint8_t mode = 0;

bool prev_clock_gate = false;
bool prev_both = false;
bool prev_shift_both = false;
uint64_t sample_counter = 0;
uint64_t last_ext_clock_sample = 0;
uint64_t ext_clock_period = 24000;
bool ext_clock_seen = false;
uint64_t last_tap_sample = 0;
uint64_t tap_period = 24000;
bool tap_seen = false;
volatile bool reset_requested = false;
volatile uint32_t kill_countdown = 0;
float input_env = 0.0f;

volatile float ui_normal[6] = {0};
volatile float ui_shift[6] = {0};
volatile bool ui_pickup[6] = {true,true,true,true,true,true};
volatile bool ui_shift_down = false;
volatile bool ui_freeze = false;
volatile bool ui_blast = false;
volatile bool ui_glitch = false;
volatile uint8_t ui_mode = 0;
volatile uint8_t ui_shift_bank = 0;

inline bool Finite(float x){ return (x==x) && x>-100000.0f && x<100000.0f; }
inline float Safe(float x){ return Finite(x)?x:0.0f; }
inline float Clamp01(float x){ return fclamp(Safe(x),0.0f,1.0f); }
inline float Bound(float x,float lim){ return fclamp(Safe(x),-lim,lim); }
inline float CleanClip(float x){ x=Bound(x,4.0f); return x/(1.0f+0.12f*fabsf(x)); }
inline float HotClip(float x){ x=Bound(x,5.0f); return tanhf(x*0.85f); }
inline float SmoothStep(float x){ x=Clamp01(x); return x*x*(3.0f-2.0f*x); }

inline float FastSine(float phase)
{
    const float x=phase*kTwoPi-kPi;
    const float ax=fabsf(x);
    float y=(4.0f*x*(kPi-ax))/(kPi*kPi);
    y=0.225f*(y*fabsf(y)-y)+y;
    return fclamp(y,-1.0f,1.0f);
}
inline float Triangle(float phase){ return 1.0f-4.0f*fabsf(phase-0.5f); }

float NextRandom()
{
    rng_state ^= rng_state<<13;
    rng_state ^= rng_state>>17;
    rng_state ^= rng_state<<5;
    const float u=static_cast<float>(rng_state&0x00ffffffu)/8388607.5f;
    return u-1.0f;
}

float ReadKnob(int i)
{
    switch(i)
    {
        case 0:return Clamp01(hw.GetKnobValue(KNOB_TIME));
        case 1:return Clamp01(hw.GetKnobValue(KNOB_REFLECT));
        case 2:return Clamp01(hw.GetKnobValue(KNOB_MIX));
        case 3:return Clamp01(hw.GetKnobValue(KNOB_ATMOSPHERE));
        case 4:return Clamp01(hw.GetKnobValue(KNOB_BLUR));
        default:return Clamp01(hw.GetKnobValue(KNOB_WARP));
    }
}

void InitializeControls()
{
    for(int i=0;i<6;++i)
    {
        normal_param[i]=ReadKnob(i);
        normal_pickup[i]=true;
        shift_capture[i]=normal_param[i];
    }
    controls_ready=true;
}

void EnterShift()
{
    for(int i=0;i<6;++i)
    {
        shift_capture[i]=ReadKnob(i);
        shift_touched[i]=false;
    }
}

void UpdateControls(bool shift)
{
    if(!controls_ready) InitializeControls();
    if(shift && !shift_was_down) EnterShift();

    if(shift)
    {
        for(int i=0;i<6;++i)
        {
            const float raw=ReadKnob(i);
            if(fabsf(raw-shift_capture[i])>=kShiftTouchThreshold)
            {
                shift_touched[i]=true;
                shift_capture[i]=raw;
            }
            if(shift_touched[i]) shift_param[shift_bank][i]=raw;
        }
    }
    else
    {
        if(shift_was_down)
            for(int i=0;i<6;++i) normal_pickup[i]=false;

        for(int i=0;i<6;++i)
        {
            const float raw=ReadKnob(i);
            if(!normal_pickup[i] && fabsf(raw-normal_param[i])<=kPickupThreshold)
                normal_pickup[i]=true;
            if(normal_pickup[i]) normal_param[i]=raw;
        }
    }
    shift_was_down=shift;
}

void Hadamard16(float x[16])
{
    for(int step=1;step<16;step<<=1)
        for(int base=0;base<16;base+=(step<<1))
            for(int j=0;j<step;++j)
            {
                const float a=x[base+j];
                const float b=x[base+j+step];
                x[base+j]=a+b;
                x[base+j+step]=a-b;
            }
    for(int i=0;i<16;++i) x[i]*=0.25f;
}

float LfoValue(int i,float shape,float phase)
{
    const float s=FastSine(phase);
    const float t=Triangle(phase);
    const float r=random_prev[i]+(random_next[i]-random_prev[i])*SmoothStep(phase);
    if(shape<0.5f){ const float m=shape*2.0f; return s+(t-s)*m; }
    const float m=(shape-0.5f)*2.0f;
    return t+(r-t)*m;
}

inline float FilterLine(int i,float x,float hp_coeff,float lp_coeff,bool bypass)
{
    if(bypass) return x;
    hp_state[i]+=hp_coeff*(x-hp_state[i]);
    const float high=x-hp_state[i];
    lp_state[i]+=lp_coeff*(high-lp_state[i]);
    return Safe(lp_state[i]);
}

inline float FeedbackClean(int i,float x,float tone_coeff)
{
    const float dc_coeff=1.0f-expf(-kTwoPi*28.0f/sample_rate);
    dc_state[i]+=dc_coeff*(x-dc_state[i]);
    const float hp=x-dc_state[i];
    tail_lp[i]+=tone_coeff*(hp-tail_lp[i]);
    return Safe(tail_lp[i]);
}

float FeedbackAmount(float knob)
{
    knob=Clamp01(knob);
    if(knob<=0.5f)
    {
        const float n=knob*2.0f;
        return 0.985f*powf(n,1.12f);
    }
    const float x=(knob-0.5f)*2.0f;
    return 0.985f+0.115f*x*x;
}

float ClockDivision(float t,float range)
{
    static const float divs[16]={0.0625f,0.083333f,0.125f,0.166667f,0.25f,0.333333f,0.5f,0.666667f,0.75f,1.0f,1.333333f,1.5f,2.0f,3.0f,4.0f,6.0f};
    int idx=static_cast<int>(Clamp01(t)*15.999f);
    if(idx>15)idx=15;
    float r=1.0f;
    if(range<0.333f)r=0.5f; else if(range>0.666f)r=2.0f;
    return divs[idx]*r;
}

float BaseDelaySamples(float t,bool sync)
{
    if(sync)
    {
        uint64_t period=tap_seen?tap_period:24000;
        const uint64_t since=sample_counter-last_ext_clock_sample;
        if(ext_clock_seen && since<static_cast<uint64_t>(sample_rate*2.5f)) period=ext_clock_period;
        const float d=static_cast<float>(period)*ClockDivision(t,shift_param[2][5]);
        return fclamp(d,19.2f,static_cast<float>(kMaxDelaySamples-4096));
    }
    const float seconds=0.0004f*powf(3875.0f,Clamp01(t));
    return fclamp(seconds*sample_rate,19.2f,static_cast<float>(kMaxDelaySamples-4096));
}

void ComputeDelayTargets(float base,float spray,uint8_t m)
{
    static const float blur[16]={1.000f,0.880f,0.760f,0.660f,0.570f,0.490f,0.420f,0.360f,0.310f,0.265f,0.225f,0.190f,0.160f,0.135f,0.112f,0.092f};
    static const float ratio[16]={1.000f,2.000f,0.500f,1.500f,0.667f,1.333f,0.750f,1.250f,0.800f,1.600f,0.600f,1.800f,0.875f,1.125f,0.444f,2.250f};
    const float s=powf(Clamp01(spray),0.68f);
    for(int i=0;i<16;++i)
    {
        float r;
        if(m==1)
        {
            const float start=(i&1)?2.0f:1.0f;
            r=start+(ratio[i]-start)*s;
        }
        else r=1.0f+(blur[i]-1.0f)*s;
        delay_target[i]=fclamp(base*r,4.0f,static_cast<float>(kMaxDelaySamples-4096));
    }
}

float PitchRead(int i,float base_delay,float ratio)
{
    const float window=1800.0f;
    const float inc=fabsf(ratio-1.0f)/window;
    float p=pitch_phase[i];
    float p2=p+0.5f; if(p2>=1.0f)p2-=1.0f;
    const float o1=ratio>=1.0f?(1.0f-p)*window:p*window;
    const float o2=ratio>=1.0f?(1.0f-p2)*window:p2*window;
    const float d1=fclamp(base_delay+o1,4.0f,static_cast<float>(kMaxDelaySamples-4));
    const float d2=fclamp(base_delay+o2,4.0f,static_cast<float>(kMaxDelaySamples-4));
    const float w=1.0f-fabsf(2.0f*p-1.0f);
    const float a=Safe(delay_line[i].Read(d1));
    const float b=Safe(delay_line[i].Read(d2));
    p+=inc; if(p>=1.0f)p-=1.0f; pitch_phase[i]=p;
    return a*w+b*(1.0f-w);
}

void RegisterClock()
{
    const uint64_t now=sample_counter;
    if(last_ext_clock_sample!=0)
    {
        const uint64_t p=now-last_ext_clock_sample;
        if(p>static_cast<uint64_t>(sample_rate*0.015f) && p<static_cast<uint64_t>(sample_rate*8.0f))
        { ext_clock_period=p; ext_clock_seen=true; }
    }
    last_ext_clock_sample=now;
}

void RegisterTap()
{
    const uint64_t now=sample_counter;
    if(last_tap_sample!=0)
    {
        const uint64_t p=now-last_tap_sample;
        if(p>static_cast<uint64_t>(sample_rate*0.12f) && p<static_cast<uint64_t>(sample_rate*4.0f))
        { tap_period=p; tap_seen=true; }
    }
    last_tap_sample=now;
}

void HandleButtons(bool shift)
{
    const bool fp=hw.GetButton(SW_FREEZE).Pressed();
    const bool rp=hw.GetButton(SW_REVERSE).Pressed();
    const bool both=fp&&rp;
    if(shift)
    {
        if(both&&!prev_shift_both) kill_countdown=static_cast<uint32_t>(sample_rate*0.016f);
        else if(hw.GetButton(SW_REVERSE).RisingEdge()&&!fp)
        { shift_bank=static_cast<uint8_t>((shift_bank+1)%3); EnterShift(); }
    }
    else
    {
        if(both&&!prev_both) RegisterTap();
        else if(hw.GetButton(SW_REVERSE).RisingEdge()&&!fp) mode=static_cast<uint8_t>((mode+1)%3);
    }
    prev_both=both&&!shift;
    prev_shift_both=both&&shift;
}

void AudioCallback(AudioHandle::InputBuffer in,AudioHandle::OutputBuffer out,size_t size)
{
    hw.ProcessAllControls();
    sample_counter+=size;
    const bool shift=hw.GetButton(SW_SHIFT).Pressed();
    HandleButtons(shift);
    UpdateControls(shift);

    const bool clock_gate=hw.GetGateState(GATE_FREEZE);
    if(clock_gate&&!prev_clock_gate)RegisterClock();
    prev_clock_gate=clock_gate;

    int gate_mode=static_cast<int>(shift_param[2][0]*2.999f); if(gate_mode>2)gate_mode=2;
    const bool gate_high=hw.GetGateState(GATE_REVERSE);
    const bool button_freeze=hw.GetButton(SW_FREEZE).Pressed()&&!shift&&!hw.GetButton(SW_REVERSE).Pressed();
    const bool button_blast=hw.GetButton(SW_FREEZE).Pressed()&&shift&&!hw.GetButton(SW_REVERSE).Pressed();
    const bool freeze=button_freeze||(gate_high&&gate_mode==0);
    const bool blast=button_blast||(gate_high&&gate_mode==1);
    const bool glitch=gate_high&&gate_mode==2;
    const bool send_mode=shift_param[2][1]>=0.5f;
    const bool sync_mode=shift_param[2][2]>=0.5f;

    const float time=Clamp01(normal_param[0]-hw.GetCvValue(CV_TIME));
    const float fb_knob=Clamp01(normal_param[1]+hw.GetCvValue(CV_REFLECT));
    const float mix=Clamp01(normal_param[2]+hw.GetCvValue(CV_MIX));
    const float spray=Clamp01(normal_param[3]+hw.GetCvValue(CV_ATMOSPHERE));
    const float scatter=Clamp01(normal_param[4]+hw.GetCvValue(CV_BLUR));
    const float mod_amount=Clamp01(normal_param[5]+hw.GetWarpVoct()/60.0f);

    const float predelay_ms=shift_param[0][0]*260.0f;
    const float rate_hz=0.03f*powf(230.0f,shift_param[0][1]);
    const float wet_gain=0.80f+1.45f*shift_param[0][2];
    const float sidechain=shift_param[0][3];
    const float slew=shift_param[0][4];
    const float spread=shift_param[0][5];
    const float center=shift_param[1][0];
    const float width=shift_param[1][1];
    const float shape=shift_param[1][2];
    float anomaly=shift_param[1][3];
    if(glitch)anomaly=fmaxf(anomaly,0.58f+0.42f*shift_param[2][4]);
    int warp_semi=static_cast<int>(floorf(shift_param[1][4]*24.999f))-12;
    if(warp_semi<-12)warp_semi=-12; if(warp_semi>12)warp_semi=12;
    const float tail_cutoff=3500.0f*powf(5.7f,shift_param[1][5]);
    const float tail_coeff=1.0f-expf(-kTwoPi*fminf(tail_cutoff,20000.0f)/sample_rate);

    float feedback=FeedbackAmount(fb_knob);
    if(blast)feedback+=0.035f+0.075f*shift_param[2][3];
    if(freeze)feedback=1.0f;
    const float blast_gain=1.6f+3.4f*shift_param[2][3];

    const float base=BaseDelaySamples(time,sync_mode);
    ComputeDelayTargets(base,spray,mode);

    const bool filter_bypass=width<0.018f;
    const float center_hz=60.0f*powf(266.0f,center);
    const float octaves=7.2f*(1.0f-width)+0.24f;
    float hp_coeff[16],lp_coeff[16];
    for(int i=0;i<16;++i)
    {
        const float pos=(static_cast<float>(i)-7.5f)/7.5f;
        const float lc=center_hz*powf(2.0f,pos*0.16f*width);
        float hp=lc/powf(2.0f,0.5f*octaves);
        float lp=lc*powf(2.0f,0.5f*octaves);
        hp=fclamp(hp,18.0f,17000.0f); lp=fclamp(lp,hp+25.0f,20000.0f);
        hp_coeff[i]=1.0f-expf(-kTwoPi*hp/sample_rate);
        lp_coeff[i]=1.0f-expf(-kTwoPi*lp/sample_rate);
    }

    const bool xfade_mode=slew<0.075f;
    const float slew_coeff=0.00008f+0.020f*(1.0f-slew)*(1.0f-slew);
    const float xfade_inc=1.0f/(kCrossfadeSeconds*sample_rate);
    for(int i=0;i<16;++i)
    {
        if(xfade_mode&&fabsf(delay_target[i]-xfade_to[i])>5.0f)
        {
            const float current=xfade_pos[i]<1.0f?xfade_from[i]+(xfade_to[i]-xfade_from[i])*SmoothStep(xfade_pos[i]):xfade_to[i];
            xfade_from[i]=current; xfade_to[i]=delay_target[i]; xfade_pos[i]=0.0f;
        }
    }

    const float warp_ratio=powf(2.0f,static_cast<float>(warp_semi)/12.0f);
    int bits=16-static_cast<int>(anomaly*12.0f); if(bits<4)bits=4; if(bits>16)bits=16;
    const float levels=static_cast<float>(1u<<bits);
    const float pre_samples=fclamp(predelay_ms*0.001f*sample_rate,1.0f,static_cast<float>(kMaxPreDelaySamples-2));
    const float lfo_inc=rate_hz/sample_rate;
    const float mod_depth=mod_amount*mod_amount*(8.0f+base*0.012f);
    const float dry_gain=send_mode?1.0f:cosf(mix*kPi*0.5f);
    const float return_gain=send_mode?wet_gain:sinf(mix*kPi*0.5f)*wet_gain;
    float send_level=send_mode?mix:1.0f; if(blast&&send_mode)send_level=1.0f;
    const float spray_power=powf(spray,0.68f);

    for(size_t n=0;n<size;++n)
    {
        float in_l=Bound(in[0][n],3.2f),in_r=Bound(in[1][n],3.2f);
        if(fabsf(in_r)<0.00001f&&fabsf(in_l)>0.00005f)in_r=in_l;
        pre_delay[0].Write(in_l); pre_delay[1].Write(in_r);
        const float src_l=predelay_ms>0.05f?Safe(pre_delay[0].Read(pre_samples)):in_l;
        const float src_r=predelay_ms>0.05f?Safe(pre_delay[1].Read(pre_samples)):in_r;

        const float level=fminf(1.0f,fmaxf(fabsf(in_l),fabsf(in_r))*0.65f);
        if(level>input_env)input_env+=0.070f*(level-input_env); else input_env+=0.0010f*(level-input_env);
        const float duck=1.0f-sidechain*0.88f*Clamp01(input_env);

        float raw[16],filtered[16],fb_src[16],dense[16],cross[16];
        float read_base[16];
        for(int i=0;i<16;++i)
        {
            float phase=lfo_phase[i]+spread*(static_cast<float>(i)/16.0f); phase-=floorf(phase);
            const float lfo=LfoValue(i,shape,phase);
            const float offset=freeze?0.0f:lfo*mod_depth;
            float rb=delay_now[i];

            if(xfade_mode)
            {
                if(xfade_pos[i]<1.0f)
                {
                    const float p=SmoothStep(xfade_pos[i]);
                    const float da=fclamp(xfade_from[i]+offset,4.0f,static_cast<float>(kMaxDelaySamples-4));
                    const float db=fclamp(xfade_to[i]+offset,4.0f,static_cast<float>(kMaxDelaySamples-4));
                    const float a=Safe(delay_line[i].Read(da)),b=Safe(delay_line[i].Read(db));
                    raw[i]=a+(b-a)*p;
                    xfade_pos[i]+=xfade_inc;
                    if(xfade_pos[i]>=1.0f){xfade_pos[i]=1.0f;delay_now[i]=xfade_to[i];}
                    rb=xfade_to[i];
                }
                else
                {
                    delay_now[i]=xfade_to[i]; rb=delay_now[i];
                    raw[i]=Safe(delay_line[i].Read(fclamp(rb+offset,4.0f,static_cast<float>(kMaxDelaySamples-4))));
                }
            }
            else
            {
                delay_now[i]+=slew_coeff*(delay_target[i]-delay_now[i]); rb=delay_now[i];
                raw[i]=Safe(delay_line[i].Read(fclamp(rb+offset,4.0f,static_cast<float>(kMaxDelaySamples-4))));
            }
            read_base[i]=rb;
            filtered[i]=freeze?raw[i]:FilterLine(i,raw[i],hp_coeff[i],lp_coeff[i],filter_bypass);
            fb_src[i]=freeze?raw[i]:FeedbackClean(i,filtered[i],tail_coeff);
            lfo_phase[i]+=lfo_inc;
            if(lfo_phase[i]>=1.0f){lfo_phase[i]-=1.0f;random_prev[i]=random_next[i];random_next[i]=NextRandom();}
        }

        for(int i=0;i<16;++i)
        {
            dense[i]=fb_src[i];
            const int paired=i^1;
            const int rotated=(i+5+((i&1)?2:0))&15;
            cross[i]=0.62f*fb_src[rotated]+0.38f*fb_src[paired];
        }
        Hadamard16(dense);

        float shifted[2]={raw[0],raw[1]};
        if(mode==2&&!freeze&&warp_semi!=0)
        {
            shifted[0]=PitchRead(0,fclamp(read_base[0],4.0f,static_cast<float>(kMaxDelaySamples-2200)),warp_ratio);
            shifted[1]=PitchRead(1,fclamp(read_base[1],4.0f,static_cast<float>(kMaxDelaySamples-2200)),warp_ratio);
        }

        float wet_l=0.0f,wet_r=0.0f;
        for(int i=0;i<16;++i)
        {
            float audible=filtered[i];
            if(mode==2&&!freeze&&i<2&&warp_semi!=0)audible=0.62f*filtered[i]+0.58f*shifted[i];
            if((i&1)==0)wet_l+=audible*0.125f; else wet_r+=audible*0.125f;
        }

        const float blur_diff=(mode==0)?0.34f*spray_power:0.0f;
        float warp_all=1.0f,warp_focus=0.0f;
        if(mode==2&&spray<0.5f){warp_all=spray*2.0f;warp_focus=1.0f-warp_all;}

        for(int i=0;i<16;++i)
        {
            float routed;
            if(scatter<0.5f)
            {
                const float m=scatter*2.0f;
                routed=fb_src[i]+(cross[i]-fb_src[i])*m;
            }
            else
            {
                const float m=(scatter-0.5f)*2.0f;
                routed=cross[i]+(dense[i]-cross[i])*m;
            }
            if(blur_diff>0.0f)routed+=(dense[i]-routed)*blur_diff;

            if(!freeze&&anomaly>0.01f)
            {
                const int32_t q=static_cast<int32_t>(routed*levels);
                const float crushed=static_cast<float>(q)/levels;
                routed+=(crushed-routed)*anomaly;
                if(anomaly>0.50f)
                {
                    const float st_amt=(anomaly-0.50f)*2.0f;
                    const float sd=18.0f+(1.0f-st_amt)*720.0f+130.0f*fabsf(Triangle(lfo_phase[i]));
                    const float st=Safe(delay_line[i].Read(fclamp(sd,4.0f,static_cast<float>(kMaxDelaySamples-4))));
                    routed+=(st-routed)*(0.20f+0.68f*st_amt);
                }
            }

            if(mode==2&&!freeze&&i<2&&warp_semi!=0)routed=0.14f*routed+0.86f*shifted[i];
            const float ch=(i&1)==0?src_l:src_r;
            float iw=1.0f;
            if(mode==2){if(i<2)iw=warp_all+1.65f*warp_focus;else iw=warp_all;}
            float inject=freeze?0.0f:ch*send_level*iw*0.82f;
            if(blast)inject=HotClip(inject*blast_gain);
            const float wr=CleanClip(inject+routed*feedback);
            delay_line[i].Write(Bound(wr,3.0f));
        }

        float kg=1.0f;
        if(kill_countdown>0)
        {
            kg=static_cast<float>(kill_countdown)/fmaxf(1.0f,sample_rate*0.016f);
            --kill_countdown; if(kill_countdown==0)reset_requested=true;
        }
        wet_l*=duck*kg; wet_r*=duck*kg;
        out[0][n]=CleanClip(in_l*dry_gain+wet_l*return_gain);
        out[1][n]=CleanClip(in_r*dry_gain+wet_r*return_gain);
    }

    ui_shift_down=shift; ui_freeze=freeze; ui_blast=blast; ui_glitch=glitch; ui_mode=mode; ui_shift_bank=shift_bank;
    for(int i=0;i<6;++i){ui_normal[i]=normal_param[i];ui_shift[i]=shift_param[shift_bank][i];ui_pickup[i]=normal_pickup[i];}
}

void SetLedValue(int led,float v,float r,float g,float b)
{
    v=Clamp01(v); const float level=0.10f+0.90f*v;
    hw.SetLed(static_cast<Leds>(led),r*level,g*level,b*level);
}

void UpdateLeds()
{
    static uint32_t tick=0; ++tick; hw.ClearLeds();
    const bool flash=((tick/12u)&1u)!=0u;
    if(ui_shift_down)
    {
        float r=1.0f,g=0.55f,b=0.02f;
        if(ui_shift_bank==1){r=0.0f;g=0.90f;b=1.0f;}
        else if(ui_shift_bank==2){r=1.0f;g=1.0f;b=1.0f;}
        for(int i=0;i<6;++i)SetLedValue(LED_1+i,ui_shift[i],r,g,b);
    }
    else
    {
        const float c[6][3]={{0,0.25f,1},{1,0.04f,0},{1,0.70f,0},{0,1,0.22f},{0,0.82f,1},{1,0,0.90f}};
        for(int i=0;i<6;++i)
        {
            if(!ui_pickup[i]&&flash)hw.SetLed(static_cast<Leds>(LED_1+i),1,0,0);
            else SetLedValue(LED_1+i,ui_normal[i],c[i][0],c[i][1],c[i][2]);
        }
    }

    hw.SetLed(LED_BOT_1,ui_mode==0?0:0.02f,ui_mode==0?0.35f:0.02f,ui_mode==0?1:0.02f);
    hw.SetLed(LED_BOT_2,ui_mode==1?0.05f:0.02f,ui_mode==1?1:0.02f,ui_mode==1?0.15f:0.02f);
    hw.SetLed(LED_BOT_3,ui_mode==2?1:0.02f,ui_mode==2?0:0.02f,ui_mode==2?1:0.02f);
    if(ui_blast)hw.SetLed(LED_FREEZE,1,0.35f,0); else if(ui_freeze)hw.SetLed(LED_FREEZE,1,1,1); else hw.SetLed(LED_FREEZE,0.03f,0.03f,0.03f);
    if(ui_mode==0)hw.SetLed(LED_REVERSE,0,0.35f,1); else if(ui_mode==1)hw.SetLed(LED_REVERSE,0.05f,1,0.15f); else hw.SetLed(LED_REVERSE,1,0,1);
    if(ui_glitch&&flash)hw.SetLed(LED_REVERSE,1,1,1);
    hw.WriteLeds();
}

void ResetNetwork()
{
    for(int i=0;i<16;++i)
    {
        delay_line[i].Reset(); hp_state[i]=0; lp_state[i]=0; dc_state[i]=0; tail_lp[i]=0;
    }
    pre_delay[0].Reset(); pre_delay[1].Reset(); reset_requested=false;
}
}

int main(void)
{
    hw.Init();
    sample_rate=hw.AudioSampleRate();
    for(int i=0;i<16;++i)
    {
        delay_line[i].Init();
        const float initial=900.0f+static_cast<float>(i)*37.0f;
        delay_now[i]=initial; delay_target[i]=initial; xfade_from[i]=initial; xfade_to[i]=initial; xfade_pos[i]=1.0f;
        lfo_phase[i]=static_cast<float>(i)/16.0f; random_prev[i]=NextRandom(); random_next[i]=NextRandom();
    }
    pre_delay[0].Init(); pre_delay[1].Init();
    hw.StartAudio(AudioCallback);
    while(1)
    {
        if(reset_requested)ResetNetwork();
        UpdateLeds();
        System::Delay(10);
    }
}
