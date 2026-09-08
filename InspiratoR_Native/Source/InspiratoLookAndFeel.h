#pragma once

#include <JuceHeader.h>

namespace inspirato_ui
{
struct Palette
{
    static juce::Colour outer()      { return juce::Colour::fromFloatRGBA(0.070f,0.075f,0.079f,1.0f); }
    static juce::Colour metal()      { return juce::Colour::fromFloatRGBA(0.115f,0.122f,0.126f,1.0f); }
    static juce::Colour metal2()     { return juce::Colour::fromFloatRGBA(0.155f,0.162f,0.166f,1.0f); }
    static juce::Colour recess()     { return juce::Colour::fromFloatRGBA(0.050f,0.054f,0.057f,1.0f); }
    static juce::Colour edge()       { return juce::Colour::fromFloatRGBA(0.19f,0.20f,0.20f,1.0f); }
    static juce::Colour edgeHi()     { return juce::Colour::fromFloatRGBA(0.32f,0.33f,0.33f,1.0f); }
    static juce::Colour text()       { return juce::Colour::fromFloatRGBA(0.92f,0.91f,0.86f,1.0f); }
    static juce::Colour dim()        { return juce::Colour::fromFloatRGBA(0.54f,0.56f,0.55f,1.0f); }
    static juce::Colour gold()       { return juce::Colour::fromFloatRGBA(0.94f,0.69f,0.20f,1.0f); }
    static juce::Colour goldHi()     { return juce::Colour::fromFloatRGBA(1.0f,0.86f,0.45f,1.0f); }
    static juce::Colour redKnob()    { return juce::Colour(0xff790b10); }
    static juce::Colour redDark()    { return juce::Colour(0xff3d0305); }
    static juce::Colour brass()      { return juce::Colour(0xffb98b38); }
    static juce::Colour ivory()      { return juce::Colour(0xfff3dfb2); }
    static juce::Colour run()        { return juce::Colour::fromFloatRGBA(0.20f,0.91f,0.42f,1.0f); }
    static juce::Colour danger()     { return juce::Colour::fromFloatRGBA(0.94f,0.20f,0.13f,1.0f); }
};

class HardwareLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    HardwareLookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;

    static void drawHardwareKnob(juce::Graphics&, juce::Point<float> centre, float radius, float angle);
    static void drawGoldButtonFace(juce::Graphics&, juce::Rectangle<float>, bool down);
};
} // namespace inspirato_ui
