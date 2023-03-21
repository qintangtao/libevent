#ifndef __THREAD_H__
#define __THREAD_H__

#include <event2/util.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*eveasyconn_cb)(struct eveasy_thread *t,
	evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg);

struct eveasy_thread_pool *eveasy_thread_pool_new(
	const struct event_config *cfg, int nthreads);

void eveasy_thread_pool_free(struct eveasy_thread_pool *evpool);

void eveasy_thread_pool_assign(struct eveasy_thread_pool *evpool,
	evutil_socket_t nfd, struct sockaddr *addr, int addrlen);

void eveasy_thread_pool_set_conncb(
	struct eveasy_thread_pool *evpool, eveasyconn_cb cb, void *arg);

struct event_base *eveasy_thread_get_base(struct eveasy_thread *evthread);


#ifdef __cplusplus
}
#endif

#endif //__THREAD_H__