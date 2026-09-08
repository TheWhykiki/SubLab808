#include "MacModulePin.h"

extern "C" __attribute__((visibility("default"))) int whykikiModulePinFixtureValue();
extern "C" __attribute__((visibility("default"))) const void* whykikiRetainModulePinFixture();
extern "C" __attribute__((visibility("default"))) bool whykikiModulePinFixtureIsRetained();

extern "C" __attribute__((visibility("default"))) int whykikiModulePinFixtureValue()
{
    return 808;
}

extern "C" __attribute__((visibility("default"))) const void* whykikiRetainModulePinFixture()
{
    const auto& status = wk::retainCurrentModuleForNativeCallbacks();
    return status.succeeded() ? &status : nullptr;
}

extern "C" __attribute__((visibility("default"))) bool whykikiModulePinFixtureIsRetained()
{
    return wk::currentModuleIsRetainedForNativeCallbacks();
}
