#include "WindowsUpdater.h"

#include <windows.h>
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    INITCOMMONCONTROLSEX controls { sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&controls);
    return wk::windows_updater::runWindowsUpdater();
}
