#include "application.h"

#include <getopt.h>
#include <string>
#include <sys/resource.h>
#include <cstdio>

static void PrintUsage(const char *prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  -t, --font PATH       font file to load (default builtin)\n"
        "  -m, --mock 0|1        enable mock telemetry\n"
        "  -c, --command-cfg PATH command templates file (default /flash/command.cfg)\n"
        "  -f, --config PATH     wfb.conf path (default /flash/wfb.conf)\n"
        "  -h, --help            this message\n",
        prog);
}

int main(int argc, char **argv) {
    std::string font_path;
    bool use_mock = false;
    std::string cmd_cfg;
    std::string cfg_path;
    const option long_opts[] = {
        {"font", required_argument, nullptr, 't'},
        {"mock", required_argument, nullptr, 'm'},
        {"command-cfg", required_argument, nullptr, 'c'},
        {"config", required_argument, nullptr, 'f'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:m:c:f:h", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 't':
            font_path = optarg;
            break;
        case 'm':
            use_mock = (std::atoi(optarg) != 0);
            break;
        case 'c':
            cmd_cfg = optarg;
            break;
        case 'f':
            cfg_path = optarg;
            break;
        case 'h':
            PrintUsage(argv[0]);
            return 0;
        default:
            PrintUsage(argv[0]);
            return 1;
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
