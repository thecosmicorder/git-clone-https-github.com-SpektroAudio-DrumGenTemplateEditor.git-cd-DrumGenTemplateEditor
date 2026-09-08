#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String weightId(int i) { return "w" + juce::String(i); }

float raw(const juce::AudioProcessorValueTreeState& s, const juce::String& id)
{
    if (auto* p = s.getRawParameterValue(id)) return p->load(std::memory_order_relaxed);
    return 0.0f;
}
}

InspiratoRAudioProcessor::InspiratoRAudioProcessor()
    : AudioProcessor(BusesProperties()),
      apvts(*this, nullptr, "STATE", createParameterLayout())
{
}

InspiratoRAudioProcessor::~InspiratoRAudioProcessor()
{
    cancelPendingUpdate();
}

juce::AudioProcessorValueTreeState::ParameterLayout InspiratoRAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    constexpr std::array<int, 12> defaults { 100, 0, 20, 0, 70, 35, 0, 65, 0, 25, 0, 55 };
    constexpr std::array<const char*, 12> names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    for (int i = 0; i < 12; ++i)
        layout.add(std::make_unique<AudioParameterInt>(ParameterID { weightId(i), 1 },
                                                       "Probability " + String(names[(size_t) i]),
                                                       0, 100, defaults[(size_t) i]));

    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "rate", 1 }, "Note Value",
                                                       StringArray { "1/4", "1/8", "1/16", "1/32" }, 2));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "variation", 1 }, "Rhythm Variation", -100, 100, 0));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "legato", 1 }, "Legato Probability", 0, 100, 15));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "rest", 1 }, "Rest Probability", 0, 100, 10));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "rrange", 1 }, "Rhythm Dice Range", 0, 100, 50));

    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "low", 1 }, "Low Octave", -2, 0, -1));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "high", 1 }, "High Octave", 0, 3, 1));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "transpose", 1 }, "Transpose", -12, 12, 0));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "steps", 1 }, "Steps", 1, 16, 16));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "direction", 1 }, "Direction",
                                                       StringArray { "FWD", "BACK", "PING", "RND" }, 0));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "velo", 1 }, "Velo",
                                                       StringArray { "LOW", "MID", "HIGH" }, 1));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "accent", 1 }, "Accent Probability", 0, 100, 18));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "gate", 1 }, "Gate Length", 15, 95, 72));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "mrange", 1 }, "Melody Dice Range", 0, 100, 50));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { "poly", 1 }, "Polyphony", 1, 4, 1));

    layout.add(std::make_unique<AudioParameterBool>(ParameterID { "run", 1 }, "Run", true));
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { "lock", 1 }, "Pattern Lock", false));

    return layout;
}

void InspiratoRAudioProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
    absoluteSampleCursor = 0;
    nextTickSample = 0;
    wasPlaying = false;
    pendingNoteOffs.clear();
    displayStep.store(0);
    displayPitchMask.store(0);
    logoPulse.store(0);
    engine.restart();
}

void InspiratoRAudioProcessor::releaseResources() {}

bool InspiratoRAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet().isDisabled()
        && layouts.getMainOutputChannelSet().isDisabled();
}

double InspiratoRAudioProcessor::quarterNotesForRateIndex(int rateIndex)
{
    static constexpr std::array<double, 4> values { 1.0, 0.5, 0.25, 0.125 };
    return values[(size_t) juce::jlimit(0, 3, rateIndex)];
}

void InspiratoRAudioProcessor::syncEngineFromParameters()
{
    const bool holdDiceValues = pendingParameterPush.load(std::memory_order_acquire);

    for (int i = 0; i < 12; ++i)
        if (!holdDiceValues)
            engine.setWeight(i, juce::roundToInt(raw(apvts, weightId(i))));

    if (!holdDiceValues)
    {
        engine.setRate(juce::roundToInt(raw(apvts, "rate")));
        engine.setVariation(juce::roundToInt(raw(apvts, "variation")));
        engine.setLegato(juce::roundToInt(raw(apvts, "legato")));
        engine.setRest(juce::roundToInt(raw(apvts, "rest")));
        engine.setTranspose(juce::roundToInt(raw(apvts, "transpose")));
        engine.setAccent(juce::roundToInt(raw(apvts, "accent")));
        engine.setGate(juce::roundToInt(raw(apvts, "gate")));
        engine.setPolyphony(juce::roundToInt(raw(apvts, "poly")));
        engine.setVeloMode(juce::roundToInt(raw(apvts, "velo")));
    }

    engine.setLowOctave(juce::roundToInt(raw(apvts, "low")));
    engine.setHighOctave(juce::roundToInt(raw(apvts, "high")));
    engine.setSteps(juce::roundToInt(raw(apvts, "steps")));
    engine.setDirection(juce::roundToInt(raw(apvts, "direction")));
    engine.setMelodyDiceRange(juce::roundToInt(raw(apvts, "mrange")));
    engine.setRhythmDiceRange(juce::roundToInt(raw(apvts, "rrange")));
    engine.setLocked(raw(apvts, "lock") >= 0.5f);
}

void InspiratoRAudioProcessor::consumeActions(std::int64_t blockStart)
{
    bool changedParams = false;

    if (actionDiceMelody.exchange(false, std::memory_order_acq_rel))
    {
        engine.diceMelody();
        changedParams = true;
    }
    if (actionDiceRhythm.exchange(false, std::memory_order_acq_rel))
    {
        engine.diceRhythm();
        changedParams = true;
    }
    if (actionDejaVu.exchange(false, std::memory_order_acq_rel))
    {
        engine.composeDejaVu();
        changedParams = true;
    }
    if (actionRestart.exchange(false, std::memory_order_acq_rel))
    {
        engine.restart();
        nextTickSample = blockStart;
        displayStep.store(engine.getCurrentStep(), std::memory_order_release);
        displayPitchMask.store(0, std::memory_order_release);
    }

    if (changedParams)
        queueEngineParameterPush();
}

void InspiratoRAudioProcessor::queueEngineParameterPush()
{
    EngineParamSnapshot s;
    s.weights = engine.getWeights();
    s.rate = engine.getRate();
    s.variation = engine.getVariation();
    s.legato = engine.getLegato();
    s.rest = engine.getRest();
    s.transpose = engine.getTranspose();
    s.accent = engine.getAccent();
    s.gate = engine.getGate();
    s.polyphony = engine.getPolyphony();
    s.veloMode = engine.getVeloMode();

    {
        const juce::SpinLock::ScopedLockType lock(snapshotLock);
        pendingSnapshot = s;
    }

    pendingParameterPush.store(true, std::memory_order_release);
    triggerAsyncUpdate();
}

void InspiratoRAudioProcessor::setParameterPlain(const juce::String& id, float value)
{
    if (auto* p = apvts.getParameter(id))
    {
        const auto normalised = p->convertTo0to1(value);
        p->beginChangeGesture();
        p->setValueNotifyingHost(normalised);
        p->endChangeGesture();
    }
}

void InspiratoRAudioProcessor::handleAsyncUpdate()
{
    EngineParamSnapshot s;
    {
        const juce::SpinLock::ScopedLockType lock(snapshotLock);
        s = pendingSnapshot;
    }

    for (int i = 0; i < 12; ++i) setParameterPlain(weightId(i), (float) s.weights[(size_t) i]);
    setParameterPlain("rate", (float) s.rate);
    setParameterPlain("variation", (float) s.variation);
    setParameterPlain("legato", (float) s.legato);
    setParameterPlain("rest", (float) s.rest);
    setParameterPlain("transpose", (float) s.transpose);
    setParameterPlain("accent", (float) s.accent);
    setParameterPlain("gate", (float) s.gate);
    setParameterPlain("poly", (float) s.polyphony);
    setParameterPlain("velo", (float) s.veloMode);

    pendingParameterPush.store(false, std::memory_order_release);
}

void InspiratoRAudioProcessor::flushGeneratedNotes(juce::MidiBuffer& midi, int sampleOffset)
{
    for (const auto& off : pendingNoteOffs)
        midi.addEvent(juce::MidiMessage::noteOff(1, off.pitch), sampleOffset);
    pendingNoteOffs.clear();
    displayPitchMask.store(0, std::memory_order_release);
}

void InspiratoRAudioProcessor::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();

    const auto numSamples = audio.getNumSamples() > 0 ? audio.getNumSamples() : getBlockSize();
    const auto blockStart = absoluteSampleCursor;
    const auto blockEnd = blockStart + numSamples;

    syncEngineFromParameters();
    consumeActions(blockStart);

    double bpm = 120.0;
    bool hostPlaying = false;
    if (auto* playHead = getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = juce::jlimit(20.0, 400.0, *b);
            hostPlaying = pos->getIsPlaying();
        }
    }

    const bool run = raw(apvts, "run") >= 0.5f;
    const bool playing = hostPlaying && run;
    transportRunning.store(playing, std::memory_order_release);

    if (!playing)
    {
        if (wasPlaying || !pendingNoteOffs.empty())
            flushGeneratedNotes(midi, 0);
        wasPlaying = false;
        absoluteSampleCursor += numSamples;
        nextTickSample = absoluteSampleCursor;
        return;
    }

    if (!wasPlaying)
    {
        engine.restart();
        nextTickSample = blockStart;
        displayStep.store(engine.getCurrentStep(), std::memory_order_release);
        displayPitchMask.store(0, std::memory_order_release);
    }
    wasPlaying = true;

    for (auto it = pendingNoteOffs.begin(); it != pendingNoteOffs.end(); )
    {
        if (it->absoluteSample < blockEnd)
        {
            const int offset = juce::jlimit(0, numSamples - 1, (int) (it->absoluteSample - blockStart));
            midi.addEvent(juce::MidiMessage::noteOff(1, it->pitch), offset);
            it = pendingNoteOffs.erase(it);
        }
        else
        {
            ++it;
        }
    }

    const double quarterSamples = currentSampleRate * 60.0 / bpm;

    while (nextTickSample < blockEnd)
    {
        const int offset = juce::jlimit(0, numSamples - 1, (int) (nextTickSample - blockStart));
        auto tick = engine.tick();

        displayStep.store(tick.sequenceStep, std::memory_order_release);
        displayPitchMask.store(tick.pitchMask, std::memory_order_release);
        logoPulse.store((logoPulse.load(std::memory_order_relaxed) + 1) % 5, std::memory_order_release);

        const double stepSamples = quarterSamples * quarterNotesForRateIndex(tick.rateIndex);
        const auto durationSamples = (std::int64_t) std::max(8.0, stepSamples * tick.durationScale);

        for (const auto& note : tick.notes)
        {
            midi.addEvent(juce::MidiMessage::noteOn(1, note.pitch, (juce::uint8) note.velocity), offset);
            const auto offAbs = nextTickSample + durationSamples;
            if (offAbs < blockEnd)
            {
                const int offOffset = juce::jlimit(offset, numSamples - 1, (int) (offAbs - blockStart));
                midi.addEvent(juce::MidiMessage::noteOff(1, note.pitch), offOffset);
            }
            else
            {
                pendingNoteOffs.push_back({ note.pitch, offAbs });
            }
        }

        nextTickSample += std::max<std::int64_t>(1, (std::int64_t) std::llround(stepSamples));
    }

    absoluteSampleCursor += numSamples;
}

juce::AudioProcessorEditor* InspiratoRAudioProcessor::createEditor()
{
    return new InspiratoRAudioProcessorEditor(*this);
}

void InspiratoRAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void InspiratoRAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new InspiratoRAudioProcessor();
}
