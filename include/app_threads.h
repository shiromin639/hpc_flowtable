#ifndef APP_THREADS_H
#define APP_THREADS_H

#include <stdint.h>

struct worker_args {
    uint32_t worker_id;
};

int dispatcher_thread(void *arg);
int worker_thread(void *arg);

#endif /* APP_THREADS_H */
