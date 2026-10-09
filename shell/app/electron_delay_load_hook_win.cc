// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

// Provides Windows 7 fallbacks for delay-loaded functions and DLLs that only
// exist on newer versions of Windows.
//
// Chromium delay-loads many Windows 8+ APIs and calls them without checking
// the OS version. Before Windows 10, a delay-load notification callback
// resolves each DLL and function itself: a DLL that doesn't exist is replaced
// by kernel32, and a function that doesn't exist is replaced by a fallback
// with the same signature, either a Windows 7 equivalent or a result that
// callers already handle as "not supported". Anything without a fallback
// still fails the usual way, after logging its name with OutputDebugString.

#include "shell/app/electron_delay_load_hook_win.h"

#include <windows.h>

#include <combaseapi.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <evntprov.h>
#include <mfapi.h>
#include <powrprof.h>
#include <roapi.h>
#include <roerrorapi.h>
#include <shellscalingapi.h>
#include <uiautomation.h>
#include <userenv.h>
#include <winstring.h>

#include <string_view>

#include "base/strings/string_util.h"
#include "chrome/common/win/delay_load_notify_hook.h"

namespace {

// Returns a fixed result, with the exact signature of the function it
// replaces so that stdcall argument cleanup is correct on 32-bit.
template <typename F, auto kResult>
struct Fixed;

template <typename R, typename... Args, auto kResult>
struct Fixed<R(WINAPI*)(Args...), kResult> {
  static R WINAPI Call(Args...) { return static_cast<R>(kResult); }
};

template <typename... Args, auto kResult>
struct Fixed<void(WINAPI*)(Args...), kResult> {
  static void WINAPI Call(Args...) {}
};

// --- Windows 7 equivalents ----------------------------------------------

HRESULT WINAPI GetDpiForMonitorFallback(HMONITOR,
                                        MONITOR_DPI_TYPE,
                                        UINT* dpi_x,
                                        UINT* dpi_y) {
  // Windows 7 only has a system-wide DPI.
  HDC screen = ::GetDC(nullptr);
  if (!screen) {
    return E_FAIL;
  }
  *dpi_x = static_cast<UINT>(::GetDeviceCaps(screen, LOGPIXELSX));
  *dpi_y = static_cast<UINT>(::GetDeviceCaps(screen, LOGPIXELSY));
  ::ReleaseDC(nullptr, screen);
  return S_OK;
}

HRESULT WINAPI GetScaleFactorForMonitorFallback(HMONITOR monitor,
                                                DEVICE_SCALE_FACTOR* scale) {
  UINT dpi_x = 96, dpi_y = 96;
  if (FAILED(GetDpiForMonitorFallback(monitor, MDT_EFFECTIVE_DPI, &dpi_x,
                                      &dpi_y))) {
    return E_FAIL;
  }
  *scale = static_cast<DEVICE_SCALE_FACTOR>(dpi_x * 100 / 96);
  return S_OK;
}

HRESULT WINAPI SetProcessDpiAwarenessFallback(PROCESS_DPI_AWARENESS value) {
  // Windows 7 only supports system DPI awareness.
  if (value != PROCESS_DPI_UNAWARE) {
    ::SetProcessDPIAware();
  }
  return S_OK;
}

HRESULT WINAPI GetProcessDpiAwarenessFallback(HANDLE,
                                              PROCESS_DPI_AWARENESS* value) {
  *value =
      ::IsProcessDPIAware() ? PROCESS_SYSTEM_DPI_AWARE : PROCESS_DPI_UNAWARE;
  return S_OK;
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRoleExFallback(ULONG) {
  return ::PowerDeterminePlatformRole();
}

VOID WINAPI QueryUnbiasedInterruptTimePreciseFallback(PULONGLONG time) {
  ::QueryUnbiasedInterruptTime(time);
}

VOID WINAPI QueryInterruptTimeFallback(PULONGLONG time) {
  // QueryInterruptTime() reads KUSER_SHARED_DATA.InterruptTime, a KSYSTEM_TIME
  // at a fixed address. High2Time is written first and High1Time last, so a
  // torn read shows up as a mismatch.
  struct KSystemTime {
    ULONG low_part;
    LONG high1_time;
    LONG high2_time;
  };
  const volatile KSystemTime* const interrupt_time =
      reinterpret_cast<const volatile KSystemTime*>(0x7FFE0008);
  LONG high;
  ULONG low;
  do {
    high = interrupt_time->high1_time;
    low = interrupt_time->low_part;
  } while (high != interrupt_time->high2_time);
  *time = (static_cast<ULONGLONG>(static_cast<ULONG>(high)) << 32) | low;
}

// Returns whether the value at |address| still equals |compare_address|'s.
// WaitOnAddress() only supports 1, 2, 4 and 8 byte values.
bool ValueUnchanged(volatile VOID* address,
                    PVOID compare_address,
                    SIZE_T address_size) {
  switch (address_size) {
    case 1:
      return *static_cast<volatile UINT8*>(address) ==
             *static_cast<UINT8*>(compare_address);
    case 2:
      return *static_cast<volatile UINT16*>(address) ==
             *static_cast<UINT16*>(compare_address);
    case 4:
      return *static_cast<volatile UINT32*>(address) ==
             *static_cast<UINT32*>(compare_address);
    case 8:
      return *static_cast<volatile UINT64*>(address) ==
             *static_cast<UINT64*>(compare_address);
    default:
      return false;
  }
}

// WaitOnAddress() may return spuriously, so polling is a valid (if slower)
// implementation, and waking is then unnecessary.
BOOL WINAPI WaitOnAddressFallback(volatile VOID* address,
                                  PVOID compare_address,
                                  SIZE_T address_size,
                                  DWORD milliseconds) {
  if (!ValueUnchanged(address, compare_address, address_size)) {
    return TRUE;
  }
  if (milliseconds == 0) {
    ::SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  ::Sleep(1);
  return TRUE;
}

VOID WINAPI WakeByAddressFallback(PVOID) {}

// ProcessPrng() requires Windows 10. RtlGenRandom, exported by advapi32 as
// SystemFunction036, takes a ULONG size and returns a BOOLEAN.
BOOL WINAPI ProcessPrngFallback(PBYTE data, SIZE_T size) {
  using RtlGenRandomFunc = BOOLEAN(WINAPI*)(PVOID, ULONG);
  static const auto rtl_gen_random = reinterpret_cast<RtlGenRandomFunc>(
      ::GetProcAddress(::LoadLibraryW(L"advapi32.dll"), "SystemFunction036"));
  if (!rtl_gen_random) {
    return FALSE;
  }
  // Requests never come close to 4GiB; reject them rather than split them.
  if (size > 0xFFFFFFFFu) {
    return FALSE;
  }
  return rtl_gen_random(data, static_cast<ULONG>(size)) ? TRUE : FALSE;
}

BOOL WINAPI CompareObjectHandlesFallback(HANDLE first, HANDLE second) {
  // Without the Windows 10 API, only identical handle values can be
  // recognized as the same object.
  return first == second;
}

PCWSTR WINAPI WindowsGetStringRawBufferFallback(HSTRING, UINT32* length) {
  if (length) {
    *length = 0;
  }
  return L"";
}

ULONG WINAPI EventSetInformationFallback(REGHANDLE,
                                         EVENT_INFO_CLASS,
                                         PVOID,
                                         ULONG) {
  return ERROR_NOT_SUPPORTED;
}

BOOL WINAPI GetPointerTypeFallback(UINT32, POINTER_INPUT_TYPE*) {
  // Pointer messages are never sent on Windows 7.
  ::SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}

struct Fallback {
  const char* name;
  FARPROC function;
};

#define FALLBACK(name, function) {#name, reinterpret_cast<FARPROC>(&function)}
#define FIXED(name, result) \
  {#name, reinterpret_cast<FARPROC>(&Fixed<decltype(&::name), result>::Call)}

const Fallback kFallbacks[] = {
    // shcore.dll (Windows 8.1)
    FALLBACK(GetDpiForMonitor, GetDpiForMonitorFallback),
    FALLBACK(GetScaleFactorForMonitor, GetScaleFactorForMonitorFallback),
    FALLBACK(SetProcessDpiAwareness, SetProcessDpiAwarenessFallback),
    FALLBACK(GetProcessDpiAwareness, GetProcessDpiAwarenessFallback),

    // powrprof.dll (Windows 8)
    FALLBACK(PowerDeterminePlatformRoleEx,
             PowerDeterminePlatformRoleExFallback),
    FIXED(PowerRegisterSuspendResumeNotification, ERROR_NOT_SUPPORTED),
    FIXED(PowerUnregisterSuspendResumeNotification, ERROR_SUCCESS),

    // kernel32.dll API sets (Windows 8 / 10)
    FALLBACK(QueryUnbiasedInterruptTimePrecise,
             QueryUnbiasedInterruptTimePreciseFallback),
    FALLBACK(QueryInterruptTime, QueryInterruptTimeFallback),
    FALLBACK(QueryInterruptTimePrecise, QueryInterruptTimeFallback),
    FALLBACK(WaitOnAddress, WaitOnAddressFallback),
    FALLBACK(WakeByAddressSingle, WakeByAddressFallback),
    FALLBACK(WakeByAddressAll, WakeByAddressFallback),
    FALLBACK(CompareObjectHandles, CompareObjectHandlesFallback),

    // bcryptprimitives.dll (Windows 10)
    FALLBACK(ProcessPrng, ProcessPrngFallback),

    // advapi32.dll (Windows 8)
    FALLBACK(EventSetInformation, EventSetInformationFallback),

    // user32.dll (Windows 8 / 10 / 11)
    FIXED(RegisterSuspendResumeNotification, nullptr),
    FIXED(UnregisterSuspendResumeNotification, TRUE),
    FIXED(RegisterPointerDeviceNotifications, FALSE),
    FIXED(GetPointerDevices, FALSE),
    FIXED(GetPointerDevice, FALSE),
    FIXED(GetPointerInfo, FALSE),
    FIXED(GetPointerPenInfo, FALSE),
    FIXED(GetPointerTouchInfo, FALSE),
    FIXED(GetPointerFrameTouchInfo, FALSE),
    FALLBACK(GetPointerType, GetPointerTypeFallback),
    FIXED(EnableMouseInPointer, FALSE),
    FIXED(IsMouseInPointerEnabled, FALSE),
    FIXED(IsWindowArranged, FALSE),

    // uiautomationcore.dll (Windows 8)
    FIXED(UiaDisconnectProvider, S_OK),
    FIXED(UiaDisconnectAllProviders, S_OK),

    // WinRT core (Windows 8)
    FIXED(RoInitialize, E_NOTIMPL),
    FIXED(RoUninitialize, 0),
    FIXED(RoGetActivationFactory, E_NOTIMPL),
    FIXED(RoActivateInstance, E_NOTIMPL),
    FIXED(RoGetAgileReference, E_NOTIMPL),
    FIXED(RoOriginateError, FALSE),
    FIXED(RoOriginateErrorW, FALSE),
    FIXED(WindowsCreateString, E_NOTIMPL),
    FIXED(WindowsCreateStringReference, E_NOTIMPL),
    FIXED(WindowsDeleteString, S_OK),
    FIXED(WindowsDuplicateString, E_NOTIMPL),
    FALLBACK(WindowsGetStringRawBuffer, WindowsGetStringRawBufferFallback),
    FIXED(WindowsGetStringLen, 0),
    FIXED(WindowsIsStringEmpty, TRUE),

    // dcomp.dll, d3d12.dll, dxgi.dll (Windows 8 / 8.1 / 10)
    FIXED(DCompositionCreateDevice, E_NOTIMPL),
    FIXED(DCompositionCreateDevice2, E_NOTIMPL),
    FIXED(DCompositionCreateDevice3, E_NOTIMPL),
    FIXED(DCompositionCreateSurfaceHandle, E_NOTIMPL),
    FIXED(D3D12CreateDevice, E_NOTIMPL),
    FIXED(D3D12GetDebugInterface, E_NOTIMPL),
    FIXED(D3D12SerializeRootSignature, E_NOTIMPL),
    FIXED(D3D12SerializeVersionedRootSignature, E_NOTIMPL),
    FIXED(CreateDXGIFactory2, E_NOTIMPL),
    FIXED(DXGIGetDebugInterface1, E_NOTIMPL),

    // mfplat.dll (Windows 8)
    FIXED(MFCreateDXGIDeviceManager, E_NOTIMPL),
    FIXED(MFCreateDXGISurfaceBuffer, E_NOTIMPL),
    FIXED(MFLockDXGIDeviceManager, E_NOTIMPL),
    FIXED(MFUnlockDXGIDeviceManager, E_NOTIMPL),

    // userenv.dll (Windows 8)
    FIXED(CreateAppContainerProfile, E_NOTIMPL),
    FIXED(DeleteAppContainerProfile, E_NOTIMPL),
    FIXED(DeriveAppContainerSidFromAppContainerName, E_NOTIMPL),
    FIXED(GetAppContainerFolderPath, E_NOTIMPL),
};

#undef FALLBACK
#undef FIXED

FARPROC FindFallback(const char* name) {
  for (const Fallback& fallback : kFallbacks) {
    if (std::string_view(fallback.name) == name) {
      return fallback.function;
    }
  }
  return nullptr;
}

void LogMissing(const char* dll, const char* name) {
  char message[256];
  ::wsprintfA(message, "Electron: no Windows 7 fallback for %s!%s\n",
              dll ? dll : "?", name ? name : "?");
  ::OutputDebugStringA(message);
}

// Used for ordinal imports from a DLL that doesn't exist, whose signature is
// unknown. On x64 every function shares a calling convention, so returning
// E_NOTIMPL is safe; on x86 this is only correct for functions without
// arguments, but nothing reaches it there in practice.
HRESULT WINAPI NotImplementedStub() {
  return E_NOTIMPL;
}

// Returns the Windows 7 DLL that implements |dll|, an API set or DLL that
// doesn't exist on this version of Windows. kernel32 is never delay-loaded, so
// a module resolved to kernel32 is always a stand-in.
HMODULE GetStandInModule(const char* dll) {
  if (base::StartsWith(dll, "api-ms-win-power-",
                       base::CompareCase::INSENSITIVE_ASCII)) {
    if (HMODULE powrprof = ::LoadLibraryExW(L"powrprof.dll", nullptr,
                                            LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      return powrprof;
    }
  }
  return ::GetModuleHandleW(L"kernel32.dll");
}

FARPROC DelayLoadCallback(unsigned delay_load_event, DelayLoadInfo* info) {
  switch (delay_load_event) {
    case dliNotePreLoadLibrary: {
      // Load the DLL the same way the delay-load helper does. If it doesn't
      // exist on this version of Windows (e.g. dcomp.dll or an API set), use
      // a stand-in so that each function resolves or falls back below.
      HMODULE module = ::LoadLibraryExA(info->szDll, nullptr, 0);
      if (!module) {
        module = GetStandInModule(info->szDll);
      }
      return reinterpret_cast<FARPROC>(module);
    }

    case dliNotePreGetProcAddress: {
      if (!info->dlp.fImportByName) {
        // An ordinal means nothing in a stand-in module.
        if (info->hmodCur == ::GetModuleHandleW(L"kernel32.dll")) {
          LogMissing(info->szDll, "(ordinal)");
          return reinterpret_cast<FARPROC>(&NotImplementedStub);
        }
        return nullptr;  // Let the helper resolve it.
      }
      if (FARPROC function =
              ::GetProcAddress(info->hmodCur, info->dlp.szProcName)) {
        return function;
      }
      FARPROC fallback = FindFallback(info->dlp.szProcName);
      if (!fallback) {
        LogMissing(info->szDll, info->dlp.szProcName);
      }
      return fallback;
    }

    default:
      return nullptr;
  }
}

bool IsBeforeWindows10() {
  // RtlGetVersion isn't affected by the executable's compatibility manifest.
  using RtlGetVersionFunc = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  const auto rtl_get_version = reinterpret_cast<RtlGetVersionFunc>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOEXW version = {};
  version.dwOSVersionInfoSize = sizeof(version);
  return rtl_get_version && rtl_get_version(&version) == 0 &&
         version.dwMajorVersion < 10;
}

}  // namespace

namespace electron {

void InstallLegacyWindowsDelayLoadFallbacks() {
  if (IsBeforeWindows10()) {
    SetDelayLoadHookCallback(&DelayLoadCallback);
  }
}

}  // namespace electron
