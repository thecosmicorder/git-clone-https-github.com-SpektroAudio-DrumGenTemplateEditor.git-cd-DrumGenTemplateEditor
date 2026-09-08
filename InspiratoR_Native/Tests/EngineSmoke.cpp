#include "InspiratoEngine.h"

#include <array>
#include <cassert>
#include <iostream>
#include <set>

using inspirato::Engine;

int main()
{
    Engine e;
    e.seed(123456u);
    e.setRest(0);
    e.setLowOctave(-1);
    e.setHighOctave(1);
    e.setSteps(16);

    for (int i = 0; i < 16; ++i) (void) e.tick();

    for (int voices = 1; voices <= 4; ++voices)
    {
        e.setPolyphony(voices);
        for (int i = 0; i < 128; ++i)
        {
            auto tick = e.tick();
            assert(!tick.isRest);
            assert((int) tick.notes.size() == voices);
            std::set<int> pitches;
            for (auto n : tick.notes) pitches.insert(n.pitch);
            assert((int) pitches.size() == voices);
        }
    }

    e.setPolyphony(4);
    e.setVeloMode(0);
    for (int i = 0; i < 512; ++i)
    {
        auto tick = e.tick();
        for (auto n : tick.notes) assert(n.velocity >= 20 && n.velocity <= 64);
    }

    e.setVeloMode(2);
    for (int i = 0; i < 512; ++i)
    {
        auto tick = e.tick();
        for (auto n : tick.notes) assert(n.velocity >= 64 && n.velocity <= 127);
    }

    e.setPolyphony(3);
    e.composeDejaVu();
    e.setPolyphony(3);
    e.setVeloMode(0);
    for (int i = 0; i < 32; ++i)
    {
        auto tick = e.tick();
        if (!tick.isRest)
        {
            assert((int) tick.notes.size() == 3);
            for (auto n : tick.notes) assert(n.velocity >= 20 && n.velocity <= 64);
        }
    }

    e.setVeloMode(2);
    for (int i = 0; i < 32; ++i)
    {
        auto tick = e.tick();
        if (!tick.isRest)
        {
            assert((int) tick.notes.size() == 3);
            for (auto n : tick.notes) assert(n.velocity >= 64 && n.velocity <= 127);
        }
    }

    e.setMelodyDiceRange(0);
    const auto beforeZeroDice = e.getWeights();
    e.diceMelody();
    assert(e.getWeights() == beforeZeroDice);

    e.setMelodyDiceRange(100);
    e.setWeight(1, 0);
    e.setWeight(3, 0);
    e.setWeight(6, 0);
    e.diceMelody();
    assert(e.getWeights()[1] == 0);
    assert(e.getWeights()[3] == 0);
    assert(e.getWeights()[6] == 0);

    e.setSteps(4);
    e.setDirection(0);
    std::array<int, 4> forward {};
    for (int i = 0; i < 4; ++i) forward[(size_t)i] = e.tick().sequenceStep;
    assert((forward == std::array<int,4>{0,1,2,3}));

    e.setDirection(1);
    std::array<int, 4> back {};
    for (int i = 0; i < 4; ++i) back[(size_t)i] = e.tick().sequenceStep;
    assert((back == std::array<int,4>{3,2,1,0}));

    e.setDirection(2);
    std::array<int, 8> ping {};
    for (int i = 0; i < 8; ++i) ping[(size_t)i] = e.tick().sequenceStep;
    assert((ping == std::array<int,8>{0,1,2,3,2,1,0,1}));

    e.setDirection(0);
    e.setSteps(8);
    e.setPolyphony(1);
    e.composeDejaVu();
    e.setPolyphony(1);
    std::array<int,8> cycleA {}, cycleB {};
    std::array<bool,8> restA {}, restB {};
    for (int i=0;i<8;++i) { auto t=e.tick(); restA[(size_t)i]=t.isRest; cycleA[(size_t)i]=t.notes.empty()?-1:t.notes[0].pitch; }
    for (int i=0;i<8;++i) { auto t=e.tick(); restB[(size_t)i]=t.isRest; cycleB[(size_t)i]=t.notes.empty()?-1:t.notes[0].pitch; }
    assert(restA == restB);
    assert(cycleA == cycleB);

    e.setSteps(4);
    e.setDirection(0);
    for (int i = 0; i < 16; ++i)
    {
        auto tick = e.tick();
        assert(tick.sequenceStep >= 0 && tick.sequenceStep < 4);
    }

    std::cout << "InspiratoEngineSmoke PASS\n";
    return 0;
}
