#include "../../Source/UpdaterLauncher.h"

#include <cstdlib>

#if ! JUCE_WINDOWS || ! WK_UPDATER_ENABLED
#error "The launcher link-shape is an enabled Windows-only CI target"
#endif

int main()
{
    // The invalid slash is intentional: it creates a real reference to the
    // production launch function, but guarantees an accidental invocation
    // stops before bundle discovery, signature verification or CreateProcess.
    const auto result = wk::launchNativeUpdater("INVALID/LAUNCHER-LINK-SHAPE", "0.0.0");
    return result.failed() ? EXIT_SUCCESS : EXIT_FAILURE;
}
