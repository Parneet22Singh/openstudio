#pragma once

#include <atomic>

/**
    SharedState — lock-free bridge between GUI and audio threads.

    Model (FL-Studio style):
      - CHANNELS are global: each channel owns an instrument (SoundFont preset), a
        mixer strip (gain / pan / mute), and an insert FX chain (3-band EQ + compressor).
      - PATTERNS hold the notes: a pattern is a full [channel][pitch][step] grid with its
        own length. Switching pattern swaps which notes play; the channels stay the same.
      - The SONG (arrangement) chains patterns on a timeline. In song mode the sequencer
        walks the arrangement slot by slot; in pattern mode it loops the current pattern.

    RULES: the audio thread only reads / atomically stores here; never allocates or
    locks. Torn reads of a single scalar are harmless (heard one block late).
*/
struct SharedState
{
    static constexpr int numChannels = 16;
    static constexpr int numPitches  = 128;   // full MIDI range
    static constexpr int maxSteps    = 64;     // longest pattern
    static constexpr int numSteps    = 16;     // default pattern length
    static constexpr int numPatterns = 8;
    static constexpr int maxSong     = 64;     // arrangement slots

    // --- A channel: instrument + mixer strip + insert FX (all global) --------
    struct Channel
    {
        std::atomic<int>   preset   { 0 };     // SoundFont preset index
        std::atomic<int>   editNote { 60 };    // pitch shown/edited in the rack's quick row
        std::atomic<bool>  muted    { false };
        std::atomic<float> gain     { 0.85f };
        std::atomic<float> pan      { 0.0f };  // -1 = left, 0 = centre, +1 = right

        // Insert FX: 3-band EQ (dB gains) + simple compressor.
        std::atomic<float> eqLowDb  { 0.0f };  // low shelf   (~180 Hz)
        std::atomic<float> eqMidDb  { 0.0f };  // mid peak    (~1 kHz)
        std::atomic<float> eqHighDb { 0.0f };  // high shelf  (~4 kHz)
        std::atomic<bool>  compOn   { false };
        std::atomic<float> compThreshDb { -18.0f };
        std::atomic<float> compRatio    { 3.0f };
    };

    // --- A pattern: notes for every channel + its own length -----------------
    struct Pattern
    {
        std::atomic<int>  length { numSteps };
        std::atomic<bool> cells[numChannels][numPitches][maxSteps];

        Pattern()
        {
            for (auto& ch : cells)
                for (auto& row : ch)
                    for (auto& c : row)
                        c.store (false, std::memory_order_relaxed);
        }
    };

    Channel channels[numChannels];
    Pattern patterns[numPatterns];

    // Convenience accessor for a single cell in a given pattern.
    std::atomic<bool>& cell (int pat, int ch, int pitch, int step)
    {
        return patterns[pat].cells[ch][pitch][step];
    }

    // --- Transport / arrangement --------------------------------------------
    std::atomic<double> bpm     { 120.0 };
    std::atomic<bool>   playing { false };
    std::atomic<int>    currentStep    { -1 };
    std::atomic<int>    currentPattern  { 0 };   // pattern being edited & (pattern-mode) played
    std::atomic<int>    playingPattern  { 0 };   // pattern the audio is actually sounding
    std::atomic<float>  swing           { 0.0f };// 0..0.7: delay of off-beat 16ths

    // Song / arrangement
    std::atomic<bool>   songMode        { false };
    std::atomic<int>    songLength      { 0 };    // number of used arrangement slots
    std::atomic<int>    songPos         { -1 };   // current slot while playing the song
    std::atomic<int>    songSlots[maxSong];       // pattern index per slot (-1 = empty)

    // Metronome: sequencer posts a click request (in-block sample offset) that the
    // master bus renders. -1 = no click this block.
    std::atomic<bool>   metronomeOn { false };
    std::atomic<int>    clickOffset { -1 };
    std::atomic<int>    clickAccent { 0 };        // 1 on the downbeat

    // --- Master bus effects (GUI writes, audio reads) -----------------------
    struct Master
    {
        std::atomic<float> gain          { 0.9f };
        std::atomic<float> reverbMix     { 0.14f };   // 0..1 wet
        std::atomic<float> reverbRoom    { 0.5f };    // 0..1
        std::atomic<float> delayMix      { 0.0f };    // 0..1 wet
        std::atomic<float> delayMs       { 375.0f };  // delay time
        std::atomic<float> delayFeedback { 0.35f };   // 0..0.95
    };
    Master master;

    // --- Oscilloscope: master bus writes, GUI reads (lock-free ring) --------
    static constexpr int scopeSize = 512;
    std::atomic<float> scope[scopeSize];
    std::atomic<int>   scopePos { 0 };

    SharedState()
    {
        for (auto& s : songSlots)
            s.store (-1, std::memory_order_relaxed);
        for (auto& s : scope)
            s.store (0.0f, std::memory_order_relaxed);
    }

    // Legacy subtractive-synth params (synth node currently dormant).
    std::atomic<int>   waveform  { 0 };
    std::atomic<float> cutoff    { 1800.0f };
    std::atomic<float> resonance { 0.6f };
};
