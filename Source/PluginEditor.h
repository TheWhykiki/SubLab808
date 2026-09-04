#pragma once
#include "PresetBar.h"
#include <JuceHeader.h>
#include "PluginProcessor.h"

class SubLabLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SubLabLookAndFeel();
    void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
};

class SubLab808Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SubLab808Editor(SubLab808Processor&);
    ~SubLab808Editor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Dial {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    void addDial(Dial&, const juce::String&, const juce::String&, const juce::String&);
    void timerCallback() override;
    SubLab808Processor& audioProcessor;
    SubLabLookAndFeel look;
    Dial decay, release, punch, pitchDecay, glide, tune, body, click, drive, tone, velocity, output;
    std::array<Dial*, 12> dials;
    wk::PresetBar presetBar;
    juce::TextButton updates;
    juce::ToggleButton oneShotButton { "ONE SHOT" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> oneShotAttachment;
    float meter = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubLab808Editor)
};
