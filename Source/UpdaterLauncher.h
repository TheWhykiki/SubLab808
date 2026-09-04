#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#ifndef WK_UPDATER_ENABLED
 #define WK_UPDATER_ENABLED 0
#endif

#if JUCE_MAC && WK_UPDATER_ENABLED
namespace wk {
__attribute__((visibility("hidden"))) juce::Result launchNativeUpdater(const juce::String& product,
                                                                      const juce::String& version);
}
#endif

namespace wk {
inline void configureUpdaterButton(juce::TextButton& button, juce::String product, juce::String version)
{
    button.setButtonText("Updates...");
    button.setTitle(product + " updates");
    button.setTooltip("Check, download and install a newer stable version with the macOS updater");
   #if JUCE_MAC && WK_UPDATER_ENABLED
    button.onClick = [product = std::move(product), version = std::move(version)]
    {
        const auto result = launchNativeUpdater(product, version);
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                 "Updater", result.getErrorMessage());
    };
   #else
    juce::ignoreUnused(product, version);
    button.setEnabled(false);
    button.setTooltip("Automatic updates are available in installed macOS VST3 builds");
   #endif
}
}
