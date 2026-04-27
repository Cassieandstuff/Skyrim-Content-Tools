#include "App.h"
#include <cstdlib>

#if defined(_WIN32) && defined(_DEBUG)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <cstring>
static void OpenDebugConsole() {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONIN$",  "r", stdin);

    // Write the log file next to the exe so the path is always predictable.
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (slash) *(slash + 1) = '\0'; else exePath[0] = '\0';
    char logPath[MAX_PATH];
    snprintf(logPath, MAX_PATH, "%ssct_debug.log", exePath);

    freopen(logPath, "w", stderr);
    setvbuf(stderr, nullptr, _IONBF, 0);  // unbuffered: every write flushes immediately

    time_t now = time(nullptr);
    fprintf(stderr, "=== SCT debug log %s", ctime(&now));
    printf("[SCT] log -> %s\n", logPath);
}
#else
static void OpenDebugConsole() {}
#endif

int main() {
    OpenDebugConsole();
    App app;
    if (!app.Init("Skyrim Content Tools  v0.1", 1600, 900))
        return EXIT_FAILURE;
    app.Run();
    return EXIT_SUCCESS;
}
