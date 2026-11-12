// Operating-system information on Windows.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
// powrprof.h needs the declarations from windows.h, so it stays in its own block after it.
#include <powrprof.h>

#include <eigenbook/platform/system_info.hpp>

namespace eigenbook::platform {

namespace {

std::string toUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

} // namespace

std::string osDescription() {
    // RtlGetVersion reports the real version even without a compatibility manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const FARPROC proc = ntdll != nullptr ? GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    if (proc == nullptr) {
        return "Windows (version unknown)";
    }
    RtlGetVersionFn rtlGetVersion = nullptr;
    static_assert(sizeof rtlGetVersion == sizeof proc);
    std::memcpy(&rtlGetVersion, &proc, sizeof proc);

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    if (rtlGetVersion(&info) != 0) {
        return "Windows (version unknown)";
    }
    return "Windows " + std::to_string(info.dwMajorVersion) + "." +
           std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
}

std::string powerPolicy() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr) {
        return "unknown";
    }
    DWORD size = 0;
    std::string name = "unknown";
    if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS &&
        size > 0) {
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (PowerReadFriendlyName(nullptr,
                                  scheme,
                                  nullptr,
                                  nullptr,
                                  reinterpret_cast<UCHAR*>(buffer.data()),
                                  &size) == ERROR_SUCCESS) {
            name = toUtf8(buffer.data());
        }
    }
    LocalFree(scheme);
    return name;
}

std::string powerSource() {
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status) == 0) {
        return "unknown";
    }
    switch (status.ACLineStatus) {
        case 1:
            return "AC";
        case 0:
            return "battery";
        default:
            return "unknown";
    }
}

bool largePagesAvailable() {
    if (GetLargePageMinimum() == 0) {
        return false;
    }
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    LUID lockMemory{};
    bool found = false;
    if (LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &lockMemory) != 0) {
        DWORD size = 0;
        GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
        if (size > 0) {
            std::vector<unsigned char> buffer(size);
            if (GetTokenInformation(token, TokenPrivileges, buffer.data(), size, &size) != 0) {
                const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
                const LUID_AND_ATTRIBUTES* entries = privileges->Privileges;
                for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
                    if (entries[i].Luid.LowPart == lockMemory.LowPart &&
                        entries[i].Luid.HighPart == lockMemory.HighPart) {
                        found = true;
                    }
                }
            }
        }
    }
    CloseHandle(token);
    return found;
}

std::size_t pageSize() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
}

} // namespace eigenbook::platform
