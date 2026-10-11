#include "StartupLog.h"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static SRWLOCK s_lock = SRWLOCK_INIT;
static HANDLE s_hFile = INVALID_HANDLE_VALUE;
static std::atomic<bool> s_bActive{ false };
static ULONGLONG s_ullStart = 0;
static ULONGLONG s_ullSongLiveAt = 0;
static size_t s_cbWritten = 0;
static wchar_t s_wszPath[MAX_PATH] = L"";

// Boot is a few hundred lines; the cap only matters if something loops before a song loads.
static const size_t kMaxBytes = 1024 * 1024;
static const ULONGLONG kSongLiveMs = 5000; // Covers the async synth open and the first notes

static HANDLE OpenLogAt(const wchar_t* dir)
{
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, _countof(path), _TRUNCATE, L"%sstartup_log.txt", dir) < 0)
        return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        wcscpy_s(s_wszPath, path);
    return h;
}

// Caller holds s_lock.
static void WriteRaw(const char* text, size_t len)
{
    if (s_hFile == INVALID_HANDLE_VALUE || len == 0)
        return;
    DWORD written = 0;
    WriteFile(s_hFile, text, (DWORD)len, &written, NULL);
    s_cbWritten += written;
}

// Caller holds s_lock.
static void WriteLine(const char* text)
{
    char line[2048];
    const ULONGLONG ms = GetTickCount64() - s_ullStart;
    int n = _snprintf_s(line, _countof(line), _TRUNCATE, "[%5llu.%03u] [T%05lu] %s\r\n",
                        ms / 1000, (unsigned)(ms % 1000), GetCurrentThreadId(), text);
    if (n < 0) {
        // Truncated: keep the line terminator.
        n = (int)strlen(line);
        if (n >= 2) { line[n - 2] = '\r'; line[n - 1] = '\n'; }
    }
    WriteRaw(line, (size_t)n);
    if (s_cbWritten >= kMaxBytes) {
        static const char kCap[] = "[startup log size cap reached, logging stopped]\r\n";
        WriteRaw(kCap, sizeof(kCap) - 1);
        s_bActive.store(false, std::memory_order_release);
        CloseHandle(s_hFile);
        s_hFile = INVALID_HANDLE_VALUE;
    }
}

void StartupLog_Begin()
{
    AcquireSRWLockExclusive(&s_lock);
    if (s_hFile != INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    s_ullStart = GetTickCount64();

    wchar_t dir[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (wchar_t* slash = wcsrchr(dir, L'\\'))
            *(slash + 1) = L'\0';
        s_hFile = OpenLogAt(dir);
    }
    if (s_hFile == INVALID_HANDLE_VALUE && GetTempPathW(MAX_PATH, dir))
        s_hFile = OpenLogAt(dir);
    if (s_hFile == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    s_bActive.store(true, std::memory_order_release);

    char buf[1024];
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
#ifdef __AVX2__
    const char* flavor = "AVX2";
#else
    const char* flavor = "SSE4.2";
#endif
    _snprintf_s(buf, _countof(buf), _TRUNCATE, "PlayGroundFromAbove startup log - %s build, compiled %s %s",
                flavor, __DATE__, __TIME__);
    WriteLine(buf);
    _snprintf_s(buf, _countof(buf), _TRUNCATE, "exe: %ls", exe);
    WriteLine(buf);

    // GetVersionEx lies without a manifest entry; RtlGetVersion does not.
    typedef LONG(WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
    OSVERSIONINFOW ovi = {};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    if (auto pRtlGetVersion = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))
        pRtlGetVersion(&ovi);
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    _snprintf_s(buf, _countof(buf), _TRUNCATE, "os: Windows %lu.%lu build %lu, cpus=%lu, ram=%llu MB (%llu MB free)",
                ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber, si.dwNumberOfProcessors,
                ms.ullTotalPhys >> 20, ms.ullAvailPhys >> 20);
    WriteLine(buf);
    ReleaseSRWLockExclusive(&s_lock);
}

void StartupLog(const char* format, ...)
{
    if (!s_bActive.load(std::memory_order_acquire))
        return;
    char text[1536];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, _countof(text), _TRUNCATE, format, args);
    va_end(args);

    AcquireSRWLockExclusive(&s_lock);
    if (s_bActive.load(std::memory_order_relaxed))
        WriteLine(text);
    ReleaseSRWLockExclusive(&s_lock);
}

void StartupLogMirror(const char* text)
{
    if (!s_bActive.load(std::memory_order_acquire) || !text)
        return;
    // FNV-1a over the text with digit runs collapsed, so "fps=59" and "fps=60" count as one.
    unsigned hash = 2166136261u;
    bool inDigits = false;
    for (const char* p = text; *p; ++p) {
        const bool digit = *p >= '0' && *p <= '9';
        if (digit && inDigits)
            continue;
        inDigits = digit;
        hash = (hash ^ (unsigned char)(digit ? '#' : *p)) * 16777619u;
    }
    static const int kMaxRepeats = 5;
    static unsigned s_hashes[512];
    static unsigned char s_counts[512];

    AcquireSRWLockExclusive(&s_lock);
    if (s_bActive.load(std::memory_order_relaxed)) {
        unsigned slot = hash & 511u;
        for (unsigned probe = 0; probe < 512; ++probe, slot = (slot + 1) & 511u) {
            if (s_counts[slot] == 0 || s_hashes[slot] == hash)
                break;
        }
        // A full table just stops limiting; the size cap still bounds the file.
        if (s_counts[slot] == 0)
            s_hashes[slot] = hash;
        if (s_hashes[slot] != hash) {
            WriteLine(text);
        } else if (s_counts[slot] < kMaxRepeats) {
            ++s_counts[slot];
            WriteLine(text);
            if (s_counts[slot] == kMaxRepeats) {
                char note[1600];
                _snprintf_s(note, _countof(note), _TRUNCATE, "(repeats of \"%s\" are no longer logged)", text);
                WriteLine(note);
            }
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void StartupLog_End(const char* reason)
{
    AcquireSRWLockExclusive(&s_lock);
    if (s_bActive.load(std::memory_order_relaxed)) {
        char text[512];
        _snprintf_s(text, _countof(text), _TRUNCATE, "startup log finished: %s", reason ? reason : "");
        WriteLine(text);
        s_bActive.store(false, std::memory_order_release);
    }
    if (s_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(s_hFile);
        s_hFile = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

bool StartupLog_IsActive()
{
    return s_bActive.load(std::memory_order_acquire);
}

const wchar_t* StartupLog_Path()
{
    return s_wszPath;
}

void StartupLog_CrashNote(const char* note)
{
    if (!s_bActive.load(std::memory_order_acquire))
        return;
    if (!TryAcquireSRWLockExclusive(&s_lock))
        return;
    if (s_bActive.load(std::memory_order_relaxed))
        WriteLine(note);
    ReleaseSRWLockExclusive(&s_lock);
}

void StartupLog_OnFrame(const char* stateName)
{
    if (!s_bActive.load(std::memory_order_acquire))
        return;
    static const char* s_lastState = nullptr;
    if (stateName != s_lastState) {
        s_lastState = stateName;
        StartupLog("game state now %s", stateName ? stateName : "?");
    }
    if (!stateName || strcmp(stateName, "MainScreen") != 0)
        return;
    const ULONGLONG now = GetTickCount64();
    if (!s_ullSongLiveAt) {
        s_ullSongLiveAt = now;
        StartupLog("first song is live, startup log ends in %llu ms", kSongLiveMs);
    } else if (now - s_ullSongLiveAt >= kSongLiveMs) {
        StartupLog_End("first MIDI loaded and playing");
    }
}
