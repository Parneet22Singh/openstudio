# OpenStudio

A native, locally-installed desktop DAW built in C++20 with the JUCE framework, delivered in verifiable phases. Real software, honest scope - each phase compiles and makes audible, low-latency sound before the next begins. This is not, and does not claim to be, a commercial-grade superset of Ableton/FL/GarageBand; it is a genuine, growing DAW that implements those workflows phase by phase.

## Current build - what it does

- Opens the default audio device via `juce::AudioDeviceManager` (WASAPI/ASIO on Windows) with a small buffer for low latency.
- A master `AudioProcessorGraph`: Sequencer → (MIDI) → Synth/Sampler → Mixer → Master → Output.
- A subtractive synth (saw / square / sine oscillator, ADSR, resonant TPT low-pass filter), 16-voice polyphonic.
- A sampler engine with SFZ and SF2 loading, backed by a sample library browser.
- A piano roll for note-level MIDI editing, alongside the step sequencer.
- A mixer with a master processing bus, and a live oscilloscope for visual feedback.
- Project save/load (project I/O).
- A 16-step × 4-row step sequencer with sample-accurate scheduling, tempo 40–240 BPM, and a live playhead.
- Real-time-safe audio thread: no allocation, no locks, no file I/O on the audio callback. GUI ↔ audio communication is via atomics in `Source/SharedState.h`.
- Dark DAW look-and-feel.

## Try it

1. Click cells in the grid to program a pattern (four rows = four notes).
2. Press Play. Adjust Tempo, Wave, Cutoff, Reso while it runs - changes are heard immediately, with no dropouts.

## Prerequisites (Windows) - install these first

> If your system drive is low on space, install the toolchain to a drive with more free room - a JUCE build tree plus (later) bundled content can run into the tens of GB.

### 1. CMake (add to PATH)

```
winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
```

### 2. Visual Studio 2022 Build Tools (C++ toolchain, ~7 GB)

Install the Build Tools with the C++ workload. To steer the install onto a different drive, use the layout/nickname options, or run the interactive installer and choose your preferred install location:

```
winget install --id Microsoft.VisualStudio.2022.BuildTools -e ^
  --override "--wait --passive --nocache --installPath <INSTALL_PATH> ^
  --add Microsoft.VisualStudio.Workload.VCTools ^
  --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
  --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

(If winget isn't available, download "Build Tools for Visual Studio 2022" from Microsoft, run it, and choose Desktop development with C++.)

Reopen your terminal after installing so `cmake` is on PATH.

### 3. Bundled tools (optional, already included)

A `tools/` folder in the project root ships with portable copies of **Ninja**, **CMake (Windows)**, and **MinGW**. If you'd rather not install the Visual Studio Build Tools, or want a self-contained build, the build scripts can pick these up from `tools/` automatically instead of relying on system-wide installs. This folder is not tracked by version control - restore it from your distribution/release package if missing.

## Build

```
cd <path-to-OpenStudio>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The first configure downloads JUCE (shallow clone) into `build/` - allow a few minutes. The executable lands at:

```
<path-to-OpenStudio>\build\OpenStudio_artefacts\Release\OpenStudio.exe
```

Run it by double-clicking, or:

```
.\build\OpenStudio_artefacts\Release\OpenStudio.exe
```

## Project layout

| File | Role |
|---|---|
| `Source/SharedState.h` | Lock-free GUI↔audio state (atomics). The one shared truth. |
| `Source/SubtractiveSynth.h` | Synth voice + SynthProcessor graph node. |
| `Source/SequencerProcessor.h` | MIDI-generating step sequencer graph node (sample-accurate). |
| `Source/SamplerEngine.h` | Sampler voice/engine - plays back loaded sample and multisample content. |
| `Source/SamplerTypes.h` | Shared types/structs used by the sampler (regions, zones, velocity layers, etc.). |
| `Source/SampleLibrary.h` | Sample browsing and library management. |
| `Source/Sf2Loader.h` | Loader for SoundFont (`.sf2`) instrument files. |
| `Source/SfzParser.h` | Parser for the SFZ sample-instrument format. |
| `Source/PianoRoll.h` | Piano roll component for note-level MIDI editing. |
| `Source/Mixer.h` | Mixer channel strips and bus routing. |
| `Source/MasterProcessor.h` | Master bus processing node in the graph. |
| `Source/Oscilloscope.h` | Real-time waveform/level visualization component. |
| `Source/ProjectIO.h` | Project save/load (serialization). |
| `Source/NoteNames.h` | Note-name/pitch utility helpers (e.g. MIDI note number ↔ name). |
| `Source/AudioEngine.{h,cpp}` | Owns the device + AudioProcessorGraph; wires the nodes. |
| `Source/MainComponent.{h,cpp}` | GUI: transport, step grid, piano roll, mixer, synth/sampler controls. |
| `Source/Main.cpp` | App + window bootstrap. |
| `CMakeLists.txt` | Build; fetches JUCE 8.0.4. |
| `Content/` | User-created folder for your own samples and multisample libraries (see below). |
| `tools/` | Bundled portable build tools - Ninja, CMake (Windows), and MinGW (see above). Not tracked by version control. |

### The `Content/` folder

`Content/` lives in the project root and is not created automatically - add it yourself if you want to bring your own sound library. Drop your own samples, one-shots, and multisample instruments in here; from Phase 2 onward (the sampler/drum-rack work), OpenStudio will look in `Content/` for user-supplied audio material alongside anything bundled with the app. Keeping user content in its own top-level folder keeps it separate from the source tree and easy to back up, gitignore, or swap out.

### The `tools/` folder

`tools/` also lives in the project root and holds portable, no-install-required copies of the build toolchain: **Ninja** (fast build backend), **CMake for Windows**, and **MinGW** (an alternative to the MSVC toolchain). These let you build the project without installing Visual Studio Build Tools system-wide. Like `Content/`, this folder is excluded from version control - it ships with releases/distributions, or you can populate it yourself with matching binaries if you're setting up the repo from scratch.

## Architecture (holds for every later phase)

- Audio graph = `juce::AudioProcessorGraph`; instruments/effects/plugins are nodes.
- Audio thread is lock-free & allocation-free. All cross-thread data flows through atomics / lock-free FIFOs.
- 64-bit-capable, 44.1k–192k. Later phases add the ValueTree project model + undo, the sampler, effects, plugin hosting (VST3/AU/CLAP), arrangement/session views, recording, and export - all onto this same graph.

## Roadmap

- Phase 1 - transport + synth + step sequencer through the graph
- Phase 2 - SFZ/SF2 sampler + sample library + piano roll in progress; drum rack + bundled multisample library still to come
- Phase 3 - mixer + master bus underway; effects racks, automation, and VST3/AU/CLAP hosting still to come
- Phase 4 - arrangement + session clip launcher + warp/time-stretch
- Phase 5 - recording (audio+MIDI, comping, PDC) + loop browser + AI drummer
- Phase 6 - basic project save/load in progress; collect-assets + export (mixdown/stems/MIDI, LUFS) + installers still to come

See `LICENSES.md` for licensing (JUCE + future content packs).