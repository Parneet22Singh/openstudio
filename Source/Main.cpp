#include <JuceHeader.h>
#include "SharedState.h"
#include "AudioEngine.h"
#include "MainComponent.h"

/**
    OpenStudio — Phase 1 application entry point.

    Owns the single SharedState and the AudioEngine, and hosts the MainComponent in a
    resizable document window with a dark look and feel.
*/
class OpenStudioApplication : public juce::JUCEApplication
{
public:
    OpenStudioApplication() = default;

    const juce::String getApplicationName()    override { return "OpenStudio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed()          override { return true; }

    void initialise (const juce::String&) override
    {
        lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        state  = std::make_unique<SharedState>();
        engine = std::make_unique<AudioEngine> (*state);

        const juce::String err = engine->start();
        if (err.isNotEmpty())
            juce::Logger::writeToLog ("Audio device error: " + err);

        mainWindow = std::make_unique<MainWindow> ("OpenStudio", *state, *engine);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        engine     = nullptr;
        state      = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, SharedState& state, AudioEngine& engine)
            : juce::DocumentWindow (name,
                  juce::Colour (0xff14171c),
                  juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (state, engine), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    juce::LookAndFeel_V4         lookAndFeel;
    std::unique_ptr<SharedState> state;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MainWindow>  mainWindow;
};

START_JUCE_APPLICATION (OpenStudioApplication)
