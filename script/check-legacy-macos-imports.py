#!/usr/bin/env python

"""Reports imports in an Electron.app that won't resolve on this macOS version.

Run this on the old macOS release itself. Every system library and symbol
that the app's Mach-O files bind without a weak import is looked up with
dlopen() and dlsym(), so anything reported here makes dyld refuse to launch
the binary. Weak imports resolve to NULL instead, so they are only listed
with --weak and need an availability check at the call site.

Uses only the standard library and works with both the Python 2.7 that ships
with macOS 10.13 through 12.2 and Python 3.

Usage:
  python check-legacy-macos-imports.py /path/to/Electron.app
  python check-legacy-macos-imports.py --weak /path/to/Electron.app
"""

from __future__ import print_function

import argparse
import ctypes
import os
import platform
import struct
import subprocess
import sys

LC_REQ_DYLD = 0x80000000
LC_LOAD_DYLIB = 0xc
LC_LOAD_WEAK_DYLIB = 0x18 | LC_REQ_DYLD
LC_REEXPORT_DYLIB = 0x1f | LC_REQ_DYLD
LC_LAZY_LOAD_DYLIB = 0x20
LC_LOAD_UPWARD_DYLIB = 0x23 | LC_REQ_DYLD
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x22 | LC_REQ_DYLD
LC_DYLD_CHAINED_FIXUPS = 0x34 | LC_REQ_DYLD
LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
DYLIB_COMMANDS = (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB,
                  LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB)

MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe
FAT_MAGIC_64 = 0xcafebabf
CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000c

BIND_SYMBOL_FLAGS_WEAK_IMPORT = 0x1


def read_uleb(data, pos):
  result = shift = 0
  while True:
    byte = data[pos]
    pos += 1
    result |= (byte & 0x7f) << shift
    shift += 7
    if not byte & 0x80:
      return result, pos


def read_sleb(data, pos):
  result = shift = 0
  while True:
    byte = data[pos]
    pos += 1
    result |= (byte & 0x7f) << shift
    shift += 7
    if not byte & 0x80:
      if byte & 0x40:
        result -= 1 << shift
      return result, pos


def read_cstring(data, pos):
  end = data.index(b'\x00', pos)
  return bytes(data[pos:end]).decode('utf-8', 'replace'), end + 1


def parse_bind_opcodes(data, imports):
  """Adds (ordinal, symbol, weak) entries from bind or lazy bind opcodes."""
  pos = 0
  ordinal = 0
  symbol = None
  flags = 0
  while pos < len(data):
    opcode = data[pos] & 0xf0
    imm = data[pos] & 0x0f
    pos += 1
    if opcode == 0x00:  # DONE (separates entries in lazy bind info)
      continue
    elif opcode == 0x10:  # SET_DYLIB_ORDINAL_IMM
      ordinal = imm
    elif opcode == 0x20:  # SET_DYLIB_ORDINAL_ULEB
      ordinal, pos = read_uleb(data, pos)
    elif opcode == 0x30:  # SET_DYLIB_SPECIAL_IMM
      ordinal = (imm | 0xf0) - 0x100 if imm else 0
    elif opcode == 0x40:  # SET_SYMBOL_TRAILING_FLAGS_IMM
      flags = imm
      symbol, pos = read_cstring(data, pos)
    elif opcode == 0x50:  # SET_TYPE_IMM
      pass
    elif opcode == 0x60:  # SET_ADDEND_SLEB
      _, pos = read_sleb(data, pos)
    elif opcode in (0x70, 0x80):  # SET_SEGMENT_AND_OFFSET_ULEB, ADD_ADDR_ULEB
      _, pos = read_uleb(data, pos)
    elif opcode in (0x90, 0xb0):  # DO_BIND, DO_BIND_ADD_ADDR_IMM_SCALED
      imports.add((ordinal, symbol, bool(flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT)))
    elif opcode == 0xa0:  # DO_BIND_ADD_ADDR_ULEB
      imports.add((ordinal, symbol, bool(flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT)))
      _, pos = read_uleb(data, pos)
    elif opcode == 0xc0:  # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
      imports.add((ordinal, symbol, bool(flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT)))
      _, pos = read_uleb(data, pos)
      _, pos = read_uleb(data, pos)
    elif opcode == 0xd0:  # THREADED
      if imm == 0x00:  # SET_BIND_ORDINAL_TABLE_SIZE_ULEB
        _, pos = read_uleb(data, pos)
    else:
      raise ValueError('unknown bind opcode 0x%02x' % opcode)


def parse_chained_imports(data, imports):
  """Adds (ordinal, symbol, weak) entries from LC_DYLD_CHAINED_FIXUPS data."""
  (_, _, imports_offset, symbols_offset, imports_count, imports_format,
   _) = struct.unpack_from('<7I', data, 0)
  for i in range(imports_count):
    if imports_format == 1:  # DYLD_CHAINED_IMPORT
      (value,) = struct.unpack_from('<I', data, imports_offset + i * 4)
      ordinal, weak, name = value & 0xff, (value >> 8) & 1, value >> 9
      ordinal = ordinal - 0x100 if ordinal > 0xf0 else ordinal
    elif imports_format == 2:  # DYLD_CHAINED_IMPORT_ADDEND
      (value,) = struct.unpack_from('<I', data, imports_offset + i * 8)
      ordinal, weak, name = value & 0xff, (value >> 8) & 1, value >> 9
      ordinal = ordinal - 0x100 if ordinal > 0xf0 else ordinal
    elif imports_format == 3:  # DYLD_CHAINED_IMPORT_ADDEND64
      (value,) = struct.unpack_from('<Q', data, imports_offset + i * 16)
      ordinal, weak, name = value & 0xffff, (value >> 16) & 1, value >> 32
      ordinal = ordinal - 0x10000 if ordinal > 0xfff0 else ordinal
    else:
      raise ValueError('unknown chained imports format %d' % imports_format)
    symbol, _ = read_cstring(data, symbols_offset + name)
    imports.add((ordinal, symbol, bool(weak)))


def slice_offset(data):
  """Returns the offset of the 64-bit slice for this machine's CPU."""
  (magic,) = struct.unpack_from('>I', data, 0)
  if magic not in (FAT_MAGIC, FAT_MAGIC_64):
    return 0
  want = CPU_TYPE_ARM64 if platform.machine() == 'arm64' else CPU_TYPE_X86_64
  (count,) = struct.unpack_from('>I', data, 4)
  for i in range(count):
    if magic == FAT_MAGIC:
      cputype, _, offset, _, _ = struct.unpack_from('>5I', data, 8 + i * 20)
    else:
      cputype, _, offset, _, _, _ = struct.unpack_from('>2I2QII', data,
                                                       8 + i * 32)
    if cputype == want:
      return offset
  return None


def parse_macho(path):
  """Returns (min_os, [(dylib, weak_dylib)], {(ordinal, symbol, weak)}).

  Returns None if |path| is not a 64-bit Mach-O file for this machine.
  """
  with open(path, 'rb') as f:
    head = f.read(4)
    if len(head) < 4 or (struct.unpack('<I', head)[0] != MH_MAGIC_64 and
                         struct.unpack('>I', head)[0] not in (FAT_MAGIC,
                                                              FAT_MAGIC_64)):
      return None
    data = bytearray(head + f.read())
  base = slice_offset(data)
  if base is None:
    return None
  magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, base)
  if magic != MH_MAGIC_64:
    return None

  min_os = None
  dylibs = []
  imports = set()
  pos = base + 32
  for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from('<2I', data, pos)
    if cmd in DYLIB_COMMANDS:
      (name_offset,) = struct.unpack_from('<I', data, pos + 8)
      name, _ = read_cstring(data, pos + name_offset)
      dylibs.append((name, cmd == LC_LOAD_WEAK_DYLIB))
    elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
      fields = struct.unpack_from('<10I', data, pos + 8)
      for offset, size in ((fields[2], fields[3]), (fields[6], fields[7])):
        if size:
          parse_bind_opcodes(data[base + offset:base + offset + size], imports)
    elif cmd == LC_DYLD_CHAINED_FIXUPS:
      offset, size = struct.unpack_from('<2I', data, pos + 8)
      parse_chained_imports(data[base + offset:base + offset + size], imports)
    elif cmd == LC_BUILD_VERSION:
      (min_os,) = struct.unpack_from('<I', data, pos + 12)
    elif cmd == LC_VERSION_MIN_MACOSX:
      (min_os,) = struct.unpack_from('<I', data, pos + 8)
    pos += cmdsize
  return min_os, dylibs, imports


def format_version(packed):
  return '%d.%d.%d' % (packed >> 16, (packed >> 8) & 0xff, packed & 0xff)


def os_version():
  # platform.mac_ver() can report 10.16 on macOS 11 and later.
  version = subprocess.check_output(['sw_vers', '-productVersion'])
  parts = [int(p) for p in version.decode().strip().split('.')] + [0, 0]
  return (parts[0] << 16) | (parts[1] << 8) | parts[2]


_handles = {}


def load_system_library(path):
  """Returns a ctypes handle for |path|, or None if it doesn't load here."""
  if path not in _handles:
    try:
      _handles[path] = ctypes.CDLL(path)
    except OSError:
      _handles[path] = None
  return _handles[path]


def has_symbol(handle, symbol):
  # Mach-O symbol names carry a leading underscore that dlsym() adds itself.
  name = symbol[1:] if symbol.startswith('_') else symbol
  try:
    handle[name]
    return True
  except AttributeError:
    return False


def is_bundled(dylib):
  return dylib.startswith(('@rpath/', '@loader_path/', '@executable_path/'))


def find_binaries(paths):
  binaries = []
  for p in paths:
    if not os.path.isdir(p):
      binaries.append(p)
      continue
    for root, _, files in os.walk(p):
      for name in sorted(files):
        path = os.path.join(root, name)
        if os.path.isfile(path) and not os.path.islink(path):
          binaries.append(path)
  return binaries


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument('paths', nargs='+',
                      help='Electron.app, or individual Mach-O binaries')
  parser.add_argument('--weak', action='store_true',
                      help='also list weak imports missing on this macOS')
  args = parser.parse_args()

  current = os_version()
  print('Checking against macOS %s' % format_version(current))
  errors = 0
  for binary in find_binaries(args.paths):
    try:
      parsed = parse_macho(binary)
    except (IOError, OSError, ValueError, struct.error) as e:
      print('warning: %s: could not parse: %s' % (binary, e))
      continue
    if parsed is None:
      continue
    min_os, dylibs, imports = parsed

    if min_os is not None and min_os > current:
      print('error: %s: minimum macOS is %s' % (binary, format_version(min_os)))
      errors += 1

    missing_dylibs = set()
    for dylib, weak_dylib in dylibs:
      if is_bundled(dylib) or load_system_library(dylib):
        continue
      missing_dylibs.add(dylib)
      if not weak_dylib:
        print('error: %s: links %s (missing on this macOS)' % (binary, dylib))
        errors += 1

    for ordinal, symbol, weak in sorted(imports, key=lambda i: (i[0], i[1])):
      # Ordinals <= 0 mean this image, the main executable or a flat lookup.
      if ordinal <= 0 or ordinal > len(dylibs):
        continue
      dylib, weak_dylib = dylibs[ordinal - 1]
      if is_bundled(dylib) or dylib in missing_dylibs:
        continue
      if has_symbol(load_system_library(dylib), symbol):
        continue
      if weak or weak_dylib:
        if args.weak:
          print('weak: %s: %s from %s' % (binary, symbol, dylib))
        continue
      print('error: %s: imports %s from %s' % (binary, symbol, dylib))
      errors += 1

  if errors:
    print('%d import(s) will prevent launching on this macOS.' % errors)
    return 1
  print('No imports that are missing on this macOS.')
  return 0


if __name__ == '__main__':
  sys.exit(main())
