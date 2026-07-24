#pragma once

#include <JuceHeader.h>
#include "SharedState.h"

/**
    Polyphonic, pattern- and song-aware step sequencer.

    - Pattern mode: loops SharedState::currentPattern using that pattern's own length.
    - Song mode: walks the arrangement (songSlots[0..songLength)), playing each slot's
      pattern for its full length before advancing to the next slot (looping the song).

    Timing is absolute-sample based, so it survives block splits and supports swing
    (odd 16th-notes are nudged late by up to ~70% of a step). Each channel emits on its
    own MIDI channel so the multi-timbral sampler routes it to that channel's instrument.
    A quarter-note metronome click request is posted to SharedState for the master bus.

    Real-time safe: no allocation, no locks; fixed-size per-channel active-note tracking.
*/
class SequencerProcessor : public juce::AudioProcessor
{
public:
    explicit SequencerProcessor (SharedState& s)
        : juce::AudioProcessor (BusesProperties()), state (s) {}

    const juce::String getName() const override { return "Sequencer"; }

    void prepareToPlay (double sr, int) override { sampleRate = sr; resetTransport(); }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        midi.clear();

        const bool playing = state.playing.load (std::memory_order_relaxed);
        if (playing != wasPlaying)
        {
            wasPlaying = playing;
            if (playing) startTransport();
            else { allNotesOff (midi, 0); state.currentStep.store (-1, std::memory_order_relaxed);
                   state.songPos.store (-1, std::memory_order_relaxed); return; }
        }
        if (! playing)
            return;

        const double bpm = state.bpm.load (std::memory_order_relaxed);
        samplesPerStep   = (60.0 / bpm) / 4.0 * sampleRate;               // 16th note
        const int    n   = buffer.getNumSamples();
        const double blockStart = (double) samplePos;
        const double blockEnd   = blockStart + (double) n;

        while (nextTrigAbs < blockEnd)
        {
            const int at = (int) juce::jlimit (0.0, (double) juce::jmax (0, n - 1), nextTrigAbs - blockStart);

            // Fire the pending step.
            state.currentStep.store (pendingStep, std::memory_order_relaxed);
            state.playingPattern.store (pendingPattern, std::memory_order_relaxed);
            state.songPos.store (songSlot >= 0 && state.songMode.load (std::memory_order_relaxed) ? songSlot : -1,
                                 std::memory_order_relaxed);

            if (pendingPattern >= 0)
                for (int c = 0; c < SharedState::numChannels; ++c)
                    triggerChannel (midi, pendingPattern, c, pendingStep, at);

            if (state.metronomeOn.load (std::memory_order_relaxed) && (pendingStep % 4) == 0)
            {
                state.clickOffset.store (at, std::memory_order_relaxed);
                state.clickAccent.store (pendingStep == 0 ? 1 : 0, std::memory_order_relaxed);
            }

            scheduleNext();
        }

        samplePos += n;
    }

    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return true;  }
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    static constexpr int maxPoly = 16;

    int clampLen (int pattern) const
    {
        if (pattern < 0) return SharedState::numSteps;
        return juce::jlimit (1, SharedState::maxSteps,
                             state.patterns[pattern].length.load (std::memory_order_relaxed));
    }

    int resolvePattern (int slot) const
    {
        if (state.songMode.load (std::memory_order_relaxed)
            && state.songLength.load (std::memory_order_relaxed) > 0)
            return state.songSlots[juce::jlimit (0, SharedState::maxSong - 1, slot)]
                       .load (std::memory_order_relaxed);
        return state.currentPattern.load (std::memory_order_relaxed);
    }

    void startTransport()
    {
        samplePos      = 0;
        songSlot       = 0;
        pendingPattern = resolvePattern (0);
        pendingStep    = 0;
        gridAnchor     = 0.0;
        nextTrigAbs    = 0.0;
        for (auto& a : activeCount) a = 0;
    }

    void scheduleNext()
    {
        const int    L   = clampLen (pendingPattern);
        const double sps = samplesPerStep;

        if (pendingStep + 1 < L)
        {
            ++pendingStep;
        }
        else
        {
            gridAnchor += (double) L * sps;
            pendingStep = 0;

            const bool song = state.songMode.load (std::memory_order_relaxed);
            const int  len  = state.songLength.load (std::memory_order_relaxed);
            if (song && len > 0)
            {
                if (++songSlot >= len) songSlot = 0;
            }
            pendingPattern = resolvePattern (songSlot);
        }

        const double sw = (pendingStep % 2 == 1)
                        ? (double) state.swing.load (std::memory_order_relaxed) * sps : 0.0;
        nextTrigAbs = gridAnchor + (double) pendingStep * sps + sw;
    }

    void triggerChannel (juce::MidiBuffer& midi, int pat, int c, int step, int at)
    {
        if (state.channels[c].muted.load (std::memory_order_relaxed))
        {
            channelOff (midi, c, at);
            return;
        }

        int hits[maxPoly];
        int nh = 0;
        for (int p = 0; p < SharedState::numPitches && nh < maxPoly; ++p)
            if (state.patterns[pat].cells[c][p][step].load (std::memory_order_relaxed))
                hits[nh++] = p;

        if (nh == 0)
            return;                                 // nothing new -> let ringing notes decay

        channelOff (midi, c, at);                   // stop the previous hit on this channel
        for (int k = 0; k < nh; ++k)
        {
            midi.addEvent (juce::MidiMessage::noteOn (c + 1, hits[k], 0.85f), at);
            active[c][k] = hits[k];
        }
        activeCount[c] = nh;
    }

    void channelOff (juce::MidiBuffer& midi, int c, int at)
    {
        for (int k = 0; k < activeCount[c]; ++k)
            midi.addEvent (juce::MidiMessage::noteOff (c + 1, active[c][k]), at);
        activeCount[c] = 0;
    }

    void allNotesOff (juce::MidiBuffer& midi, int at)
    {
        for (int c = 0; c < SharedState::numChannels; ++c)
            channelOff (midi, c, at);
    }

    void resetTransport()
    {
        samplePos = 0; wasPlaying = false;
        pendingStep = 0; pendingPattern = 0; songSlot = 0;
        gridAnchor = 0.0; nextTrigAbs = 0.0;
        for (auto& a : activeCount) a = 0;
    }

    SharedState& state;
    double    sampleRate     = 44100.0;
    double    samplesPerStep = 11025.0;
    long long samplePos      = 0;
    double    gridAnchor     = 0.0;   // absolute sample of the current bar's step 0
    double    nextTrigAbs    = 0.0;   // absolute sample the pending step fires
    int       pendingStep    = 0;
    int       pendingPattern = 0;
    int       songSlot       = 0;
    bool      wasPlaying     = false;

    int active[SharedState::numChannels][maxPoly] {};
    int activeCount[SharedState::numChannels] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerProcessor)
};
