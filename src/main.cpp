#include "App.h"
#include <cstdlib>

#if defined(_WIN32) && defined(_DEBUG)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
static void OpenDebugConsole() {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$",  "r", stdin);
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
