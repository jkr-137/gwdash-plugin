// Toolbox's ToolboxIni.cpp logs through Log::LogW, which normally comes from
// GWToolboxdll's export table. We don't link GWToolboxdll (see
// compat/gwtoolboxdll_export.h), so provide our own implementations here.
//
// Diagnostics go to the debugger; the user-facing variants go to game chat via
// GWCA, which is a real DLL already loaded into the process.

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include <GWCA/Managers/ChatMgr.h>

#include <Logger.h>

namespace {
    constexpr size_t MAX_MESSAGE = 4096;

    void DebugOutA(const char* prefix, const char* format, va_list args)
    {
        char message[MAX_MESSAGE];
        vsnprintf(message, _countof(message), format, args);

        char line[MAX_MESSAGE + 64];
        snprintf(line, _countof(line), "[GWDash]%s %s\n", prefix, message);
        OutputDebugStringA(line);
    }

    void DebugOutW(const wchar_t* prefix, const wchar_t* format, va_list args)
    {
        wchar_t message[MAX_MESSAGE];
        _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);

        wchar_t line[MAX_MESSAGE + 64];
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"[GWDash]%s %ls\n", prefix, message);
        OutputDebugStringW(line);
    }

    void ChatOutA(const char* format, va_list args, const bool transient)
    {
        char message[MAX_MESSAGE];
        vsnprintf(message, _countof(message), format, args);

        wchar_t wide[MAX_MESSAGE];
        if (MultiByteToWideChar(CP_UTF8, 0, message, -1, wide, static_cast<int>(_countof(wide))) > 0) {
            GW::Chat::WriteChat(GWTOOLBOX_CHAN, wide, L"GWDash", transient);
        }
    }

    void ChatOutW(const wchar_t* format, va_list args, const bool transient)
    {
        wchar_t message[MAX_MESSAGE];
        _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
        GW::Chat::WriteChat(GWTOOLBOX_CHAN, message, L"GWDash", transient);
    }
}

namespace Log {
    void Log(const char* msg, ...)
    {
        va_list args;
        va_start(args, msg);
        DebugOutA("", msg, args);
        va_end(args);
    }

    void LogW(const wchar_t* msg, ...)
    {
        va_list args;
        va_start(args, msg);
        DebugOutW(L"", msg, args);
        va_end(args);
    }

    void FlushFile() {}

    void Info(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutA(format, args, false);
        va_end(args);
    }

    void InfoW(const wchar_t* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutW(format, args, false);
        va_end(args);
    }

    void Flash(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutA(format, args, true);
        va_end(args);
    }

    void FlashW(const wchar_t* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutW(format, args, true);
        va_end(args);
    }

    void Error(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutA(format, args, true);
        va_end(args);
    }

    void ErrorW(const wchar_t* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutW(format, args, true);
        va_end(args);
    }

    void Warning(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutA(format, args, true);
        va_end(args);
    }

    void WarningW(const wchar_t* format, ...)
    {
        va_list args;
        va_start(args, format);
        ChatOutW(format, args, true);
        va_end(args);
    }

    // Deliberately does not terminate: a plugin should never take the game down
    // with it. Nothing we compile from the Toolbox tree uses ASSERT today.
    void FatalAssert(const char* expr, const char* file, const unsigned line)
    {
        char line_buffer[MAX_MESSAGE];
        snprintf(line_buffer, _countof(line_buffer), "[GWDash] assertion failed: %s (%s:%u)\n", expr, file, line);
        OutputDebugStringA(line_buffer);
    }
}
