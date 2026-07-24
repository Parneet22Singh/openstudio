#pragma once

#include <JuceHeader.h>
#include "SharedState.h"

/**
    A live output waveform display ("sinewave" scope). Reads the lock-free ring that the
    master bus fills with the final post-limiter signal and draws it as a glowing trace.
*/
class Oscilloscope : public juce::Component,
                     private juce::Timer
{
public:
    explicit Oscilloscope (SharedState& s) : state (s) { startTimerHz (30); }
    ~Oscilloscope() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff0e1013));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (juce::Colour (0xff262b33));
        g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);

        // centre line
        const float midY = b.getCentreY();
        g.setColour (juce::Colour (0xff1c2129));
        g.drawHorizontalLine ((int) midY, b.getX(), b.getRight());

        const int   N   = SharedState::scopeSize;
        const int   pos = state.scopePos.load (std::memory_order_relaxed);
        const float w   = b.getWidth();
        const float h   = b.getHeight() * 0.45f;

        juce::Path path;
        bool started = false;
        for (int i = 0; i < N; ++i)
        {
            const int   idx = (pos + i) & (N - 1);
            const float v   = juce::jlimit (-1.0f, 1.0f, state.scope[idx].load (std::memory_order_relaxed));
            const float x   = b.getX() + w * ((float) i / (float) (N - 1));
            const float y   = midY - v * h;
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else            path.lineTo (x, y);
        }

        g.setColour (juce::Colour (0xff3fc9a0));
        g.strokePath (path, juce::PathStrokeType (1.4f));

        g.setColour (juce::Colour (0xff5b6472));
        g.setFont (juce::Font (9.5f, juce::Font::bold));
        g.drawText ("OUTPUT", (int) b.getX() + 6, (int) b.getY() + 4, 60, 12, juce::Justification::topLeft);
    }

private:
    void timerCallback() override { repaint(); }

    SharedState& state;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Oscilloscope)
};
