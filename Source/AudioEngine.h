#pragma once

#include <JuceHeader.h>
#include "SharedState.h"

/**
    Owns the audio device and the master signal graph.

    Graph topology (Phase 1):

        [Sequencer] --MIDI--> [Synth] --audio--> [Audio Output]

    Later phases add per-track FX chains, buses, sends/returns and plugin nodes to
    this same AudioProcessorGraph, which is the whole point of building on it now.
*/
class AudioEngine
{
public:
    explicit AudioEngine (SharedState& sharedState);
    ~AudioEngine();

    /** Opens the default audio device and starts the graph running. Returns an
        empty string on success, or an error message. */
    juce::String start();

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    /** The sampler processor, for loading instruments / querying its name from the UI. */
    class SamplerProcessor* getSampler() noexcept { return sampler; }

    /** Offline (faster-than-real-time) render of `bars` bars to a stereo 24-bit WAV.
        Stops live audio during the render and restores it afterwards. */
    bool renderToFile (const juce::File& file, int bars);

    /** Re-scan the sample folder (extract dropped .zip packs, catalog instruments).
        Returns the number of sample-folder instruments found. */
    int rescanSamples();

    /** The folder that user sample packs live in. */
    static juce::File sampleFolder();

private:
    void setupDefaultKit();
    void restoreLive();

    using Node = juce::AudioProcessorGraph::Node;

    SharedState&                   state;
    juce::AudioDeviceManager       deviceManager;
    juce::AudioProcessorGraph      graph;
    juce::AudioProcessorPlayer     player;

    Node::Ptr sequencerNode, synthNode, samplerNode, masterNode, outputNode;
    class SamplerProcessor*        sampler = nullptr;   // owned by the graph node

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
