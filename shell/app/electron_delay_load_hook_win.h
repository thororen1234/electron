// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_APP_ELECTRON_DELAY_LOAD_HOOK_WIN_H_
#define ELECTRON_SHELL_APP_ELECTRON_DELAY_LOAD_HOOK_WIN_H_

namespace electron {

// Before Windows 10, resolves delay-loaded DLLs and functions that don't exist
// to fallbacks. Must be called before any delay-loaded function is used.
void InstallLegacyWindowsDelayLoadFallbacks();

}  // namespace electron

#endif  // ELECTRON_SHELL_APP_ELECTRON_DELAY_LOAD_HOOK_WIN_H_
