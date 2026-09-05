#pragma once

#if ! defined(_WIN32)
#error "WindowsUpdater is a Windows-only target"
#endif

namespace wk::windows_updater
{
// Runs the visible, on-demand updater. The caller must invoke this only from the
// separate updater executable, never from a plugin/audio callback.
int runWindowsUpdater();

// Side-effect-free checks compiled only into the explicitly non-installing test target.
#if defined(WK_WINDOWS_UPDATER_TEST_MODE) && WK_WINDOWS_UPDATER_TEST_MODE
int runWindowsUpdaterSelfTests();
#endif
}
