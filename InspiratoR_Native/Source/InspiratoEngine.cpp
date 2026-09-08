#include "InspiratoEngine.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace inspirato
{
namespace
{
constexpr std::array<double, 4> rateQuarterNotes { 1.0, 0.5, 0.25, 0.125 };
}

Engine::Engine()
    : rng(std::random_device{}())
{
    ensurePattern();
}

void Engine::seed(std::uint32_t value) { rng.seed(value); }

int Engine::clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
double Engine::clampDouble(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

int Engine::randInt(int lo, int hi)
{
    if (hi < lo) std::swap(lo, hi);
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

double Engine::rand01()
{
    return std::generate_canonical<double, 53>(rng);
}

bool Engine::chance(double percent)
{
    return rand01() * 100.0 < clampDouble(percent, 0.0, 100.0);
}

int Engine::octaveLo() const { return std::min(lowOct, highOct); }
int Engine::octaveHi() const { return std::max(lowOct, highOct); }

int Engine::chooseWeighted()
{
    int sum = 0;
    for (auto w : weights) sum += std::max(0, w);
    if (sum <= 0) return 0;

    std::uniform_int_distribution<int> dist(1, sum);
    int r = dist(rng);
    for (int i = 0; i < 12; ++i)
    {
        r -= std::max(0, weights[(size_t) i]);
        if (r <= 0) return i;
    }
    return 11;
}

int Engine::highestWeightPc() const
{
    int best = 0;
    int bestW = -1;
    for (int i = 0; i < 12; ++i)
    {
        const auto w = std::max(0, weights[(size_t) i]);
        if (w > bestW)
        {
            bestW = w;
            best = i;
        }
    }
    return best;
}

int Engine::chooseOct() { return randInt(octaveLo(), octaveHi()); }

int Engine::chooseRate()
{
    int idx = clampInt(baseRate, 0, 3);
    const auto mag = std::abs(variation);
    if (mag > 0 && chance((double) mag))
    {
        const int amount = (mag > 72 && chance(45.0)) ? 2 : 1;
        idx += variation > 0 ? amount : -amount;
    }
    return clampInt(idx, 0, 3);
}

Engine::Event Engine::makeEvent()
{
    Event ev;
    ev.pc = chooseWeighted();
    ev.oct = chooseOct();
    ev.rest = chance(restProb);
    ev.leg = chance(legatoProb);
    ev.rate = chooseRate();
    ev.accent = chance(accentProb);
    return ev;
}

void Engine::normaliseEventOctave(Event& ev)
{
    if (ev.oct < octaveLo() || ev.oct > octaveHi())
        ev.oct = chooseOct();
}

void Engine::normalisePatternOctaves()
{
    for (int i = 0; i < steps; ++i)
        if (patternValid[(size_t) i])
            normaliseEventOctave(pattern[(size_t) i]);
}

int Engine::fitPitchToRange(int pc, int oct) const
{
    const int lo = octaveLo();
    const int hi = octaveHi();
    oct = clampInt(oct, lo, hi);

    int pitch = 60 + transpose + pc + (12 * oct);
    const int minPitch = 60 + (12 * lo);
    const int maxPitch = 71 + (12 * hi);

    while (pitch < minPitch) pitch += 12;
    while (pitch > maxPitch) pitch -= 12;
    return clampInt(pitch, minPitch, maxPitch);
}

void Engine::ensurePattern()
{
    for (int i = 0; i < steps; ++i)
    {
        if (!patternValid[(size_t) i])
        {
            pattern[(size_t) i] = makeEvent();
            patternValid[(size_t) i] = true;
        }
        else
        {
            normaliseEventOctave(pattern[(size_t) i]);
        }
    }
}

void Engine::clearTail()
{
    for (int i = steps; i < 16; ++i)
        patternValid[(size_t) i] = false;
}

void Engine::resetPositionForDirection()
{
    pingDir = 1;
    playPos = direction == 1 ? std::max(0, steps - 1) : 0;
}

void Engine::advance()
{
    if (direction == 0)
        playPos = (playPos + 1) % steps;
    else if (direction == 1)
        playPos = (playPos - 1 + steps) % steps;
    else if (direction == 2)
    {
        if (steps <= 1)
        {
            playPos = 0;
            return;
        }

        playPos += pingDir;
        if (playPos >= steps - 1)
        {
            playPos = steps - 1;
            pingDir = -1;
        }
        else if (playPos <= 0)
        {
            playPos = 0;
            pingDir = 1;
        }
    }
    else
    {
        playPos = randInt(0, steps - 1);
    }
}

std::vector<int> Engine::activePcs() const
{
    std::vector<int> a;
    for (int i = 0; i < 12; ++i)
        if (weights[(size_t) i] > 0)
            a.push_back(i);
    if (a.empty()) a.push_back(0);
    return a;
}

int Engine::candidateScore(int root, int pc) const
{
    const int iv = (pc - root + 12) % 12;
    int bonus = 0;
    if (iv == 7) bonus = 44;
    else if (iv == 3 || iv == 4) bonus = 38;
    else if (iv == 10 || iv == 11) bonus = 25;
    else if (iv == 5 || iv == 2 || iv == 9) bonus = 12;
    else if (iv == 8) bonus = 5;
    else bonus = -10;
    return std::max(0, weights[(size_t) pc]) + bonus;
}

std::vector<int> Engine::chordPcs(int voices) const
{
    const int root = highestWeightPc();
    std::vector<int> out { root };
    if (voices <= 1) return out;

    auto act = activePcs();
    act.erase(std::remove(act.begin(), act.end(), root), act.end());
    std::sort(act.begin(), act.end(), [this, root](int a, int b)
    {
        return candidateScore(root, a) > candidateScore(root, b);
    });

    for (auto pc : act)
    {
        if ((int) out.size() >= voices) break;
        out.push_back(pc);
    }

    const int minorW = weights[(size_t) ((root + 3) % 12)];
    const int majorW = weights[(size_t) ((root + 4) % 12)];
    const int third = minorW > majorW ? 3 : 4;
    const int seventh = weights[(size_t) ((root + 10) % 12)] > weights[(size_t) ((root + 11) % 12)] ? 10 : 11;
    const std::array<int, 6> fallback { third, 7, seventh, 2, 5, 9 };

    for (auto interval : fallback)
    {
        if ((int) out.size() >= voices) break;
        const int pc = (root + interval) % 12;
        if (std::find(out.begin(), out.end(), pc) == out.end())
            out.push_back(pc);
    }

    if ((int) out.size() > voices) out.resize((size_t) voices);
    return out;
}

std::vector<int> Engine::allRangePitchesForPc(int pc) const
{
    std::vector<int> vals;
    for (int o = octaveLo(); o <= octaveHi(); ++o)
    {
        const int p = fitPitchToRange(pc, o);
        if (std::find(vals.begin(), vals.end(), p) == vals.end())
            vals.push_back(p);
    }
    std::sort(vals.begin(), vals.end());
    return vals;
}

std::vector<int> Engine::chordPitches(const Event& ev) const
{
    const int voices = clampInt(polyMode, 0, 3) + 1;
    if (voices <= 1)
        return { fitPitchToRange(ev.pc, ev.oct) };

    const auto pcs = chordPcs(voices);
    std::vector<int> result;
    const int rootPitch = fitPitchToRange(pcs.front(), ev.oct);
    result.push_back(rootPitch);

    int prev = rootPitch;
    for (size_t i = 1; i < pcs.size() && (int) result.size() < voices; ++i)
    {
        auto choices = allRangePitchesForPc(pcs[i]);
        int chosen = -1;

        for (auto p : choices)
            if (p > prev && std::find(result.begin(), result.end(), p) == result.end())
            {
                chosen = p;
                break;
            }

        if (chosen < 0)
            for (auto p : choices)
                if (std::find(result.begin(), result.end(), p) == result.end())
                {
                    chosen = p;
                    break;
                }

        if (chosen >= 0)
        {
            result.push_back(chosen);
            prev = chosen;
        }
    }

    if ((int) result.size() < voices)
    {
        const int lo = 60 + 12 * octaveLo();
        const int hi = 71 + 12 * octaveHi();
        constexpr std::array<int, 8> intervals { 12, -12, 7, 4, 3, 19, -5, 16 };
        for (auto interval : intervals)
        {
            if ((int) result.size() >= voices) break;
            int pp = rootPitch + interval;
            while (pp < lo) pp += 12;
            while (pp > hi) pp -= 12;
            if (pp >= lo && pp <= hi && std::find(result.begin(), result.end(), pp) == result.end())
                result.push_back(pp);
        }
    }

    if ((int) result.size() > voices) result.resize((size_t) voices);
    return result;
}

std::uint16_t Engine::pitchMaskFor(const std::vector<int>& pitches)
{
    std::uint16_t mask = 0;
    for (auto pitch : pitches)
    {
        const int pc = ((pitch % 12) + 12) % 12;
        mask = (std::uint16_t) (mask | (std::uint16_t) (1u << pc));
    }
    return mask;
}

std::array<int, 2> Engine::velocityBounds() const
{
    if (veloMode == 0) return { 20, 64 };
    if (veloMode == 2) return { 64, 127 };
    return { 20, 127 };
}

int Engine::clampVelocityToMode(int value) const
{
    const auto b = velocityBounds();
    return clampInt(value, b[0], b[1]);
}

int Engine::velocityForEvent(const Event& ev)
{
    if (ev.velocityRole >= 0)
        return clampVelocityToMode(composerVelocity(ev.velocityRole));
    if (ev.velocityOverride >= 0)
        return clampVelocityToMode(ev.velocityOverride);

    if (veloMode == 0) return randInt(20, 64);
    if (veloMode == 2) return randInt(64, 127);

    const auto r = rand01();
    if (r < 0.20) return randInt(20, 52);
    if (r < 0.76) return randInt(53, 102);
    return randInt(103, 127);
}

int Engine::velocityForVoice(int baseVelocity, int voiceIndex) const
{
    return clampVelocityToMode(baseVelocity - voiceIndex * 2);
}

int Engine::accentedVelocity(int baseVelocity) const
{
    return clampVelocityToMode(baseVelocity + 12);
}

std::vector<int> Engine::selectedScalePcs() const { return activePcs(); }

int Engine::scaleNeighborPc(int pc, int dirAmount) const
{
    const auto scale = selectedScalePcs();
    if (scale.size() <= 1) return scale.front();

    int bestIndex = 0;
    int bestDist = 99;
    for (int i = 0; i < (int) scale.size(); ++i)
    {
        const int d = std::abs(((scale[(size_t) i] - pc + 18) % 12) - 6);
        if (d < bestDist)
        {
            bestDist = d;
            bestIndex = i;
        }
    }

    const int move = dirAmount >= 0 ? 1 : -1;
    const int n = (int) scale.size();
    return scale[(size_t) ((bestIndex + move + n) % n)];
}

int Engine::chooseComposerPoly()
{
    const int active = (int) selectedScalePcs().size();
    const int maxVoices = std::min(4, std::max(1, active));
    const double r = rand01();
    int voices = 1;

    if (maxVoices == 2) voices = r < 0.58 ? 1 : 2;
    else if (maxVoices == 3) voices = r < 0.32 ? 1 : (r < 0.74 ? 2 : 3);
    else if (maxVoices >= 4) voices = r < 0.22 ? 1 : (r < 0.58 ? 2 : (r < 0.84 ? 3 : 4));

    polyMode = voices - 1;
    return voices;
}

int Engine::chooseComposerVelo()
{
    const auto r = rand01();
    veloMode = r < 0.20 ? 0 : (r < 0.78 ? 1 : 2);
    return veloMode;
}

double Engine::randomAround(double current, double minv, double maxv, double depth)
{
    depth = clampDouble(depth, 0.0, 1.0);
    if (depth <= 0.0) return current;
    const auto radius = (maxv - minv) * depth;
    const auto delta = (rand01() * 2.0 - 1.0) * radius;
    return clampDouble(current + delta, minv, maxv);
}

void Engine::applyMelodyDiceCore()
{
    const double depth = melodyDiceRange / 100.0;
    if (depth <= 0.0) return;

    for (int i = 0; i < 12; ++i)
        if (weights[(size_t) i] > 0)
            weights[(size_t) i] = (int) std::lround(randomAround(weights[(size_t) i], 1.0, 100.0, depth));

    const int maxTransDelta = (int) std::lround(12.0 * depth);
    if (maxTransDelta > 0)
        transpose = clampInt(transpose + randInt(-maxTransDelta, maxTransDelta), -12, 12);
}

void Engine::applyRhythmDiceCore()
{
    const double depth = rhythmDiceRange / 100.0;
    if (depth <= 0.0) return;

    const int rateRadius = std::max(1, (int) std::lround(3.0 * depth));
    baseRate = clampInt(baseRate + randInt(-rateRadius, rateRadius), 0, 3);
    variation = (int) std::lround(randomAround(variation, -100.0, 100.0, depth));
    legatoProb = (int) std::lround(randomAround(legatoProb, 0.0, 100.0, depth));
    restProb = (int) std::lround(randomAround(restProb, 0.0, 100.0, depth));
    accentProb = (int) std::lround(randomAround(accentProb, 0.0, 100.0, depth));
    gatePct = (int) std::lround(randomAround(gatePct, 15.0, 95.0, depth));
}

int Engine::composerVelocity(int role)
{
    if (veloMode == 0)
    {
        if (role == 0) return randInt(20, 34);
        if (role == 1) return randInt(28, 46);
        if (role == 2) return randInt(38, 56);
        return randInt(50, 64);
    }

    if (veloMode == 2)
    {
        if (role == 0) return randInt(64, 72);
        if (role == 1) return randInt(68, 86);
        if (role == 2) return randInt(82, 105);
        return randInt(102, 127);
    }

    if (role == 0) return randInt(20, 42);
    if (role == 1) return randInt(42, 76);
    if (role == 2) return randInt(68, 102);
    return randInt(92, 127);
}

int Engine::scaleIndexOf(int pc) const
{
    const auto scale = selectedScalePcs();
    const auto found = std::find(scale.begin(), scale.end(), pc);
    if (found != scale.end()) return (int) std::distance(scale.begin(), found);

    int best = 0;
    int bestDist = 99;
    for (int i = 0; i < (int) scale.size(); ++i)
    {
        const int up = (scale[(size_t) i] - pc + 12) % 12;
        const int dn = (pc - scale[(size_t) i] + 12) % 12;
        const int d = std::min(up, dn);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

int Engine::moveScaleSteps(int pc, int amount) const
{
    const auto scale = selectedScalePcs();
    if (scale.size() <= 1) return scale.front();
    const int idx = scaleIndexOf(pc);
    const int n = (int) scale.size();
    const int target = ((idx + amount) % n + n) % n;
    return scale[(size_t) target];
}

int Engine::composerMotifLength() const
{
    if (steps <= 2) return steps;
    if (steps <= 5) return 3;
    return 4;
}

void Engine::makeFreshMotif()
{
    motifLength = composerMotifLength();
    motifMemory.clear();
    motifRoles.clear();

    const int root = highestWeightPc();
    int cur = root;
    motifMemory.push_back(root);

    for (int i = 1; i < motifLength; ++i)
    {
        if (i == 1)
            cur = moveScaleSteps(cur, chance(68.0) ? 1 : -1);
        else if (i == 2)
            cur = chance(48.0) ? root : moveScaleSteps(cur, chance(60.0) ? 1 : -1);
        else
            cur = chance(55.0) ? moveScaleSteps(root, chance(50.0) ? 1 : -1) : root;
        motifMemory.push_back(cur);
    }

    constexpr std::array<int, 4> roles { 3, 0, 2, 1 };
    for (int i = 0; i < motifLength; ++i)
        motifRoles.push_back(roles[(size_t) i]);

    motifMutationIndex = motifLength > 1 ? randInt(1, motifLength - 1) : 0;
    motifDirection = chance(50.0) ? 1 : -1;
    motifAge = 1;
}

void Engine::refreshRememberedMotif()
{
    const auto scale = selectedScalePcs();
    motifLength = composerMotifLength();

    bool valid = (int) motifMemory.size() == motifLength;
    if (valid)
        for (auto pc : motifMemory)
            if (std::find(scale.begin(), scale.end(), pc) == scale.end())
            {
                valid = false;
                break;
            }

    if (!valid || motifAge <= 0)
    {
        makeFreshMotif();
        return;
    }

    const int root = highestWeightPc();
    motifMemory[0] = root;

    if (motifLength > 1)
    {
        motifMutationIndex = randInt(1, motifLength - 1);
        const int old = motifMemory[(size_t) motifMutationIndex];
        int next = moveScaleSteps(old, chance(50.0) ? 1 : -1);
        if (next == old && scale.size() > 1) next = moveScaleSteps(old, 1);
        motifMemory[(size_t) motifMutationIndex] = next;
    }

    if (motifLength > 2 && chance(34.0))
    {
        const int ri = randInt(1, motifLength - 1);
        constexpr std::array<int, 3> alternatives { 0, 1, 2 };
        const int oldRole = motifRoles[(size_t) ri];
        int candidate = alternatives[(size_t) randInt(0, 2)];
        if (candidate == oldRole)
        {
            const auto it = std::find(alternatives.begin(), alternatives.end(), candidate);
            const int idx = (int) std::distance(alternatives.begin(), it);
            candidate = alternatives[(size_t) ((idx + 1) % 3)];
        }
        motifRoles[(size_t) ri] = candidate;
    }

    motifDirection *= -1;
    ++motifAge;
}

int Engine::composerSectionForStep(int i) const
{
    return (i / std::max(1, motifLength)) % 4;
}

int Engine::phrasePc(int i) const
{
    const int mi = i % motifLength;
    const int section = composerSectionForStep(i);
    const int pc = motifMemory[(size_t) mi];

    if (section == 0) return pc;
    if (section == 1)
        return mi == motifMutationIndex ? moveScaleSteps(pc, motifDirection) : pc;
    if (section == 2)
        return mi == 0 ? motifMemory[0] : moveScaleSteps(pc, motifDirection);
    if (mi == motifLength - 1 && motifLength > 1)
        return moveScaleSteps(pc, -motifDirection);
    return pc;
}

int Engine::phraseRole(int i) const
{
    const int mi = i % motifLength;
    const int section = composerSectionForStep(i);
    int role = motifRoles[(size_t) mi];
    if (section == 1 && mi == motifMutationIndex && role == 0) return 1;
    if (section == 3 && mi == motifLength - 1 && role == 1) return 0;
    return role;
}

void Engine::applyComposerDiceCores()
{
    const int savedM = melodyDiceRange;
    const int savedR = rhythmDiceRange;
    melodyDiceRange = (int) std::lround(savedM * 0.38);
    rhythmDiceRange = (int) std::lround(savedR * 0.46);
    applyMelodyDiceCore();
    applyRhythmDiceCore();
    melodyDiceRange = savedM;
    rhythmDiceRange = savedR;
}

Engine::Event Engine::composerEventForStep(int i)
{
    auto ev = makeEvent();
    const int role = phraseRole(i);
    const int pc = phrasePc(i);

    ev.pc = pc;
    ev.oct = chooseOct();
    ev.rest = false;
    ev.ghost = false;
    ev.accent = false;
    ev.velocityOverride = -1;
    ev.velocityRole = role;

    if (role == 3)
    {
        ev.rate = baseRate == 0 ? 1 : std::min(2, baseRate);
        ev.accent = true;
    }
    else if (role == 2)
    {
        ev.rate = chance(62.0) ? 2 : 3;
        ev.accent = chance(18.0);
    }
    else if (role == 1)
    {
        ev.rate = 3;
        ev.leg = chance(16.0);
        ev.rest = chance(std::min(18.0, 6.0 + restProb * 0.16));
    }
    else
    {
        ev.rate = 3;
        ev.ghost = true;
        ev.leg = false;
        ev.rest = chance(std::min(22.0, 8.0 + restProb * 0.18));
    }

    if (i == steps - 1)
    {
        ev.pc = motifMemory[0];
        ev.ghost = false;
        ev.rest = false;
        ev.rate = std::min(2, std::max(1, baseRate));
        ev.accent = true;
    }

    normaliseEventOctave(ev);
    composerLastPc = ev.pc;
    return ev;
}

void Engine::composePhrase()
{
    applyComposerDiceCores();
    chooseComposerVelo();
    chooseComposerPoly();
    refreshRememberedMotif();
    ensurePattern();
    composerLastPc = motifMemory[0];

    for (int i = 0; i < steps; ++i)
    {
        pattern[(size_t) i] = composerEventForStep(i);
        patternValid[(size_t) i] = true;
    }

    composerMode = true;
    resetPositionForDirection();
}

TickResult Engine::tick()
{
    ensurePattern();

    const int idx = clampInt(playPos, 0, steps - 1);
    auto& ev = pattern[(size_t) idx];
    normaliseEventOctave(ev);

    TickResult result;
    result.sequenceStep = idx;
    result.rateIndex = clampInt(ev.rate, 0, 3);
    result.isRest = ev.rest;
    result.legato = ev.leg;
    result.accent = ev.accent;
    result.durationScale = ev.leg ? 1.18 : (gatePct / 100.0);

    if (!ev.rest)
    {
        const auto pitches = chordPitches(ev);
        int vel = velocityForEvent(ev);
        if (ev.accent) vel = accentedVelocity(vel);

        result.rootPitch = pitches.front();
        result.pitchMask = pitchMaskFor(pitches);
        for (int i = 0; i < (int) pitches.size(); ++i)
            result.notes.push_back({ pitches[(size_t) i], velocityForVoice(vel, i) });
    }

    if (!locked && !composerMode)
    {
        pattern[(size_t) idx] = makeEvent();
        patternValid[(size_t) idx] = true;
    }

    advance();
    return result;
}

void Engine::setWeight(int index, int value)
{
    if (index >= 0 && index < 12) weights[(size_t) index] = clampInt(value, 0, 100);
}
void Engine::setRate(int value)
{
    value = clampInt(value, 0, 3);
    if (value != baseRate) { baseRate = value; composerMode = false; }
}
void Engine::setVariation(int value)
{
    value = clampInt(value, -100, 100);
    if (value != variation) { variation = value; composerMode = false; }
}
void Engine::setLegato(int value)
{
    value = clampInt(value, 0, 100);
    if (value != legatoProb) { legatoProb = value; composerMode = false; }
}
void Engine::setRest(int value)
{
    value = clampInt(value, 0, 100);
    if (value != restProb) { restProb = value; composerMode = false; }
}
void Engine::setLowOctave(int value)
{
    value = clampInt(value, -2, 0);
    if (value != lowOct) { lowOct = value; normalisePatternOctaves(); }
}
void Engine::setHighOctave(int value)
{
    value = clampInt(value, 0, 3);
    if (value != highOct) { highOct = value; normalisePatternOctaves(); }
}
void Engine::setTranspose(int value) { transpose = clampInt(value, -12, 12); }
void Engine::setSteps(int value)
{
    value = clampInt(value, 1, 16);
    if (value == steps) return;
    steps = value;
    clearTail();
    ensurePattern();
    resetPositionForDirection();
}
void Engine::setDirection(int value)
{
    value = clampInt(value, 0, 3);
    if (value != direction) { direction = value; resetPositionForDirection(); }
}
void Engine::setAccent(int value)
{
    value = clampInt(value, 0, 100);
    if (value != accentProb) { accentProb = value; composerMode = false; }
}
void Engine::setGate(int value) { gatePct = clampInt(value, 15, 95); }
void Engine::setMelodyDiceRange(int value) { melodyDiceRange = clampInt(value, 0, 100); }
void Engine::setRhythmDiceRange(int value) { rhythmDiceRange = clampInt(value, 0, 100); }
void Engine::setPolyphony(int voices) { polyMode = clampInt(voices, 1, 4) - 1; }
void Engine::setVeloMode(int mode) { veloMode = clampInt(mode, 0, 2); }

void Engine::setLocked(bool shouldLock)
{
    const bool was = locked;
    locked = shouldLock;
    if (locked && !was)
    {
        ensurePattern();
        resetPositionForDirection();
    }
    if (!locked && was)
        composerMode = false;
}

void Engine::restart() { resetPositionForDirection(); }

void Engine::diceMelody()
{
    composerMode = false;
    applyMelodyDiceCore();
    ensurePattern();
    for (int i = 0; i < steps; ++i)
    {
        pattern[(size_t) i].pc = chooseWeighted();
        pattern[(size_t) i].oct = chooseOct();
        normaliseEventOctave(pattern[(size_t) i]);
        patternValid[(size_t) i] = true;
    }
    resetPositionForDirection();
}

void Engine::diceRhythm()
{
    composerMode = false;
    applyRhythmDiceCore();
    ensurePattern();
    for (int i = 0; i < steps; ++i)
    {
        auto& ev = pattern[(size_t) i];
        ev.rest = chance(restProb);
        ev.leg = chance(legatoProb);
        ev.rate = chooseRate();
        ev.accent = chance(accentProb);
        patternValid[(size_t) i] = true;
    }
    resetPositionForDirection();
}

void Engine::composeDejaVu() { composePhrase(); }

} // namespace inspirato
