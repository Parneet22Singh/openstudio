#pragma once

#include <JuceHeader.h>
#include "SharedState.h"
#include "AudioEngine.h"
#include "SamplerEngine.h"   // ChannelRack calls SamplerProcessor methods inline
#include "PianoRoll.h"
#include "Mixer.h"
#include "ProjectIO.h"
#include "Oscilloscope.h"

/**
    Channel Rack: one row per channel (16), each with its own instrument, a mute button,
    a piano-roll button and a quick step row that edits the CURRENT pattern. A playhead
    tracks the audio when the current pattern is the one sounding.
*/
class ChannelRack : public juce::Component,
                    private juce::Timer
{
public:
    ChannelRack (SharedState& s, AudioEngine& e) : state (s), engine (e)
    {
        if (auto* smp = engine.getSampler())
            presetNames = smp->getSoundFontPresetNames();

        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            auto& combo = combos[c];
            for (int i = 0; i < presetNames.size(); ++i)
                combo.addItem (presetNames[i], i + 1);
            combo.setEnabled (! presetNames.isEmpty());
            combo.setSelectedItemIndex (state.channels[c].preset.load(), juce::dontSendNotification);
            combo.onChange = [this, c]
            {
                const int idx = combos[c].getSelectedItemIndex();
                state.channels[c].preset.store (idx);
                if (auto* smp = engine.getSampler())
                    smp->setChannelPreset (c, idx);
            };
            addAndMakeVisible (combo);

            auto& m = mutes[c];
            m.setButtonText ("M");
            m.setClickingTogglesState (true);
            m.setToggleState (state.channels[c].muted.load(), juce::dontSendNotification);
            m.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffcc5555));
            m.onClick = [this, c] { state.channels[c].muted.store (mutes[c].getToggleState()); };
            addAndMakeVisible (m);

            auto& ed = edits[c];
            ed.setButtonText ("Piano Roll");
            ed.onClick = [this, c] { openEditor (c); };
            addAndMakeVisible (ed);
        }

        startTimerHz (30);
    }

    ~ChannelRack() override { stopTimer(); }

    void refreshFromState()
    {
        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            combos[c].setSelectedItemIndex (state.channels[c].preset.load(), juce::dontSendNotification);
            mutes[c].setToggleState (state.channels[c].muted.load(), juce::dontSendNotification);
        }
        repaint();
    }

    /** Repopulate every channel's instrument dropdown (after importing samples). */
    void reloadInstrumentList()
    {
        if (auto* smp = engine.getSampler())
            presetNames = smp->getSoundFontPresetNames();

        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            auto& combo = combos[c];
            combo.clear (juce::dontSendNotification);
            for (int i = 0; i < presetNames.size(); ++i)
                combo.addItem (presetNames[i], i + 1);
            combo.setEnabled (! presetNames.isEmpty());
            combo.setSelectedItemIndex (state.channels[c].preset.load(), juce::dontSendNotification);
        }
    }

    void resized() override
    {
        const int rh = getHeight() / SharedState::numChannels;
        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            auto row = juce::Rectangle<int> (0, c * rh, leftPanel, rh).reduced (6, 4);
            mutes[c].setBounds (row.removeFromRight (26));
            row.removeFromRight (5);
            edits[c].setBounds (row.removeFromRight (78).withSizeKeepingCentre (78, 24));
            row.removeFromRight (5);
            combos[c].setBounds (row.withSizeKeepingCentre (row.getWidth(), 24));
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff14171c));

        const int  pat  = curPattern();
        const int  nsp  = steps();
        const int  rh   = getHeight() / SharedState::numChannels;
        const float cw  = (float) (getWidth() - leftPanel) / (float) nsp;
        const int  play = state.currentStep.load (std::memory_order_relaxed);
        const bool live = state.playingPattern.load (std::memory_order_relaxed) == pat;

        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            g.setColour (juce::Colour (0xff1b1f26));
            g.fillRect (0, c * rh, leftPanel, rh);
            g.setColour (juce::Colour (0xff2a2f38));
            g.drawHorizontalLine ((c + 1) * rh - 1, 0.0f, (float) getWidth());

            const int editNote = state.channels[c].editNote.load (std::memory_order_relaxed);
            for (int s = 0; s < nsp; ++s)
            {
                juce::Rectangle<float> cell (leftPanel + s * cw, (float) (c * rh), cw, (float) rh);
                cell.reduce (2.0f, 3.0f);

                const bool on     = state.patterns[pat].cells[c][editNote][s].load (std::memory_order_relaxed);
                const bool onBeat = (s % 4) == 0;
                const bool isPlay = live && (s == play);

                juce::Colour base = on ? juce::Colour (0xff3fc9a0)
                                       : (onBeat ? juce::Colour (0xff2a2f38) : juce::Colour (0xff20242b));
                if (isPlay) base = base.brighter (on ? 0.25f : 0.55f);

                g.setColour (base);
                g.fillRoundedRectangle (cell, 3.0f);
                if (isPlay)
                {
                    g.setColour (juce::Colour (0xffe0b341));
                    g.drawRoundedRectangle (cell, 3.0f, 1.2f);
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x < leftPanel)
            return;

        const int  nsp = steps();
        const int  rh  = getHeight() / SharedState::numChannels;
        const float cw = (float) (getWidth() - leftPanel) / (float) nsp;
        const int c = juce::jlimit (0, SharedState::numChannels - 1, e.y / rh);
        const int s = juce::jlimit (0, nsp - 1, (int) ((e.x - leftPanel) / cw));
        const int pat = curPattern();

        const int editNote = state.channels[c].editNote.load (std::memory_order_relaxed);
        const bool now = ! state.patterns[pat].cells[c][editNote][s].load (std::memory_order_relaxed);
        state.patterns[pat].cells[c][editNote][s].store (now);
        repaint();
    }

private:
    void timerCallback() override { repaint(); }

    int curPattern() const { return juce::jlimit (0, SharedState::numPatterns - 1, state.currentPattern.load (std::memory_order_relaxed)); }
    int steps()      const { return juce::jlimit (1, SharedState::maxSteps, state.patterns[curPattern()].length.load (std::memory_order_relaxed)); }

    void openEditor (int channel)
    {
        if (rollWindow == nullptr)
            rollWindow = std::make_unique<PianoRollWindow> (state);

        bool drum = false;
        if (auto* smp = engine.getSampler())
            drum = smp->isDrumPreset (state.channels[channel].preset.load());

        const juce::String title = "Piano Roll  -  Channel " + juce::String (channel + 1)
                                  + (drum ? "  (Drum Kit)" : "");
        rollWindow->show (channel, drum, title);
    }

    static constexpr int leftPanel = 340;

    SharedState& state;
    AudioEngine& engine;
    juce::StringArray presetNames;
    juce::ComboBox   combos[SharedState::numChannels];
    juce::TextButton mutes [SharedState::numChannels];
    juce::TextButton edits [SharedState::numChannels];
    std::unique_ptr<PianoRollWindow> rollWindow;
};

/**
    Arrangement bar: click a slot to cycle its pattern (empty -> 1..N -> empty). The song
    plays these slots in order (song mode). The playhead marks the current slot.
*/
class ArrangementBar : public juce::Component,
                       private juce::Timer
{
public:
    explicit ArrangementBar (SharedState& s) : state (s) { startTimerHz (20); }
    ~ArrangementBar() override { stopTimer(); }

    void addSlot()
    {
        int n = state.songLength.load();
        if (n < maxVisible())
        {
            if (state.songSlots[n].load() < 0) state.songSlots[n].store (0);   // default to pattern 1
            state.songLength.store (n + 1);
            repaint();
        }
    }

    void delSlot()
    {
        int n = state.songLength.load();
        if (n > 0) { state.songLength.store (n - 1); state.songSlots[n - 1].store (-1); repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff0e1013));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        const int n   = state.songLength.load();
        const int pos = state.songPos.load();

        for (int i = 0; i < n && i < maxVisible(); ++i)
        {
            auto r = slotRect (i);
            const int v = state.songSlots[i].load();
            g.setColour (i == pos ? juce::Colour (0xff3fc9a0) : juce::Colour (0xff262b33));
            g.fillRoundedRectangle (r.toFloat().reduced (2.0f), 3.0f);
            g.setColour (i == pos ? juce::Colour (0xff10141a) : juce::Colour (0xffe6e9ef));
            g.setFont (juce::Font (12.0f, juce::Font::bold));
            g.drawText (v < 0 ? "-" : juce::String (v + 1), r, juce::Justification::centred);
        }

        g.setColour (juce::Colour (0xff5b6472));
        g.setFont (juce::Font (10.0f));
        if (n == 0)
            g.drawText ("(empty — Add slots, click to set pattern)", getLocalBounds().reduced (8, 0),
                        juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int n = state.songLength.load();
        for (int i = 0; i < n && i < maxVisible(); ++i)
            if (slotRect (i).contains (e.getPosition()))
            {
                int v = state.songSlots[i].load() + 1;
                if (v >= SharedState::numPatterns) v = -1;   // cycle back to empty
                state.songSlots[i].store (v);
                repaint();
                return;
            }
    }

private:
    void timerCallback() override { repaint(); }

    int slotW() const { return 30; }
    int maxVisible() const { return juce::jmax (1, getWidth() / slotW()); }
    juce::Rectangle<int> slotRect (int i) const { return { i * slotW(), 2, slotW(), getHeight() - 4 }; }

    SharedState& state;
};

class MainComponent : public juce::Component
{
public:
    MainComponent (SharedState& state, AudioEngine& engine);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void selectPattern (int p);

    static constexpr int topH = 100;   // header height (two control rows)

    SharedState& state;
    AudioEngine& engine;

    // Transport
    juce::TextButton playButton  { "Play" };
    juce::TextButton mixerButton { "Mixer" };
    juce::TextButton saveButton  { "Save" };
    juce::TextButton loadButton  { "Load" };
    juce::TextButton exportButton{ "Export WAV" };
    juce::TextButton rescanButton{ "Rescan" };
    juce::TextButton metroButton { "Metronome" };
    juce::Slider     bpmSlider;
    juce::Label      bpmLabel  { {}, "TEMPO" };
    juce::Slider     swingSlider;
    juce::Label      swingLabel { {}, "SWING" };

    // Pattern / song row
    juce::TextButton patternButtons[SharedState::numPatterns];
    juce::Label      patternLabel { {}, "PATTERN" };
    juce::Slider     lengthSlider;
    juce::Label      lengthLabel  { {}, "STEPS" };
    juce::TextButton songButton   { "Song Mode" };
    juce::TextButton addSlotButton{ "+" };
    juce::TextButton delSlotButton{ "-" };

    Oscilloscope     scope;
    ArrangementBar   arrangement;
    ChannelRack      rack;

    std::unique_ptr<MixerWindow> mixerWindow;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
