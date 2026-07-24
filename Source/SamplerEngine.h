#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include <map>
#include <cmath>
#include "SharedState.h"
#include "SamplerTypes.h"
#include "Sf2Loader.h"
#include "SampleLibrary.h"

/**
    Multi-timbral multisample sampler with per-channel insert FX.

    Each rack channel has its own Instrument. Incoming MIDI is routed by channel:
    MIDI channel c plays channelInstrument[c-1]. This is what lets the Channel Rack
    sound many instruments at once (drums + bass + piano + strings...).

    Each channel is rendered into a scratch buffer and passed through that channel's
    insert chain (3-band EQ -> compressor) with its gain / pan, then summed to the
    stereo output. Filters keep their state across the sample-accurate render segments.

    Real-time safety:
      - The audio thread reads each channel's instrument through an atomically-loaded
        shared_ptr, and each sounding voice holds its own shared_ptr so the instrument
        can't be freed while it plays.
      - Every instrument ever built is retained on the message thread (presetCache /
        fallback), so a ref-count reaching zero on the audio thread can never happen.
      - Instruments are built on the message thread and swapped in atomically.
      - No allocation on the audio thread (scratch buffer sized in prepareToPlay).
*/
class SamplerProcessor : public juce::AudioProcessor
{
public:
    explicit SamplerProcessor (SharedState& s)
        : juce::AudioProcessor (BusesProperties()
              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          state (s)
    {
        formatManager.registerBasicFormats();
        fallback = createDefaultInstrument (44100.0);
        for (auto& ci : channelInstrument)
            std::atomic_store (&ci, fallback);           // audible even before a SoundFont loads

        for (auto& v : buildVoiceInstruments (44100.0))   // synthesized vocal library
            extras.push_back ({ v->name, {}, v });
        voiceCount = (int) extras.size();
    }

    const juce::String getName() const override { return "Sampler"; }

    void prepareToPlay (double sr, int blockSize) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.env.setSampleRate (sr);

        scratch.setSize (2, juce::jmax (16, blockSize), false, false, true);

        // Compressor ballistics (fixed attack / release).
        compAtk = (float) std::exp (-1.0 / (0.010 * sr));   // 10 ms
        compRel = (float) std::exp (-1.0 / (0.120 * sr));    // 120 ms

        for (auto& f : fx) f.reset();
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();

        int sample = 0;
        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            const int  pos = meta.samplePosition;

            render (buffer, sample, pos - sample);
            sample = pos;

            if (msg.isNoteOn())        startVoice (msg.getChannel() - 1, msg.getNoteNumber(), msg.getFloatVelocity());
            else if (msg.isNoteOff())  stopVoice  (msg.getChannel() - 1, msg.getNoteNumber());
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                for (auto& v : voices) v.env.noteOff();
        }
        render (buffer, sample, buffer.getNumSamples() - sample);
    }

    // --- SoundFont / instrument management (message thread) -----------------

    bool loadSoundFont (const juce::File& file)
    {
        auto sf = std::make_unique<SoundFont>();
        if (! sf->load (file))
            return false;
        soundFont = std::move (sf);
        return true;
    }

    int soundFontPresetCount() const
    {
        return soundFont != nullptr ? (int) soundFont->getPresets().size() : 0;
    }

    /** SoundFont presets first, then the synthesized extras (voices) appended. */
    juce::StringArray getSoundFontPresetNames() const
    {
        juce::StringArray names;
        if (soundFont != nullptr)
            for (const auto& p : soundFont->getPresets())
                names.add (p.name);
        for (const auto& e : extras)
            names.add (e.name);
        return names;
    }

    /** Import a folder tree of audio files as lazily-built instruments (appended after
        the SoundFont presets and the synthesized voices). Also auto-extracts any .zip
        dropped in the folder. Safe to call again to rescan. Message thread only. */
    int importSampleFolder (const juce::File& root)
    {
        SampleLib::extractZips (root);
        auto found = SampleLib::scan (root);

        extras.resize ((size_t) voiceCount);              // drop previously-scanned entries, keep voices
        for (auto& fi : found)
            extras.push_back ({ fi.name, fi.files, nullptr });
        return (int) found.size();
    }

    /** Find the first preset whose name contains `text` (case-insensitive), or -1. */
    int findPreset (juce::StringRef text) const
    {
        if (soundFont != nullptr)
        {
            const auto& ps = soundFont->getPresets();
            for (int i = 0; i < (int) ps.size(); ++i)
                if (ps[(size_t) i].name.containsIgnoreCase (text))
                    return i;
        }
        return -1;
    }

    /** Find a preset by exact General MIDI bank + program (standardised), or -1. */
    int findProgram (int bank, int program) const
    {
        if (soundFont != nullptr)
        {
            const auto& ps = soundFont->getPresets();
            for (int i = 0; i < (int) ps.size(); ++i)
                if (ps[(size_t) i].bank == bank && ps[(size_t) i].program == program)
                    return i;
        }
        return -1;
    }

    /** First drum-kit preset (GM bank 128), or -1. */
    int findDrumKit() const
    {
        if (soundFont != nullptr)
        {
            const auto& ps = soundFont->getPresets();
            for (int i = 0; i < (int) ps.size(); ++i)
                if (ps[(size_t) i].bank == 128)
                    return i;
        }
        return -1;
    }

    /** Assign an instrument to a rack channel (SoundFont preset, or a synthesized
        extra when the index is past the SoundFont range). Swaps in atomically. */
    bool setChannelPreset (int channel, int presetIndex)
    {
        if (channel < 0 || channel >= SharedState::numChannels)
            return false;

        const int sf = soundFontPresetCount();
        std::shared_ptr<Instrument> inst;

        if (presetIndex >= sf)
        {
            inst = buildExtra (presetIndex - sf);
        }
        else
        {
            if (soundFont == nullptr)
                return false;
            inst = getOrBuildPreset (presetIndex);
        }

        if (inst == nullptr)
            return false;

        std::atomic_store (&channelInstrument[channel], inst);
        return true;
    }

    bool hasSoundFont() const noexcept { return soundFont != nullptr; }

    /** True if a preset is a GM drum kit (bank 128) - editor shows drum names then. */
    bool isDrumPreset (int index) const
    {
        if (soundFont == nullptr) return false;
        const auto& ps = soundFont->getPresets();
        return index >= 0 && index < (int) ps.size() && ps[(size_t) index].bank == 128;
    }

    juce::String soundFontReport() const { return soundFont != nullptr ? soundFont->report() : juce::String(); }

    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

    // --- boilerplate ---------------------------------------------------------
    bool   acceptsMidi()  const override { return true;  }
    bool   producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }
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
    struct Voice
    {
        std::shared_ptr<const Instrument> keep;   // keeps the instrument alive while sounding
        const SampleZone* zone = nullptr;
        double pos = 0.0;
        double pitchRatio = 1.0;
        float  level = 1.0f;
        int    note = -1;
        int    channel = -1;
        juce::ADSR env;
    };

    // --- Per-channel insert FX: biquad EQ + compressor ----------------------
    struct Coeffs { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    struct Biquad
    {
        Coeffs c;
        float z1 = 0, z2 = 0;
        inline float process (float x) noexcept
        {
            const float y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0; }
    };

    struct ChannelFx
    {
        Biquad low[2], peak[2], high[2];
        float lastLow = -999, lastMid = -999, lastHigh = -999;
        float env = 0.0f;
        void reset()
        {
            for (auto& b : low)  b.reset();
            for (auto& b : peak) b.reset();
            for (auto& b : high) b.reset();
            lastLow = lastMid = lastHigh = -999;
            env = 0.0f;
        }
    };

    static Coeffs peakEQ (double sr, double f0, float dB)
    {
        const double A  = std::pow (10.0, dB / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * f0 / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * 0.707);
        const double a0 = 1.0 + alpha / A;
        Coeffs c;
        c.b0 = (float) ((1.0 + alpha * A) / a0);
        c.b1 = (float) ((-2.0 * cw)       / a0);
        c.b2 = (float) ((1.0 - alpha * A) / a0);
        c.a1 = (float) ((-2.0 * cw)       / a0);
        c.a2 = (float) ((1.0 - alpha / A) / a0);
        return c;
    }

    static Coeffs shelf (double sr, double f0, float dB, bool low)
    {
        const double A  = std::pow (10.0, dB / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * f0 / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / 2.0 * std::sqrt (2.0);
        const double tsa = 2.0 * std::sqrt (A) * alpha;
        Coeffs c;
        if (low)
        {
            const double a0 = (A + 1.0) + (A - 1.0) * cw + tsa;
            c.b0 = (float) ((A * ((A + 1.0) - (A - 1.0) * cw + tsa)) / a0);
            c.b1 = (float) ((2.0 * A * ((A - 1.0) - (A + 1.0) * cw)) / a0);
            c.b2 = (float) ((A * ((A + 1.0) - (A - 1.0) * cw - tsa)) / a0);
            c.a1 = (float) ((-2.0 * ((A - 1.0) + (A + 1.0) * cw))    / a0);
            c.a2 = (float) (((A + 1.0) + (A - 1.0) * cw - tsa)       / a0);
        }
        else
        {
            const double a0 = (A + 1.0) - (A - 1.0) * cw + tsa;
            c.b0 = (float) ((A * ((A + 1.0) + (A - 1.0) * cw + tsa)) / a0);
            c.b1 = (float) ((-2.0 * A * ((A - 1.0) + (A + 1.0) * cw))/ a0);
            c.b2 = (float) ((A * ((A + 1.0) + (A - 1.0) * cw - tsa)) / a0);
            c.a1 = (float) ((2.0 * ((A - 1.0) - (A + 1.0) * cw))     / a0);
            c.a2 = (float) (((A + 1.0) - (A - 1.0) * cw - tsa)       / a0);
        }
        return c;
    }

    void applyChannelFx (int c, int num)
    {
        auto& f  = fx[c];
        auto& ch = state.channels[c];

        const float lowDb  = ch.eqLowDb.load  (std::memory_order_relaxed);
        const float midDb  = ch.eqMidDb.load  (std::memory_order_relaxed);
        const float highDb = ch.eqHighDb.load (std::memory_order_relaxed);
        const bool  eqOn   = std::abs (lowDb) > 0.05f || std::abs (midDb) > 0.05f || std::abs (highDb) > 0.05f;

        float* l = scratch.getWritePointer (0);
        float* r = scratch.getWritePointer (1);

        if (eqOn)
        {
            if (std::abs (lowDb - f.lastLow) > 0.01f)
            {
                const auto cs = shelf (sampleRate, 180.0, lowDb, true);
                f.low[0].c = cs; f.low[1].c = cs; f.lastLow = lowDb;
            }
            if (std::abs (midDb - f.lastMid) > 0.01f)
            {
                const auto cs = peakEQ (sampleRate, 1000.0, midDb);
                f.peak[0].c = cs; f.peak[1].c = cs; f.lastMid = midDb;
            }
            if (std::abs (highDb - f.lastHigh) > 0.01f)
            {
                const auto cs = shelf (sampleRate, 4000.0, highDb, false);
                f.high[0].c = cs; f.high[1].c = cs; f.lastHigh = highDb;
            }

            for (int i = 0; i < num; ++i)
            {
                l[i] = f.high[0].process (f.peak[0].process (f.low[0].process (l[i])));
                r[i] = f.high[1].process (f.peak[1].process (f.low[1].process (r[i])));
            }
        }
        else
        {
            f.lastLow = f.lastMid = f.lastHigh = -999;   // force recompute when re-enabled
        }

        if (ch.compOn.load (std::memory_order_relaxed))
        {
            const float thr    = ch.compThreshDb.load (std::memory_order_relaxed);
            const float ratio  = juce::jmax (1.0f, ch.compRatio.load (std::memory_order_relaxed));
            const float makeup = juce::Decibels::decibelsToGain ((1.0f - 1.0f / ratio) * (-thr) * 0.5f);

            for (int i = 0; i < num; ++i)
            {
                const float in   = juce::jmax (std::abs (l[i]), std::abs (r[i]));
                const float coef = in > f.env ? compAtk : compRel;
                f.env = in + coef * (f.env - in);

                const float envDb  = juce::Decibels::gainToDecibels (f.env + 1.0e-6f);
                float gainDb = 0.0f;
                if (envDb > thr)
                    gainDb = -(envDb - thr) * (1.0f - 1.0f / ratio);

                const float g = juce::Decibels::decibelsToGain (gainDb) * makeup;
                l[i] *= g;
                r[i] *= g;
            }
        }
        else
        {
            f.env = 0.0f;
        }
    }

    std::shared_ptr<Instrument> getOrBuildPreset (int presetIndex)
    {
        auto it = presetCache.find (presetIndex);
        if (it != presetCache.end())
            return it->second;

        auto inst = soundFont->buildInstrument (presetIndex);
        if (inst != nullptr)
            presetCache[presetIndex] = inst;             // retained forever -> no RT dealloc
        return inst;
    }

    void startVoice (int channel, int note, float velocity)
    {
        if (channel < 0 || channel >= SharedState::numChannels)
            return;

        auto inst = std::atomic_load (&channelInstrument[channel]);
        if (inst == nullptr || inst->zones.empty())
            return;

        const int vel = juce::jlimit (1, 127, (int) std::round (velocity * 127.0f));
        const SampleZone* chosen = pickZone (inst.get(), note, vel);
        if (chosen == nullptr)
            return;

        Voice* v = findFreeVoice();
        v->keep = inst;
        v->zone = chosen;
        v->pos = 0.0;
        v->note = note;
        v->channel = channel;
        v->level = velocity;   // per-channel gain/pan applied live in render()

        const double srcHz = juce::MidiMessage::getMidiNoteInHertz (chosen->rootNote);
        const double dstHz = juce::MidiMessage::getMidiNoteInHertz (note)
                           * std::pow (2.0, chosen->tuneSemitones / 12.0);
        v->pitchRatio = (dstHz / srcHz) * (chosen->sourceSampleRate / sampleRate);

        v->env.setParameters ({ chosen->envAttack, chosen->envDecay, chosen->envSustain, chosen->envRelease });
        v->env.noteOn();
    }

    void stopVoice (int channel, int note)
    {
        for (auto& v : voices)
            if (v.zone != nullptr && v.channel == channel && v.note == note && v.env.isActive())
                v.env.noteOff();
    }

    static const SampleZone* pickZone (const Instrument* inst, int note, int vel)
    {
        for (const auto& z : inst->zones)
            if (note >= z.loKey && note <= z.hiKey && vel >= z.loVel && vel <= z.hiVel)
                return &z;
        return nullptr;
    }

    Voice* findFreeVoice()
    {
        for (auto& v : voices)
            if (! v.env.isActive())
                return &v;
        Voice* oldest = &voices[0];
        double best = -1.0;
        for (auto& v : voices)
            if (v.pos > best) { best = v.pos; oldest = &v; }
        return oldest;
    }

    void render (juce::AudioBuffer<float>& out, int start, int num)
    {
        if (num <= 0)
            return;

        const int outCh = out.getNumChannels();

        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            // Any active voice on this channel?
            bool any = false;
            for (auto& v : voices)
                if (v.channel == c && v.zone != nullptr && v.env.isActive()) { any = true; break; }
            if (! any)
                continue;

            scratch.clear (0, num);

            const float g   = state.channels[c].gain.load (std::memory_order_relaxed);
            const float pan = juce::jlimit (-1.0f, 1.0f, state.channels[c].pan.load (std::memory_order_relaxed));
            const float th  = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;   // 0..pi/2
            const float lg  = std::cos (th) * g;
            const float rg  = std::sin (th) * g;

            float* sl = scratch.getWritePointer (0);
            float* sr = scratch.getWritePointer (1);

            for (auto& v : voices)
            {
                if (v.channel != c || v.zone == nullptr || ! v.env.isActive())
                    continue;

                const auto& buf = v.zone->data;
                const int   len = buf.getNumSamples();

                for (int i = 0; i < num; ++i)
                {
                    if (v.pos >= (double) (len - 1) && ! v.zone->loopEnabled)
                    {
                        v.env.reset();
                        v.zone = nullptr;
                        v.keep.reset();                  // safe: message thread still holds a ref
                        break;
                    }

                    const float env  = v.env.getNextSample();
                    const float samp = readInterpolated (buf, 0, v.pos) * env * v.zone->gain * v.level;
                    sl[i] += samp * lg;
                    sr[i] += samp * rg;

                    v.pos += v.pitchRatio;

                    if (v.zone->loopEnabled && v.zone->loopEnd > v.zone->loopStart
                        && v.pos >= (double) v.zone->loopEnd)
                        v.pos -= (double) (v.zone->loopEnd - v.zone->loopStart);
                }
            }

            applyChannelFx (c, num);

            if (outCh >= 2)
            {
                out.addFrom (0, start, scratch, 0, 0, num);
                out.addFrom (1, start, scratch, 1, 0, num);
            }
            else if (outCh == 1)
            {
                out.addFrom (0, start, scratch, 0, 0, num, 0.5f);
                out.addFrom (0, start, scratch, 1, 0, num, 0.5f);
            }
        }
    }

    static float readInterpolated (const juce::AudioBuffer<float>& buf, int ch, double pos)
    {
        const int i0 = (int) pos;
        const int i1 = juce::jmin (i0 + 1, buf.getNumSamples() - 1);
        const float f = (float) (pos - (double) i0);
        return buf.getSample (ch, i0) * (1.0f - f) + buf.getSample (ch, i1) * f;
    }

    /** A generated plucked EP so channels are audible before any SoundFont loads. */
    static std::shared_ptr<Instrument> createDefaultInstrument (double sr)
    {
        auto inst = std::make_shared<Instrument>();
        inst->name = "EP (generated)";
        const int roots[] = { 36, 48, 60, 72, 84 };
        const int lo[]    = { 0,  42, 54, 66, 78 };
        const int hi[]    = { 41, 53, 65, 77, 127 };

        for (int k = 0; k < 5; ++k)
        {
            SampleZone z;
            z.sourceSampleRate = sr;
            z.rootNote = roots[k];
            z.loKey = lo[k]; z.hiKey = hi[k];
            const double freq = juce::MidiMessage::getMidiNoteInHertz (roots[k]);
            const int    n    = (int) (sr * 1.6);
            z.data.setSize (1, n);
            auto* d = z.data.getWritePointer (0);
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / sr;
                const double e = std::exp (-3.2 * t);
                const double ph = juce::MathConstants<double>::twoPi * freq * t;
                double v = std::sin (ph) + std::sin (2*ph)*0.5*std::exp(-6*t) + std::sin (3*ph)*0.25*std::exp(-9*t);
                d[i] = (float) (v * e * 0.25);
            }
            inst->zones.push_back (std::move (z));
        }
        return inst;
    }

    // --- Synthesized vocal library (formant vowels + choir, no download) -----
    static std::shared_ptr<Instrument> makeVowel (double sr, const juce::String& name,
                                                  double f1, double f2, double f3)
    {
        auto inst = std::make_shared<Instrument>();
        inst->name = name;

        const int roots[] = { 48, 60, 72 };
        const int lo[]    = { 0,  55, 67 };
        const int hi[]    = { 54, 66, 127 };

        auto formantAmp = [f1, f2, f3] (double f)
        {
            auto res = [] (double freq, double fc, double bw, double g)
            {
                const double x = (freq - fc) / (bw * 0.5);
                return g / (1.0 + x * x);
            };
            return res (f, f1, 90.0, 1.0) + res (f, f2, 110.0, 0.6)
                 + res (f, f3, 140.0, 0.35) + 0.02;
        };

        for (int k = 0; k < 3; ++k)
        {
            SampleZone z;
            z.sourceSampleRate = sr;
            z.rootNote = roots[k];
            z.loKey = lo[k]; z.hiKey = hi[k];

            const double f0base   = juce::MidiMessage::getMidiNoteInHertz (roots[k]);
            const int    period   = juce::jmax (2, (int) std::round (sr / f0base));
            const double f0        = sr / (double) period;         // sample-aligned -> seamless loop
            const int    cycles    = juce::jmax (1, (int) std::round (f0 * 1.2));
            const int    n         = period * cycles;
            const int    maxH      = juce::jlimit (1, 48, (int) ((sr * 0.45) / f0));

            z.data.setSize (1, n);
            auto* d = z.data.getWritePointer (0);

            float peak = 1.0e-6f;
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / sr;
                double s = 0.0;
                for (int h = 1; h <= maxH; ++h)
                    s += (formantAmp (f0 * h) / (double) h)
                       * std::sin (juce::MathConstants<double>::twoPi * f0 * (double) h * t);
                d[i] = (float) s;
                peak = juce::jmax (peak, std::abs (d[i]));
            }
            const float norm = 0.6f / peak;
            for (int i = 0; i < n; ++i) d[i] *= norm;

            z.loopEnabled = true;
            z.loopStart   = 0;
            z.loopEnd     = n;
            z.envAttack   = 0.03f;
            z.envDecay    = 0.0f;
            z.envSustain  = 1.0f;
            z.envRelease  = 0.25f;
            z.gain        = 1.0f;
            inst->zones.push_back (std::move (z));
        }
        return inst;
    }

    // --- Lazily-built extra instruments (voices + imported sample folders) ---
    struct Extra
    {
        juce::String name;
        std::vector<juce::File> files;              // empty => prebuilt (voice)
        std::shared_ptr<Instrument> built;          // cached once built
    };

    std::shared_ptr<Instrument> buildExtra (int e)
    {
        if (e < 0 || e >= (int) extras.size())
            return nullptr;
        auto& ex = extras[(size_t) e];
        if (ex.built != nullptr)
            return ex.built;
        ex.built = buildInstrumentFromFiles (ex.name, ex.files);   // cached (retained forever)
        return ex.built;
    }

    /** Build an Instrument from a folder's audio files. If >=2 files carry note names
        in their filename it becomes a melodic multisample; otherwise the files are laid
        out one-per-key as a one-shot bank (drum kit / vocal-chop pad), starting at C1. */
    std::shared_ptr<Instrument> buildInstrumentFromFiles (const juce::String& name,
                                                          const std::vector<juce::File>& files)
    {
        auto inst = std::make_shared<Instrument>();
        inst->name = name;
        if (files.empty())
            return inst;

        std::vector<int> notes ((size_t) files.size(), -1);
        int named = 0;
        for (size_t i = 0; i < files.size(); ++i)
        {
            notes[i] = SampleLib::parseNoteName (files[i].getFileName());
            if (notes[i] >= 0) ++named;
        }
        const bool melodic = named >= 2 && named >= (int) files.size() / 2;

        auto loadZone = [this] (const juce::File& f, SampleZone& z) -> bool
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (f));
            if (reader == nullptr) return false;
            int len = (int) juce::jmin ((juce::int64) (reader->sampleRate * 30.0), reader->lengthInSamples);
            if (len <= 0) return false;

            juce::AudioBuffer<float> tmp ((int) juce::jmax ((juce::uint32) 1, reader->numChannels), len);
            reader->read (&tmp, 0, len, 0, true, true);
            z.data.setSize (1, len);
            z.data.copyFrom (0, 0, tmp, 0, 0, len);
            if (tmp.getNumChannels() > 1) { z.data.addFrom (0, 0, tmp, 1, 0, len); z.data.applyGain (0.5f); }
            z.sourceSampleRate = reader->sampleRate;
            return true;
        };

        if (melodic)
        {
            struct NF { int note; juce::File file; };
            std::vector<NF> nfs;
            for (size_t i = 0; i < files.size(); ++i)
                if (notes[i] >= 0) nfs.push_back ({ notes[i], files[i] });
            std::sort (nfs.begin(), nfs.end(), [] (const NF& a, const NF& b) { return a.note < b.note; });

            for (size_t i = 0; i < nfs.size(); ++i)
            {
                SampleZone z;
                if (! loadZone (nfs[i].file, z)) continue;
                z.rootNote = nfs[i].note;
                z.loKey = (i == 0)              ? 0   : (nfs[i - 1].note + nfs[i].note) / 2 + 1;
                z.hiKey = (i + 1 == nfs.size()) ? 127 : (nfs[i].note + nfs[i + 1].note) / 2;
                z.envAttack = 0.005f; z.envSustain = 1.0f; z.envRelease = 0.15f;
                inst->zones.push_back (std::move (z));
            }
        }
        else
        {
            int key = 36;                              // C1 upward, one file per key
            for (const auto& f : files)
            {
                if (key > 96) break;
                SampleZone z;
                if (! loadZone (f, z)) continue;
                z.rootNote = key; z.loKey = key; z.hiKey = key;
                z.envAttack = 0.001f; z.envSustain = 1.0f; z.envRelease = 0.06f;
                inst->zones.push_back (std::move (z));
                ++key;
            }
        }
        return inst;
    }

    static std::vector<std::shared_ptr<Instrument>> buildVoiceInstruments (double sr)
    {
        std::vector<std::shared_ptr<Instrument>> v;
        v.push_back (makeVowel (sr, "Voice: Ah",    700.0, 1220.0, 2600.0));
        v.push_back (makeVowel (sr, "Voice: Eh",    530.0, 1840.0, 2480.0));
        v.push_back (makeVowel (sr, "Voice: Ee",    270.0, 2290.0, 3010.0));
        v.push_back (makeVowel (sr, "Voice: Oh",    570.0,  840.0, 2410.0));
        v.push_back (makeVowel (sr, "Voice: Oo",    300.0,  870.0, 2240.0));
        v.push_back (makeVowel (sr, "Voice: Choir", 660.0, 1120.0, 2750.0));
        return v;
    }

    SharedState& state;
    juce::AudioFormatManager formatManager;
    double sampleRate = 44100.0;

    std::unique_ptr<SoundFont> soundFont;
    std::shared_ptr<Instrument> fallback;
    std::vector<Extra> extras;                         // voices + imported folders (lazy, retained)
    int voiceCount = 0;                                // first `voiceCount` extras are the voices
    std::shared_ptr<Instrument> channelInstrument[SharedState::numChannels];   // atomic_load/store
    std::map<int, std::shared_ptr<Instrument>> presetCache;                    // retained forever

    std::array<Voice, 64> voices;

    juce::AudioBuffer<float> scratch;      // per-channel render scratch (sized in prepareToPlay)
    ChannelFx fx[SharedState::numChannels];
    float compAtk = 0.0f, compRel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerProcessor)
};
