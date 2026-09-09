#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <utility>
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
namespace detail {
class UpdaterButtonState final
{
public:
    explicit UpdaterButtonState(juce::TextButton& buttonToWatch)
        : button(&buttonToWatch), ownerWatcher(*this, buttonToWatch) {}

    void showError(const juce::String& errorMessage)
    {
        if (button == nullptr || ! button->isShowing()) return;
        closeMessage();
        if (button == nullptr || ! button->isShowing()) return;
        message = juce::AlertWindow::showScopedAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Updater")
                .withMessage(errorMessage)
                .withButton("OK")
                .withAssociatedComponent(button.getComponent()),
            [](int) {});
    }

private:
    class OwnerWatcher final : public juce::ComponentMovementWatcher
    {
    public:
        OwnerWatcher(UpdaterButtonState& stateToWatch, juce::TextButton& owner)
            : ComponentMovementWatcher(&owner), state(stateToWatch) {}
        void componentMovedOrResized(bool, bool) override {}
        void componentPeerChanged() override { state.closeMessage(); }
        void componentVisibilityChanged() override
        {
            if (! state.ownerIsShowing()) state.closeMessage();
        }
    private:
        UpdaterButtonState& state;
    };

    bool ownerIsShowing() const { return button != nullptr && button->isShowing(); }
    void closeMessage()
    {
        if (closingMessage) return;
        const juce::ScopedValueSetter<bool> closing(closingMessage, true);
        message.close();
    }

    juce::Component::SafePointer<juce::TextButton> button;
    bool closingMessage = false;
    juce::ScopedMessageBox message;
    // Must remain last so it unregisters before the scoped message is closed.
    OwnerWatcher ownerWatcher;
};
}

inline void configureUpdaterButton(juce::TextButton& button, juce::String product, juce::String version)
{
    button.setButtonText("Updates...");
    button.setTitle(product + " updates");
    button.setTooltip("Check, verify and install a newer stable version");
   #if (JUCE_MAC || JUCE_WINDOWS) && WK_UPDATER_ENABLED
    auto state = std::make_shared<detail::UpdaterButtonState>(button);
    button.onClick = [state = std::move(state), product = std::move(product), version = std::move(version)]
    {
        const auto result = launchNativeUpdater(product, version);
        if (result.failed()) state->showError(result.getErrorMessage());
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
