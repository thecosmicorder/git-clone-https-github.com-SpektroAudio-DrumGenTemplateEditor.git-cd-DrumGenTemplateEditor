#pragma once
#include "aurora.h"

// Aurora Hardware::SetLed expects the strongly typed aurora::Leds enum.
#define SetLed(idx, r, g, b) SetLed(static_cast<aurora::Leds>(idx), r, g, b)

// Current Aurora SDK starts control scanning through StartAudio; the legacy-style
// StartAdc call in the shared source is mapped to the same audio/control start.
#define StartAdc() StartAudio(AudioCallback)
