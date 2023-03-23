#ifndef __EASY_THREAD_H_INCLUDED_
#define __EASY_THREAD_H_INCLUDED_

#include <event2/util.h>
#include <event2/visibility.h>

#ifdef __cplusplus
extern "C" {
#endif

struct eveasy_thread;
struct eveasy_thread_pool;

typedef void (*eveasyconn_cb)(struct eveasy_thread *t,
	evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg);

EVENT2_EXPORT_SYMBOL
struct eveasy_thread_pool *eveasy_thread_pool_new(const struct event_config *cfg, int nthreads);

EVENT2_EXPORT_SYMBOL
void eveasy_thread_pool_free(struct eveasy_thread_pool *evpool);

EVENT2_EXPORT_SYMBOL
void eveasy_thread_pool_assign(struct eveasy_thread_pool *evpool, evutil_socket_t nfd, struct sockaddr *addr, int addrlen);

EVENT2_EXPORT_SYMBOL
void eveasy_thread_pool_set_conncb(struct eveasy_thread_pool *evpool, eveasyconn_cb cb, void *arg);

EVENT2_EXPORT_SYMBOL
struct eveasy_thread *eveasy_thread_pool_get_thread(struct eveasy_thread_pool *evpool, int index);

EVENT2_EXPORT_SYMBOL
struct event_base *eveasy_thread_get_base(struct eveasy_thread *evthread);

#ifdef __cplusplus
}
#endif

#endif //__EASY_THREAD_H__