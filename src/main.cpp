#include "application.h"

#include <string>

int main(int argc, char **argv) {
    std::string font_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) {
            font_path = argv[++i];
        }
    }

    Application app;
    if (!app.Initialize(font_path)) {
        return 1;
    }

    app.Run();
    app.Shutdown();

    return 0;
}
