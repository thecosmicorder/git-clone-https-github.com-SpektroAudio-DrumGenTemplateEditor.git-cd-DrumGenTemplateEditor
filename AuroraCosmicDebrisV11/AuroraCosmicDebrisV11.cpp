/* Aurora Cosmic Debris v1.1 POWER
   16-line stereo delay network: 8 left + 8 right.
   Designed for a strong, unmistakable wet effect on Aurora hardware.

   Main:
   TIME       = base delay time
   REFLECT    = feedback
   MIX        = dry/wet
   ATMOSPHERE = SPRAY
   BLUR       = SCATTER
   WARP       = active mode amount

   FREEZE  = hold network
   REVERSE = cycle BLUR -> RATIO -> WARP

   SHIFT + knobs:
   TIME       = feedback filter centre
   REFLECT    = feedback filter resonance
   MIX        = stereo width
   ATMOSPHERE = modulation rate
   BLUR       = delay slew
   WARP       = Warp interval
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
constexpr int kLinesPerSide = 8;
constexpr int kTotalLines = 16;
constexpr size_t kMaxDelaySamples = 96000;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kBypass = 0.0035f;

Hardware hw;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS lines[kTotalLines];
float sample_rate = 48000.0f;

struct SvfState { float ic1 = 0.0f; float ic2 = 0.0f; };
struct SvfOut { float lp, bp, hp, notch; };
SvfState feedback_filter[kTotalLines];

float smooth_delay[kTotalLines] = {0.0f};
float mod_phase[kTotalLines] = {0.0f};
float warp_phase[kTotalLines] = {0.0f};
float feedback_state[kTotalLines] = {0.0f};

bool freeze_latched = false;
uint8_t mode = 0; // 0 Blur, 1 Ratio, 2 Warp
bool prev_reverse_gate = false;

// Secondary page: filter centre, Q, width, mod rate, slew, interval.
float secondary[6] = {0.58f, 0.35f, 0.80f, 0.30f, 0.35f, 0.58f};
float shift_capture[6] = {0.0f};
bool shift_was_down = false;

volatile float ui_time=0.0f, ui_feedback=0.0f, ui_mix=0.0f;
volatile float ui_spray=0.0f, ui_scatter=0.0f, ui_amount=0.0f;
volatile bool ui_freeze=false, ui_shift=false;
volatile uint8_t ui_mode=0;

inline float Safe(float x){ return std::isfinite(x) ? x : 0.0f; }
inline float Clamp01(float x){ return fclamp(Safe(x),0.0f,1.0f); }
inline float Bound(float x,float lim){ return fclamp(Safe(x),-lim,lim); }
inline float Sat(float x){ return tanhf(Bound(x,6.0f)); }

float ReadKnob(int i)
{
    switch(i)
    {
        case 0: return Clamp01(hw.GetKnobValue(KNOB_TIME));
        case 1: return Clamp01(hw.GetKnobValue(KNOB_REFLECT));
        case 2: return Clamp01(hw.GetKnobValue(KNOB_MIX));
        case 3: return Clamp01(hw.GetKnobValue(KNOB_ATMOSPHERE));
        case 4: return Clamp01(hw.GetKnobValue(KNOB_BLUR));
        default:return Clamp01(hw.GetKnobValue(KNOB_WARP));
    }
}

SvfOut ProcessSvf(SvfState& s,float input,float cutoff,float resonance)
{
    cutoff=fclamp(cutoff,55.0f,sample_rate*0.40f);
    resonance=Clamp01(resonance);
    const float g=tanf(kPi*cutoff/sample_rate);
    const float k=2.0f-1.86f*resonance;
    const float a1=1.0f/(1.0f+g*(g+k));
    const float a2=g*a1;
    const float a3=g*a2;
    const float v3=input-s.ic2;
    const float v1=a1*s.ic1+a2*v3;
    const float v2=s.ic2+a2*s.ic1+a3*v3;
    s.ic1=Bound(2.0f*v1-s.ic1,4.0f);
    s.ic2=Bound(2.0f*v2-s.ic2,4.0f);
    SvfOut o{v2,v1,input-k*v1-v2,0.0f};
    o.notch=o.lp+o.hp;
    return o;
}

float PitchRead(int idx,float base_delay,float semis,float amount)
{
    if(amount<0.02f || fabsf(semis)<0.1f)
        return Safe(lines[idx].ReadHermite(base_delay));

    const float ratio=powf(2.0f,(semis*amount)/12.0f);
    const float window=720.0f+2500.0f*amount;
    const float rate=fabsf(ratio-1.0f)/fmaxf(window,64.0f);
    warp_phase[idx]+=rate;
    if(warp_phase[idx]>=1.0f) warp_phase[idx]-=floorf(warp_phase[idx]);

    const float p1=warp_phase[idx];
    float p2=p1+0.5f; if(p2>=1.0f) p2-=1.0f;
    const float o1=ratio>=1.0f ? (1.0f-p1)*window : p1*window;
    const float o2=ratio>=1.0f ? (1.0f-p2)*window : p2*window;
    const float d1=fclamp(base_delay+o1,4.0f,static_cast<float>(kMaxDelaySamples-8));
    const float d2=fclamp(base_delay+o2,4.0f,static_cast<float>(kMaxDelaySamples-8));
    const float w1=0.5f-0.5f*cosf(kTwoPi*p1);
    return Safe(lines[idx].ReadHermite(d1))*w1 + Safe(lines[idx].ReadHermite(d2))*(1.0f-w1);
}

void HandleShift(bool shift)
{
    if(shift && !shift_was_down)
        for(int i=0;i<6;++i) shift_capture[i]=ReadKnob(i);

    if(shift)
    {
        for(int i=0;i<6;++i)
        {
            const float now=ReadKnob(i);
            const float delta=now-shift_capture[i];
            if(fabsf(delta)>0.0015f)
            {
                secondary[i]=Clamp01(secondary[i]+delta);
                shift_capture[i]=now;
            }
        }
    }
    shift_was_down=shift;
}

void AudioCallback(AudioHandle::InputBuffer in,AudioHandle::OutputBuffer out,size_t size)
{
    hw.ProcessAllControls();
    const bool shift=hw.GetButton(SW_SHIFT).Pressed();
    HandleShift(shift);

    if(hw.GetButton(SW_FREEZE).RisingEdge() && !shift)
        freeze_latched=!freeze_latched;
    if(hw.GetButton(SW_REVERSE).RisingEdge() && !shift)
        mode=static_cast<uint8_t>((mode+1)%3);

    const bool rg=hw.GetGateState(GATE_REVERSE);
    if(rg && !prev_reverse_gate)
        mode=static_cast<uint8_t>((mode+1)%3);
    prev_reverse_gate=rg;

    static float p[6]={0.34f,0.45f,0.58f,0.32f,0.20f,0.45f};
    if(!shift)
        for(int i=0;i<6;++i) p[i]=ReadKnob(i);

    const float time_knob=Clamp01(p[0]+hw.GetCvValue(CV_TIME));
    const float reflect=Clamp01(p[1]+hw.GetCvValue(CV_REFLECT));
    const float mix=Clamp01(p[2]+hw.GetCvValue(CV_MIX));
    const float spray=Clamp01(p[3]+hw.GetCvValue(CV_ATMOSPHERE));
    const float scatter=Clamp01(p[4]+hw.GetCvValue(CV_BLUR));
    const float amount=Clamp01(p[5]+hw.GetWarpVoct()/60.0f);
    const bool freeze=freeze_latched != hw.GetGateState(GATE_FREEZE);

    ui_time=time_knob; ui_feedback=reflect; ui_mix=mix;
    ui_spray=spray; ui_scatter=scatter; ui_amount=amount;
    ui_freeze=freeze; ui_shift=shift; ui_mode=mode;

    if(mix<=kBypass)
    {
        for(size_t i=0;i<size;++i){ out[0][i]=in[0][i]; out[1][i]=in[1][i]; }
        return;
    }

    // Always-audible delay range: 12 ms to 1.25 s.
    const float base_seconds=0.012f*powf(104.166667f,time_knob);
    const float base_samples=fclamp(base_seconds*sample_rate,64.0f,
                                    static_cast<float>(kMaxDelaySamples-4096));

    // Strong feedback, still below unity. Freeze nearly infinite.
    float feedback=0.10f+0.86f*powf(reflect,1.35f);
    if(freeze) feedback=0.989f;

    const float filter_center=180.0f*powf(72.0f,secondary[0]);
    const float filter_res=0.15f+0.80f*secondary[1];
    const float width=0.18f+0.82f*secondary[2];
    const float mod_rate=0.02f+1.10f*secondary[3]*secondary[3];
    const float slew_coeff=0.0007f+(1.0f-secondary[4])*(1.0f-secondary[4])*0.035f;
    static const float intervals[6]={3.0f,5.0f,7.0f,12.0f,19.0f,24.0f};
    int ii=static_cast<int>(secondary[5]*5.999f); if(ii>5) ii=5;
    const float warp_interval=intervals[ii];

    // Equal-power dry/wet, but wet intentionally hotter for a powerful character.
    const float dry_gain=cosf(mix*kPi*0.5f);
    const float wet_gain=sinf(mix*kPi*0.5f)*1.55f;

    static const float narrow[8]={1.00f,1.04f,1.09f,1.15f,1.22f,1.30f,1.39f,1.49f};
    static const float wide[8]  ={0.35f,0.52f,0.70f,0.91f,1.18f,1.50f,1.88f,2.35f};
    static const float foff[8]  ={-12.0f,-8.0f,-5.0f,-2.0f,2.0f,5.0f,8.0f,12.0f};

    float target[16], cutoff[16], pinc[16];
    for(int side=0;side<2;++side)
    {
        for(int j=0;j<8;++j)
        {
            const int idx=side*8+j;
            float ratio=narrow[j]+(wide[j]-narrow[j])*spray;

            if(mode==1)
            {
                // Ratio mode: very obvious alternating time relationships.
                if(((j+side)&1)!=0) ratio*=1.0f+amount;       // up to 2x
                else ratio*=1.0f-0.28f*amount;               // complementary shorter taps
            }

            const float skew=1.0f+(side?1.0f:-1.0f)*width*(0.008f+0.035f*spray);
            target[idx]=fclamp(base_samples*ratio*skew,16.0f,
                               static_cast<float>(kMaxDelaySamples-4096));
            cutoff[idx]=fclamp(filter_center*powf(2.0f,foff[j]/12.0f),70.0f,18000.0f);
            pinc[idx]=(mod_rate*(1.0f+0.14f*j+0.09f*side))/sample_rate;
        }
    }

    for(size_t n=0;n<size;++n)
    {
        const float inL=Bound(in[0][n],2.0f);
        const float inR=Bound(in[1][n],2.0f);
        float tap[16];
        float fbshape[16];

        // READ FIRST. Wet output comes directly from the raw delay taps so it can never disappear.
        for(int idx=0;idx<16;++idx)
        {
            smooth_delay[idx]+=slew_coeff*(target[idx]-smooth_delay[idx]);
            mod_phase[idx]+=pinc[idx]; if(mod_phase[idx]>=1.0f) mod_phase[idx]-=1.0f;
            const int j=idx&7;

            // Spray also adds large independent motion to smear discrete echoes into a cloud.
            const float smear=(0.0008f+0.010f*spray*spray*(0.35f+0.65f*j/7.0f))*sample_rate;
            const float wobble=sinf(kTwoPi*mod_phase[idx])*smear*(0.20f+0.80f*spray);
            const float rd=fclamp(smooth_delay[idx]+wobble,8.0f,
                                  static_cast<float>(kMaxDelaySamples-3500));

            if(mode==2)
            {
                float semis=0.0f;
                if((j%3)==0) semis=warp_interval;
                else if((j%3)==1) semis=warp_interval*0.583333f;
                else semis=-warp_interval*0.333333f;
                if((idx&8) && (j&1)) semis*=-1.0f;
                tap[idx]=PitchRead(idx,rd,semis,amount);
            }
            else tap[idx]=Safe(lines[idx].ReadHermite(rd));

            // Filter is inside feedback path only. Blend keeps tone rich instead of hollow.
            const SvfOut fo=ProcessSvf(feedback_filter[idx],tap[idx],cutoff[idx],filter_res);
            fbshape[idx]=Sat(tap[idx]*0.62f + fo.bp*(0.80f+0.55f*filter_res));
        }

        // Strong direct wet mix with stereo sum of all 16 lines.
        float wetL=0.0f, wetR=0.0f;
        for(int j=0;j<8;++j)
        {
            // alternating weighting prevents phase cancellation while keeping all lines loud
            const float g=(j&1)?0.145f:0.175f;
            wetL+=tap[j]*g;
            wetR+=tap[8+j]*g;
        }

        // BLUR = diffusion topology, not generic reverb.
        if(mode==0)
        {
            const float d=amount*(0.35f+0.65f*spray);
            const float a=wetL, b=wetR;
            wetL=Sat(a*(1.0f+0.30f*d)+b*0.72f*d);
            wetR=Sat(b*(1.0f+0.30f*d)+a*0.62f*d);
        }

        // SCATTER routing. From subtle neighboring crossfeed to aggressive opposite-side matrix.
        int step=1+static_cast<int>(scatter*6.999f); if(step>7) step=7;
        const float scat=powf(scatter,0.72f);
        const float scat2=scat*scat;

        for(int side=0;side<2;++side)
        {
            const float live=side==0?inL:inR;
            const float other=side==0?inR:inL;
            for(int j=0;j<8;++j)
            {
                const int idx=side*8+j;
                const int same=side*8+((j+step)&7);
                const int cross=(1-side)*8+((7-j+step)&7);
                const int neighbor=side*8+((j+1)&7);

                float routed=fbshape[idx]*(1.0f-0.72f*scat);
                routed+=fbshape[same]*(0.70f*scat);
                routed+=fbshape[cross]*(0.58f*scat2*width);

                if(mode==0)
                    routed+=fbshape[neighbor]*amount*(0.18f+0.42f*spray);

                feedback_state[idx]+=0.18f*(routed-feedback_state[idx]);
                const float fb=Sat(feedback_state[idx]*(1.0f+1.65f*reflect));

                // High injection ensures delay builds instantly and audibly.
                const float inject=freeze?0.0f:Bound((live+other*0.18f*width)*0.72f,1.6f);
                lines[idx].Write(Bound(inject+fb*feedback,1.55f));
            }
        }

        // Additional mode emphasis so the three modes are obviously different on hardware.
        if(mode==1)
        {
            wetL=Sat(wetL*(1.12f+0.28f*amount));
            wetR=Sat(wetR*(1.12f+0.28f*amount));
        }
        else if(mode==2)
        {
            const float cross=0.28f*amount;
            const float a=wetL,b=wetR;
            wetL=Sat(a+b*cross);
            wetR=Sat(b-a*cross*0.75f);
        }

        const float freeze_trim=freeze?0.84f:1.0f;
        out[0][n]=Sat(inL*dry_gain + wetL*wet_gain*freeze_trim);
        out[1][n]=Sat(inR*dry_gain + wetR*wet_gain*freeze_trim);
    }
}

float Pulse(float v,float ph,float off)
{
    v=Clamp01(v);
    const float wave=0.5f+0.5f*sinf(ph*(0.8f+4.5f*v)+off);
    return fclamp(0.05f+v*(0.32f+0.68f*wave),0.0f,1.0f);
}

void UpdateLeds()
{
    static float ph=0.0f; ph+=0.07f; if(ph>1000.0f) ph=0.0f;
    hw.ClearLeds();
    if(ui_shift)
    {
        for(int i=0;i<6;++i)
        {
            const float p=Pulse(secondary[i],ph,0.45f*i);
            hw.SetLed(static_cast<Leds>(LED_1+i),p,p,p);
        }
    }
    else
    {
        hw.SetLed(LED_1,0.0f,0.0f,Pulse(ui_time,ph,0.0f));
        hw.SetLed(LED_2,Pulse(ui_feedback,ph,0.5f),0.0f,0.0f);
        const float m=Pulse(ui_mix,ph,1.0f); hw.SetLed(LED_3,m,m*0.65f,0.0f);
        const float s=Pulse(ui_spray,ph,1.5f); hw.SetLed(LED_4,0.0f,s,0.25f*s);
        const float c=Pulse(ui_scatter,ph,2.0f); hw.SetLed(LED_5,0.0f,0.65f*c,c);
        const float a=Pulse(ui_amount,ph,2.5f); hw.SetLed(LED_6,a,0.0f,a);
    }

    if(ui_mode==0){ hw.SetLed(LED_BOT_1,0.0f,0.18f,1.0f); hw.SetLed(LED_BOT_2,0.03f,0.03f,0.03f); hw.SetLed(LED_BOT_3,0.03f,0.03f,0.03f); }
    else if(ui_mode==1){ hw.SetLed(LED_BOT_1,0.03f,0.03f,0.03f); hw.SetLed(LED_BOT_2,0.05f,1.0f,0.20f); hw.SetLed(LED_BOT_3,0.03f,0.03f,0.03f); }
    else { hw.SetLed(LED_BOT_1,0.03f,0.03f,0.03f); hw.SetLed(LED_BOT_2,0.03f,0.03f,0.03f); hw.SetLed(LED_BOT_3,1.0f,0.0f,1.0f); }

    if(ui_freeze) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else hw.SetLed(LED_FREEZE,0.03f,0.03f,0.03f);

    if(ui_mode==0) hw.SetLed(LED_REVERSE,0.0f,0.25f,1.0f);
    else if(ui_mode==1) hw.SetLed(LED_REVERSE,0.05f,1.0f,0.20f);
    else hw.SetLed(LED_REVERSE,1.0f,0.0f,1.0f);
    hw.WriteLeds();
}
}

int main(void)
{
    hw.Init();
    sample_rate=hw.AudioSampleRate();
    for(int i=0;i<16;++i)
    {
        lines[i].Init();
        smooth_delay[i]=850.0f+120.0f*i;
        lines[i].SetDelay(smooth_delay[i]);
        mod_phase[i]=static_cast<float>(i)/16.0f;
        warp_phase[i]=fmodf(0.137f*i,1.0f);
    }
    hw.SetAudioBlockSize(4);
    hw.StartAudio(AudioCallback);
    while(1){ UpdateLeds(); System::Delay(10); }
}
