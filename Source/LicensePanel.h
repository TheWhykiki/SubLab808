// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace wk
{
// Owned by the editor: closing a DAW window also closes this panel and its callbacks.
class LicensePanel final : public juce::Component
{
public:
    LicensePanel()
    {
        setOpaque(true);
        setTitle("About and licences");
        text.setJustificationType(juce::Justification::topLeft);
        text.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(text);
        source.setButtonText("Source for this version");
        license.setButtonText("GNU AGPL version 3");
        license.setURL(juce::URL("https://www.gnu.org/licenses/agpl-3.0.html"));
        for (auto* link : { &source, &license })
        {
            link->setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xff8fdcff));
            addAndMakeVisible(*link);
        }
        close.setButtonText("Close");
        close.onClick = [this] { setVisible(false); };
        addAndMakeVisible(close);
    }

    void configure(const juce::String& product, const juce::String& version)
    {
        text.setText(product + " " + version
            + "\nCopyright (c) 2026 TheWhykiki / Whykiki Audio"
              "\n\nFree software under GNU AGPL version 3 only."
              "\nYou may use, study, modify and redistribute it under that licence."
              "\nProvided without warranty."
              "\n\nComplete source and third-party notices are included with each release.",
            juce::dontSendNotification);
        source.setURL(juce::URL("https://github.com/TheWhykiki/" + product + "/tree/v" + version));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff171c23));
        g.setColour(juce::Colour(0xff8fdcff));
        g.drawRect(getLocalBounds(), 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(20);
        close.setBounds(area.removeFromBottom(30).removeFromRight(90));
        license.setBounds(area.removeFromBottom(28));
        source.setBounds(area.removeFromBottom(28));
        text.setBounds(area);
    }

private:
    juce::Label text;
    juce::HyperlinkButton source, license;
    juce::TextButton close;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LicensePanel)
};
}

