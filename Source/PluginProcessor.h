#pragma once
#include <JuceHeader.h>

class SubLab808Processor final : public juce::AudioProcessor
{
public:
    SubLab808Processor();
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "SubLab808"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float> outputMeter { 0.0f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void triggerNote(int note, float newVelocity);
    float renderSample();
    double sampleRate = 44100.0, phase = 0.0, currentHz = 55.0, targetHz = 55.0;
    float amp = 0.0f, pitchEnv = 0.0f, filterState = 0.0f, velocity = 1.0f;
    float ampCoef = 0.999f, pitchCoef = 0.99f, glideCoef = 0.0f;
    float drive = 1.0f, outputGain = 0.7f, filterCoef = 0.1f, click = 0.0f, body = 0.0f;
    float releaseCoef = 0.99f, bendSemitones = 0.0f;
    uint32_t noiseState = 0x6d2b79f5u;
    std::atomic<int> currentProgram { 0 };
    int currentNote = -1;
    bool gateReleased = false;
    bool active = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubLab808Processor)
};
