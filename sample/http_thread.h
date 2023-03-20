#ifndef __HTTP_THREAD_H__
#define __HTTP_THREAD_H__

#include <event2/util.h>

#ifdef __cplusplus
extern "C" {
#endif

struct evhttp_thread_pool *evhttp_thread_pool_new(const struct event_config *cfg, int nthreads);

void evhttp_thread_pool_free(struct evhttp_thread_pool *evpool);

void evhttp_thread_pool_assign(struct evhttp_thread_pool *evpool, 
	evutil_socket_t nfd, 
	struct sockaddr *addr, 
	int addrlen);

int evhttp_thread_pool_get_connection_count(struct evhttp_thread_pool *evpool);

int evhttp_thread_pool_get_thread_count(struct evhttp_thread_pool *evpool);

struct evhttp *evhttp_thread_pool_get_http(
	struct evhttp_thread_pool *evpool, int index);

#ifdef __cplusplus
}
#endif

#endif //__HTTP_THREAD_H__