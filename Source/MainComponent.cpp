#include "MainComponent.h"
#include "SamplerEngine.h"   // full SamplerProcessor definition for engine.getSampler()

MainComponent::MainComponent (SharedState& s, AudioEngine& e)
    : state (s), engine (e), scope (s), arrangement (s), rack (s, e)
{
    // Transport 
    playButton.setClickingTogglesState (true);
    playButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3fc9a0));
    playButton.onClick = [this]
    {
        const bool play = playButton.getToggleState();
        state.playing.store (play, std::memory_order_relaxed);
        playButton.setButtonText (play ? "Stop" : "Play");
    };
    addAndMakeVisible (playButton);

    mixerButton.onClick = [this]
    {
        if (mixerWindow == nullptr)
            mixerWindow = std::make_unique<MixerWindow> (state, engine);
        mixerWindow->setVisible (true);
        mixerWindow->toFront (true);
    };
    addAndMakeVisible (mixerButton);

    saveButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Save Project", juce::File(), "*.ostudio");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f != juce::File())
                {
                    if (! f.hasFileExtension ("ostudio")) f = f.withFileExtension ("ostudio");
                    ProjectIO::save (state, f);
                }
            });
    };
    addAndMakeVisible (saveButton);

    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Open Project", juce::File(), "*.ostudio");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.existsAsFile() && ProjectIO::load (state, f, engine.getSampler()))
                {
                    rack.refreshFromState();
                    bpmSlider.setValue (state.bpm.load(), juce::dontSendNotification);
                    swingSlider.setValue (state.swing.load(), juce::dontSendNotification);
                    metroButton.setToggleState (state.metronomeOn.load(), juce::dontSendNotification);
                    songButton.setToggleState (state.songMode.load(), juce::dontSendNotification);
                    selectPattern (state.currentPattern.load());
                    playButton.setToggleState (false, juce::dontSendNotification);
                    playButton.setButtonText ("Play");
                }
            });
    };
    addAndMakeVisible (loadButton);

    exportButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Export WAV (4 bars)", juce::File(), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f != juce::File())
                {
                    if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
                    engine.renderToFile (f, 4);
                }
            });
    };
    addAndMakeVisible (exportButton);

    rescanButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2f38));
    rescanButton.onClick = [this]
    {
        const int n = engine.rescanSamples();
        rack.reloadInstrumentList();
        rescanButton.setButtonText (n > 0 ? "Rescan (" + juce::String (n) + ")" : "Rescan");
    };
    addAndMakeVisible (rescanButton);

    metroButton.setClickingTogglesState (true);
    metroButton.setToggleState (state.metronomeOn.load(), juce::dontSendNotification);
    metroButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe0b341));
    metroButton.onClick = [this] { state.metronomeOn.store (metroButton.getToggleState()); };
    addAndMakeVisible (metroButton);

    bpmSlider.setRange (40.0, 240.0, 1.0);
    bpmSlider.setValue (state.bpm.load(), juce::dontSendNotification);
    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 22);
    bpmSlider.onValueChange = [this] { state.bpm.store (bpmSlider.getValue(), std::memory_order_relaxed); };
    addAndMakeVisible (bpmSlider);

    swingSlider.setRange (0.0, 0.7, 0.01);
    swingSlider.setValue (state.swing.load(), juce::dontSendNotification);
    swingSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 22);
    swingSlider.onValueChange = [this] { state.swing.store ((float) swingSlider.getValue()); };
    addAndMakeVisible (swingSlider);

    for (auto* l : { &bpmLabel, &swingLabel, &patternLabel, &lengthLabel })
    {
        l->setFont (juce::Font (11.0f, juce::Font::bold));
        l->setColour (juce::Label::textColourId, juce::Colour (0xff8b93a0));
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (*l);
    }

    // Pattern selector 
    for (int p = 0; p < SharedState::numPatterns; ++p)
    {
        auto& b = patternButtons[p];
        b.setButtonText (juce::String (p + 1));
        b.setClickingTogglesState (true);
        b.setRadioGroupId (100);
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f8fc9));
        b.onClick = [this, p] { selectPattern (p); };
        addAndMakeVisible (b);
    }
    patternButtons[0].setToggleState (true, juce::dontSendNotification);

    lengthSlider.setSliderStyle (juce::Slider::IncDecButtons);
    lengthSlider.setRange (1, SharedState::maxSteps, 1);
    lengthSlider.setValue (state.patterns[0].length.load(), juce::dontSendNotification);
    lengthSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 22);
    lengthSlider.onValueChange = [this]
    {
        const int p = juce::jlimit (0, SharedState::numPatterns - 1, state.currentPattern.load());
        state.patterns[p].length.store ((int) lengthSlider.getValue());
    };
    addAndMakeVisible (lengthSlider);

    songButton.setClickingTogglesState (true);
    songButton.setToggleState (state.songMode.load(), juce::dontSendNotification);
    songButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f8fc9));
    songButton.onClick = [this] { state.songMode.store (songButton.getToggleState()); };
    addAndMakeVisible (songButton);

    addSlotButton.onClick = [this] { arrangement.addSlot(); };
    delSlotButton.onClick = [this] { arrangement.delSlot(); };
    addAndMakeVisible (addSlotButton);
    addAndMakeVisible (delSlotButton);

    addAndMakeVisible (scope);
    addAndMakeVisible (arrangement);
    addAndMakeVisible (rack);

    setSize (1480, 880);
}

MainComponent::~MainComponent() {}

void MainComponent::selectPattern (int p)
{
    p = juce::jlimit (0, SharedState::numPatterns - 1, p);
    state.currentPattern.store (p);
    patternButtons[p].setToggleState (true, juce::dontSendNotification);
    lengthSlider.setValue (state.patterns[p].length.load(), juce::dontSendNotification);
    rack.refreshFromState();
    repaint();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14171c));

    auto top = getLocalBounds().removeFromTop (topH);
    g.setColour (juce::Colour (0xff1b1f26));
    g.fillRect (top);

    g.setColour (juce::Colour (0xffe6e9ef));
    g.setFont (juce::Font (19.0f, juce::Font::bold));
    g.drawText ("OpenStudio", 16, 6, 160, 26, juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xff5b6472));
    g.setFont (juce::Font (10.0f));
    g.drawText ("16-channel rack · patterns · song · FX", 16, 30, 220, 16, juce::Justification::centredLeft);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    auto top  = area.removeFromTop (topH);

    // Oscilloscope on the far right of the header.
    scope.setBounds (top.removeFromRight (236).reduced (8, 10));

    auto content = top;
    content.removeFromLeft (220);   // title area

    // Row 1: transport
    auto row1 = content.removeFromTop (44).reduced (0, 6);
    auto btn = [&row1] (juce::Component& c, int w) { c.setBounds (row1.removeFromLeft (w).withHeight (32)); row1.removeFromLeft (6); };
    btn (playButton, 72);
    btn (mixerButton, 60);
    btn (saveButton, 50);
    btn (loadButton, 50);
    btn (exportButton, 88);
    btn (rescanButton, 70);
    btn (metroButton, 84);
    row1.removeFromLeft (6);
    bpmLabel.setBounds  (row1.removeFromLeft (42).withHeight (32));
    bpmSlider.setBounds (row1.removeFromLeft (176).withHeight (32));
    row1.removeFromLeft (6);
    swingLabel.setBounds  (row1.removeFromLeft (40).withHeight (32));
    swingSlider.setBounds (row1.removeFromLeft (128).withHeight (32));

    // Row 2: pattern selector + length + song + arrangement
    auto row2 = content.removeFromTop (40).reduced (0, 4);
    patternLabel.setBounds (row2.removeFromLeft (60).withHeight (28));
    row2.removeFromLeft (4);
    for (int p = 0; p < SharedState::numPatterns; ++p)
    {
        patternButtons[p].setBounds (row2.removeFromLeft (28).withHeight (28));
        row2.removeFromLeft (3);
    }
    row2.removeFromLeft (10);
    lengthLabel.setBounds (row2.removeFromLeft (46).withHeight (28));
    lengthSlider.setBounds (row2.removeFromLeft (96).withHeight (28));
    row2.removeFromLeft (12);
    songButton.setBounds (row2.removeFromLeft (94).withHeight (28));
    row2.removeFromLeft (8);
    addSlotButton.setBounds (row2.removeFromLeft (28).withHeight (28));
    row2.removeFromLeft (3);
    delSlotButton.setBounds (row2.removeFromLeft (28).withHeight (28));
    row2.removeFromLeft (8);
    arrangement.setBounds (row2.reduced (0, 1));

    rack.setBounds (area.reduced (12, 8));
}
