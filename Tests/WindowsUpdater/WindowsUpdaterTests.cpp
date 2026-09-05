#include "WindowsUpdater.h"

int main()
{
#if ! defined(WK_WINDOWS_UPDATER_TEST_MODE) || ! WK_WINDOWS_UPDATER_TEST_MODE
#error "Windows updater tests must be compiled in the non-installing test mode"
#endif
    return wk::windows_updater::runWindowsUpdaterSelfTests();
}
