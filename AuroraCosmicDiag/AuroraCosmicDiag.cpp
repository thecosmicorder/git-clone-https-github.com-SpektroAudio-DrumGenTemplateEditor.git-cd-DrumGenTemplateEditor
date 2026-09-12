#include "aurora.h"
#include "daisysp.h"
#include <cmath>
using namespace daisy;
using namespace daisysp;
using namespace aurora;

namespace {
Hardware hw;
constexpr size_t kMax = 96000;
DelayLine<float,kMax> DSY_SDRAM_BSS dl;
DelayLine<float,kMax> DSY_SDRAM_BSS dr;
float sr = 48000.0f;
bool freeze = false;
volatile float ui_time=0.4f, ui_fb=0.35f, ui_mix=0.65f;
inline float C(float x){return fclamp(std::isfinite(x)?x:0.0f,0.0f,1.0f);} 
inline float S(float x){return tanhf(fclamp(x,-4.0f,4.0f));}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();
    if(hw.GetButton(SW_FREEZE).RisingEdge()) freeze = !freeze;
    const float t = C(hw.GetKnobValue(KNOB_TIME) + hw.GetCvValue(CV_TIME));
    const float fb = C(hw.GetKnobValue(KNOB_REFLECT) + hw.GetCvValue(CV_REFLECT));
    const float mix = C(hw.GetKnobValue(KNOB_MIX) + hw.GetCvValue(CV_MIX));
    ui_time=t; ui_fb=fb; ui_mix=mix;
    const float sec = 0.020f * powf(40.0f,t); // 20..800 ms
    const float dly = fclamp(sec*sr,2.0f,(float)kMax-4.0f);
    dl.SetDelay(dly);
    dr.SetDelay(fclamp(dly*1.07f,2.0f,(float)kMax-4.0f));
    const float dry = cosf(mix*1.57079632679f);
    const float wet = sinf(mix*1.57079632679f)*1.35f;
    const float regen = freeze ? 0.985f : (0.05f + 0.88f*fb);
    for(size_t i=0;i<size;++i){
        const float a=dl.Read(); const float b=dr.Read();
        const float il=in[0][i], ir=in[1][i];
        dl.Write(freeze ? a*regen : S(il*0.95f + a*regen));
        dr.Write(freeze ? b*regen : S(ir*0.95f + b*regen));
        out[0][i]=S(il*dry + a*wet);
        out[1][i]=S(ir*dry + b*wet);
    }
}

void Leds(){
    hw.ClearLeds();
    hw.SetLed(LED_1,0.0f,0.0f,0.15f+0.85f*ui_time);      // blue
    hw.SetLed(LED_2,0.15f+0.85f*ui_fb,0.0f,0.0f);        // red
    hw.SetLed(LED_3,0.0f,0.15f+0.85f*ui_mix,0.0f);       // green
    hw.SetLed(LED_4,0.0f,0.8f,0.8f);                     // cyan
    hw.SetLed(LED_5,0.8f,0.0f,0.8f);                     // magenta
    hw.SetLed(LED_6,0.0f,0.0f,0.9f);                     // blue
    hw.SetLed(LED_BOT_1,1.0f,0.0f,0.0f);                 // red
    hw.SetLed(LED_BOT_2,0.0f,1.0f,0.0f);                 // green
    hw.SetLed(LED_BOT_3,0.0f,0.0f,1.0f);                 // blue
    if(freeze) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else hw.SetLed(LED_FREEZE,0.0f,0.0f,0.1f);
    hw.SetLed(LED_REVERSE,1.0f,0.35f,0.0f);               // orange
    hw.WriteLeds();
}
}

int main(void){
    hw.Init();
    sr=hw.AudioSampleRate();
    dl.Init(); dr.Init(); dl.SetDelay(sr*0.18f); dr.SetDelay(sr*0.19f);
    hw.SetAudioBlockSize(4);
    hw.StartAudio(AudioCallback);
    while(1){Leds(); System::Delay(10);} 
}
