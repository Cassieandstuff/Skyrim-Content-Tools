#include "App.h"
#include <cstdlib>

int main() {
    App app;
    if (!app.Init("Skyrim Content Tools  v0.1", 1600, 900))
        return EXIT_FAILURE;
    app.Run();
    return EXIT_SUCCESS;
}
