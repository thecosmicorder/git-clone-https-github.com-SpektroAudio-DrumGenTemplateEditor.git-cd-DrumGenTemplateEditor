#pragma once

#include <JuceHeader.h>
#include "InspiratoEngine.h"

#include <atomic>
#include <vector>

class InspiratoRAudioProcessor final : public juce::AudioProcessor,
                                       private juce::AsyncUpdater
{
public:
    InspiratoRAudioProcessor();
    ~InspiratoRAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    void triggerDiceMelody() noexcept { actionDiceMelody.store(true, std::memory_order_release); }
    void triggerDiceRhythm() noexcept { actionDiceRhythm.store(true, std::memory_order_release); }
    void triggerDejaVu() noexcept { actionDejaVu.store(true, std::memory_order_release); }
    void triggerRestart() noexcept { actionRestart.store(true, std::memory_order_release); }

    int getDisplayStep() const noexcept { return displayStep.load(std::memory_order_acquire); }
    std::uint16_t getDisplayPitchMask() const noexcept { return displayPitchMask.load(std::memory_order_acquire); }
    int getLogoPulse() const noexcept { return logoPulse.load(std::memory_order_acquire); }
    bool getTransportRunning() const noexcept { return transportRunning.load(std::memory_order_acquire); }

private:
    struct PendingNoteOff
    {
        int pitch = 60;
        std::int64_t absoluteSample = 0;
    };

    struct EngineParamSnapshot
    {
        std::array<int, 12> weights {};
        int rate = 2;
        int variation = 0;
        int legato = 15;
        int rest = 10;
        int transpose = 0;
        int accent = 18;
        int gate = 72;
        int polyphony = 1;
        int veloMode = 1;
    };

    void syncEngineFromParameters();
    void consumeActions(std::int64_t blockStart);
    void queueEngineParameterPush();
    void handleAsyncUpdate() override;
    void setParameterPlain(const juce::String& id, float value);
    void flushGeneratedNotes(juce::MidiBuffer& midi, int sampleOffset);

    static double quarterNotesForRateIndex(int rateIndex);

    inspirato::Engine engine;
    double currentSampleRate = 44100.0;
    std::int64_t absoluteSampleCursor = 0;
    std::int64_t nextTickSample = 0;
    bool wasPlaying = false;
    std::vector<PendingNoteOff> pendingNoteOffs;

    std::atomic<bool> actionDiceMelody { false };
    std::atomic<bool> actionDiceRhythm { false };
    std::atomic<bool> actionDejaVu { false };
    std::atomic<bool> actionRestart { false };
    std::atomic<bool> pendingParameterPush { false };

    juce::SpinLock snapshotLock;
    EngineParamSnapshot pendingSnapshot;

    std::atomic<int> displayStep { 0 };
    std::atomic<std::uint16_t> displayPitchMask { 0 };
    std::atomic<int> logoPulse { 0 };
    std::atomic<bool> transportRunning { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspiratoRAudioProcessor)
};
