#!/usr/bin/env python3

"""Reports imports in Windows binaries that won't resolve on Windows 7 SP1.

Static imports that are missing make the binary fail to load, so they are
errors. Delay-loaded imports only fail when the function is first called, so
they are reported separately and need to be checked for a Windows 7 code path.

With --system32, every import from a DLL found in that directory (a copy of a
Windows 7 SP1 System32) is checked against that DLL's real export table.
Without it, a list of known Windows 8+ APIs is used instead, which can miss
things.

Usage:
  script/check-legacy-windows-imports.py out/Testing
  script/check-legacy-windows-imports.py --system32 D:/win7/System32 out/Testing
"""

import argparse
import functools
import glob
import os
import re
import subprocess
import sys

# Known Windows 8+ functions, used when no Windows 7 System32 is available.
WIN8_PLUS_FUNCTIONS = {
    'CopyFile2', 'CreateFile2', 'CreatePseudoConsole',
    'DiscardVirtualMemory', 'EventSetInformation',
    'GetCurrentPackageFullName', 'GetCurrentThreadStackLimits',
    'GetFileInformationByName', 'GetFirmwareType', 'GetHostNameW',
    'GetOverlappedResultEx', 'GetPackageFamilyName',
    'GetProcessDefaultCpuSets', 'GetProcessInformation',
    'GetProcessMitigationPolicy', 'GetSystemCpuSetInformation',
    'GetSystemTimePreciseAsFileTime', 'GetTempPath2A', 'GetTempPath2W',
    'GetThreadDescription', 'GetThreadInformation', 'MapViewOfFile3',
    'MapViewOfFileFromApp', 'MapViewOfFileNuma2', 'OfferVirtualMemory',
    'PrefetchVirtualMemory', 'ProcessPrng', 'QueryInterruptTime',
    'QueryInterruptTimePrecise', 'QueryUnbiasedInterruptTimePrecise',
    'ReclaimVirtualMemory', 'RegisterSuspendResumeNotification',
    'SetProcessInformation', 'SetProcessMitigationPolicy',
    'SetThreadDescription', 'SetThreadInformation',
    'SetThreadSelectedCpuSets', 'UnregisterSuspendResumeNotification',
    'VirtualAlloc2', 'VirtualAllocFromApp', 'WaitOnAddress',
    'WakeByAddressAll', 'WakeByAddressSingle',
    # user32 / shcore / dwmapi
    'AdjustWindowRectExForDpi', 'AreDpiAwarenessContextsEqual',
    'EnableNonClientDpiScaling', 'GetAwarenessFromDpiAwarenessContext',
    'GetDpiForMonitor', 'GetDpiForSystem', 'GetDpiForWindow',
    'GetPointerDevice', 'GetPointerDevices', 'GetPointerFrameTouchInfo',
    'GetPointerInfo', 'GetPointerPenInfo', 'GetPointerTouchInfo',
    'GetPointerType', 'GetProcessDpiAwareness', 'GetSystemDpiForProcess',
    'GetSystemMetricsForDpi', 'GetThreadDpiAwarenessContext',
    'GetWindowDpiAwarenessContext', 'InitializeTouchInjection',
    'InjectTouchInput', 'IsImmersiveProcess',
    'SetProcessDpiAwareness', 'SetProcessDpiAwarenessContext',
    'SetThreadDpiAwarenessContext', 'SystemParametersInfoForDpi',
}

# DLLs that don't exist on Windows 7 at all. API sets are resolved by the
# loader from Windows 8 on.
WIN8_PLUS_DLLS = re.compile(
    r'^(api-ms-win-|ext-ms-win-|combase\.dll$|synchronization\.dll$|'
    r'shcore\.dll$|dcomp\.dll$|d3d12\.dll$|dxgidebug\.dll$|'
    r'windows\.storage\.dll$|coremessaging\.dll$|mfsensorgroup\.dll$|'
    r'icu\.dll$|bcp47langs\.dll$|wer\.dll$|dxcore\.dll$)', re.IGNORECASE)

# Exports added by updates that are required on top of Windows 7 SP1, so a
# plain SP1 System32 doesn't have them. KB2533623 is also required by libuv.
REQUIRED_UPDATE_EXPORTS = {
    'kernel32.dll': {
        'AddDllDirectory', 'RemoveDllDirectory', 'SetDefaultDllDirectories'
    },
}


def find_dumpbin():
  vswhere = os.path.join(os.environ['ProgramFiles(x86)'],
                         'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
  vs = subprocess.check_output(
      [vswhere, '-latest', '-products', '*', '-property', 'installationPath'],
      text=True).strip()
  matches = sorted(
      glob.glob(
          os.path.join(vs, 'VC', 'Tools', 'MSVC', '*', 'bin', 'Hostx64', 'x64',
                       'dumpbin.exe')))
  if not matches:
    sys.exit('dumpbin.exe not found')
  return matches[-1]


def run_dumpbin(dumpbin, flag, path):
  return subprocess.run([dumpbin, '/nologo', flag, path],
                        capture_output=True,
                        text=True,
                        errors='replace').stdout


def parse_imports(text):
  """Returns {'static': {dll: set(names)}, 'delay': {dll: set(names)}}."""
  result = {'static': {}, 'delay': {}}
  kind = 'static'
  dll = None
  # Static entries are "<hint> <name>"; delay-load entries are
  # "<address> <hint> <name>" and are preceded by single-word header lines
  # such as "00000001 Characteristics" that must not match.
  entry = {
      'static': re.compile(r'^\s+[0-9A-F]+\s+(\S+)\s*$'),
      'delay': re.compile(r'^\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)\s*$'),
  }
  ordinal = re.compile(r'^\s+(?:[0-9A-F]+\s+)?Ordinal\s+(\d+)\s*$')
  for line in text.splitlines():
    if line.strip() == 'Summary':
      break
    if 'delay load imports' in line:
      kind = 'delay'
      dll = None
      continue
    m = re.match(r'^\s{4}(\S+\.(?:dll|exe|drv|sys|cpl|ocx))\s*$', line,
                 re.IGNORECASE)
    if m:
      dll = m.group(1).lower()
      result[kind].setdefault(dll, set())
      continue
    if dll is None:
      continue
    m = ordinal.match(line)
    if m:
      result[kind][dll].add('#' + m.group(1))
      continue
    m = entry[kind].match(line)
    if m:
      result[kind][dll].add(m.group(1))
  return result


@functools.lru_cache(maxsize=None)
def win7_exports(dumpbin, system32, dll):
  """Returns the export names of |dll| in |system32|, or None if absent."""
  path = os.path.join(system32, dll)
  if not os.path.exists(path):
    return None
  names = set()
  in_table = False
  for line in run_dumpbin(dumpbin, '/exports', path).splitlines():
    if re.match(r'^\s+ordinal\s+hint\s+RVA\s+name', line):
      in_table = True
      continue
    if not in_table:
      continue
    m = re.match(r'^\s+(\d+)\s+(?:[0-9A-F]+\s+)?(?:[0-9A-F]{8}\s+)?(\S+)',
                 line)
    if m:
      names.add('#' + m.group(1))
      names.add(m.group(2))
    elif line.strip() == 'Summary':
      break
  return frozenset(names | REQUIRED_UPDATE_EXPORTS.get(dll, set()))


def check_binary(dumpbin, path, system32, own_dlls):
  imports = parse_imports(run_dumpbin(dumpbin, '/imports', path))
  problems = {'static': [], 'delay': []}
  for kind, dlls in imports.items():
    for dll, names in sorted(dlls.items()):
      # comctl32 v6 lives in WinSxS, not System32.
      if dll in own_dlls or dll == 'comctl32.dll':
        continue
      exports = win7_exports(dumpbin, system32, dll) if system32 else None
      if system32 and exports is None and not WIN8_PLUS_DLLS.match(dll):
        # Not a system DLL we know about (e.g. a redistributable).
        continue
      if WIN8_PLUS_DLLS.match(dll) or (system32 and exports is None):
        problems[kind].append(f'{dll} (DLL missing on Windows 7)')
        continue
      for name in sorted(names):
        missing = (name not in exports) if exports is not None else (
            name in WIN8_PLUS_FUNCTIONS)
        if missing:
          problems[kind].append(f'{dll}!{name}')

  for line in run_dumpbin(dumpbin, '/headers', path).splitlines():
    m = re.match(r'^\s+(\d+)\.(\d+) (operating system|subsystem) version', line)
    if m and (int(m.group(1)), int(m.group(2))) > (6, 1):
      problems['static'].append(
          f'{m.group(3)} version is {m.group(1)}.{m.group(2)} (> 6.1)')
  return problems


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument('paths', nargs='+',
                      help='binaries, or directories to scan for .exe/.dll')
  parser.add_argument('--system32',
                      help='directory with Windows 7 SP1 System32 DLLs')
  args = parser.parse_args()

  binaries = []
  for p in args.paths:
    if os.path.isdir(p):
      binaries += sorted(glob.glob(os.path.join(p, '*.exe')) +
                         glob.glob(os.path.join(p, '*.dll')))
    else:
      binaries.append(p)
  # DLLs shipped next to the binaries are not system DLLs.
  own_dlls = {os.path.basename(b).lower() for b in binaries}

  dumpbin = find_dumpbin()
  errors = 0
  for binary in binaries:
    problems = check_binary(dumpbin, binary, args.system32, own_dlls)
    for item in problems['static']:
      print(f'error: {binary}: imports {item}')
      errors += 1
    for item in problems['delay']:
      print(f'warning: {binary}: delay-loads {item}')

  if errors:
    print(f'{errors} static import(s) will prevent loading on Windows 7.')
    return 1
  print('No static imports that require Windows 8 or newer.')
  return 0


if __name__ == '__main__':
  sys.exit(main())
