#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <vector>
#include <chrono>

namespace inspirato
{
struct MidiNote
{
    int pitch = 60;
    int velocity = 96;
};

struct TickResult
{
    int sequenceStep = 0;
    int rateIndex = 2;
    bool isRest = false;
    bool legato = false;
    bool accent = false;
    double durationScale = 0.72;
    int rootPitch = 60;
    std::uint16_t pitchMask = 0;
    std::vector<MidiNote> notes;
};

class Engine
{
public:
    Engine();

    void seed(std::uint32_t value);
    void setWeight(int index, int value);
    void setRate(int value);
    void setVariation(int value);
    void setLegato(int value);
    void setRest(int value);
    void setLowOctave(int value);
    void setHighOctave(int value);
    void setTranspose(int value);
    void setSteps(int value);
    void setDirection(int value);
    void setAccent(int value);
    void setGate(int value);
    void setMelodyDiceRange(int value);
    void setRhythmDiceRange(int value);
    void setPolyphony(int voices);
    void setVeloMode(int mode);
    void setLocked(bool shouldLock);

    void restart();
    void diceMelody();
    void diceRhythm();
    void composeDejaVu();

    TickResult tick();

    const std::array<int, 12>& getWeights() const noexcept { return weights; }
    int getCurrentStep() const noexcept { return playPos; }
    int getSteps() const noexcept { return steps; }
    int getDirection() const noexcept { return direction; }
    int getPolyphony() const noexcept { return polyMode + 1; }
    int getVeloMode() const noexcept { return veloMode; }
    int getRate() const noexcept { return baseRate; }
    int getVariation() const noexcept { return variation; }
    int getLegato() const noexcept { return legatoProb; }
    int getRest() const noexcept { return restProb; }
    int getLowOctave() const noexcept { return lowOct; }
    int getHighOctave() const noexcept { return highOct; }
    int getTranspose() const noexcept { return transpose; }
    int getAccent() const noexcept { return accentProb; }
    int getGate() const noexcept { return gatePct; }
    int getMelodyDiceRange() const noexcept { return melodyDiceRange; }
    int getRhythmDiceRange() const noexcept { return rhythmDiceRange; }
    bool isLocked() const noexcept { return locked; }
    bool isComposerActive() const noexcept { return composerMode; }

private:
    struct Event
    {
        int pc = 0;
        int oct = 0;
        bool rest = false;
        bool leg = false;
        int rate = 2;
        bool accent = false;
        bool ghost = false;
        int velocityOverride = -1;
        int velocityRole = -1;
    };

    static int clampInt(int v, int lo, int hi);
    static double clampDouble(double v, double lo, double hi);
    int randInt(int lo, int hi);
    double rand01();
    bool chance(double percent);

    int octaveLo() const;
    int octaveHi() const;
    int chooseWeighted();
    int highestWeightPc() const;
    int chooseOct();
    int chooseRate();
    Event makeEvent();
    void normaliseEventOctave(Event& ev);
    void normalisePatternOctaves();
    int fitPitchToRange(int pc, int oct) const;
    void ensurePattern();
    void clearTail();
    void resetPositionForDirection();
    void advance();

    std::vector<int> activePcs() const;
    int candidateScore(int root, int pc) const;
    std::vector<int> chordPcs(int voices) const;
    std::vector<int> allRangePitchesForPc(int pc) const;
    std::vector<int> chordPitches(const Event& ev) const;
    static std::uint16_t pitchMaskFor(const std::vector<int>& pitches);

    std::array<int, 2> velocityBounds() const;
    int clampVelocityToMode(int value) const;
    int velocityForEvent(const Event& ev);
    int velocityForVoice(int baseVelocity, int voiceIndex) const;
    int accentedVelocity(int baseVelocity) const;

    std::vector<int> selectedScalePcs() const;
    int scaleNeighborPc(int pc, int dirAmount) const;
    int chooseComposerPoly();
    int chooseComposerVelo();
    double randomAround(double current, double minv, double maxv, double depth);
    void applyMelodyDiceCore();
    void applyRhythmDiceCore();
    int composerVelocity(int role);
    int scaleIndexOf(int pc) const;
    int moveScaleSteps(int pc, int amount) const;
    int composerMotifLength() const;
    void makeFreshMotif();
    void refreshRememberedMotif();
    int composerSectionForStep(int i) const;
    int phrasePc(int i) const;
    int phraseRole(int i) const;
    void applyComposerDiceCores();
    Event composerEventForStep(int i);
    void composePhrase();

    std::array<int, 12> weights { 100, 0, 20, 0, 70, 35, 0, 65, 0, 25, 0, 55 };

    int baseRate = 2;
    int variation = 0;
    int legatoProb = 15;
    int restProb = 10;
    int lowOct = -1;
    int highOct = 1;
    int transpose = 0;
    int steps = 16;
    int direction = 0;
    int accentProb = 18;
    int gatePct = 72;
    int melodyDiceRange = 50;
    int rhythmDiceRange = 50;
    int polyMode = 0;
    int veloMode = 1;
    bool locked = false;
    bool composerMode = false;

    int playPos = 0;
    int pingDir = 1;

    std::array<Event, 16> pattern {};
    std::array<bool, 16> patternValid {};

    std::vector<int> motifMemory;
    std::vector<int> motifRoles;
    int motifLength = 4;
    int motifAge = 0;
    int motifMutationIndex = 1;
    int motifDirection = 1;
    int composerLastPc = 0;

    std::mt19937 rng;
    std::chrono::steady_clock::time_point lastMelodyDice {};
    std::chrono::steady_clock::time_point lastRhythmDice {};
    std::chrono::steady_clock::time_point lastCompose {};
};
} // namespace inspirato
