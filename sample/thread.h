#ifndef __THREAD_H__
#define __THREAD_H__

#include <event2/util.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bufferevent_socket_new_cb)(struct bufferevent *bev, void *ctx);

struct eveasy_thread_pool *eveasy_thread_pool_new(
	const struct event_config *cfg, int nthreads);

void eveasy_thread_pool_free(struct eveasy_thread_pool *evpool);

void eveasy_thread_pool_assign(struct eveasy_thread_pool *evpool,
	evutil_socket_t nfd, struct sockaddr *addr, int addrlen);

void eveasy_thread_pool_setcb(
	struct eveasy_thread_pool *evpool, bufferevent_socket_new_cb cb, void *arg);

#ifdef __cplusplus
}
#endif

#endif //__THREAD_H__