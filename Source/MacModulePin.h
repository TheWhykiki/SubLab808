#pragma once

namespace wk
{
enum class MacModulePinError
{
    none,
    addressResolutionFailed,
    imageNotLoaded
};

struct MacModulePinStatus
{
    MacModulePinError error = MacModulePinError::addressResolutionFailed;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == MacModulePinError::none;
    }

    [[nodiscard]] const char* errorMessage() const noexcept;
};

// The internal handle is intentionally never exposed or closed. A native AppKit panel may
// retain its completion block after its JUCE owner has gone; keeping one private
// dyld reference prevents the block's machine code from being unmapped first.
[[nodiscard]] const MacModulePinStatus& retainCurrentModuleForNativeCallbacks() noexcept;

// Observation only: unlike retainCurrentModuleForNativeCallbacks(), this never
// initializes or pins the module and is safe for the native-panel test hook.
[[nodiscard]] bool currentModuleIsRetainedForNativeCallbacks() noexcept;
}
