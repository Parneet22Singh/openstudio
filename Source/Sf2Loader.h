#pragma once

#include <JuceHeader.h>
#include <vector>
#include <map>
#include <algorithm>
#include "SamplerTypes.h"

/**
    A pragmatic SoundFont 2 (.sf2) loader.

    SF2 is a RIFF file with three top-level LISTs: INFO, sdta (the raw 16-bit PCM
    sample pool), and pdta (the articulation data: preset/instrument/sample tables
    plus "generators" that map samples to key/velocity zones).

    Strategy for an 8 GB machine: parse the small pdta tables into memory once, but
    keep only the file offset of the big sample pool. When an instrument (preset) is
    selected we read just that preset's sample audio from disk. So RAM holds one
    instrument's samples, not the whole 140 MB bank.

    Scope: resolves the preset -> instrument -> sample zone hierarchy and the common
    generators (key/vel range, root key, tune, loop, attenuation). Modulators, filters
    and envelope generators beyond a basic release are ignored - enough to play every
    GM instrument and drum kit cleanly.
*/
class SoundFont
{
public:
    struct PresetInfo
    {
        juce::String name;
        int bank = 0, program = 0;
        int bagStart = 0, bagEnd = 0;
    };

    bool load (const juce::File& f)
    {
        file = f;
        juce::FileInputStream in (f);
        if (! in.openedOk())
            return false;

        char id[5] = {};
        in.read (id, 4);                         // 'RIFF'
        if (juce::String (id) != "RIFF") return false;
        in.readInt();                            // riff size
        in.read (id, 4);                         // 'sfbk'
        if (juce::String (id) != "sfbk") return false;

        while (! in.isExhausted())
        {
            char cid[5] = {};
            if (in.read (cid, 4) != 4) break;
            const juce::int64 csize = (juce::uint32) in.readInt();
            const juce::int64 chunkStart = in.getPosition();

            const juce::String cstr (cid);
            if (cstr == "LIST")
            {
                char listType[5] = {};
                in.read (listType, 4);
                const juce::String lt (listType);

                if (lt == "sdta")
                {
                    // find 'smpl' subchunk
                    while (in.getPosition() < chunkStart + csize && ! in.isExhausted())
                    {
                        char sid[5] = {};
                        in.read (sid, 4);
                        const juce::int64 ssize = (juce::uint32) in.readInt();
                        if (juce::String (sid) == "smpl")
                        {
                            smplOffset = in.getPosition();
                            smplSize   = ssize;
                        }
                        in.setPosition (in.getPosition() + ssize);
                    }
                }
                else if (lt == "pdta")
                {
                    parsePdta (in, chunkStart + csize);
                }
            }
            in.setPosition (chunkStart + csize + (csize & 1));   // word align
        }

        buildPresetList();
        return smplOffset > 0 && ! presets.empty();
    }

    const std::vector<PresetInfo>& getPresets() const { return presets; }

    /** Diagnostic: for each preset, report the instrument + first sample it resolves to,
        and how many playable zones it produced (0 = would fail to build). */
    juce::String report() const
    {
        juce::String out;
        for (int pi = 0; pi < (int) presets.size(); ++pi)
        {
            const auto& p = presets[(size_t) pi];
            juce::String instName = "(none)", sampName = "(none)";
            int nZones = 0;

            for (int pz = p.bagStart; pz < p.bagEnd; ++pz)
            {
                int instId = -1;
                for (int g = pbag[pz].genNdx; g < pbag[pz + 1].genNdx; ++g)
                    if (pgen[g].op == GEN_instrument) instId = (juce::uint16) pgen[g].amount;

                if (instId >= 0 && instId + 1 < (int) instRec.size())
                {
                    instName = instRec[instId].name;
                    for (int iz = instRec[instId].bagNdx; iz < instRec[instId + 1].bagNdx; ++iz)
                    {
                        int sid = -1;
                        for (int g = ibag[iz].genNdx; g < ibag[iz + 1].genNdx; ++g)
                            if (igen[g].op == GEN_sampleID) sid = (juce::uint16) igen[g].amount;
                        if (sid >= 0 && sid < (int) samples.size())
                        {
                            ++nZones;
                            if (sampName == "(none)") sampName = samples[(size_t) sid].name;
                        }
                    }
                    break;
                }
            }

            out << pi << " | " << p.bank << ":" << p.program << " | " << p.name
                << "  ->  inst[" << instName << "]  zones=" << nZones
                << "  firstSample=" << sampName << "\n";
        }
        return out;
    }

    /** Build a playable Instrument for the given preset index (reads its samples from disk). */
    std::shared_ptr<Instrument> buildInstrument (int presetIndex)
    {
        if (presetIndex < 0 || presetIndex >= (int) presets.size())
            return {};

        const auto& p = presets[presetIndex];
        auto inst = std::make_shared<Instrument>();
        inst->name = p.name;

        juce::FileInputStream in (file);
        if (! in.openedOk())
            return {};

        std::map<int, int> presetGlobal;

        for (int pz = p.bagStart; pz < p.bagEnd; ++pz)
        {
            std::map<int, int> pgens = presetGlobal;
            int instId = -1;
            for (int g = pbag[pz].genNdx; g < pbag[pz + 1].genNdx; ++g)
            {
                if (pgen[g].op == GEN_instrument) instId = (juce::uint16) pgen[g].amount;
                else                              pgens[pgen[g].op] = pgen[g].amount;
            }

            if (instId < 0) { presetGlobal = pgens; continue; }
            if (instId + 1 >= (int) instRec.size()) continue;

            std::map<int, int> instGlobal;
            for (int iz = instRec[instId].bagNdx; iz < instRec[instId + 1].bagNdx; ++iz)
            {
                std::map<int, int> igens = instGlobal;
                int sampleId = -1;
                for (int g = ibag[iz].genNdx; g < ibag[iz + 1].genNdx; ++g)
                {
                    if (igen[g].op == GEN_sampleID) sampleId = (juce::uint16) igen[g].amount;
                    else                            igens[igen[g].op] = igen[g].amount;
                }

                if (sampleId < 0) { instGlobal = igens; continue; }
                if (sampleId >= (int) samples.size()) continue;

                buildZone (*inst, in, sampleId, igens, pgens);
            }
        }

        return inst->zones.empty() ? std::shared_ptr<Instrument>() : inst;
    }

private:
    // --- SF2 record structs -------------------------------------------------
    struct Phdr { juce::String name; int preset, bank, bagNdx; };
    struct Bag  { int genNdx, modNdx; };
    struct Gen  { int op, amount; };
    struct Inst { juce::String name; int bagNdx; };
    struct Shdr { juce::String name; juce::uint32 start, end, startLoop, endLoop, sampleRate;
                  int originalPitch, pitchCorrection; };

    enum {
        GEN_startAddrsOffset = 0, GEN_endAddrsOffset = 1,
        GEN_startloopAddrsOffset = 2, GEN_endloopAddrsOffset = 3,
        GEN_attackVolEnv = 34, GEN_decayVolEnv = 36, GEN_sustainVolEnv = 37, GEN_releaseVolEnv = 38,
        GEN_instrument = 41, GEN_keyRange = 43, GEN_velRange = 44,
        GEN_initialAttenuation = 48, GEN_coarseTune = 51, GEN_fineTune = 52,
        GEN_sampleID = 53, GEN_sampleModes = 54, GEN_overridingRootKey = 58
    };

    static float timecentsToSeconds (int tc) { return (float) std::pow (2.0, tc / 1200.0); }

    static juce::String readName (juce::InputStream& in)
    {
        char buf[21] = {};
        in.read (buf, 20);
        return juce::String (buf);
    }

    void parsePdta (juce::FileInputStream& in, juce::int64 end)
    {
        while (in.getPosition() < end && ! in.isExhausted())
        {
            char sid[5] = {};
            in.read (sid, 4);
            const juce::int64 sz = (juce::uint32) in.readInt();
            const juce::int64 next = in.getPosition() + sz;
            const juce::String s (sid);

            if (s == "phdr")
                for (int i = 0; i < sz / 38; ++i)
                { Phdr h; h.name = readName (in); h.preset = (juce::uint16) in.readShort();
                  h.bank = (juce::uint16) in.readShort(); h.bagNdx = (juce::uint16) in.readShort();
                  in.readInt(); in.readInt(); in.readInt(); phdr.push_back (h); }
            else if (s == "pbag")
                for (int i = 0; i < sz / 4; ++i)
                { Bag b; b.genNdx = (juce::uint16) in.readShort(); b.modNdx = (juce::uint16) in.readShort(); pbag.push_back (b); }
            else if (s == "pgen")
                for (int i = 0; i < sz / 4; ++i)
                { Gen g; g.op = (juce::uint16) in.readShort(); g.amount = (juce::uint16) in.readShort(); pgen.push_back (g); }
            else if (s == "inst")
                for (int i = 0; i < sz / 22; ++i)
                { Inst n; n.name = readName (in); n.bagNdx = (juce::uint16) in.readShort(); instRec.push_back (n); }
            else if (s == "ibag")
                for (int i = 0; i < sz / 4; ++i)
                { Bag b; b.genNdx = (juce::uint16) in.readShort(); b.modNdx = (juce::uint16) in.readShort(); ibag.push_back (b); }
            else if (s == "igen")
                for (int i = 0; i < sz / 4; ++i)
                { Gen g; g.op = (juce::uint16) in.readShort(); g.amount = (juce::uint16) in.readShort(); igen.push_back (g); }
            else if (s == "shdr")
                for (int i = 0; i < sz / 46; ++i)
                { Shdr h; h.name = readName (in);
                  h.start = (juce::uint32) in.readInt(); h.end = (juce::uint32) in.readInt();
                  h.startLoop = (juce::uint32) in.readInt(); h.endLoop = (juce::uint32) in.readInt();
                  h.sampleRate = (juce::uint32) in.readInt();
                  h.originalPitch = in.readByte(); h.pitchCorrection = (juce::int8) in.readByte();
                  in.readShort(); in.readShort();          // sampleLink, sampleType
                  samples.push_back (h); }

            in.setPosition (next);
        }
    }

    void buildPresetList()
    {
        for (int i = 0; i + 1 < (int) phdr.size(); ++i)      // last phdr is the EOP terminal
        {
            PresetInfo p;
            p.name = phdr[i].name;
            p.bank = phdr[i].bank;
            p.program = phdr[i].preset;
            p.bagStart = phdr[i].bagNdx;
            p.bagEnd = phdr[i + 1].bagNdx;
            presets.push_back (p);
        }
        // Order: melodic banks first (bank 0), then drums (bank 128), by program.
        std::sort (presets.begin(), presets.end(), [] (const PresetInfo& a, const PresetInfo& b)
                   { return a.bank != b.bank ? a.bank < b.bank : a.program < b.program; });
    }

    static int genOr (const std::map<int, int>& g, int op, int fallback)
    {
        auto it = g.find (op);
        return it != g.end() ? it->second : fallback;
    }

    void buildZone (Instrument& inst, juce::FileInputStream& in, int sampleId,
                    const std::map<int, int>& igens, const std::map<int, int>& pgens)
    {
        const Shdr& sh = samples[(size_t) sampleId];

        SampleZone z;
        z.sourceSampleRate = sh.sampleRate > 0 ? (double) sh.sampleRate : 44100.0;

        const int kr = genOr (igens, GEN_keyRange, 0x7f00);      // default 0..127
        z.loKey = kr & 0xff;  z.hiKey = (kr >> 8) & 0xff;
        const int vr = genOr (igens, GEN_velRange, 0x7f00);
        z.loVel = juce::jmax (1, vr & 0xff);  z.hiVel = (vr >> 8) & 0xff;

        const int rootOverride = genOr (igens, GEN_overridingRootKey, -1);
        z.rootNote = rootOverride >= 0 ? rootOverride : sh.originalPitch;

        const int coarse = (juce::int16) genOr (igens, GEN_coarseTune, 0) + (juce::int16) genOr (pgens, GEN_coarseTune, 0);
        const int fine   = (juce::int16) genOr (igens, GEN_fineTune, 0)   + (juce::int16) genOr (pgens, GEN_fineTune, 0);
        z.tuneSemitones = coarse + (fine + sh.pitchCorrection) / 100.0;

        const int atten = genOr (igens, GEN_initialAttenuation, 0) + genOr (pgens, GEN_initialAttenuation, 0);
        z.gain = (float) std::pow (10.0, -atten / 200.0);        // centibels -> linear

        // Volume envelope (instrument gens + preset gens as timecent/centibel offsets).
        const int atk = (juce::int16) genOr (igens, GEN_attackVolEnv,  -12000) + (juce::int16) genOr (pgens, GEN_attackVolEnv,  0);
        const int dec = (juce::int16) genOr (igens, GEN_decayVolEnv,   -12000) + (juce::int16) genOr (pgens, GEN_decayVolEnv,   0);
        const int rel = (juce::int16) genOr (igens, GEN_releaseVolEnv, -12000) + (juce::int16) genOr (pgens, GEN_releaseVolEnv, 0);
        const int sus = (juce::int16) genOr (igens, GEN_sustainVolEnv, 0)      + (juce::int16) genOr (pgens, GEN_sustainVolEnv, 0);

        z.envAttack  = juce::jlimit (0.001f, 8.0f, timecentsToSeconds (atk));
        z.envDecay   = juce::jlimit (0.001f, 12.0f, timecentsToSeconds (dec));
        z.envRelease = juce::jlimit (0.02f, 12.0f, timecentsToSeconds (rel));
        z.envSustain = juce::jlimit (0.0f, 1.0f, (float) std::pow (10.0, -sus / 200.0));

        // Sample address (with fine offset generators applied).
        const juce::int64 start = (juce::int64) sh.start
                                + genOr (igens, GEN_startAddrsOffset, 0);
        const juce::int64 endS  = (juce::int64) sh.end
                                + genOr (igens, GEN_endAddrsOffset, 0);
        const juce::int64 len   = juce::jmax ((juce::int64) 0, endS - start);
        if (len <= 0) return;

        z.data.setSize (1, (int) len);
        readPcm (in, start, (int) len, z.data.getWritePointer (0));

        const int mode = genOr (igens, GEN_sampleModes, 0);
        z.loopEnabled = (mode == 1 || mode == 3);
        z.loopStart = (int) ((juce::int64) sh.startLoop + genOr (igens, GEN_startloopAddrsOffset, 0) - start);
        z.loopEnd   = (int) ((juce::int64) sh.endLoop   + genOr (igens, GEN_endloopAddrsOffset, 0)   - start);
        if (z.loopEnd <= z.loopStart || z.loopEnd > (int) len)
            z.loopEnabled = false;

        inst.zones.push_back (std::move (z));
    }

    void readPcm (juce::FileInputStream& in, juce::int64 startFrame, int numFrames, float* dest)
    {
        in.setPosition (smplOffset + startFrame * 2);
        juce::HeapBlock<juce::int16> raw ((size_t) numFrames);
        in.read (raw.getData(), numFrames * 2);              // little-endian 16-bit PCM
        for (int i = 0; i < numFrames; ++i)
            dest[i] = (float) juce::ByteOrder::littleEndianShort (&raw[i]) / 32768.0f;
    }

    juce::File file;
    juce::int64 smplOffset = 0, smplSize = 0;

    std::vector<Phdr> phdr;
    std::vector<Bag>  pbag, ibag;
    std::vector<Gen>  pgen, igen;
    std::vector<Inst> instRec;
    std::vector<Shdr> samples;
    std::vector<PresetInfo> presets;
};
