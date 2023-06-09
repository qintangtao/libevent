
#include <event2/http_thread.h>
#include <event2/event.h>
#include <event2/event_compat.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include "evthread-internal.h"
#include "mm-internal.h"
#include "time-internal.h"
#include "http-internal.h"
#include "compat/sys/queue.h"

#include <stdbool.h>

#define MAX_THREAD_COUNT 128
#define SOCKET_PER_ALLOC 64

/* each bound socket is stored in one of these */
struct evhttp_socket {
	TAILQ_ENTRY(evhttp_socket) next;

	evutil_socket_t nfd;
	struct sockaddr addr;
	int addrlen;
};

struct evhttp_socket_per {
	TAILQ_ENTRY(evhttp_socket_per) next;

	struct evhttp_socket *socket;
};

struct evhttp_thread {
	int thread_id; /* unique ID of this thread */
	struct evhttp *http;
	struct event_base *base;		   /* libevent handle this thread uses */
	struct event *notify_event;		   /* listen event for notify pipe */
	evutil_socket_t notify_receive_fd; /* receiving end of notify pipe */
	evutil_socket_t notify_send_fd;	/* sending end of notify pipe */
	struct evhttp_thread_pool *pool;

	TAILQ_HEAD(evhttp_socketq, evhttp_socket)
	sockets; /* queue of new connections */
	void *lock;
};

struct evhttp_thread_pool {
	int thread_idx;
	int thread_cnt;
	struct evhttp_thread *threads;

	int socket_cnt;
	TAILQ_HEAD(evhttp_socketf, evhttp_socket)
	sockets; /* queue of free connections */
	void *lock;

	TAILQ_HEAD(evhttp_socket_refq, evhttp_socket_per)
	socket_pers; /* queue of socket pers*/
};

#define EVHTTP_THREAD_LOCK(b)        \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->lock, 0); \
	} while (0)

#define EVHTTP_THREAD_UNLOCK(b)        \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->lock, 0); \
	} while (0)


static unsigned long
sys_os_create_thread(void *(*func)(void *), void *arg)
{
#ifdef _WIN32
	HANDLE hThread;
	DWORD threadid = 0;
	hThread =
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, &threadid);
	if (hThread == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Can't create thread: %d\n", GetLastError());
		goto error;
	}
	CloseHandle(hThread);

	return (unsigned long)threadid;
#else
	int ret;
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
		fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
		goto error;
	}
	pthread_detach(thread);

	return (unsigned long)thread;
#endif

error:
	return 0;
}

static struct evhttp_socket *
evhttp_socket_new(struct evhttp_thread_pool *evpool)
{
	struct evhttp_socket *evsocket = NULL;
	struct evhttp_socket_per *evsocket_per;
	int i;

	EVHTTP_THREAD_LOCK(evpool);
	if ((evsocket = TAILQ_FIRST(&evpool->sockets)) != NULL) {
		TAILQ_REMOVE(&evpool->sockets, evsocket, next);
		evpool->socket_cnt--;
	}
	EVHTTP_THREAD_UNLOCK(evpool);

	if (NULL == evsocket) {

		evsocket_per = mm_calloc(1, sizeof(struct evhttp_socket_per));
		if (NULL == evsocket_per) {
			event_warn("%s: calloc failed", __func__);
			return NULL;
		}

		evsocket = mm_calloc(SOCKET_PER_ALLOC, sizeof(struct evhttp_socket));
		if (NULL == evsocket) {
			event_warn("%s: calloc failed", __func__);
			mm_free(evsocket_per);
			return NULL;
		}

		evsocket_per->socket = evsocket;
		TAILQ_INSERT_TAIL(&evpool->socket_pers, evsocket_per, next);

		EVHTTP_THREAD_LOCK(evpool);
		for (i = 1; i < SOCKET_PER_ALLOC; i++)
			TAILQ_INSERT_TAIL(&evpool->sockets, &evsocket[i], next);
		evpool->socket_cnt += (SOCKET_PER_ALLOC-1);
		EVHTTP_THREAD_UNLOCK(evpool);
	}

	return evsocket;
}

static void
evhttp_socket_free(
	struct evhttp_thread_pool *evpool, struct evhttp_socket *evsocket)
{
#if 0
	if (evpool->socket_cnt > SOCKET_PER_ALLOC * 2) {
		mm_free(evsocket);
		return;
	}
#endif

	EVHTTP_THREAD_LOCK(evpool);
	TAILQ_INSERT_TAIL(&evpool->sockets, evsocket, next);
	evpool->socket_cnt++;
	EVHTTP_THREAD_UNLOCK(evpool);
}

static void 
evhttp_socket_push(
	struct evhttp_thread *evthread, struct evhttp_socket *evsocket)
{
	EVHTTP_THREAD_LOCK(evthread);
	TAILQ_INSERT_TAIL(&evthread->sockets, evsocket, next);
	EVHTTP_THREAD_UNLOCK(evthread);
}

static struct evhttp_socket *
evhttp_socket_pop(struct evhttp_thread *evthread)
{
	struct evhttp_socket *evsocket;

	EVHTTP_THREAD_LOCK(evthread);
	if ((evsocket = TAILQ_FIRST(&evthread->sockets)) != NULL) {
		TAILQ_REMOVE(&evthread->sockets, evsocket, next);
	}
	EVHTTP_THREAD_UNLOCK(evthread);

	return evsocket;
}

static void evhttp_thread_pool_assign(struct evhttp_thread_pool *evpool,
	evutil_socket_t nfd, struct sockaddr *addr, int addrlen);

	/* Listener callback when a connection arrives at a server. */
static void
accept_socket_cb(struct evconnlistener *listener, evutil_socket_t nfd,
	struct sockaddr *peer_sa, int peer_socklen, void *arg)
{
	struct evhttp_thread_pool *pool = arg;

	evhttp_thread_pool_assign(pool, nfd, peer_sa, peer_socklen);
}


static void
notify_cb(evutil_socket_t fd, short which, void *arg)
{
	struct evhttp_thread *evthread = arg;
	struct evhttp_thread_pool *evpool = evthread->pool;
	struct evhttp *http = evthread->http;
	struct evhttp_socket *evsocket;
	char buf[1];

	if (recv(fd, buf, 1, 0) != 1) {
		fprintf(stderr, "Can't read from libevent pipe\n");
		return;
	}

	switch (buf[0]) {
	case 'c': {
		while ((evsocket = evhttp_socket_pop(evthread)) != NULL) {

			evhttp_get_request(
				http, evsocket->nfd, &evsocket->addr, evsocket->addrlen, NULL);

			evhttp_socket_free(evpool, evsocket);
		}
	} break;
	default:
		break;
	}
}

static void
evhttp_thread_loopbreak(struct evhttp_thread *evthread)
{
	if (NULL == evthread)
		return;

	if (evthread->base)
		event_base_loopbreak(evthread->base);
}

static void
evhttp_thread_cleanup(struct evhttp_thread *evthread)
{
	struct timeval msec10 = {0, 10 * 1000};

	if (NULL == evthread)
		return;

	if (evthread->base)
		event_base_loopbreak(evthread->base);

	while (evthread->thread_id != 0)
		evutil_usleep_(&msec10);

	if (evthread->notify_receive_fd != EVUTIL_INVALID_SOCKET) {
		evutil_closesocket(evthread->notify_receive_fd);
		evthread->notify_receive_fd = EVUTIL_INVALID_SOCKET;
	}

	if (evthread->notify_send_fd != EVUTIL_INVALID_SOCKET) {
		evutil_closesocket(evthread->notify_send_fd);
		evthread->notify_send_fd = EVUTIL_INVALID_SOCKET;
	}

	if (evthread->notify_event) {
		event_free(evthread->notify_event);
		evthread->notify_event = NULL;
	}

	if (evthread->http) {
		evhttp_free(evthread->http);
		evthread->http = NULL;
	}

	if (evthread->base) {
		event_base_free(evthread->base);
		evthread->base = NULL;
	}

	if (evthread->lock) {
		EVTHREAD_FREE_LOCK(evthread->lock, EVTHREAD_LOCKTYPE_READWRITE);
		evthread->lock = NULL;
	}
}

static bool
evhttp_thread_setup(
	struct evhttp_thread *evthread, const struct event_config *cfg)
{
	evutil_socket_t fds[2];

#ifdef _WIN32
	if (evutil_socketpair(AF_INET, SOCK_STREAM, 0, fds) != 0)
#else
	if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
#endif
	{
		fprintf(stderr, "Can't create notify socket pair\n");
		goto error;
	}

	evutil_make_socket_nonblocking(fds[0]);
	evutil_make_socket_nonblocking(fds[1]);

	evthread->notify_receive_fd = fds[0];
	evthread->notify_send_fd = fds[1];

	evthread->base = event_base_new_with_config(cfg);
	if (!evthread->base) {
		fprintf(stderr, "Can't allocate event base\n");
		goto error;
	}

	evthread->http = evhttp_new(evthread->base);
	if (!evthread->http) {
		fprintf(stderr, "Can't allocate event http\n");
		goto error;
	}

	/* Listen for notifications from other threads */
	evthread->notify_event = event_new(evthread->base,
		evthread->notify_receive_fd, EV_READ | EV_PERSIST, notify_cb, evthread);
	event_base_set(evthread->base, evthread->notify_event);
	if (event_add(evthread->notify_event, 0) == -1) {
		fprintf(stderr, "Can't monitor event notify pipe\n");
		goto error;
	}

	TAILQ_INIT(&evthread->sockets);
	EVTHREAD_ALLOC_LOCK(evthread->lock, EVTHREAD_LOCKTYPE_READWRITE);

	return true;

error:
	evhttp_thread_cleanup(evthread);

	return false;
}

static void *
http_worker(void *arg)
{
	struct evhttp_thread *evthread = arg;

	event_base_dispatch(evthread->base);

	evthread->thread_id = 0;

	return NULL;
}

struct evhttp_thread_pool *
evhttp_thread_pool_new(const struct event_config *cfg, int nthreads)
{
	int i;
	struct evhttp_thread_pool *evpool = NULL;

	if (nthreads <= 0 || nthreads > MAX_THREAD_COUNT)
		nthreads = MAX_THREAD_COUNT;

	if ((evpool = mm_calloc(1, sizeof(struct evhttp_thread_pool))) == NULL) {
		event_warn("%s: calloc failed", __func__);
		goto error;
	}

	memset(evpool, 0, sizeof(struct evhttp_thread_pool));

	evpool->thread_idx = 0;
	evpool->thread_cnt = nthreads;
	evpool->threads = mm_calloc(nthreads, sizeof(struct evhttp_thread));
	if (!evpool->threads) {
		fprintf(stderr, "Can't allocate thread descriptors\n");
		goto error;
	}

	for (i = 0; i < nthreads; i++) {
		memset(&evpool->threads[i], 0, sizeof(struct evhttp_thread));
		evpool->threads[i].notify_receive_fd = EVUTIL_INVALID_SOCKET;
		evpool->threads[i].notify_send_fd = EVUTIL_INVALID_SOCKET;
		evpool->threads[i].pool = evpool;
	}

	for (i = 0; i < nthreads; i++) {
		if (!evhttp_thread_setup(&evpool->threads[i], cfg))
			goto error;
	}

	/* Create threads after we've done all the libevent setup. */
	for (i = 0; i < nthreads; i++) {
		evpool->threads[i].thread_id =
			sys_os_create_thread(http_worker, &evpool->threads[i]);
		if (evpool->threads[i].thread_id == 0)
			goto error;
	}

	TAILQ_INIT(&evpool->socket_pers);
	TAILQ_INIT(&evpool->sockets);
	EVTHREAD_ALLOC_LOCK(evpool->lock, EVTHREAD_LOCKTYPE_READWRITE);

	return evpool;

error:
	if (evpool)
		evhttp_thread_pool_free(evpool);

	return NULL;
}

void
evhttp_thread_pool_free(struct evhttp_thread_pool *evpool)
{
	struct evhttp_socket_per *evsocket_per;
	int i;

	if (NULL == evpool)
		return;

	if (evpool->threads) {
		for (i = 0; i < evpool->thread_cnt; i++) {
			evhttp_thread_loopbreak(evpool->threads + i);
		}
		for (i = 0; i < evpool->thread_cnt; i++) {
			evhttp_thread_cleanup(evpool->threads + i);
		}
		mm_free(evpool->threads);
	}

	while ((evsocket_per = TAILQ_FIRST(&evpool->socket_pers)) != NULL) {
		TAILQ_REMOVE(&evpool->socket_pers, evsocket_per, next);
		mm_free(evsocket_per->socket);
		mm_free(evsocket_per);
	}

	if (evpool->lock) {
		EVTHREAD_FREE_LOCK(evpool->lock, EVTHREAD_LOCKTYPE_READWRITE);
		evpool->lock = NULL;
	}

	mm_free(evpool);
}

static void
evhttp_thread_pool_assign(struct evhttp_thread_pool *evpool,
	evutil_socket_t nfd, struct sockaddr *addr, int addrlen)
{
	char buf[1];
	struct evhttp_socket *evsocket = NULL;
	struct evhttp_thread *evthread = NULL;

	if (!evpool)
		goto error;

	evsocket = evhttp_socket_new(evpool);
	if (evsocket == NULL) {
		goto error;
	}

	evsocket->nfd = nfd;
	evsocket->addrlen = addrlen;
	memcpy(&evsocket->addr, addr, addrlen);

	evthread = evpool->threads + (evpool->thread_idx % evpool->thread_cnt);
	evpool->thread_idx++;

	evhttp_socket_push(evthread, evsocket);

	buf[0] = 'c';
	if (send(evthread->notify_send_fd, buf, 1, 0) != 1) {
		perror("Writing to thread notify pipe");
		goto error;
	}

	return;

error:
	evutil_closesocket(nfd);

	if (evsocket)
		evhttp_socket_free(evpool, evsocket);
}

void 
evhttp_thread_pool_enable_bound_socket(struct evhttp_thread_pool *evpool, struct evhttp_bound_socket *bound)
{
	struct evconnlistener *lev;

	if (evpool == NULL)
		return;

	if (bound == NULL)
		return;

	lev = evhttp_bound_socket_get_listener(bound);
	if (!lev) {
		perror("listener is null");
		return;
	}

	evconnlistener_set_cb(lev, accept_socket_cb, evpool);
}

int
evhttp_thread_pool_get_connection_count(struct evhttp_thread_pool *evpool)
{
	int i, cnt = 0;

	if (evpool == NULL)
		return cnt;

	for (i = 0; i < evpool->thread_cnt; i++) {
		cnt += evhttp_get_connection_count(evpool->threads[i].http);
	}

	return cnt;
}

int
evhttp_thread_pool_get_thread_count(struct evhttp_thread_pool *evpool)
{
	if (evpool == NULL)
		return 0;

	return evpool->thread_cnt;
}

struct evhttp_thread *
evhttp_thread_pool_get_thread(struct evhttp_thread_pool *evpool, int index)
{
	if (evpool == NULL)
		return NULL;

	if (index < 0 || index >= evpool->thread_cnt)
		return NULL;

	return (evpool->threads + index);
}

struct evhttp *evhttp_thread_get_http(struct evhttp_thread *evthread)
{
	return evthread ? evthread->http : NULL;
}