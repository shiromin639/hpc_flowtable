#include "app_init.h"

int
main(int argc, char *argv[])
{
    if (app_install_signal_handlers() != 0)
        return 1;

    if (app_init(argc, argv) != 0)
        return 1;

    app_run();
    app_cleanup();
    return 0;
}
