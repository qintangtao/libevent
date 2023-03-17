#ifndef __HTTP_THREAD_H__
#define __HTTP_THREAD_H__

#include <event2/util.h>

#ifdef __cplusplus
extern "C" {
#endif

struct evhttp_thread_pool *evhttp_thread_pool_new(
	struct evhttp *http, int nthreads);

void evhttp_thread_pool_free(struct evhttp_thread_pool *evpool);

void evhttp_thread_dispatch_socket(
	struct evhttp_thread_pool *evpool, evutil_socket_t nfd, struct sockaddr *addr, int addrlen);

int
evhttp_thread_get_connection_count(struct evhttp_thread_pool *evpool);

#ifdef __cplusplus
}
#endif

#endif //__HTTP_THREAD_H__