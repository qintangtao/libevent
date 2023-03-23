#ifndef __HTTP_THREAD_H_INCLUDED_
#define __HTTP_THREAD_H_INCLUDED_

#include <event2/util.h>
#include <event2/visibility.h>

#ifdef __cplusplus
extern "C" {
#endif

struct evhttp_thread;
struct evhttp_thread_pool;

EVENT2_EXPORT_SYMBOL
struct evhttp_thread_pool *evhttp_thread_pool_new(const struct event_config *cfg, int nthreads);

EVENT2_EXPORT_SYMBOL
void evhttp_thread_pool_free(struct evhttp_thread_pool *evpool);

EVENT2_EXPORT_SYMBOL
void evhttp_thread_pool_assign(struct evhttp_thread_pool *evpool, evutil_socket_t nfd, struct sockaddr *addr, int addrlen);

EVENT2_EXPORT_SYMBOL
int evhttp_thread_pool_get_connection_count(struct evhttp_thread_pool *evpool);

EVENT2_EXPORT_SYMBOL
int evhttp_thread_pool_get_thread_count(struct evhttp_thread_pool *evpool);

EVENT2_EXPORT_SYMBOL
struct evhttp_thread *evhttp_thread_pool_get_thread(struct evhttp_thread_pool *evpool, int index);

EVENT2_EXPORT_SYMBOL
struct evhttp *evhttp_thread_get_http(struct evhttp_thread *evthread);

#ifdef __cplusplus
}
#endif

#endif //__HTTP_THREAD_H__