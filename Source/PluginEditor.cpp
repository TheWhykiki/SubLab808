#include "PluginEditor.h"

SubLabLookAndFeel::SubLabLookAndFeel()
{
    setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xffe9edf1));
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff11151a));
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff252d35));
}

void SubLabLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                          float pos, float start, float end, juce::Slider&)
{
    auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(8.0f);
    auto diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    auto knob = bounds.withSizeKeepingCentre(diameter, diameter);
    auto radius = diameter * 0.5f;
    auto centre = knob.getCentre();
    auto angle = start + pos * (end - start);
    g.setColour(juce::Colour(0xff090c10));
    g.fillEllipse(knob);
    g.setColour(juce::Colour(0xff28313a));
    g.drawEllipse(knob, 2.0f);
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius - 3.0f, radius - 3.0f, 0.0f, start, end, true);
    g.setColour(juce::Colour(0xff303942));
    g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path value;
    value.addCentredArc(centre.x, centre.y, radius - 3.0f, radius - 3.0f, 0.0f, start, angle, true);
    g.setColour(juce::Colour(0xffff4f2e));
    g.strokePath(value, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path pointer;
    pointer.addRoundedRectangle(-1.4f, -radius + 10.0f, 2.8f, radius * 0.42f, 1.4f);
    g.setColour(juce::Colour(0xfff7f9fb));
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
}

SubLab808Editor::SubLab808Editor(SubLab808Processor& p)
    : AudioProcessorEditor(&p), audioProcessor(p), dials { &decay, &release, &punch, &pitchDecay, &glide, &tune, &body, &click, &drive, &tone, &velocity, &output }
{
    setLookAndFeel(&look);
    setResizable(true, true);
    setResizeLimits(700, 430, 1100, 680);
    setSize(860, 520);
    addDial(decay, "decay", "DECAY", " s");
    addDial(release, "release", "RELEASE", " s");
    addDial(punch, "punch", "PITCH PUNCH", " st");
    addDial(pitchDecay, "pitchdecay", "PITCH DECAY", " s");
    addDial(glide, "glide", "GLIDE", " s");
    addDial(tune, "tune", "TUNE", " st");
    addDial(body, "body", "BODY", " %");
    addDial(click, "click", "CLICK", " %");
    addDial(drive, "drive", "DRIVE", " dB");
    addDial(tone, "tone", "TONE", " Hz");
    addDial(velocity, "velocity", "VELOCITY", " %");
    addDial(output, "output", "OUTPUT", " dB");
    presetLabel.setText("FACTORY PRESET", juce::dontSendNotification);
    presetLabel.setJustificationType(juce::Justification::centredRight);
    presetLabel.setColour(juce::Label::textColourId, juce::Colour(0xff7e8b96));
    presetLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    addAndMakeVisible(presetLabel);
    for (int i = 0; i < audioProcessor.getNumPrograms(); ++i)
        presetBox.addItem(audioProcessor.getProgramName(i), i + 1);
    presetBox.setSelectedId(audioProcessor.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff11161b));
    presetBox.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff303942));
    presetBox.setColour(juce::ComboBox::textColourId, juce::Colour(0xffe9edf1));
    presetBox.onChange = [this] { audioProcessor.setCurrentProgram(presetBox.getSelectedId() - 1); };
    addAndMakeVisible(presetBox);
    oneShotButton.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffaab4bd));
    oneShotButton.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xffff4f2e));
    addAndMakeVisible(oneShotButton);
    oneShotAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(audioProcessor.parameters, "oneshot", oneShotButton);
    startTimerHz(30);
}

SubLab808Editor::~SubLab808Editor() { setLookAndFeel(nullptr); }

void SubLab808Editor::addDial(Dial& dial, const juce::String& id, const juce::String& title, const juce::String& suffix)
{
    dial.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    dial.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 74, 22);
    dial.slider.setTextValueSuffix(suffix);
    if (id == "decay" || id == "release" || id == "pitchdecay" || id == "glide") dial.slider.setNumDecimalPlacesToDisplay(3);
    else if (id == "drive" || id == "output") dial.slider.setNumDecimalPlacesToDisplay(1);
    else dial.slider.setNumDecimalPlacesToDisplay(0);
    auto* parameter = audioProcessor.parameters.getParameter(id);
    dial.slider.setDoubleClickReturnValue(true, parameter->convertFrom0to1(parameter->getDefaultValue()));
    dial.label.setText(title, juce::dontSendNotification);
    dial.label.setJustificationType(juce::Justification::centred);
    dial.label.setColour(juce::Label::textColourId, juce::Colour(0xffaab4bd));
    dial.label.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    addAndMakeVisible(dial.slider);
    addAndMakeVisible(dial.label);
    dial.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.parameters, id, dial.slider);
}

void SubLab808Editor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff161c22), 0, 0, juce::Colour(0xff090c10), 0, (float) getHeight(), false);
    g.setGradientFill(bg); g.fillAll();
    g.setColour(juce::Colour(0xffff4f2e)); g.fillRect(0, 0, getWidth(), 4);
    g.setColour(juce::Colour(0xfff4f6f8)); g.setFont(juce::FontOptions(30.0f, juce::Font::bold));
    g.drawText("SUBLAB", 34, 22, 180, 42, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffff4f2e)); g.drawText("808", 169, 22, 80, 42, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff7e8b96)); g.setFont(juce::FontOptions(11.0f));
    g.drawText("MONOPHONIC BASS SYNTHESIZER  /  APPLE SILICON", 36, 61, 360, 18, juce::Justification::centredLeft);
    auto panel = juce::Rectangle<float>(24.0f, 98.0f, getWidth() - 48.0f, getHeight() - 128.0f);
    g.setColour(juce::Colour(0xff11161b)); g.fillRoundedRectangle(panel, 12.0f);
    g.setColour(juce::Colour(0xff273039)); g.drawRoundedRectangle(panel, 12.0f, 1.0f);
    g.setColour(juce::Colour(0xff1f272e)); g.drawVerticalLine(getWidth() / 2, 118.0f, (float) getHeight() - 52.0f);
    auto meterBounds = juce::Rectangle<float>((float) getWidth() - 190.0f, 40.0f, 150.0f, 8.0f);
    g.setColour(juce::Colour(0xff252d35)); g.fillRoundedRectangle(meterBounds, 4.0f);
    g.setColour(meter > 0.88f ? juce::Colour(0xffffd166) : juce::Colour(0xffff4f2e));
    g.fillRoundedRectangle(meterBounds.withWidth(meterBounds.getWidth() * juce::jlimit(0.0f, 1.0f, meter)), 4.0f);
    g.setColour(juce::Colour(0xff606d77)); g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText("OUTPUT", getWidth() - 190, 53, 150, 16, juce::Justification::centredRight);
}

void SubLab808Editor::resized()
{
    presetLabel.setBounds(getWidth() / 2 - 40, 35, 112, 25);
    presetBox.setBounds(getWidth() / 2 + 78, 34, 190, 28);
    oneShotButton.setBounds(getWidth() / 2 + 278, 34, 100, 28);
    auto area = getLocalBounds().reduced(36).withTrimmedTop(82).withTrimmedBottom(12);
    auto rowHeight = area.getHeight() / 2;
    for (int row = 0; row < 2; ++row) {
        auto rowArea = area.removeFromTop(rowHeight);
        auto width = rowArea.getWidth() / 6;
        for (int col = 0; col < 6; ++col) {
            auto cell = rowArea.removeFromLeft(width).reduced(7, 4);
            auto* dial = dials[(size_t) (row * 6 + col)];
            dial->label.setBounds(cell.removeFromTop(20));
            dial->slider.setBounds(cell);
        }
    }
}

void SubLab808Editor::timerCallback()
{
    meter = audioProcessor.outputMeter.load();
    audioProcessor.outputMeter.store(meter * 0.88f);
    presetBox.setSelectedId(audioProcessor.getCurrentProgram() + 1, juce::dontSendNotification);
    repaint(getWidth() - 200, 30, 175, 45);
}
