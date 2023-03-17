#ifndef __EVENT_THREAD_H__
#define __EVENT_THREAD_H__

#include <event2/util.h>

#ifdef __cplusplus
extern "C" {
#endif

int thread_init(unsigned int nthreads, struct event_base *main_base);

void dispatch_conn_new(evutil_socket_t fd);

#ifdef __cplusplus
}
#endif

#endif //__EVENT_THREAD_H__