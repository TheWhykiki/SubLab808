#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#ifndef WK_UPDATER_ENABLED
 #define WK_UPDATER_ENABLED 0
#endif

#if (JUCE_MAC || JUCE_WINDOWS) && WK_UPDATER_ENABLED
namespace wk {
#if JUCE_MAC
__attribute__((visibility("hidden")))
#endif
juce::Result launchNativeUpdater(const juce::String& product, const juce::String& version);
}
#endif

namespace wk {
inline void configureUpdaterButton(juce::TextButton& button, juce::String product, juce::String version)
{
    button.setButtonText("Updates...");
    button.setTitle(product + " updates");
    button.setTooltip("Check, verify and install a newer stable version");
   #if (JUCE_MAC || JUCE_WINDOWS) && WK_UPDATER_ENABLED
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
   #if JUCE_WINDOWS
    button.setTooltip("Automatic updates require a signed Windows release build");
   #elif JUCE_MAC
    button.setTooltip("Automatic updates are available in installed macOS VST3 builds");
   #else
    button.setTooltip("Automatic updates are unavailable on this platform");
   #endif
   #endif
}
}
