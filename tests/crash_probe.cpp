#include <cstdio>
#include <intrin.h>
#include <windows.h>

extern "C" __declspec(dllimport) int StartSyncToolCrashReporter(const wchar_t *, const wchar_t *, const char *);

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4) return 2;
    const int state = StartSyncToolCrashReporter(argv[1], argv[2], "diagnostic-test");
    std::printf("handler=%d wer=%d\n", state >= 1, state == 2);
    std::fflush(stdout);
    if (state == 0) return 3;
    if (wcscmp(argv[3], L"fastfail") == 0) {
        if (state != 2) return 4;
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 5;
}
