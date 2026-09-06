/* Aurora ROOM v1.1 - tighter, richer Super Synthesis ROOM-inspired reverb
   Aurora-native DSP for Qu-Bit Aurora.

   Normal controls:
   TIME       = room size / predelay
   REFLECT    = decay / feedback
   MIX        = dry/wet (hard dry at minimum)
   ATMOSPHERE = brightness / diffusion
   BLUR       = tail body / low-cut shaping
   WARP       = stereo modulation / width

   Buttons:
   FREEZE            = latch reverb freeze
   REVERSE           = cycle 4 room characters
   SHIFT + FREEZE    = latch AIR / bright bloom
   SHIFT + REVERSE   = latch multimode FILTER DELAY

   Filter Delay layer:
   TIME       = delay time
   REFLECT    = delay feedback
   ATMOSPHERE = LP / BP / HP / Notch
   BLUR       = filter cutoff
   WARP       = resonance + cutoff motion
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
constexpr size_t kDelayMax = 96000;
constexpr float kBypass = 0.0035f;

Hardware hw;
float sample_rate = 48000.0f;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS predelay_l;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS predelay_r;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS filterdelay_l;
DelayLine<float, kDelayMax> DSY_SDRAM_BSS filterdelay_r;
ReverbSc DSY_SDRAM_BSS reverb;

bool freeze_latched = false;
bool air_latched = false;
bool filter_delay_latched = false;
uint8_t room_character = 0;
float mod_phase = 0.0f;
float filter_phase = 0.0f;
bool prev_reverse_gate = false;

volatile float ui_p[6] = {0.0f};
volatile bool ui_freeze = false;
volatile bool ui_air = false;
volatile bool ui_filter_delay = false;
volatile uint8_t ui_character = 0;
volatile uint8_t ui_filter_type = 0;

inline float Safe(float x) { return std::isfinite(x) ? x : 0.0f; }
inline float Clamp01(float x) { return fclamp(Safe(x), 0.0f, 1.0f); }
inline float Bound(float x, float lim) { return fclamp(Safe(x), -lim, lim); }
inline float Sat(float x) { return tanhf(Bound(x, 5.0f)); }
inline float KnobCv(int knob, int cv) { return Clamp01(hw.GetKnobValue(knob) + hw.GetCvValue(cv)); }

inline void IncPhase(float& p, float hz)
{
    p += hz / sample_rate;
    if(p >= 1.0f) p -= floorf(p);
}

struct SvfState { float ic1 = 0.0f; float ic2 = 0.0f; };
struct SvfOut { float lp, bp, hp, notch; };
SvfState fd_l, fd_r;

SvfOut ProcessSvf(SvfState& s, float input, float cutoff, float resonance)
{
    const float f = fclamp(cutoff, 20.0f, sample_rate * 0.42f);
    const float g = tanf(kPi * f / sample_rate);
    const float k = 2.0f - 1.88f * Clamp01(resonance);
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

float PickFilter(const SvfOut& o, uint8_t type)
{
    switch(type & 3u)
    {
        case 0: return o.lp;
        case 1: return o.bp;
        case 2: return o.hp;
        default: return o.notch;
    }
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
        if(modifier)
        {
            air_latched = !air_latched;
            shift_grace = 0.0f;
        }
        else
            freeze_latched = !freeze_latched;
    }

    if(redge)
    {
        if(modifier)
        {
            filter_delay_latched = !filter_delay_latched;
            shift_grace = 0.0f;
        }
        else
            room_character = static_cast<uint8_t>((room_character + 1u) & 3u);
    }

    // FREEZE gate temporarily inverts the stored freeze state.
    ui_freeze = freeze_latched != hw.GetGateState(GATE_FREEZE);

    // REVERSE gate rising edge advances the room character without changing the button latch states.
    const bool rg = hw.GetGateState(GATE_REVERSE);
    if(rg && !prev_reverse_gate)
        room_character = static_cast<uint8_t>((room_character + 1u) & 3u);
    prev_reverse_gate = rg;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    hw.ProcessAllControls();
    HandleButtons(size);

    const float p0 = KnobCv(KNOB_TIME, CV_TIME);
    const float p1 = KnobCv(KNOB_REFLECT, CV_REFLECT);
    const float p2 = KnobCv(KNOB_MIX, CV_MIX);
    const float p3 = KnobCv(KNOB_ATMOSPHERE, CV_ATMOSPHERE);
    const float p4 = KnobCv(KNOB_BLUR, CV_BLUR);
    const float p5 = Clamp01(hw.GetKnobValue(KNOB_WARP) + hw.GetWarpVoct() / 60.0f);
    ui_p[0]=p0; ui_p[1]=p1; ui_p[2]=p2; ui_p[3]=p3; ui_p[4]=p4; ui_p[5]=p5;
    ui_air = air_latched;
    ui_filter_delay = filter_delay_latched;
    ui_character = room_character;

    if(p2 <= kBypass)
    {
        for(size_t i=0;i<size;++i) { out[0][i]=in[0][i]; out[1][i]=in[1][i]; }
        return;
    }

    // Four useful room characters. TIME remains continuous within each character.
    float min_pre = 0.006f, max_pre = 0.060f, fb_trim = -0.035f, width = 0.82f, char_mod = 0.70f;
    switch(room_character & 3u)
    {
        case 0: // Tight Room
            min_pre=0.004f; max_pre=0.055f; fb_trim=-0.045f; width=0.78f; char_mod=0.55f; break;
        case 1: // Plate
            min_pre=0.003f; max_pre=0.085f; fb_trim= 0.000f; width=1.00f; char_mod=0.85f; break;
        case 2: // Hall
            min_pre=0.012f; max_pre=0.145f; fb_trim= 0.028f; width=1.18f; char_mod=0.42f; break;
        default: // Chamber
            min_pre=0.006f; max_pre=0.072f; fb_trim=-0.010f; width=0.92f; char_mod=1.00f; break;
    }

    const float predelay_s = min_pre + (max_pre - min_pre) * p0 * p0;
    float feedback = 0.48f + 0.435f * powf(p1, 0.78f) + fb_trim;
    if(air_latched) feedback += 0.018f;
    if(ui_freeze) feedback = 0.985f;
    feedback = fclamp(feedback, 0.42f, 0.985f);

    // Normal ROOM bandwidth: no more 1.2 kHz blanket. Atmosphere now stays useful from warm to open.
    float high_cut = 4300.0f * powf(4.25f, p3);       // ~4.3 kHz to ~18.3 kHz
    float low_cut  = 35.0f * powf(9.0f, p4);         // ~35 Hz to ~315 Hz
    if(air_latched)
    {
        high_cut = 18800.0f;
        low_cut = fminf(low_cut, 95.0f);
    }

    const float mod_rate = (0.055f + 0.70f * p5 * p5) * char_mod;
    const float mod_depth = (0.00020f + 0.0068f * p5 * p5) * sample_rate;
    const float diffusion = 0.02f + 0.12f * p3 + 0.08f * p4;
    const float dry_gain = cosf(p2 * kPi * 0.5f);
    float wet_gain = sinf(p2 * kPi * 0.5f) * (1.12f + 0.10f * p3);
    if(air_latched) wet_gain *= 1.06f;
    if(ui_freeze) wet_gain *= 0.86f;

    // Output tail filtering.
    static float tail_lp_l=0.0f, tail_lp_r=0.0f, tail_low_l=0.0f, tail_low_r=0.0f;
    const float lp_a = fclamp(1.0f - expf(-kTwoPi * high_cut / sample_rate), 0.001f, 0.97f);
    const float hp_a = fclamp(1.0f - expf(-kTwoPi * low_cut / sample_rate), 0.0001f, 0.8f);

    // AIR exciter isolates upper mids/highs before feeding them into the reverb.
    static float air_lp_l=0.0f, air_lp_r=0.0f;
    const float air_a = 1.0f - expf(-kTwoPi * 5200.0f / sample_rate);

    reverb.SetFeedback(feedback);
    reverb.SetLpFreq(high_cut);

    for(size_t i=0;i<size;++i)
    {
        const float il = Bound(in[0][i], 2.0f);
        const float ir = Bound(in[1][i], 2.0f);

        IncPhase(mod_phase, mod_rate);
        const float wob = sinf(kTwoPi * mod_phase) * mod_depth;
        const float base = predelay_s * sample_rate;
        predelay_l.SetDelay(fclamp(base + wob, 4.0f, static_cast<float>(kDelayMax-4)));
        predelay_r.SetDelay(fclamp(base - wob * 0.79f, 4.0f, static_cast<float>(kDelayMax-4)));
        const float pd_l = Safe(predelay_l.Read());
        const float pd_r = Safe(predelay_r.Read());

        // Small cross-diffusion creates density without the long smearing loop from v1.0.
        const float write_l = ui_freeze ? 0.0f : Bound(il * 0.92f + pd_r * diffusion * p1, 1.3f);
        const float write_r = ui_freeze ? 0.0f : Bound(ir * 0.92f + pd_l * diffusion * p1, 1.3f);
        predelay_l.Write(write_l);
        predelay_r.Write(write_r);

        air_lp_l += air_a * (il - air_lp_l);
        air_lp_r += air_a * (ir - air_lp_r);
        const float air_l = il - air_lp_l;
        const float air_r = ir - air_lp_r;
        const float air_feed = air_latched ? 0.22f : 0.0f;

        float rv_l=0.0f, rv_r=0.0f;
        const float feed_l = ui_freeze ? 0.0f : Bound(pd_l * 0.88f + il * 0.12f + air_l * air_feed, 1.15f);
        const float feed_r = ui_freeze ? 0.0f : Bound(pd_r * 0.88f + ir * 0.12f + air_r * air_feed, 1.15f);
        reverb.Process(feed_l, feed_r, &rv_l, &rv_r);

        // Clean band-limited tail: high-cut then high-pass.
        tail_lp_l += lp_a * (rv_l - tail_lp_l);
        tail_lp_r += lp_a * (rv_r - tail_lp_r);
        tail_low_l += hp_a * (tail_lp_l - tail_low_l);
        tail_low_r += hp_a * (tail_lp_r - tail_low_r);
        float wet_l = tail_lp_l - tail_low_l;
        float wet_r = tail_lp_r - tail_low_r;

        // Width increases with WARP but is kept level compensated.
        const float mid = (wet_l + wet_r) * 0.5f;
        const float side = (wet_l - wet_r) * 0.5f * (0.65f + 0.55f * p5) * width;
        wet_l = mid + side;
        wet_r = mid - side;

        // SHIFT+REVERSE: genuine resonant multimode filter delay, not a cosmetic toggle.
        if(filter_delay_latched)
        {
            const uint8_t ftype = static_cast<uint8_t>(fminf(3.0f, floorf(p3 * 4.0f)));
            ui_filter_type = ftype;
            const float fd_time_s = 0.040f + 0.360f * p0 * p0;
            const float stereo_off = (0.002f + 0.012f * p5) * sample_rate;
            filterdelay_l.SetDelay(fclamp(fd_time_s * sample_rate + stereo_off, 8.0f, static_cast<float>(kDelayMax-4)));
            filterdelay_r.SetDelay(fclamp(fd_time_s * sample_rate - stereo_off * 0.63f, 8.0f, static_cast<float>(kDelayMax-4)));
            const float dl = Safe(filterdelay_l.Read());
            const float dr = Safe(filterdelay_r.Read());

            IncPhase(filter_phase, 0.07f + 0.55f * p5);
            const float motion = sinf(kTwoPi * filter_phase) * (0.08f + 0.42f * p5);
            const float cutoff_base = 170.0f * powf(72.0f, p4); // ~170 Hz to ~12.2 kHz
            const float cutoff = fclamp(cutoff_base * powf(2.0f, motion), 90.0f, 16000.0f);
            const float resonance = 0.18f + 0.72f * p5;
            const float fl = PickFilter(ProcessSvf(fd_l, dl, cutoff, resonance), ftype);
            const float fr = PickFilter(ProcessSvf(fd_r, dr, cutoff * 1.035f, resonance), ftype);
            const float fd_fb = 0.10f + 0.66f * p1 * p1;
            filterdelay_l.Write(Bound(wet_l * 0.55f + fr * fd_fb, 1.2f));
            filterdelay_r.Write(Bound(wet_r * 0.55f + fl * fd_fb, 1.2f));
            const float fd_mix = 0.28f + 0.42f * p1;
            wet_l = Sat(wet_l + fl * fd_mix);
            wet_r = Sat(wet_r + fr * fd_mix);
        }
        else
            ui_filter_type = 0;

        if(air_latched)
        {
            wet_l = Sat(wet_l * 1.08f + air_l * 0.035f);
            wet_r = Sat(wet_r * 1.08f + air_r * 0.035f);
        }

        out[0][i] = Sat(il * dry_gain + wet_l * wet_gain);
        out[1][i] = Sat(ir * dry_gain + wet_r * wet_gain);
    }
}

float Pulse(float value, float phase, float offset)
{
    const float v = Clamp01(value);
    const float speed = 0.65f + 5.2f * v;
    const float s = 0.5f + 0.5f * sinf(phase * speed + offset);
    return fclamp(0.05f + v * (0.24f + 0.76f * s), 0.0f, 1.0f);
}

void UpdateLeds()
{
    static float phase=0.0f;
    static uint32_t ticks=0;
    phase += 0.070f;
    if(phase > kTwoPi*12.0f) phase=0.0f;
    ++ticks;
    const bool alt = ((ticks/20u)&1u)!=0u;
    hw.ClearLeds();

    const float p1=Pulse(ui_p[0],phase,0.0f);
    const float p2=Pulse(ui_p[1],phase,0.7f);
    const float p3=Pulse(ui_p[2],phase,1.4f);
    const float p4=Pulse(ui_p[3],phase,2.1f);
    const float p5=Pulse(ui_p[4],phase,2.8f);
    const float p6=Pulse(ui_p[5],phase,3.5f);
    hw.SetLed(LED_1,0.0f,0.25f*p1,p1);      // TIME blue
    hw.SetLed(LED_2,p2,0.0f,0.0f);          // REFLECT red
    hw.SetLed(LED_3,p3,p3*0.70f,0.0f);      // MIX amber

    if(ui_filter_delay)
    {
        // ATMOSPHERE shows filter type: LP blue / BP green / HP red / Notch magenta.
        if(ui_filter_type==0) hw.SetLed(LED_4,0.0f,0.15f*p4,p4);
        else if(ui_filter_type==1) hw.SetLed(LED_4,0.0f,p4,0.10f*p4);
        else if(ui_filter_type==2) hw.SetLed(LED_4,p4,0.0f,0.0f);
        else hw.SetLed(LED_4,p4,0.0f,p4);
    }
    else
        hw.SetLed(LED_4,0.0f,p4,0.25f*p4);   // ATMOSPHERE green
    hw.SetLed(LED_5,0.0f,0.70f*p5,p5);       // BLUR cyan
    hw.SetLed(LED_6,p6,0.0f,p6);             // WARP magenta

    // FREEZE button: white Freeze, blue AIR, alternate if both.
    if(ui_freeze && ui_air)
    {
        if(alt) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
        else hw.SetLed(LED_FREEZE,0.0f,0.0f,1.0f);
    }
    else if(ui_air) hw.SetLed(LED_FREEZE,0.0f,0.0f,1.0f);
    else if(ui_freeze) hw.SetLed(LED_FREEZE,1.0f,1.0f,1.0f);
    else hw.SetLed(LED_FREEZE,0.03f,0.03f,0.03f);

    // REVERSE button: room character color normally, yellow when Filter Delay is latched.
    if(ui_filter_delay)
        hw.SetLed(LED_REVERSE,1.0f,0.85f,0.0f);
    else
    {
        switch(ui_character & 3u)
        {
            case 0: hw.SetLed(LED_REVERSE,0.0f,1.0f,0.25f); break; // Tight green
            case 1: hw.SetLed(LED_REVERSE,0.0f,0.65f,1.0f); break; // Plate cyan
            case 2: hw.SetLed(LED_REVERSE,0.35f,0.0f,1.0f); break; // Hall violet
            default: hw.SetLed(LED_REVERSE,1.0f,0.25f,0.0f); break; // Chamber orange
        }
    }

    hw.SetLed(LED_BOT_1,ui_air?0.0f:0.0f,0.0f,ui_air?1.0f:0.0f);
    hw.SetLed(LED_BOT_2,ui_filter_delay?1.0f:0.0f,ui_filter_delay?0.85f:0.0f,0.0f);
    hw.SetLed(LED_BOT_3,ui_freeze?1.0f:0.0f,ui_freeze?1.0f:0.0f,ui_freeze?1.0f:0.0f);
    hw.WriteLeds();
}
}

int main(void)
{
    hw.Init();
    sample_rate = hw.AudioSampleRate();
    predelay_l.Init(); predelay_r.Init();
    filterdelay_l.Init(); filterdelay_r.Init();
    reverb.Init(sample_rate);
    hw.SetAudioBlockSize(4);
    hw.StartAudio(AudioCallback);
    while(1)
    {
        UpdateLeds();
        System::Delay(10);
    }
}
