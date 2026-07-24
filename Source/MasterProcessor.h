#pragma once

#include <JuceHeader.h>
#include "SharedState.h"

/**
    Master bus effects: stereo delay -> algorithmic reverb -> master gain -> limiter.
    Inserted between the sampler and the audio output. All parameters are read live
    from SharedState::Master (atomics); the audio thread never allocates or locks.
*/
class MasterProcessor : public juce::AudioProcessor
{
public:
    explicit MasterProcessor (SharedState& s)
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          state (s) {}

    const juce::String getName() const override { return "Master"; }

    void prepareToPlay (double sr, int blockSize) override
    {
        sampleRate = sr;
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 2 };

        delay.prepare (spec);
        delay.setMaximumDelayInSamples ((int) (sr * 2.0) + 4);   // up to 2 s
        delay.reset();

        reverb.setSampleRate (sr);

        limiter.prepare (spec);
        limiter.setThreshold (-0.8f);   // dB
        limiter.setRelease (60.0f);     // ms

        clickLenSamples = (int) (sr * 0.045);   // ~45 ms metronome click
        clickDecay      = 0.006 * sr;
        clickLeft = 0; clickPos = 0; clickPhase = 0.0; clickInc = 0.0;
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        const int n     = buffer.getNumSamples();
        if (numCh < 1) return;

        // --- Stereo delay (feedback) --------------------------------------
        const float dMix = state.master.delayMix.load (std::memory_order_relaxed);
        if (dMix > 0.0001f)
        {
            const float fb   = juce::jlimit (0.0f, 0.95f, state.master.delayFeedback.load (std::memory_order_relaxed));
            const float dsmp = juce::jlimit (1.0f, (float) (sampleRate * 2.0),
                                             state.master.delayMs.load (std::memory_order_relaxed) / 1000.0f * (float) sampleRate);
            delay.setDelay (dsmp);

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                for (int i = 0; i < n; ++i)
                {
                    const float in  = d[i];
                    const float dly = delay.popSample (ch);
                    delay.pushSample (ch, in + fb * dly);
                    d[i] = in + dMix * dly;
                }
            }
        }

        // --- Reverb --------------------------------------------------------
        const float rMix = state.master.reverbMix.load (std::memory_order_relaxed);
        juce::Reverb::Parameters rp;
        rp.roomSize = juce::jlimit (0.0f, 1.0f, state.master.reverbRoom.load (std::memory_order_relaxed));
        rp.damping  = 0.5f;
        rp.wetLevel = rMix;
        rp.dryLevel = 1.0f - juce::jmin (1.0f, rMix);
        rp.width    = 1.0f;
        reverb.setParameters (rp);

        if (numCh >= 2)
            reverb.processStereo (buffer.getWritePointer (0), buffer.getWritePointer (1), n);
        else
            reverb.processMono (buffer.getWritePointer (0), n);

        // --- Master gain + limiter ----------------------------------------
        buffer.applyGain (juce::jlimit (0.0f, 1.5f, state.master.gain.load (std::memory_order_relaxed)));

        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        limiter.process (ctx);

        // --- Metronome click (post-limiter, dry) --------------------------
        const int trigAt  = state.clickOffset.exchange (-1, std::memory_order_relaxed);
        const int accent  = state.clickAccent.load (std::memory_order_relaxed);
        if (trigAt >= 0 || clickLeft > 0)
        {
            float* L = buffer.getWritePointer (0);
            float* R = numCh >= 2 ? buffer.getWritePointer (1) : nullptr;

            for (int i = 0; i < n; ++i)
            {
                if (i == trigAt)
                {
                    clickLeft  = clickLenSamples;
                    clickPos   = 0;
                    clickPhase = 0.0;
                    clickInc   = juce::MathConstants<double>::twoPi
                               * (accent ? 2000.0 : 1200.0) / sampleRate;
                }

                if (clickLeft > 0)
                {
                    const float e = (float) std::exp (-(double) clickPos / clickDecay);
                    const float s = (float) std::sin (clickPhase) * e * 0.4f;
                    L[i] += s;
                    if (R != nullptr) R[i] += s;
                    clickPhase += clickInc;
                    ++clickPos;
                    --clickLeft;
                }
            }
        }

        // --- Feed the oscilloscope (post-everything, what you hear) --------
        {
            const float* L = buffer.getReadPointer (0);
            const float* R = numCh >= 2 ? buffer.getReadPointer (1) : L;
            int pos = state.scopePos.load (std::memory_order_relaxed);
            for (int i = 0; i < n; ++i)
            {
                state.scope[pos].store (0.5f * (L[i] + R[i]), std::memory_order_relaxed);
                pos = (pos + 1) & (SharedState::scopeSize - 1);
            }
            state.scopePos.store (pos, std::memory_order_relaxed);
        }
    }

    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }
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
    SharedState& state;
    double sampleRate = 44100.0;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 96000 };
    juce::Reverb reverb;
    juce::dsp::Limiter<float> limiter;

    // Metronome click state (persists across blocks).
    int    clickLenSamples = 0;
    int    clickLeft = 0;
    int    clickPos = 0;
    double clickDecay = 1.0;
    double clickPhase = 0.0;
    double clickInc = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
