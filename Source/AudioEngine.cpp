#include "AudioEngine.h"
#include "SequencerProcessor.h"
#include "SubtractiveSynth.h"
#include "SamplerEngine.h"
#include "SfzParser.h"   // compiled here so errors surface even before SFZ loading is wired to UI
#include "MasterProcessor.h"

using AudioGraphIOProcessor = juce::AudioProcessorGraph::AudioGraphIOProcessor;

AudioEngine::AudioEngine (SharedState& sharedState)
    : state (sharedState)
{
    // Establish a default config BEFORE wiring connections, so the output IO node
    // actually has 2 channels to connect to. The device will override these with
    // its real values when start() opens it.
    graph.setPlayConfigDetails (0, 2, 44100.0, 512);

    sequencerNode = graph.addNode (std::make_unique<SequencerProcessor> (state));
    synthNode     = graph.addNode (std::make_unique<SynthProcessor>     (state));

    auto samplerProc = std::make_unique<SamplerProcessor> (state);
    sampler       = samplerProc.get();
    samplerNode   = graph.addNode (std::move (samplerProc));

    masterNode    = graph.addNode (std::make_unique<MasterProcessor> (state));

    outputNode    = graph.addNode (std::make_unique<AudioGraphIOProcessor> (
                                       AudioGraphIOProcessor::audioOutputNode));

    // Sequencer MIDI -> multi-timbral Sampler (routes each channel to its instrument).
    // The synth node exists but is left dormant for now; it returns as a selectable
    // channel instrument in a later step.
    graph.addConnection ({ { sequencerNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex },
                           { samplerNode->nodeID,   juce::AudioProcessorGraph::midiChannelIndex } });

    // Sampler -> Master FX -> Output
    for (int ch = 0; ch < 2; ++ch)
    {
        graph.addConnection ({ { samplerNode->nodeID, ch }, { masterNode->nodeID, ch } });
        graph.addConnection ({ { masterNode->nodeID, ch }, { outputNode->nodeID, ch } });
    }

    // Load the bundled General MIDI SoundFont if present (all basic instruments).
    for (auto candidate : { juce::File ("D:/OpenStudio/Content/FluidR3_GM.sf2"),
                            juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                .getParentDirectory().getChildFile ("Content/FluidR3_GM.sf2") })
    {
        if (candidate.existsAsFile() && sampler->loadSoundFont (candidate))
            break;
    }

    if (sampler->hasSoundFont())
    {
        setupDefaultKit();
        juce::File ("D:/OpenStudio/sf2_report.txt").replaceWithText (sampler->soundFontReport());
    }

    // Import any user sample folders / dropped .zip packs.
    rescanSamples();
}

juce::File AudioEngine::sampleFolder()
{
    juce::File d ("D:/OpenStudio/Content/Samples");
    if (d.isDirectory()) return d;
    auto rel = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                   .getParentDirectory().getChildFile ("Content/Samples");
    if (rel.isDirectory()) return rel;
    d.createDirectory();
    return d;
}

int AudioEngine::rescanSamples()
{
    if (sampler == nullptr) return 0;
    return sampler->importSampleFolder (sampleFolder());
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
}

void AudioEngine::setupDefaultKit()
{
    // Resolve instruments by exact General MIDI program (standardised) so we get the
    // real drum kit (bank 128) and not, e.g., "Steel Drums" via a name match.
    int drums = sampler->findProgram (128, 0);          // Standard Drum Kit
    if (drums < 0) drums = sampler->findDrumKit();
    int bass = sampler->findProgram (0, 33);            // Electric Bass (finger)
    if (bass < 0) bass = sampler->findProgram (0, 32);  // Acoustic Bass
    const int piano   = sampler->findProgram (0, 0);    // Acoustic Grand Piano
    const int epiano  = sampler->findProgram (0, 4);    // Electric Piano 1
    int strings = sampler->findProgram (0, 48);         // String Ensemble 1
    if (strings < 0) strings = sampler->findPreset ("string ensemble");
    int lead = sampler->findProgram (0, 81);            // Lead 2 (sawtooth)
    if (lead < 0) lead = sampler->findProgram (0, 80);
    int organ = sampler->findProgram (0, 16);           // Drawbar Organ
    int guitar = sampler->findProgram (0, 27);          // Electric Guitar (clean)
    const int voiceAh = sampler->soundFontPresetCount(); // first synthesized voice (Ah)

    struct Def { int preset; int note; };
    Def defs[SharedState::numChannels] = {
        { drums,   36 },   // 1  kick
        { drums,   38 },   // 2  snare
        { drums,   42 },   // 3  closed hi-hat
        { drums,   46 },   // 4  open hat
        { bass,    40 },   // 5  bass
        { piano,   60 },   // 6  piano
        { epiano,  60 },   // 7  e-piano
        { strings, 67 },   // 8  strings
        { voiceAh, 60 },   // 9  voice (Ah)
        { lead,    72 },   // 10 lead
        { organ,   60 },   // 11 organ
        { guitar,  55 },   // 12 guitar
        { piano,   48 },   // 13 spare
        { piano,   60 },   // 14 spare
        { piano,   72 },   // 15 spare
        { drums,   49 },   // 16 crash
    };

    for (int c = 0; c < SharedState::numChannels; ++c)
    {
        const int preset = defs[c].preset >= 0 ? defs[c].preset : 0;
        state.channels[c].preset.store (preset);
        state.channels[c].editNote.store (defs[c].note);
        sampler->setChannelPreset (c, preset);
    }

    // A simple starter groove in pattern 0 so pressing Play is immediately musical.
    auto set = [this] (int ch, std::initializer_list<int> steps)
    {
        const int note = state.channels[ch].editNote.load();
        for (int s : steps) state.patterns[0].cells[ch][note][s].store (true);
    };

    set (0, { 0, 4, 8, 12 });                       // kick on the beat
    set (1, { 4, 12 });                             // snare on 2 & 4
    set (2, { 0, 2, 4, 6, 8, 10, 12, 14 });         // hats on 8ths
    set (5, { 0, 6, 10 });                          // a few piano hits
}

bool AudioEngine::renderToFile (const juce::File& file, int bars)
{
    // Take the graph offline: stop the live device callback so we can drive the graph
    // ourselves faster than real time.
    deviceManager.removeAudioCallback (&player);

    const double sr    = 44100.0;
    const int    block = 512;
    graph.setPlayConfigDetails (0, 2, sr, block);
    graph.prepareToPlay (sr, block);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;

    // Register a clean transport start.
    state.playing.store (false);
    buf.clear(); graph.processBlock (buf, midi);
    state.playing.store (true);

    const double    bpm   = state.bpm.load();
    const long long total = (long long) ((double) bars * 4.0 * (60.0 / bpm) * sr);

    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
    if (os == nullptr) { restoreLive(); return false; }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr) { restoreLive(); return false; }
    os.release();   // writer owns the stream now

    long long done = 0;
    while (done < total)
    {
        const int n = (int) juce::jmin ((long long) block, total - done);
        buf.clear();
        midi.clear();
        graph.processBlock (buf, midi);
        writer->writeFromAudioSampleBuffer (buf, 0, n);
        done += n;
    }

    // Let the reverb / delay tail ring out (~2 s).
    state.playing.store (false);
    for (int t = 0; t < (int) (sr * 2.0 / block); ++t)
    {
        buf.clear(); midi.clear();
        graph.processBlock (buf, midi);
        writer->writeFromAudioSampleBuffer (buf, 0, block);
    }

    writer.reset();      // flush + close file
    restoreLive();
    return true;
}

void AudioEngine::restoreLive()
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        graph.prepareToPlay (dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
    deviceManager.addAudioCallback (&player);
}

juce::String AudioEngine::start()
{
    // 0 inputs, 2 outputs. A low buffer size for latency is chosen automatically by
    // the device; the user can refine it later via the device selector.
    const juce::String error = deviceManager.initialiseWithDefaultDevices (0, 2);

    if (error.isNotEmpty())
        return error;

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    return {};
}
