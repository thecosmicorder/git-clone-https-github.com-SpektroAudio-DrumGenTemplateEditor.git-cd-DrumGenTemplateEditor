#include "PluginEditor.h"

#include <cmath>

using inspirato_ui::Palette;

namespace
{
constexpr std::array<const char*, 12> noteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

void drawText(juce::Graphics& g, const juce::String& text, float x, float y, float w, float h,
              float size, juce::Colour colour, juce::Justification justification = juce::Justification::centred)
{
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(size)));
    g.drawFittedText(text, juce::Rectangle<int>((int)x, (int)y, (int)w, (int)h), justification, 1);
}
}

InspiratoRAudioProcessorEditor::InspiratoRAudioProcessorEditor(InspiratoRAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p), steps(p)
{
    setLookAndFeel(&look);
    setOpaque(true);
    setResizable(true, true);
    setResizeLimits(610, 350, 1830, 1050);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio(baseW / baseH);
    setSize((int) baseW, (int) baseH);

    configureKnob(noteValue, "rate");
    configureKnob(variation, "variation");
    configureKnob(legato, "legato");
    configureKnob(rest, "rest");
    configureFader(rhythmRange, "rrange", false);

    configureKnob(velo, "velo");
    configureKnob(direction, "direction");
    configureKnob(transpose, "transpose");
    configureKnob(accent, "accent");
    configureKnob(gate, "gate");

    steps.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    steps.setRange(1.0, 16.0, 1.0);
    steps.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    steps.setRotaryParameters(juce::degreesToRadians(135.0f), juce::degreesToRadians(405.0f), true);
    addAndMakeVisible(steps);
    addSliderAttachment(steps, "steps");

    configureFader(melodyRange, "mrange", false);
    configureFader(low, "low", true);
    configureFader(high, "high", true);
    configureFader(poly, "poly", false);

    for (int i = 0; i < 12; ++i)
    {
        noteFaders[(size_t) i] = std::make_unique<NoteFader>(processor, i);
        auto& s = *noteFaders[(size_t) i];
        s.setSliderStyle(juce::Slider::LinearVertical);
        s.setRange(0.0, 100.0, 1.0);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(s);
        noteAttachments[(size_t) i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, "w" + juce::String(i), s);
    }

    diceRhythm.setComponentID("diceRhythm");
    dejaVu.setComponentID("dejaVu");
    restart.setComponentID("restart");
    diceMelody.setComponentID("diceMelody");
    run.setComponentID("run");
    lock.setComponentID("lock");

    for (auto* b : { &diceRhythm, &dejaVu, &run, &lock, &restart, &diceMelody })
    {
        b->setClickingTogglesState(false);
        addAndMakeVisible(*b);
    }

    run.setClickingTogglesState(true);
    lock.setClickingTogglesState(true);
    addButtonAttachment(run, "run");
    addButtonAttachment(lock, "lock");

    diceRhythm.onClick = [this] { processor.triggerDiceRhythm(); };
    diceMelody.onClick = [this] { processor.triggerDiceMelody(); };
    dejaVu.onClick = [this] { processor.triggerDejaVu(); };
    restart.onClick = [this] { processor.triggerRestart(); };

    constexpr std::array<int, 5> z { 50, 75, 100, 125, 150 };
    for (int i = 0; i < 5; ++i)
    {
        auto& b = zoomButtons[(size_t) i];
        b.setComponentID("zoom" + juce::String(z[(size_t) i]));
        b.setClickingTogglesState(false);
        b.onClick = [this, pct = z[(size_t) i]] { setZoomPercent(pct); };
        addAndMakeVisible(b);
    }

    startTimerHz(30);
}

InspiratoRAudioProcessorEditor::~InspiratoRAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void InspiratoRAudioProcessorEditor::configureKnob(juce::Slider& s, const juce::String& id)
{
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    s.setRotaryParameters(juce::degreesToRadians(135.0f), juce::degreesToRadians(405.0f), true);
    addAndMakeVisible(s);
    addSliderAttachment(s, id);
}

void InspiratoRAudioProcessorEditor::configureFader(juce::Slider& s, const juce::String& id, bool vertical)
{
    s.setSliderStyle(vertical ? juce::Slider::LinearVertical : juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible(s);
    addSliderAttachment(s, id);
}

void InspiratoRAudioProcessorEditor::addSliderAttachment(juce::Slider& s, const juce::String& id)
{
    sliderAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, id, s));
}

void InspiratoRAudioProcessorEditor::addButtonAttachment(juce::Button& b, const juce::String& id)
{
    buttonAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.apvts, id, b));
}

float InspiratoRAudioProcessorEditor::param(const juce::String& id) const
{
    if (auto* p = processor.apvts.getRawParameterValue(id)) return p->load(std::memory_order_relaxed);
    return 0.0f;
}

juce::Rectangle<int> InspiratoRAudioProcessorEditor::scaled(juce::Rectangle<float> r) const
{
    const auto sx = getWidth() / baseW;
    const auto sy = getHeight() / baseH;
    return { juce::roundToInt(r.getX() * sx), juce::roundToInt(r.getY() * sy),
             juce::roundToInt(r.getWidth() * sx), juce::roundToInt(r.getHeight() * sy) };
}

void InspiratoRAudioProcessorEditor::setZoomPercent(int percent)
{
    zoomPercent = juce::jlimit(50, 150, percent);
    setSize(juce::roundToInt(baseW * zoomPercent / 100.0f), juce::roundToInt(baseH * zoomPercent / 100.0f));
}

void InspiratoRAudioProcessorEditor::resized()
{
    noteValue.setBounds(scaled({55,98,76,76}));
    variation.setBounds(scaled({168,98,76,76}));
    legato.setBounds(scaled({281,98,76,76}));
    rest.setBounds(scaled({394,98,76,76}));
    rhythmRange.setBounds(scaled({52,218,306,18}));

    velo.setBounds(scaled({752,104,52,52}));
    direction.setBounds(scaled({840,104,52,52}));
    transpose.setBounds(scaled({928,104,52,52}));
    accent.setBounds(scaled({1016,104,52,52}));
    gate.setBounds(scaled({1104,104,52,52}));
    steps.setBounds(scaled({592,98,124,144}));

    melodyRange.setBounds(scaled({44,586,515,18}));
    low.setBounds(scaled({996,330,44,190}));
    high.setBounds(scaled({1092,330,44,190}));
    poly.setBounds(scaled({976,586,186,20}));

    for (int i = 0; i < 12; ++i)
        noteFaders[(size_t) i]->setBounds(scaled({42.0f + i*72.4f, 326.0f, 44.0f, 212.0f}));

    diceRhythm.setBounds(scaled({374,211,132,30}));
    dejaVu.setBounds(scaled({752,198,116,30}));
    run.setBounds(scaled({882,198,58,30}));
    lock.setBounds(scaled({952,198,58,30}));
    restart.setBounds(scaled({1022,198,96,30}));
    diceMelody.setBounds(scaled({650,584,132,30}));

    for (int i = 0; i < 5; ++i)
        zoomButtons[(size_t) i].setBounds(scaled({934.0f + i*48.0f,20,43,22}));
}

void InspiratoRAudioProcessorEditor::drawPanel(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title)
{
    g.setColour(juce::Colours::black.withAlpha(0.78f));
    g.fillRoundedRectangle(r.translated(7.0f, 9.0f), 8.0f);
    g.setColour(Palette::metal2());
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(Palette::recess());
    g.fillRoundedRectangle(r.reduced(2.0f), 6.0f);
    g.setColour(Palette::edgeHi().withAlpha(0.28f));
    g.drawRoundedRectangle(r.reduced(2.0f), 6.0f, 1.0f);

    g.setColour(Palette::gold().withAlpha(0.82f));
    g.fillRoundedRectangle(r.getX()+3.0f, r.getY()+3.0f, 5.0f, r.getHeight()-6.0f, 2.0f);

    g.setColour(Palette::gold());
    g.setFont(juce::Font(juce::FontOptions(11.2f, juce::Font::bold)));
    g.drawText(title, r.getX()+20.0f, r.getY()+8.0f, r.getWidth()-40.0f, 20.0f, juce::Justification::centred);
    g.setColour(Palette::gold().withAlpha(0.30f));
    g.drawLine(r.getX()+24.0f, r.getY()+31.0f, r.getRight()-24.0f, r.getY()+31.0f, 1.0f);
}

void InspiratoRAudioProcessorEditor::drawInnerBox(juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour(juce::Colour(0xff0b0d0e).withAlpha(0.78f));
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(Palette::edgeHi().withAlpha(0.20f));
    g.drawRoundedRectangle(r, 10.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawLine(r.getX()+12.0f, r.getY()+4.0f, r.getRight()-12.0f, r.getY()+4.0f, 1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.90f));
    g.drawLine(r.getX()+12.0f, r.getBottom()-4.0f, r.getRight()-12.0f, r.getBottom()-4.0f, 1.4f);
}

void InspiratoRAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float sx = getWidth() / baseW;
    const float sy = getHeight() / baseH;
    g.addTransform(juce::AffineTransform::scale(sx, sy));

    g.fillAll(Palette::outer());
    auto chassis = juce::Rectangle<float>(5,6,1210,688);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRoundedRectangle(chassis.translated(6,8), 26.0f);
    g.setColour(Palette::metal());
    g.fillRoundedRectangle(chassis, 26.0f);
    g.setColour(Palette::edgeHi().withAlpha(0.75f));
    g.drawRoundedRectangle(chassis.reduced(1.0f), 26.0f, 2.0f);

    g.setColour(Palette::metal2());
    g.fillRoundedRectangle(16,16,1188,34,12.0f);

    drawPanel(g, {22,64,500,184}, "RHYTHM");
    drawInnerBox(g, {36,100,472,134});
    drawPanel(g, {532,64,666,184}, "SEQUENCE / DEJA VU");
    drawInnerBox(g, {546,100,638,134});
    drawPanel(g, {22,258,904,376}, "MELODY — NOTE PROBABILITY");
    drawPanel(g, {936,258,262,376}, "VOICE / RANGE");

    drawText(g, "NOTE VALUE", 55,180,76,14,8.0f,Palette::gold());
    drawText(g, "VARIATION", 168,180,76,14,8.0f,Palette::gold());
    drawText(g, "LEGATO", 281,180,76,14,8.0f,Palette::gold());
    drawText(g, "REST", 394,180,76,14,8.0f,Palette::gold());
    drawText(g, "RHYTHM RANGE", 52,202,120,12,7.6f,Palette::gold(),juce::Justification::centredLeft);

    drawText(g, "VELO", 752,162,52,14,7.0f,Palette::text());
    drawText(g, "DIRECTION", 832,162,68,14,7.0f,Palette::text());
    drawText(g, "TRANSPOSE", 918,162,72,14,7.0f,Palette::text());
    drawText(g, "ACCENT", 1016,162,52,14,7.0f,Palette::text());
    drawText(g, "GATE", 1104,162,52,14,7.0f,Palette::text());

    for (int i = 0; i < 12; ++i)
    {
        const float x = 42.0f + i * 72.4f;
        drawText(g, noteNames[(size_t) i], x-8,304,60,15,8.2f,Palette::text());
        const bool on = (processor.getDisplayPitchMask() & (1u << i)) != 0;
        drawText(g, juce::String(juce::roundToInt(param("w"+juce::String(i)))), x-8,544,60,16,7.3f,on?Palette::gold():Palette::dim());
    }

    drawText(g, "MELODY RANGE", 44,568,120,12,7.6f,Palette::gold(),juce::Justification::centredLeft);
    drawText(g, "0", 44,606,20,14,6.8f,Palette::dim(),juce::Justification::centredLeft);
    drawText(g, juce::String(juce::roundToInt(param("mrange")))+"%", 270,606,64,14,7.4f,Palette::gold());
    drawText(g, "100", 529,606,30,14,6.8f,Palette::dim(),juce::Justification::centredRight);

    drawText(g, "LOW", 984,305,68,16,8.0f,Palette::text());
    drawText(g, "HIGH", 1080,305,68,16,8.0f,Palette::text());
    drawText(g, juce::String(juce::roundToInt(param("low"))), 984,526,68,16,7.8f,Palette::gold());
    drawText(g, juce::String(juce::roundToInt(param("high"))), 1080,526,68,16,7.8f,Palette::gold());

    drawText(g, "POLYPHONY", 976,568,186,12,7.8f,Palette::gold());
    constexpr std::array<const char*, 4> polyLabels { "1", "2", "3", "4" };
    for (int i = 0; i < 4; ++i)
    {
        const float x = 986.0f + i * (166.0f / 3.0f);
        const bool on = juce::roundToInt(param("poly")) == i+1;
        drawText(g, polyLabels[(size_t) i], x-12,610,24,14,7.2f,on?Palette::gold():Palette::dim());
    }

    drawText(g, "ZOOM", 888,23,42,14,6.7f,Palette::dim(),juce::Justification::centredRight);
    drawFooterBrand(g);
}

void InspiratoRAudioProcessorEditor::drawFooterBrand(juce::Graphics& g)
{
    g.setColour(Palette::edge().withAlpha(0.52f));
    g.drawLine(22,642,1198,642,1.0f);

    {
        juce::Graphics::ScopedSaveState save(g);
        const float x = 516.0f;
        const float y = 656.0f;
        const float s = 32.0f;

        g.addTransform(juce::AffineTransform::rotation(0.13f, x+s*0.5f, y+s*0.5f));
        g.setColour(Palette::gold().withAlpha(0.06f));
        g.fillEllipse(x-8,y-8,s+16,s+16);
        g.setColour(juce::Colours::black.withAlpha(0.74f));
        g.fillRect(x+3,y+4,s,s);
        g.setColour(juce::Colour(0xff7a7566));
        g.fillRect(x,y,s,s);
        g.setColour(juce::Colour(0xffc5bba2));
        g.fillRect(x+2,y+2,s-4,s-4);

        constexpr std::array<juce::Point<float>, 5> pp {
            juce::Point<float>(0.28f,0.28f), juce::Point<float>(0.72f,0.28f), juce::Point<float>(0.50f,0.50f),
            juce::Point<float>(0.28f,0.72f), juce::Point<float>(0.72f,0.72f)
        };
        const int pulse = processor.getLogoPulse();
        const float pr = s * 0.085f;
        for (int i = 0; i < 5; ++i)
        {
            const auto c = juce::Point<float>(x+s*pp[(size_t)i].x, y+s*pp[(size_t)i].y);
            const bool on = i == pulse;
            if (on)
            {
                g.setColour(Palette::gold().withAlpha(0.22f));
                g.fillEllipse(c.x-pr*2.0f,c.y-pr*2.0f,pr*4.0f,pr*4.0f);
                g.setColour(Palette::goldHi());
            }
            else
            {
                g.setColour(juce::Colour(0xff17120a));
            }
            g.fillEllipse(c.x-pr,c.y-pr,pr*2.0f,pr*2.0f);
        }
    }

    g.setColour(juce::Colours::black.withAlpha(0.80f));
    g.setFont(juce::Font(juce::FontOptions("Trebuchet MS", 21.0f, juce::Font::plain)));
    g.drawText("Inspirato", 569,660,100,28,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffd4bf94));
    g.drawText("Inspirato", 568,658,100,28,juce::Justification::centredLeft);

    g.setColour(juce::Colour(0xffe6c276));
    g.setFont(juce::Font(juce::FontOptions("Times New Roman", 27.0f, juce::Font::plain)));
    g.drawText(juce::String::fromUTF8("ℛ"), 646,657,40,32,juce::Justification::centredLeft);
}

void InspiratoRAudioProcessorEditor::NoteFader::paint(juce::Graphics& g)
{
    juce::Slider::paint(g);

    const auto proportion = (float) valueToProportionOfLength(getValue());
    const auto y = getHeight() - 10.0f - proportion * (getHeight() - 20.0f);
    const auto x = getWidth() * 0.5f;
    const bool on = (processor.getDisplayPitchMask() & (1u << pc)) != 0;

    g.setColour(juce::Colours::black.withAlpha(0.95f));
    g.fillEllipse(x-6.8f,y-6.8f,13.6f,13.6f);
    g.setColour(on ? Palette::gold() : juce::Colour(0xff090d0b));
    g.fillEllipse(x-5.5f,y-5.5f,11.0f,11.0f);
    if (on)
    {
        g.setColour(Palette::gold().withAlpha(0.40f));
        g.drawEllipse(x-8.2f,y-8.2f,16.4f,16.4f,1.3f);
        g.setColour(juce::Colour(0xfffff2bd).withAlpha(0.85f));
        g.fillEllipse(x-3.0f,y-3.0f,3.0f,3.0f);
    }
}

void InspiratoRAudioProcessorEditor::StepDial::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto c = juce::Point<float>(bounds.getCentreX(), 62.0f);
    constexpr float ringRadius = 54.0f;
    constexpr float knobRadius = 32.0f;

    g.setColour(Palette::gold().withAlpha(0.20f));
    g.drawEllipse(c.x-ringRadius-8,c.y-ringRadius-8,(ringRadius+8)*2,(ringRadius+8)*2,1.0f);
    g.setColour(juce::Colour(0xff2a2410).withAlpha(0.55f));
    g.drawEllipse(c.x-ringRadius+9,c.y-ringRadius+9,(ringRadius-9)*2,(ringRadius-9)*2,1.0f);

    const int activeStep = processor.getDisplayStep();
    const int selectedSteps = juce::roundToInt(getValue());
    for (int i = 0; i < 16; ++i)
    {
        const auto a = -juce::MathConstants<float>::halfPi + i * juce::MathConstants<float>::twoPi / 16.0f;
        const auto p = c + juce::Point<float>(std::cos(a), std::sin(a)) * ringRadius;
        const bool enabled = i < selectedSteps;
        const bool current = enabled && i == activeStep;
        const bool quarter = (i % 4) == 0;
        auto colour = current ? Palette::goldHi()
                              : enabled ? (quarter ? Palette::gold().withAlpha(0.58f) : Palette::gold().withAlpha(0.25f))
                                        : juce::Colour(0xff090a0a);
        g.setColour(juce::Colours::black.withAlpha(0.90f));
        g.fillEllipse(p.x-5.4f,p.y-5.4f,10.8f,10.8f);
        g.setColour(colour);
        g.fillEllipse(p.x-4.25f,p.y-4.25f,8.5f,8.5f);
        if (current)
        {
            g.setColour(Palette::gold().withAlpha(0.38f));
            g.drawEllipse(p.x-7.8f,p.y-7.8f,15.6f,15.6f,1.35f);
        }
    }

    const float start = juce::degreesToRadians(135.0f);
    const float end = juce::degreesToRadians(405.0f);
    const float pos = (float) valueToProportionOfLength(getValue());
    inspirato_ui::HardwareLookAndFeel::drawHardwareKnob(g, c, knobRadius, start + pos * (end-start));

    g.setColour(Palette::gold());
    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    g.drawText("STEPS", 0,124,getWidth(),16,juce::Justification::centred);
}

void InspiratoRAudioProcessorEditor::timerCallback()
{
    repaint();
}
