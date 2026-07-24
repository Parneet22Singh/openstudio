#pragma once

#include <JuceHeader.h>
#include "SharedState.h"
#include "NoteNames.h"

/**
    A per-channel Piano Roll for the CURRENT pattern: rows are pitches (top = high),
    columns are the pattern's steps. Click / drag to place notes at any pitch ->
    melodies and chords (multi-pitch).

    For a drum channel the rows are labelled with General MIDI drum names, so it doubles
    as a drum rack: each drum component (kick, snare, hi-hat, cymbal...) is its own row.
    The grid follows the current pattern and its length live.
*/
class PianoRoll : public juce::Component,
                  private juce::Timer
{
public:
    explicit PianoRoll (SharedState& s) : state (s) { startTimerHz (30); updateSize(); }
    ~PianoRoll() override { stopTimer(); }

    void setChannel (int c, bool drum)
    {
        channel = c;
        drumLabels = drum;
        loPitch = drum ? 35 : 36;
        hiPitch = drum ? 81 : 84;
        updateSize();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff14171c));

        const int   pat  = curPattern();
        const int   nsp  = steps();
        const int   rows = hiPitch - loPitch + 1;
        const float cw   = (float) (getWidth() - labelW) / (float) nsp;
        const int   play = state.currentStep.load (std::memory_order_relaxed);
        const bool  live = state.playingPattern.load (std::memory_order_relaxed) == pat;

        for (int r = 0; r < rows; ++r)
        {
            const int   pitch = hiPitch - r;
            const float y     = (float) (r * rowH);
            const bool  black = isBlackKey (pitch);

            g.setColour (black ? juce::Colour (0xff181c22) : juce::Colour (0xff1f242c));
            g.fillRect (0.0f, y, (float) labelW, (float) rowH);
            g.setColour (juce::Colour (0xff8b93a0));
            g.setFont (juce::Font (11.0f));
            g.drawText (NoteNames::label (pitch, drumLabels), 6, (int) y, labelW - 8, rowH,
                        juce::Justification::centredLeft);

            for (int s = 0; s < nsp; ++s)
            {
                juce::Rectangle<float> cell (labelW + s * cw, y, cw, (float) rowH);
                cell.reduce (1.5f, 1.5f);

                const bool on     = state.patterns[pat].cells[channel][pitch][s].load (std::memory_order_relaxed);
                const bool onBeat = (s % 4) == 0;
                const bool isPlay = live && (s == play);

                juce::Colour base = on ? juce::Colour (0xff3fc9a0)
                                       : (black ? juce::Colour (0xff191d23)
                                                : (onBeat ? juce::Colour (0xff262b33) : juce::Colour (0xff20242b)));
                if (isPlay) base = base.brighter (on ? 0.2f : 0.4f);

                g.setColour (base);
                g.fillRoundedRectangle (cell, 2.0f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override { paintValue = ! cellAt (e); apply (e, paintValue); }
    void mouseDrag (const juce::MouseEvent& e) override { apply (e, paintValue); }

private:
    void timerCallback() override
    {
        if (steps() != lastSteps)
            updateSize();
        repaint();
    }

    int curPattern() const { return juce::jlimit (0, SharedState::numPatterns - 1, state.currentPattern.load (std::memory_order_relaxed)); }
    int steps()      const { return juce::jlimit (1, SharedState::maxSteps, state.patterns[curPattern()].length.load (std::memory_order_relaxed)); }

    void updateSize()
    {
        lastSteps = steps();
        setSize (labelW + lastSteps * 44, (hiPitch - loPitch + 1) * rowH);
    }

    static bool isBlackKey (int pitch)
    {
        switch (((pitch % 12) + 12) % 12) { case 1: case 3: case 6: case 8: case 10: return true; default: return false; }
    }

    bool coords (const juce::MouseEvent& e, int& pitch, int& step) const
    {
        if (e.x < labelW) return false;
        const int nsp = steps();
        const float cw = (float) (getWidth() - labelW) / (float) nsp;
        const int r = e.y / rowH;
        pitch = hiPitch - r;
        step  = (int) ((e.x - labelW) / cw);
        return pitch >= loPitch && pitch <= hiPitch && step >= 0 && step < nsp;
    }

    bool cellAt (const juce::MouseEvent& e) const
    {
        int p, s;
        return coords (e, p, s) && state.patterns[curPattern()].cells[channel][p][s].load (std::memory_order_relaxed);
    }

    void apply (const juce::MouseEvent& e, bool value)
    {
        int p, s;
        if (coords (e, p, s))
        {
            state.patterns[curPattern()].cells[channel][p][s].store (value);
            repaint();
        }
    }

    static constexpr int rowH = 15;
    static constexpr int labelW = 104;

    SharedState& state;
    int  channel = 0;
    bool drumLabels = false;
    int  loPitch = 36, hiPitch = 84;
    int  lastSteps = SharedState::numSteps;
    bool paintValue = true;
};

/** A resizable window that hosts a PianoRoll inside a scrollable viewport. */
class PianoRollWindow : public juce::DocumentWindow
{
public:
    PianoRollWindow (SharedState& s)
        : juce::DocumentWindow ("Piano Roll", juce::Colour (0xff14171c),
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        auto* vp = new juce::Viewport();
        roll = new PianoRoll (s);
        vp->setViewedComponent (roll, true);              // viewport owns the roll
        setContentOwned (vp, false);
        setResizable (true, true);
        setSize (820, 520);
        centreWithSize (820, 520);   // keep the title bar / close button on-screen
    }

    void show (int channel, bool drum, const juce::String& title)
    {
        roll->setChannel (channel, drum);
        setName (title);
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override { setVisible (false); }

private:
    PianoRoll* roll = nullptr;                             // owned by the viewport
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollWindow)
};
