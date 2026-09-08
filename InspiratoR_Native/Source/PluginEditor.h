#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "InspiratoLookAndFeel.h"

#include <array>
#include <memory>
#include <vector>

class InspiratoRAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                             private juce::Timer
{
public:
    explicit InspiratoRAudioProcessorEditor(InspiratoRAudioProcessor&);
    ~InspiratoRAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class NoteFader final : public juce::Slider
    {
    public:
        NoteFader(InspiratoRAudioProcessor& p, int pitchClass) : processor(p), pc(pitchClass) {}
        void paint(juce::Graphics&) override;
    private:
        InspiratoRAudioProcessor& processor;
        int pc = 0;
    };

    class StepDial final : public juce::Slider
    {
    public:
        explicit StepDial(InspiratoRAudioProcessor& p) : processor(p) {}
        void paint(juce::Graphics&) override;
    private:
        InspiratoRAudioProcessor& processor;
    };

    void timerCallback() override;
    void configureKnob(juce::Slider&, const juce::String& id);
    void configureFader(juce::Slider&, const juce::String& id, bool vertical = true);
    void addSliderAttachment(juce::Slider&, const juce::String& id);
    void addButtonAttachment(juce::Button&, const juce::String& id);
    void drawPanel(juce::Graphics&, juce::Rectangle<float>, const juce::String& title);
    void drawInnerBox(juce::Graphics&, juce::Rectangle<float>);
    void drawFooterBrand(juce::Graphics&);
    void setZoomPercent(int percent);
    float param(const juce::String& id) const;
    juce::Rectangle<int> scaled(juce::Rectangle<float> r) const;

    InspiratoRAudioProcessor& processor;
    inspirato_ui::HardwareLookAndFeel look;

    std::array<std::unique_ptr<NoteFader>, 12> noteFaders;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 12> noteAttachments;

    juce::Slider noteValue, variation, legato, rest, rhythmRange;
    juce::Slider velo, direction, transpose, accent, gate;
    StepDial steps;
    juce::Slider melodyRange, low, high, poly;

    juce::TextButton diceRhythm { "DICE RHYTHM" };
    juce::TextButton dejaVu { "DEJA VU" };
    juce::TextButton run { "RUN" };
    juce::TextButton lock { "LOCK" };
    juce::TextButton restart { "RESTART" };
    juce::TextButton diceMelody { "DICE MELODY" };
    std::array<juce::TextButton, 5> zoomButtons { juce::TextButton("50%"), juce::TextButton("75%"), juce::TextButton("100%"), juce::TextButton("125%"), juce::TextButton("150%") };

    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments;

    int zoomPercent = 100;
    static constexpr float baseW = 1220.0f;
    static constexpr float baseH = 700.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspiratoRAudioProcessorEditor)
};
