#ifndef APP_INIT_H
#define APP_INIT_H

int app_install_signal_handlers(void);
int app_init(int argc, char *argv[]);
int app_run(void);
void app_cleanup(void);

#endif /* APP_INIT_H */
