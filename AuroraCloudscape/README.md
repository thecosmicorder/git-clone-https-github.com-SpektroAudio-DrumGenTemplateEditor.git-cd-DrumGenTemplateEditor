# Aurora Cloudscape v1.0

Aurora Cloudscape is an independent Qu-Bit Aurora firmware adaptation of the open-source Clouds DSP as maintained by Electrosmith in the DaisyExamples Nimbus port.

It is not an official Mutable Instruments or Qu-Bit firmware release. Mutable Instruments is a registered trademark; the derivative firmware intentionally uses a different name.

## Aurora controls

- TIME: grain Position
- REFLECT: grain Size
- ATMOSPHERE: Density
- BLUR: Texture
- WARP: Pitch, approximately +/-24 semitones from the knob plus Aurora WARP V/Oct CV transpose
- MIX: Clouds-style Blend knob
- SHIFT press: cycle Blend assignment: Dry/Wet -> Stereo Spread -> Feedback -> Reverb
- FREEZE press: latch Freeze
- FREEZE gate: momentary Freeze
- REVERSE press: cycle playback mode: Granular -> Stretch -> Looping Delay -> Spectral
- REVERSE gate: grain Trigger/Gate

Firmware runs the Clouds engine at 48 kHz with a 32-sample audio block and 16-bit stereo quality.

## LED language

- REVERSE LED: green=Granular, blue=Stretch, yellow=Looping Delay, red=Spectral
- FREEZE LED: bright white while frozen
- Bottom LEDs show the current MIX/Blend assignment
- Turning a knob temporarily turns the six center LEDs into a value bar using that parameter's color

## Attribution

Original Clouds DSP: Emilie Gillet, Mutable Instruments, MIT-licensed STM32F code.
Daisy Nimbus port: Electrosmith contributors, MIT-licensed.
See THIRD_PARTY_LICENSES.md.
