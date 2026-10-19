// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/electron_api_native_theme.h"

#include "shell/browser/mac/electron_application.h"

namespace electron::api {

void NativeTheme::UpdateMacOSAppearanceForOverrideValue(
    ui::NativeTheme::ThemeSource override) {
  NSAppearance* new_appearance;
  switch (override) {
    case ui::NativeTheme::ThemeSource::kForcedDark:
      // There is no dark appearance before macOS 10.14.
      if (@available(macOS 10.14, *)) {
        new_appearance =
            [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
      } else {
        new_appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
      }
      break;
    case ui::NativeTheme::ThemeSource::kForcedLight:
      new_appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
      break;
    case ui::NativeTheme::ThemeSource::kSystem:
    default:
      new_appearance = nil;
      break;
  }
  [[NSApplication sharedApplication] setAppearance:new_appearance];
}

bool NativeTheme::ShouldDifferentiateWithoutColor() {
  return [[NSWorkspace sharedWorkspace]
      accessibilityDisplayShouldDifferentiateWithoutColor];
}

}  // namespace electron::api
