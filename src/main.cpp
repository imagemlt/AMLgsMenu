#include "application.h"

#include <string>
#include <sys/resource.h>
#include <cstdio>

int main(int argc, char **argv) {
    std::string font_path;
    bool use_mock = false;
    std::string cmd_cfg;
    std::string cfg_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::printf("Usage: %s [-t font.ttf] [-m 1] [-c command.cfg] [-f wfb.conf]\n", argv[0]);
            return 0;
        }
        if (arg == "-t" && i + 1 < argc) {
            font_path = argv[++i];
        } else if (arg == "-m" && i + 1 < argc) {
            use_mock = (std::atoi(argv[++i]) != 0);
        } else if (arg == "-c" && i + 1 < argc) {
            cmd_cfg = argv[++i];
        } else if (arg == "-f" && i + 1 < argc) {
            cfg_path = argv[++i];
        }
    }

    if (setpriority(PRIO_PROCESS, 0, 5) != 0) {
        std::perror("[AMLgsMenu] setpriority");
    }

    Application app;
    if (!cmd_cfg.empty()) {
        app.SetCommandCfgPath(cmd_cfg);
    }
    if (!cfg_path.empty()) {
        app.SetConfigPath(cfg_path);
    }
    if (!app.Initialize(font_path, use_mock)) {
        return 1;
    }

    app.Run();
    app.Shutdown();

    return 0;
}
