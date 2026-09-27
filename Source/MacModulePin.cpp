#include "MacModulePin.h"

#include <atomic>
#include <dlfcn.h>

namespace wk
{
namespace
{
// An object address cannot be interposed and always resolves to this final
// executable or plug-in image, even when this translation unit came from a
// static/shared-code target.
char moduleAnchor;
std::atomic<void*> retainedModuleHandle { nullptr };
}

const char* MacModulePinStatus::errorMessage() const noexcept
{
    switch (error)
    {
        case MacModulePinError::none: return "";
        case MacModulePinError::addressResolutionFailed:
            return "the current macOS module path could not be resolved";
        case MacModulePinError::imageNotLoaded:
            return "dyld could not retain the already-loaded macOS module";
    }
    return "the macOS module lifetime could not be secured";
}

const MacModulePinStatus& retainCurrentModuleForNativeCallbacks() noexcept
{
    static const MacModulePinStatus status = []() noexcept
    {
        Dl_info image {};
        if (dladdr(&moduleAnchor, &image) == 0 || image.dli_fname == nullptr
            || image.dli_fname[0] == '\0' || image.dli_fbase == nullptr)
            return MacModulePinStatus { MacModulePinError::addressResolutionFailed };

        // RTLD_NOLOAD is deliberate: it returns a reference-counted handle only
        // for this already-loaded image, so a replaced path cannot load a second
        // copy. Do not combine it with RTLD_NODELETE: current Darwin dyld returns
        // from its NOLOAD branch before applying NODELETE. The private handle is
        // itself the lifetime guarantee and is intentionally never dlclose'd.
        auto* handle = dlopen(image.dli_fname, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
        if (handle == nullptr)
            return MacModulePinStatus { MacModulePinError::imageNotLoaded };

        retainedModuleHandle.store(handle, std::memory_order_release);
        return MacModulePinStatus { MacModulePinError::none };
    }();

    return status;
}

bool currentModuleIsRetainedForNativeCallbacks() noexcept
{
    return retainedModuleHandle.load(std::memory_order_acquire) != nullptr;
}
}
