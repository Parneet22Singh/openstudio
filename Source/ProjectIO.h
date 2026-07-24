#pragma once

#include <JuceHeader.h>
#include "SharedState.h"
#include "SamplerEngine.h"

/**
    Project persistence (.ostudio XML, version 2).

    Saves the whole SharedState: tempo / swing / metronome, master FX, every channel
    (instrument + mixer strip + insert EQ/compressor), every pattern (length + note
    grid), and the song arrangement. Loading restores the state and re-applies each
    channel's instrument on the sampler.
*/
namespace ProjectIO
{
    inline juce::ValueTree toTree (SharedState& s)
    {
        juce::ValueTree root ("OpenStudioProject");
        root.setProperty ("version", 2, nullptr);
        root.setProperty ("bpm",     (double) s.bpm.load(), nullptr);
        root.setProperty ("swing",   s.swing.load(), nullptr);
        root.setProperty ("metro",   s.metronomeOn.load(), nullptr);
        root.setProperty ("curPat",  s.currentPattern.load(), nullptr);
        root.setProperty ("songMode",s.songMode.load(), nullptr);
        root.setProperty ("songLen", s.songLength.load(), nullptr);
        root.setProperty ("mGain",   s.master.gain.load(), nullptr);
        root.setProperty ("revMix",  s.master.reverbMix.load(), nullptr);
        root.setProperty ("revRoom", s.master.reverbRoom.load(), nullptr);
        root.setProperty ("dlyMix",  s.master.delayMix.load(), nullptr);
        root.setProperty ("dlyMs",   s.master.delayMs.load(), nullptr);
        root.setProperty ("dlyFb",   s.master.delayFeedback.load(), nullptr);

        juce::ValueTree chans ("Channels");
        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            auto& ch = s.channels[c];
            juce::ValueTree t ("Channel");
            t.setProperty ("preset",   ch.preset.load(), nullptr);
            t.setProperty ("editNote", ch.editNote.load(), nullptr);
            t.setProperty ("pan",      ch.pan.load(), nullptr);
            t.setProperty ("gain",     ch.gain.load(), nullptr);
            t.setProperty ("muted",    ch.muted.load(), nullptr);
            t.setProperty ("eqLo",     ch.eqLowDb.load(), nullptr);
            t.setProperty ("eqMid",    ch.eqMidDb.load(), nullptr);
            t.setProperty ("eqHi",     ch.eqHighDb.load(), nullptr);
            t.setProperty ("compOn",   ch.compOn.load(), nullptr);
            t.setProperty ("compThr",  ch.compThreshDb.load(), nullptr);
            t.setProperty ("compRat",  ch.compRatio.load(), nullptr);
            chans.addChild (t, -1, nullptr);
        }
        root.addChild (chans, -1, nullptr);

        juce::ValueTree pats ("Patterns");
        for (int p = 0; p < SharedState::numPatterns; ++p)
        {
            juce::ValueTree t ("Pattern");
            t.setProperty ("length", s.patterns[p].length.load(), nullptr);

            juce::String cells;
            for (int c = 0; c < SharedState::numChannels; ++c)
                for (int pit = 0; pit < SharedState::numPitches; ++pit)
                    for (int st = 0; st < SharedState::maxSteps; ++st)
                        if (s.patterns[p].cells[c][pit][st].load())
                            cells << c << "," << pit << "," << st << ";";
            t.setProperty ("cells", cells, nullptr);
            pats.addChild (t, -1, nullptr);
        }
        root.addChild (pats, -1, nullptr);

        juce::String song;
        for (int i = 0; i < SharedState::maxSong; ++i)
            song << s.songSlots[i].load() << ";";
        root.setProperty ("song", song, nullptr);

        return root;
    }

    inline bool save (SharedState& s, const juce::File& file)
    {
        if (auto xml = toTree (s).createXml())
            return xml->writeTo (file);
        return false;
    }

    inline bool load (SharedState& s, const juce::File& file, SamplerProcessor* sampler)
    {
        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr)
            return false;

        auto root = juce::ValueTree::fromXml (*xml);
        if (! root.isValid() || root.getType().toString() != "OpenStudioProject")
            return false;

        s.playing.store (false);
        s.bpm.store          ((double) root.getProperty ("bpm", 120.0));
        s.swing.store        ((float) (double) root.getProperty ("swing", 0.0));
        s.metronomeOn.store  ((bool) root.getProperty ("metro", false));
        s.currentPattern.store ((int) root.getProperty ("curPat", 0));
        s.songMode.store     ((bool) root.getProperty ("songMode", false));
        s.songLength.store   ((int) root.getProperty ("songLen", 0));
        s.master.gain.store         ((float) (double) root.getProperty ("mGain", 0.9));
        s.master.reverbMix.store    ((float) (double) root.getProperty ("revMix", 0.14));
        s.master.reverbRoom.store   ((float) (double) root.getProperty ("revRoom", 0.5));
        s.master.delayMix.store     ((float) (double) root.getProperty ("dlyMix", 0.0));
        s.master.delayMs.store      ((float) (double) root.getProperty ("dlyMs", 375.0));
        s.master.delayFeedback.store((float) (double) root.getProperty ("dlyFb", 0.35));

        // Channels
        auto chans = root.getChildWithName ("Channels");
        int c = 0;
        for (auto t : chans)
        {
            if (c >= SharedState::numChannels) break;
            auto& ch = s.channels[c];
            const int preset = (int) t.getProperty ("preset", 0);
            ch.preset.store (preset);
            ch.editNote.store ((int) t.getProperty ("editNote", 60));
            ch.pan.store   ((float) (double) t.getProperty ("pan", 0.0));
            ch.gain.store  ((float) (double) t.getProperty ("gain", 0.85));
            ch.muted.store ((bool) t.getProperty ("muted", false));
            ch.eqLowDb.store  ((float) (double) t.getProperty ("eqLo", 0.0));
            ch.eqMidDb.store  ((float) (double) t.getProperty ("eqMid", 0.0));
            ch.eqHighDb.store ((float) (double) t.getProperty ("eqHi", 0.0));
            ch.compOn.store   ((bool) t.getProperty ("compOn", false));
            ch.compThreshDb.store ((float) (double) t.getProperty ("compThr", -18.0));
            ch.compRatio.store    ((float) (double) t.getProperty ("compRat", 3.0));

            if (sampler != nullptr)
                sampler->setChannelPreset (c, preset);
            ++c;
        }

        // Patterns
        for (int p = 0; p < SharedState::numPatterns; ++p)
            for (int cc = 0; cc < SharedState::numChannels; ++cc)
                for (int pit = 0; pit < SharedState::numPitches; ++pit)
                    for (int st = 0; st < SharedState::maxSteps; ++st)
                        s.patterns[p].cells[cc][pit][st].store (false);

        auto pats = root.getChildWithName ("Patterns");
        int pi = 0;
        for (auto t : pats)
        {
            if (pi >= SharedState::numPatterns) break;
            s.patterns[pi].length.store (juce::jlimit (1, SharedState::maxSteps, (int) t.getProperty ("length", 16)));

            const juce::String cells = t.getProperty ("cells", "").toString();
            for (auto& tok : juce::StringArray::fromTokens (cells, ";", ""))
            {
                if (tok.isEmpty()) continue;
                auto v = juce::StringArray::fromTokens (tok, ",", "");
                if (v.size() == 3)
                {
                    const int cc = v[0].getIntValue(), pit = v[1].getIntValue(), st = v[2].getIntValue();
                    if (cc >= 0 && cc < SharedState::numChannels
                        && pit >= 0 && pit < SharedState::numPitches
                        && st >= 0 && st < SharedState::maxSteps)
                        s.patterns[pi].cells[cc][pit][st].store (true);
                }
            }
            ++pi;
        }

        // Song arrangement
        for (int i = 0; i < SharedState::maxSong; ++i)
            s.songSlots[i].store (-1);
        const juce::String song = root.getProperty ("song", "").toString();
        int si = 0;
        for (auto& tok : juce::StringArray::fromTokens (song, ";", ""))
        {
            if (si >= SharedState::maxSong) break;
            if (tok.isEmpty()) continue;
            s.songSlots[si].store (juce::jlimit (-1, SharedState::numPatterns - 1, tok.getIntValue()));
            ++si;
        }

        return true;
    }
}
