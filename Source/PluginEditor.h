#pragma once
#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

class AnalogKnobLAF : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, const float rotaryStartAngle,
                          const float rotaryEndAngle, juce::Slider& slider) override;
};

class DeReverbenjaminV3Editor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DeReverbenjaminV3Editor(DeReverbenjaminV3Processor&);
    ~DeReverbenjaminV3Editor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateStage3Labels(int mode);
    void setupKnob(juce::Slider& slider, juce::Label& label, const juce::String& name, const juce::String& suffix = "");
    void drawScrew(juce::Graphics& g, float cx, float cy) const;
    void drawVuMeter(juce::Graphics& g, juce::Rectangle<float> area) const;

    DeReverbenjaminV3Processor& audioProcessor;
    AnalogKnobLAF analogLAF;

    juce::Slider modeSlider;
    juce::Label modeLabel;

    juce::Slider roomAmountSlider, focusSlider, decaySlider, smoothSlider;
    juce::Slider tailAmountSlider, sensSlider, releaseSlider, outputSlider;
    juce::Slider res1Slider, res2Slider, res3Slider, mixSlider;

    juce::Label roomAmountLabel, focusLabel, decayLabel, smoothLabel;
    juce::Label tailAmountLabel, sensLabel, releaseLabel, outputLabel;
    juce::Label res1Label, res2Label, res3Label, mixLabel;

    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<Attachment> modeAtt;
    std::unique_ptr<Attachment> roomAmountAtt, focusAtt, decayAtt, smoothAtt;
    std::unique_ptr<Attachment> tailAmountAtt, sensAtt, releaseAtt, outputAtt;
    std::unique_ptr<Attachment> res1Att, res2Att, res3Att, mixAtt;

    float smoothedGr = 1.0f;
    int lastDisplayedMode = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeReverbenjaminV3Editor)
};
