#include "core/crash_handler.hpp"

#include "core/log.hpp"
#include "core/paths.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <string>
#include <system_error>

namespace core {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Everything the filter needs is prepared at install time: no allocation, loader or log-library work in the crash.
MiniDumpWriteDumpFn g_writeDump = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
wchar_t g_logDir[MAX_PATH] = {};
wchar_t g_logFile[MAX_PATH] = {};
volatile LONG g_inFilter = 0;
bool g_installed = false;

// Module name and offset of `address`, as "TT2.exe+0x1234".
void DescribeAddress(const void* address, char* out, int size)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* base = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                base = p + 1;
            }
        }
        lstrcpynA(name, base, MAX_PATH);
    }
    const auto offset = module ? static_cast<unsigned long long>(reinterpret_cast<const char*>(address) -
                                                                 reinterpret_cast<const char*>(module))
                               : reinterpret_cast<unsigned long long>(address);
    wsprintfA(out, "%s+0x%I64x", name, offset);
    (void)size;
}

// Raw append to tt2vr.log (spdlog may hold its lock in the crashing thread).
void AppendLogLine(const char* line, int length)
{
    HANDLE f = CreateFileW(g_logFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(f);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info)
{
    if (InterlockedExchange(&g_inFilter, 1) == 0 && info && info->ExceptionRecord) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        wchar_t dumpPath[MAX_PATH] = {};
        wsprintfW(dumpPath, L"%s\\crash_%04u%02u%02u_%02u%02u%02u.dmp", g_logDir, t.wYear, t.wMonth, t.wDay, t.wHour,
            t.wMinute, t.wSecond);
        bool dumped = false;
        if (g_writeDump) {
            HANDLE f = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
                const auto type = static_cast<MINIDUMP_TYPE>(
                    MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
                dumped = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr) != FALSE;
                CloseHandle(f);
            }
        }
        char where[MAX_PATH + 32] = {};
        DescribeAddress(info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        char line[MAX_PATH * 2 + 160] = {};
        char dumpName[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, dumpPath, -1, dumpName, MAX_PATH, nullptr, nullptr);
        const int n = wsprintfA(line, "[crash] exception 0x%08lx at %s, thread %lu; minidump %s%s\r\n",
            info->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId(), dumpName, dumped ? "" : " (NOT written)");
        AppendLogLine(line, n);
    }
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI CrashThread(LPVOID)
{
    *static_cast<volatile int*>(nullptr) = 0; // deliberate access violation (test mode "crashtest")
    return 0;
}

} // namespace

void InstallCrashHandler()
{
    if (g_installed) {
        return;
    }
    g_installed = true;
    const std::filesystem::path dir = ModDir() / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    lstrcpynW(g_logDir, dir.c_str(), MAX_PATH);
    lstrcpynW(g_logFile, (dir / L"tt2vr.log").c_str(), MAX_PATH);
    // The system copy: the game folder ships its own (older) dbghelp.dll for CrashRpt; an already loaded one is fine.
    HMODULE dbghelp = GetModuleHandleW(L"dbghelp.dll");
    if (!dbghelp) {
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW(system, MAX_PATH);
        dbghelp = LoadLibraryW((std::wstring(system) + L"\\dbghelp.dll").c_str());
    }
    g_writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    g_previous = SetUnhandledExceptionFilter(&Filter);
    char previous[MAX_PATH + 32] = "none";
    if (g_previous) {
        DescribeAddress(reinterpret_cast<const void*>(g_previous), previous, sizeof(previous));
    }
    TT2VR_LOG_INFO("crash handler: installed (minidumps to {}{}); previous filter {} is called after it", dir.string(),
        g_writeDump ? "" : ", MiniDumpWriteDump NOT found: log line only", previous);
}

void CrashForTest()
{
    TT2VR_LOG_WARN("crashtest: crashing the process now (test mode)"); // info and above are flushed at once
    HANDLE t = CreateThread(nullptr, 0, &CrashThread, nullptr, 0, nullptr);
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}

} // namespace core
