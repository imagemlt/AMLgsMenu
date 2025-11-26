#include "application.h"

#include <string>
#include <sys/resource.h>
#include <cstdio>

int main(int argc, char **argv) {
    std::string font_path;
    bool use_mock = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) {
            font_path = argv[++i];
        } else if (arg == "-m" && i + 1 < argc) {
            use_mock = (std::atoi(argv[++i]) != 0);
        }
    }

    if (setpriority(PRIO_PROCESS, 0, 5) != 0) {
        std::perror("[AMLgsMenu] setpriority");
    }

    Application app;
    if (!app.Initialize(font_path, use_mock)) {
        return 1;
    }

    app.Run();
    app.Shutdown();

    return 0;
}
