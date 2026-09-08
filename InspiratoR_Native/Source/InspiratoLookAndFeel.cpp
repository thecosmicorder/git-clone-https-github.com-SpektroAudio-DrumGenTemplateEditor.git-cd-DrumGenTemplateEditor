#include "InspiratoLookAndFeel.h"

#include <cmath>

namespace inspirato_ui
{
HardwareLookAndFeel::HardwareLookAndFeel()
{
    setColour(juce::Slider::thumbColourId, Palette::redKnob());
    setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    setColour(juce::TextButton::textColourOnId, juce::Colours::black);
}

void HardwareLookAndFeel::drawHardwareKnob(juce::Graphics& g, juce::Point<float> c, float r, float angle)
{
    g.setColour(juce::Colours::black.withAlpha(0.70f));
    g.fillEllipse(c.x-r-5.0f, c.y-r-4.0f, (r+5.0f)*2.0f, (r+5.0f)*2.0f);

    g.setColour(Palette::brass().darker(0.45f));
    g.fillEllipse(c.x-r-3.0f, c.y-r-3.0f, (r+3.0f)*2.0f, (r+3.0f)*2.0f);
    g.setColour(Palette::brass().brighter(0.20f));
    g.drawEllipse(c.x-r-3.0f, c.y-r-3.0f, (r+3.0f)*2.0f, (r+3.0f)*2.0f, 1.5f);

    g.setColour(Palette::redDark());
    g.fillEllipse(c.x-r, c.y-r, r*2.0f, r*2.0f);

    g.setColour(juce::Colours::black.withAlpha(0.55f));
    for (int i = 0; i < 34; ++i)
    {
        const auto a = juce::MathConstants<float>::twoPi * (float) i / 34.0f;
        const auto p1 = c + juce::Point<float>(std::cos(a), std::sin(a)) * (r - 5.0f);
        const auto p2 = c + juce::Point<float>(std::cos(a), std::sin(a)) * (r - 0.8f);
        g.drawLine({ p1, p2 }, 0.8f);
    }

    g.setColour(Palette::redKnob());
    g.fillEllipse(c.x-r+7.0f, c.y-r+7.0f, (r-7.0f)*2.0f, (r-7.0f)*2.0f);
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.drawEllipse(c.x-r+8.0f, c.y-r+8.0f, (r-8.0f)*2.0f, (r-8.0f)*2.0f, 1.0f);

    const auto pointerStart = c + juce::Point<float>(std::sin(angle), -std::cos(angle)) * (r * 0.22f);
    const auto pointerEnd = c + juce::Point<float>(std::sin(angle), -std::cos(angle)) * (r * 0.72f);
    g.setColour(Palette::ivory());
    g.drawLine({ pointerStart, pointerEnd }, std::max(2.0f, r * 0.09f));
}

void HardwareLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float pos, float start, float end, juce::Slider&)
{
    const auto r = 0.5f * (float) std::min(width, height) - 6.0f;
    const auto c = juce::Point<float>((float) x + width * 0.5f, (float) y + height * 0.5f);

    g.setColour(Palette::dim().withAlpha(0.50f));
    for (int i = 0; i <= 10; ++i)
    {
        const auto t = (float) i / 10.0f;
        const auto a = start + t * (end - start);
        const auto p1 = c + juce::Point<float>(std::sin(a), -std::cos(a)) * (r + 3.5f);
        const auto p2 = c + juce::Point<float>(std::sin(a), -std::cos(a)) * (r + 6.5f);
        g.drawLine({ p1, p2 }, 0.8f);
    }

    drawHardwareKnob(g, c, r, start + pos * (end - start));
}

void HardwareLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float, float,
                                            juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const bool vertical = style == juce::Slider::LinearVertical;
    const auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height);

    g.setColour(juce::Colours::black.withAlpha(0.65f));
    g.fillRoundedRectangle(area.translated(2.0f, 3.0f), 5.0f);
    g.setColour(Palette::metal2());
    g.fillRoundedRectangle(area, 5.0f);
    g.setColour(Palette::edgeHi().withAlpha(0.55f));
    g.drawRoundedRectangle(area.reduced(0.5f), 5.0f, 1.0f);

    if (vertical)
    {
        const float cx = area.getCentreX();
        g.setColour(juce::Colours::black.withAlpha(0.90f));
        g.fillRoundedRectangle(cx-5.0f, area.getY()+10.0f, 10.0f, area.getHeight()-20.0f, 4.0f);

        auto cap = juce::Rectangle<float>(area.getX()-5.0f, sliderPos-13.0f, area.getWidth()+10.0f, 26.0f);
        g.setColour(Palette::metal2().brighter(0.15f));
        g.fillRoundedRectangle(cap, 4.0f);
        g.setColour(Palette::edgeHi());
        g.drawRoundedRectangle(cap, 4.0f, 1.0f);
        g.setColour(juce::Colour(0xff760709));
        g.fillRect(cap.getX()+3.0f, cap.getCentreY()-2.0f, cap.getWidth()-6.0f, 4.0f);
    }
    else
    {
        const float cy = area.getCentreY();
        g.setColour(juce::Colours::black.withAlpha(0.90f));
        g.fillRoundedRectangle(area.getX()+8.0f, cy-4.0f, area.getWidth()-16.0f, 8.0f, 4.0f);

        const auto minX = area.getX()+8.0f;
        g.setColour(Palette::gold().withAlpha(0.65f));
        g.fillRoundedRectangle(minX, cy-2.0f, juce::jmax(0.0f, sliderPos-minX), 4.0f, 2.0f);

        auto cap = juce::Rectangle<float>(sliderPos-7.0f, area.getY()-2.0f, 14.0f, area.getHeight()+4.0f);
        g.setColour(Palette::metal2().brighter(0.18f));
        g.fillRoundedRectangle(cap, 3.0f);
        g.setColour(Palette::edgeHi());
        g.drawRoundedRectangle(cap, 3.0f, 1.0f);
    }

    juce::ignoreUnused(slider);
}

void HardwareLookAndFeel::drawGoldButtonFace(juce::Graphics& g, juce::Rectangle<float> r, bool down)
{
    g.setColour(juce::Colours::black.withAlpha(down ? 0.38f : 0.82f));
    g.fillRoundedRectangle(r.translated(5.0f, 6.0f), 6.0f);

    g.setColour(juce::Colour(0xff1b1d1e));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(Palette::brass().darker(0.25f));
    g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.3f);

    auto face = r.reduced(2.0f).translated(0.0f, down ? 3.0f : 0.0f);
    g.setColour(down ? juce::Colour(0xff8c641f) : juce::Colour(0xffc89a38));
    g.fillRoundedRectangle(face, 5.0f);
    if (!down)
    {
        g.setColour(Palette::goldHi().withAlpha(0.55f));
        g.fillRoundedRectangle(face.withHeight(5.0f).reduced(4.0f, 0.0f), 2.0f);
    }
    g.setColour(juce::Colour(0xff4d350d));
    g.drawRoundedRectangle(face.reduced(1.0f), 4.0f, 1.0f);
}

void HardwareLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                const juce::Colour&, bool, bool down)
{
    const auto id = button.getComponentID();
    auto r = button.getLocalBounds().toFloat().reduced(1.0f);

    if (id == "run" || id == "lock")
    {
        const bool on = button.getToggleState();
        auto colour = id == "run" ? Palette::run() : Palette::danger();
        g.setColour(juce::Colours::black.withAlpha(0.75f));
        g.fillRoundedRectangle(r.translated(3.0f, 4.0f), 5.0f);
        g.setColour(on ? colour.darker(0.15f) : Palette::metal2());
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(on ? colour.brighter(0.15f) : Palette::edgeHi());
        g.drawRoundedRectangle(r, 5.0f, 1.0f);
        return;
    }

    if (id.startsWith("zoom"))
    {
        g.setColour(button.getToggleState() ? Palette::metal2().brighter(0.45f) : Palette::metal2());
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(Palette::edgeHi().withAlpha(0.7f));
        g.drawRoundedRectangle(r, 4.0f, 1.0f);
        return;
    }

    drawGoldButtonFace(g, r, down);
}

void HardwareLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool down)
{
    auto r = button.getLocalBounds().translated(0, down ? 3 : 0);
    const auto id = button.getComponentID();
    g.setFont(juce::Font(juce::FontOptions(id.startsWith("zoom") ? 11.0f : 13.0f, juce::Font::bold)));
    g.setColour((id == "run" || id == "lock" || id.startsWith("zoom")) ? Palette::text() : juce::Colour(0xff261a05));
    g.drawFittedText(button.getButtonText(), r, juce::Justification::centred, 1);
}
} // namespace inspirato_ui
