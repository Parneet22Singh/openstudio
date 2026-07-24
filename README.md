# OpenStudio

A native, locally-installed desktop DAW built in C++20 with the JUCE framework,
delivered in verifiable phases. **Real software, honest scope** — each phase compiles
and makes audible, low-latency sound before the next begins. This is not, and does not
claim to be, a commercial-grade superset of Ableton/FL/GarageBand; it is a genuine,
growing DAW that implements those workflows phase by phase.

Lives on `D:\OpenStudio` because the C: drive is full and a JUCE build tree +
(later) bundled content is tens of GB.

---

## Phase 1 (this build) — what it does

- Opens the default audio device via `juce::AudioDeviceManager` (WASAPI/ASIO on
  Windows) with a small buffer for low latency.
- A master **`AudioProcessorGraph`**: `Sequencer → (MIDI) → Synth → (audio) → Output`.
- A **subtractive synth** (saw / square / sine oscillator, ADSR, resonant TPT
  low-pass filter), 16-voice polyphonic.
- A **16-step × 4-row step sequencer** with sample-accurate scheduling, tempo
  40–240 BPM, and a live playhead.
- **Real-time-safe audio thread**: no allocation, no locks, no file I/O on the audio
  callback. GUI ↔ audio communication is via atomics in `Source/SharedState.h`.
- Dark DAW look-and-feel.

### Try it
1. Click cells in the grid to program a pattern (four rows = four notes).
2. Press **Play**. Adjust **Tempo**, **Wave**, **Cutoff**, **Reso** while it runs —
   changes are heard immediately, with no dropouts.

---

## Prerequisites (Windows) — install these first

Install to **D:** wherever possible; C: has almost no free space.

### 1. CMake (add to PATH)
```powershell
winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
```

### 2. Visual Studio 2022 Build Tools (C++ toolchain, ~7 GB)
Install the Build Tools with the C++ workload. To steer the install onto D:, use the
layout/nickname options, or run the interactive installer and set the install
location to `D:\VS`:
```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e ^
  --override "--wait --passive --nocache --installPath D:\VS ^
  --add Microsoft.VisualStudio.Workload.VCTools ^
  --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
  --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```
(If `winget` isn't available, download "Build Tools for Visual Studio 2022" from
Microsoft, run it, choose **Desktop development with C++**, and set the install
location to `D:\VS`.)

Reopen your terminal after installing so `cmake` is on PATH.

---

## Build

```powershell
cd D:\OpenStudio
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The first configure downloads JUCE (shallow clone) into `build/` — allow a few
minutes. The executable lands at:

```
D:\OpenStudio\build\OpenStudio_artefacts\Release\OpenStudio.exe
```

Run it by double-clicking, or:
```powershell
.\build\OpenStudio_artefacts\Release\OpenStudio.exe
```

---

## Project layout

| File | Role |
|------|------|
| `Source/SharedState.h`        | Lock-free GUI↔audio state (atomics). The one shared truth. |
| `Source/SubtractiveSynth.h`   | Synth voice + `SynthProcessor` graph node. |
| `Source/SequencerProcessor.h` | MIDI-generating step sequencer graph node (sample-accurate). |
| `Source/AudioEngine.{h,cpp}`  | Owns the device + `AudioProcessorGraph`; wires the nodes. |
| `Source/MainComponent.{h,cpp}`| GUI: transport, step grid, synth controls. |
| `Source/Main.cpp`             | App + window bootstrap. |
| `CMakeLists.txt`              | Build; fetches JUCE 8.0.4. |

## Architecture (holds for every later phase)
- **Audio graph** = `juce::AudioProcessorGraph`; instruments/effects/plugins are nodes.
- **Audio thread is lock-free & allocation-free.** All cross-thread data flows through
  atomics / lock-free FIFOs.
- 64-bit-capable, 44.1k–192k. Later phases add the ValueTree project model + undo, the
  sampler, effects, plugin hosting (VST3/AU/CLAP), arrangement/session views, recording,
  and export — all onto this same graph.

## Roadmap
- **Phase 1 ✅** transport + synth + step sequencer through the graph *(this build)*
- Phase 2 — SFZ/SF2 sampler + bundled multisample library + drum rack + piano roll
- Phase 3 — built-in effects + racks + automation + VST3/AU/CLAP hosting
- Phase 4 — arrangement + session clip launcher + warp/time-stretch
- Phase 5 — recording (audio+MIDI, comping, PDC) + loop browser + AI drummer
- Phase 6 — save/load + collect-assets + export (mixdown/stems/MIDI, LUFS) + installers

See `LICENSES.md` for licensing (JUCE + future content packs).
