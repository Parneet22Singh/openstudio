#pragma once

#include <JuceHeader.h>
#include "SharedState.h"

/**
    A minimal but real subtractive synth voice: one oscillator (saw / square / sine),
    an ADSR amplitude envelope, and a resonant state-variable low-pass filter.

    Everything the voice needs is pre-allocated in setCurrentPlaybackSampleRate();
    renderNextBlock() is allocation-free and lock-free (it only reads atomics).
*/
struct SynthSound : public juce::SynthesiserSound
{
    bool appliesToNote    (int)  override { return true; }
    bool appliesToChannel (int)  override { return true; }
};

class SubtractiveVoice : public juce::SynthesiserVoice
{
public:
    explicit SubtractiveVoice (SharedState& s) : state (s) {}

    bool canPlaySound (juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SynthSound*> (sound) != nullptr;
    }

    void setCurrentPlaybackSampleRate (double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);

        if (newRate > 0.0)
        {
            adsr.setSampleRate (newRate);

            juce::dsp::ProcessSpec spec;
            spec.sampleRate       = newRate;
            spec.maximumBlockSize = 1024;
            spec.numChannels      = 1;
            filter.prepare (spec);
            filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        }
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int /*currentPitchWheel*/) override
    {
        frequency = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        phase     = 0.0;
        level     = juce::jmax (0.2f, velocity);

        filter.reset();
        adsr.setParameters ({ 0.004f, 0.14f, 0.55f, 0.18f });
        adsr.noteOn();
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            adsr.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                          int startSample, int numSamples) override
    {
        if (! adsr.isActive())
            return;

        const double sr  = getSampleRate();
        const double inc = frequency / sr;                 // phase increment (cycles/sample)
        const int    wf  = state.waveform.load (std::memory_order_relaxed);

        filter.setCutoffFrequency (juce::jlimit (20.0f, (float) (sr * 0.45),
                                                 state.cutoff.load (std::memory_order_relaxed)));
        filter.setResonance       (juce::jlimit (0.1f, 5.0f,
                                                 state.resonance.load (std::memory_order_relaxed)));

        const int numCh = outputBuffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            const float osc = oscillator (wf, (float) phase);

            phase += inc;
            if (phase >= 1.0)
                phase -= 1.0;

            const float env      = adsr.getNextSample();
            const float filtered = filter.processSample (0, osc);
            const float sampleV  = filtered * env * level * 0.28f;

            for (int ch = 0; ch < numCh; ++ch)
                outputBuffer.addSample (ch, startSample + i, sampleV);
        }

        if (! adsr.isActive())
            clearCurrentNote();
    }

private:
    static float oscillator (int waveform, float ph) noexcept
    {
        switch (waveform)
        {
            case 1:  return ph < 0.5f ? 1.0f : -1.0f;                                  // square
            case 2:  return std::sin (ph * juce::MathConstants<float>::twoPi);         // sine
            default: return 2.0f * ph - 1.0f;                                          // saw
        }
    }

    SharedState& state;

    double phase     = 0.0;
    double frequency = 440.0;
    float  level     = 1.0f;

    juce::ADSR                                adsr;
    juce::dsp::StateVariableTPTFilter<float>  filter;
};

/**
    Wraps a polyphonic juce::Synthesiser as an AudioProcessor so it can be a node in
    the AudioProcessorGraph. Accepts MIDI, produces stereo audio.
*/
class SynthProcessor : public juce::AudioProcessor
{
public:
    explicit SynthProcessor (SharedState& s)
        : juce::AudioProcessor (BusesProperties()
              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          state (s)
    {
        for (int i = 0; i < 16; ++i)
            synth.addVoice (new SubtractiveVoice (state));

        synth.addSound (new SynthSound());
    }

    const juce::String getName() const override { return "Synth"; }

    void prepareToPlay (double sampleRate, int /*blockSize*/) override
    {
        synth.setCurrentPlaybackSampleRate (sampleRate);
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());
    }

    // --- boilerplate ---------------------------------------------------------
    bool   acceptsMidi()  const override { return true;  }
    bool   producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.4; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms()    override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    SharedState&       state;
    juce::Synthesiser  synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthProcessor)
};
