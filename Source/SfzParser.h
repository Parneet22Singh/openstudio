#pragma once

#include <JuceHeader.h>
#include "SamplerEngine.h"

/**
    A pragmatic SFZ loader. Supports the opcodes that cover the vast majority of
    free SFZ libraries:

      headers : <control> <group> <region>
      control : default_path
      mapping : sample, key, lokey, hikey, pitch_keycenter,
                lovel, hivel, seq_length, seq_position, group
      playback: loop_mode (no_loop | loop_continuous | one_shot),
                loop_start, loop_end, volume (dB), tune (cents), transpose

    Group opcodes act as defaults for the regions that follow, until the next group.
    Not a full SFZ v2 implementation, but enough to play real instrument packs.
*/
struct SfzParser
{
    static std::shared_ptr<Instrument> parse (const juce::File& sfzFile,
                                              juce::AudioFormatManager& fm)
    {
        if (! sfzFile.existsAsFile())
            return {};

        auto inst = std::make_shared<Instrument>();
        inst->name = sfzFile.getFileNameWithoutExtension();

        const juce::File baseDir = sfzFile.getParentDirectory();
        juce::String defaultPath;

        // Opcode maps for the current <group> defaults and the region being built.
        juce::StringPairArray groupOpcodes, regionOpcodes;
        juce::String section;                            // "control" | "group" | "region"
        bool haveRegion = false;

        auto flushRegion = [&]
        {
            if (! haveRegion)
                return;

            juce::StringPairArray op = groupOpcodes;
            for (auto& key : regionOpcodes.getAllKeys())
                op.set (key, regionOpcodes[key]);

            addRegion (*inst, op, baseDir, defaultPath, fm);
            regionOpcodes.clear();
            haveRegion = false;
        };

        juce::StringArray lines;
        lines.addLines (sfzFile.loadFileAsString());

        for (auto rawLine : lines)
        {
            auto line = stripComment (rawLine).trim();
            if (line.isEmpty())
                continue;

            // A line may contain a header token and/or several opcode= pairs.
            juce::StringArray tokens;
            tokens.addTokens (line, " \t", "\"");
            tokens.removeEmptyStrings();

            for (auto token : tokens)
            {
                if (token.startsWithChar ('<'))
                {
                    const juce::String hdr = token.removeCharacters ("<>").toLowerCase();
                    if (hdr == "region")
                    {
                        flushRegion();
                        section = "region";
                        haveRegion = true;
                    }
                    else if (hdr == "group")
                    {
                        flushRegion();
                        section = "group";
                        groupOpcodes.clear();
                    }
                    else
                    {
                        section = hdr;                   // control / global / etc.
                    }
                    continue;
                }

                const int eq = token.indexOfChar ('=');
                if (eq <= 0)
                    continue;

                const juce::String key = token.substring (0, eq).trim().toLowerCase();
                const juce::String val = token.substring (eq + 1).trim();

                if (section == "control" && key == "default_path")
                    defaultPath = val;
                else if (section == "group")
                    groupOpcodes.set (key, val);
                else if (section == "region")
                    regionOpcodes.set (key, val);
            }
        }

        flushRegion();
        return inst->zones.empty() ? std::shared_ptr<Instrument>() : inst;
    }

private:
    static juce::String stripComment (const juce::String& line)
    {
        const int c = line.indexOf ("//");
        return c >= 0 ? line.substring (0, c) : line;
    }

    static int opInt (const juce::StringPairArray& op, juce::StringRef key, int fallback)
    {
        return op.containsKey (key) ? op[key].getIntValue() : fallback;
    }

    static void addRegion (Instrument& inst, const juce::StringPairArray& op,
                           const juce::File& baseDir, const juce::String& defaultPath,
                           juce::AudioFormatManager& fm)
    {
        if (! op.containsKey ("sample"))
            return;

        juce::String rel = op["sample"].replaceCharacter ('\\', '/');
        juce::File sampleFile = baseDir.getChildFile (defaultPath).getChildFile (rel);
        if (! sampleFile.existsAsFile())
            sampleFile = baseDir.getChildFile (rel);
        if (! sampleFile.existsAsFile())
            return;

        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (sampleFile));
        if (reader == nullptr)
            return;

        SampleZone z;
        z.sourceSampleRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;
        z.data.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&z.data, 0, (int) reader->lengthInSamples, 0, true, true);

        const int key = opInt (op, "key", -1);
        z.rootNote = opInt (op, "pitch_keycenter", key >= 0 ? key : 60);
        z.loKey = opInt (op, "lokey", key >= 0 ? key : 0);
        z.hiKey = opInt (op, "hikey", key >= 0 ? key : 127);
        z.loVel = opInt (op, "lovel", 1);
        z.hiVel = opInt (op, "hivel", 127);
        z.seqLength   = opInt (op, "seq_length", 1);
        z.seqPosition = opInt (op, "seq_position", 1);
        z.group       = opInt (op, "group", 0);

        const juce::String loopMode = op.containsKey ("loop_mode") ? op["loop_mode"] : juce::String();
        z.loopEnabled = loopMode.containsIgnoreCase ("loop");
        z.loopStart = opInt (op, "loop_start", 0);
        z.loopEnd   = opInt (op, "loop_end", 0);

        if (op.containsKey ("volume"))
            z.gain = juce::Decibels::decibelsToGain (op["volume"].getFloatValue());

        z.tuneSemitones = opInt (op, "transpose", 0)
                        + (op.containsKey ("tune") ? op["tune"].getFloatValue() / 100.0 : 0.0);

        inst.zones.push_back (std::move (z));
    }
};
