#include "aurora.h"

using namespace daisy;
using namespace aurora;

Hardware hw;

int main(void)
{
    hw.Init();
    bool led_state = true;

    while(1)
    {
        hw.ClearLeds();
        hw.SetLed(LED_FREEZE, 0.0f, 0.0f, led_state ? 1.0f : 0.0f);
        hw.SetLed(LED_REVERSE, led_state ? 1.0f : 0.0f, 0.0f, 0.0f);
        hw.SetLed(LED_1, 0.0f, 1.0f, 0.0f);
        hw.SetLed(LED_2, 0.0f, 0.0f, 1.0f);
        hw.SetLed(LED_3, 1.0f, 0.0f, 1.0f);
        hw.WriteLeds();
        led_state = !led_state;
        System::Delay(500);
    }
}
