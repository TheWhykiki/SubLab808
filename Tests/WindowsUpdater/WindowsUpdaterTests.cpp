#include "WindowsUpdater.h"
#include <string_view>

int wmain(int argc, wchar_t** argv)
{
#if ! defined(WK_WINDOWS_UPDATER_TEST_MODE) || ! WK_WINDOWS_UPDATER_TEST_MODE
#error "Windows updater tests must be compiled in the non-installing test mode"
#endif
    if (argc == 4 && std::wstring_view(argv[1]) == L"--validate-msi-database")
        return wk::windows_updater::probeWindowsMsiDatabase(argv[2], argv[3]);
    if (argc != 1) return 2;
    return wk::windows_updater::runWindowsUpdaterSelfTests();
}
