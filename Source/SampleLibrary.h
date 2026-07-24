#pragma once

#include <JuceHeader.h>
#include <vector>

/**
    SampleLibrary — turns a folder tree of audio files into a catalog of instruments.

    - extractZips(): any .zip sitting in the root is unpacked into a folder of the same
      name (once), so a user can just drop downloaded packs in and rescan.
    - scan(): recursively finds every "leaf" folder (one that directly contains audio
      files) and returns it as one instrument (name = path relative to the root, files =
      the audio files in it). SamplerEngine builds the actual Instrument lazily from this.
    - parseNoteName(): pulls a MIDI note out of a filename like "Piano_C3.wav" (C4 = 60),
      used to decide melodic multisample vs. one-shot drum/vox bank.
*/
namespace SampleLib
{
    inline bool isAudio (const juce::File& f)
    {
        return f.hasFileExtension ("wav;aiff;aif;flac;ogg;mp3");
    }

    /** MIDI note from a filename token like C3 / C#3 / Db3 / A-1, or -1 if none. C4 = 60. */
    inline int parseNoteName (const juce::String& nameIn)
    {
        const juce::String name = nameIn.upToLastOccurrenceOf (".", false, false);
        const int n = name.length();
        int result = -1;   // take the LAST plausible match (note usually near the end)

        for (int i = 0; i < n; ++i)
        {
            const juce::juce_wchar ch = name[i];
            const int letter = juce::String ("CDEFGAB").indexOfChar (ch);
            if (letter < 0) continue;

            // Must be delimited on the left (start or a separator) to avoid false hits.
            if (i > 0)
            {
                const juce::juce_wchar p = name[i - 1];
                if (juce::CharacterFunctions::isLetterOrDigit (p)) continue;
            }

            const int semis[] = { 0, 2, 4, 5, 7, 9, 11 };  // C D E F G A B
            int semitone = semis[letter];
            int j = i + 1;

            if (j < n && (name[j] == '#' || name[j] == 's' || name[j] == 'S')) { ++semitone; ++j; }
            else if (j < n && (name[j] == 'b' || name[j] == 'B'))              { --semitone; ++j; }

            bool neg = false;
            if (j < n && name[j] == '-') { neg = true; ++j; }

            int digits = 0, octave = 0;
            while (j < n && name[j] >= '0' && name[j] <= '9') { octave = octave * 10 + (int) (name[j] - '0'); ++digits; ++j; }
            if (digits == 0) continue;                       // needs an octave number
            if (neg) octave = -octave;
            if (octave < -1 || octave > 9) continue;

            const int midi = (octave + 1) * 12 + semitone;   // C4 = 60
            if (midi >= 0 && midi <= 127) result = midi;
        }
        return result;
    }

    inline void extractZips (const juce::File& root)
    {
        if (! root.isDirectory()) return;
        for (const auto& z : root.findChildFiles (juce::File::findFiles, false, "*.zip"))
        {
            auto outDir = root.getChildFile (z.getFileNameWithoutExtension());
            if (outDir.isDirectory()) continue;             // already extracted
            juce::ZipFile zip (z);
            outDir.createDirectory();
            zip.uncompressTo (outDir, false);
        }
    }

    struct FolderInstrument
    {
        juce::String name;                  // path relative to root
        std::vector<juce::File> files;
    };

    /** Recursively collect leaf folders (folders that directly contain audio files). */
    inline void scanInto (const juce::File& dir, const juce::File& root,
                          std::vector<FolderInstrument>& out, int depth)
    {
        if (depth > 8 || out.size() > 4000) return;

        std::vector<juce::File> audio;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false))
            if (isAudio (f))
                audio.push_back (f);

        if (! audio.empty())
        {
            std::sort (audio.begin(), audio.end(),
                       [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
            FolderInstrument fi;
            if (dir == root)
            {
                fi.name = "Samples";
            }
            else
            {
                // Display the last two path segments (e.g. "01-hard-trap/808s") so the
                // dropdown stays navigable even with hundreds of imported folders.
                const auto rel   = dir.getRelativePathFrom (root).replaceCharacter ('\\', '/');
                const auto parts = juce::StringArray::fromTokens (rel, "/", "");
                fi.name = parts.size() >= 2 ? parts[parts.size() - 2] + "/" + parts[parts.size() - 1]
                                            : rel;
            }
            fi.files = std::move (audio);
            out.push_back (std::move (fi));
        }

        for (const auto& sub : dir.findChildFiles (juce::File::findDirectories, false))
            scanInto (sub, root, out, depth + 1);
    }

    inline std::vector<FolderInstrument> scan (const juce::File& root)
    {
        std::vector<FolderInstrument> out;
        if (root.isDirectory())
            scanInto (root, root, out, 0);
        return out;
    }
}
