#pragma once

#include <JuceHeader.h>
#include "SharedState.h"
#include "AudioEngine.h"
#include "SamplerEngine.h"

/**
    Mixer: one strip per channel and a master section.

    Each channel strip has a 3-band EQ (low / mid / high shelves + peak), a compressor
    toggle, a pan knob, a volume fader and a mute button. The master section has reverb,
    delay and output-gain controls. Every control writes straight to SharedState atomics.
    The strip row is wide, so it lives inside a scrollable viewport (see MixerWindow).
*/
class Mixer : public juce::Component
{
public:
    static constexpr int stripW   = 78;
    static constexpr int masterW  = 220;
    static constexpr int headerH  = 40;

    Mixer (SharedState& s, AudioEngine& e) : state (s), engine (e)
    {
        if (auto* smp = engine.getSampler())
            presetNames = smp->getSoundFontPresetNames();

        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            miniKnob (eqHi[c],  -12.0, 12.0, state.channels[c].eqHighDb);
            miniKnob (eqMid[c], -12.0, 12.0, state.channels[c].eqMidDb);
            miniKnob (eqLo[c],  -12.0, 12.0, state.channels[c].eqLowDb);

            auto& cp = comp[c];
            cp.setButtonText ("C");
            cp.setClickingTogglesState (true);
            cp.setToggleState (state.channels[c].compOn.load(), juce::dontSendNotification);
            cp.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f8fc9));
            cp.onClick = [this, c] { state.channels[c].compOn.store (comp[c].getToggleState()); };
            addAndMakeVisible (cp);

            auto& p = pan[c];
            p.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            p.setRange (-1.0, 1.0, 0.01);
            p.setValue (state.channels[c].pan.load(), juce::dontSendNotification);
            p.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            p.onValueChange = [this, c] { state.channels[c].pan.store ((float) pan[c].getValue()); };
            addAndMakeVisible (p);

            auto& v = vol[c];
            v.setSliderStyle (juce::Slider::LinearVertical);
            v.setRange (0.0, 1.3, 0.01);
            v.setValue (state.channels[c].gain.load(), juce::dontSendNotification);
            v.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 16);
            v.onValueChange = [this, c] { state.channels[c].gain.store ((float) vol[c].getValue()); };
            addAndMakeVisible (v);

            auto& m = mute[c];
            m.setButtonText ("M");
            m.setClickingTogglesState (true);
            m.setToggleState (state.channels[c].muted.load(), juce::dontSendNotification);
            m.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffcc5555));
            m.onClick = [this, c] { state.channels[c].muted.store (mute[c].getToggleState()); };
            addAndMakeVisible (m);
        }

        bindRotary (revMix,  0.0, 1.0,     state.master.reverbMix);
        bindRotary (revRoom, 0.0, 1.0,     state.master.reverbRoom);
        bindRotary (dlyMix,  0.0, 1.0,     state.master.delayMix);
        bindRotary (dlyTime, 20.0, 1000.0, state.master.delayMs);
        bindRotary (dlyFb,   0.0, 0.95,    state.master.delayFeedback);

        masterGain.setSliderStyle (juce::Slider::LinearVertical);
        masterGain.setRange (0.0, 1.5, 0.01);
        masterGain.setValue (state.master.gain.load(), juce::dontSendNotification);
        masterGain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 16);
        masterGain.onValueChange = [this] { state.master.gain.store ((float) masterGain.getValue()); };
        addAndMakeVisible (masterGain);

        setSize (SharedState::numChannels * stripW + masterW, 470);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff14171c));
        g.setFont (juce::Font (11.0f, juce::Font::bold));

        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            const int x = c * stripW;
            g.setColour ((c % 2) ? juce::Colour (0xff171b21) : juce::Colour (0xff14171c));
            g.fillRect (x, 0, stripW, getHeight());

            const juce::String nm = presetNames.isEmpty() ? juce::String()
                                    : presetNames[juce::jlimit (0, presetNames.size() - 1, state.channels[c].preset.load())];
            g.setColour (juce::Colour (0xffe6e9ef));
            g.setFont (juce::Font (11.0f, juce::Font::bold));
            g.drawText ("Ch " + juce::String (c + 1), x, 2, stripW, 14, juce::Justification::centred);
            g.setColour (juce::Colour (0xff8b93a0));
            g.setFont (juce::Font (9.0f));
            g.drawFittedText (nm, x + 2, 16, stripW - 4, 22, juce::Justification::centredTop, 2);

            g.setColour (juce::Colour (0xff5b6472));
            g.setFont (juce::Font (8.5f));
            g.drawText ("EQ  H  M  L", x, headerH - 2, stripW, 10, juce::Justification::centred);
        }

        const int mx = SharedState::numChannels * stripW;
        g.setColour (juce::Colour (0xff1b1f26));
        g.fillRect (mx, 0, getWidth() - mx, getHeight());
        g.setColour (juce::Colour (0xffe0b341));
        g.setFont (juce::Font (11.0f, juce::Font::bold));
        g.drawText ("MASTER", mx, 4, getWidth() - mx, 16, juce::Justification::centred);

        g.setColour (juce::Colour (0xff8b93a0));
        g.setFont (juce::Font (10.0f));
        auto lbl = [&] (juce::Slider& s, const char* t) { g.drawText (t, s.getX(), s.getY() - 13, s.getWidth(), 12, juce::Justification::centred); };
        lbl (revMix, "Reverb"); lbl (revRoom, "Room");
        lbl (dlyMix, "Delay");  lbl (dlyTime, "Time"); lbl (dlyFb, "Fbk");
    }

    void resized() override
    {
        for (int c = 0; c < SharedState::numChannels; ++c)
        {
            auto strip = juce::Rectangle<int> (c * stripW, headerH + 10, stripW, getHeight() - headerH - 10).reduced (5, 4);

            auto eqRow = strip.removeFromTop (30);
            const int kw = eqRow.getWidth() / 3;
            eqHi [c].setBounds (eqRow.removeFromLeft (kw));
            eqMid[c].setBounds (eqRow.removeFromLeft (kw));
            eqLo [c].setBounds (eqRow);

            comp[c].setBounds (strip.removeFromTop (20).withSizeKeepingCentre (28, 18));
            pan [c].setBounds (strip.removeFromTop (36));
            mute[c].setBounds (strip.removeFromBottom (22).withSizeKeepingCentre (30, 20));
            vol [c].setBounds (strip);
        }

        const int mx = SharedState::numChannels * stripW;
        auto master = juce::Rectangle<int> (mx, 24, getWidth() - mx, getHeight() - 24).reduced (10, 8);

        auto knobs = master.removeFromTop (150);
        auto row1 = knobs.removeFromTop (70);
        revMix.setBounds  (row1.removeFromLeft (row1.getWidth() / 2).reduced (6));
        revRoom.setBounds (row1.reduced (6));
        auto row2 = knobs.removeFromTop (70);
        dlyMix.setBounds  (row2.removeFromLeft (row2.getWidth() / 3).reduced (4));
        dlyTime.setBounds (row2.removeFromLeft (row2.getWidth() / 2).reduced (4));
        dlyFb.setBounds   (row2.reduced (4));

        masterGain.setBounds (master.withSizeKeepingCentre (60, master.getHeight()));
    }

private:
    void miniKnob (juce::Slider& s, double lo, double hi, std::atomic<float>& target)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setRange (lo, hi, 0.1);
        s.setValue (target.load(), juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.onValueChange = [&s, &target] { target.store ((float) s.getValue()); };
        addAndMakeVisible (s);
    }

    void bindRotary (juce::Slider& s, double lo, double hi, std::atomic<float>& target)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setRange (lo, hi, (hi - lo) / 200.0);
        s.setValue (target.load(), juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 52, 16);
        s.onValueChange = [&s, &target] { target.store ((float) s.getValue()); };
        addAndMakeVisible (s);
    }

    SharedState& state;
    AudioEngine& engine;
    juce::StringArray presetNames;

    juce::Slider     eqLo [SharedState::numChannels];
    juce::Slider     eqMid[SharedState::numChannels];
    juce::Slider     eqHi [SharedState::numChannels];
    juce::TextButton comp [SharedState::numChannels];
    juce::Slider     pan  [SharedState::numChannels];
    juce::Slider     vol  [SharedState::numChannels];
    juce::TextButton mute [SharedState::numChannels];

    juce::Slider revMix, revRoom, dlyMix, dlyTime, dlyFb, masterGain;
};

class MixerWindow : public juce::DocumentWindow
{
public:
    MixerWindow (SharedState& s, AudioEngine& e)
        : juce::DocumentWindow ("Mixer", juce::Colour (0xff14171c), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        auto* vp = new juce::Viewport();
        vp->setViewedComponent (new Mixer (s, e), true);
        vp->setScrollBarsShown (false, true);
        setContentOwned (vp, false);
        setResizable (true, true);
        setSize (1100, 500);
        centreWithSize (1100, 500);   // keep the title bar / close button on-screen
    }

    void closeButtonPressed() override { setVisible (false); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerWindow)
};
