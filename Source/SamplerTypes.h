#pragma once

#include <JuceHeader.h>
#include <vector>

/** One mapped sample region: a sample plus its key/velocity/round-robin placement. */
struct SampleZone
{
    juce::AudioBuffer<float> data;        // the audio (1 or 2 channels)
    double sourceSampleRate = 44100.0;

    int rootNote = 60;                    // MIDI note the sample was recorded at
    int loKey = 0,  hiKey = 127;
    int loVel = 1,  hiVel = 127;

    int seqPosition = 1;                  // 1-based round-robin index within its group
    int seqLength   = 1;                  // number of round-robin variants
    int group       = 0;                  // zones sharing a group round-robin together

    bool loopEnabled = false;
    int  loopStart = 0, loopEnd = 0;

    float gain = 1.0f;                    // linear
    double tuneSemitones = 0.0;           // fine/coarse tune offset

    // Volume envelope (seconds / linear sustain) — read from the SF2 so each
    // instrument keeps its own character (plucked decay vs sustained pad).
    float envAttack  = 0.002f;
    float envDecay   = 0.0f;
    float envSustain = 1.0f;
    float envRelease = 0.12f;
};

/** A playable instrument = a set of sample zones. */
struct Instrument
{
    juce::String name { "Empty" };
    std::vector<SampleZone> zones;
};
