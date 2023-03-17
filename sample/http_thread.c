#include "http_thread.h"

#include <event2/event.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/event_compat.h>

#include "../evthread-internal.h"
#include "../mm-internal.h"
#include "../time-internal.h"
#include "../http-internal.h"
#include "../compat/sys/queue.h"

#include <stdbool.h>

#define MAX_THREAD_COUNT 128

/* each bound socket is stored in one of these */
struct evhttp_socket {
	TAILQ_ENTRY(evhttp_socket) next;

	evutil_socket_t nfd;
	struct sockaddr addr;
	int addrlen;
};

struct evhttp_thread {
	int thread_id; /* unique ID of this thread */
	struct evhttp *http;
	struct event_base *base;		   /* libevent handle this thread uses */
	struct event *notify_event;		   /* listen event for notify pipe */
	evutil_socket_t notify_receive_fd; /* receiving end of notify pipe */
	evutil_socket_t notify_send_fd;	/* sending end of notify pipe */

	TAILQ_HEAD(evhttp_socketq, evhttp_socket)
	sockets; /* queue of new connections to handle */
	void *lock;
};

struct evhttp_thread_pool {
	struct evhttp *http;
	int thread_idx;
	int thread_cnt;
	struct evhttp_thread *threads;
};

#define EVHTTP_THREAD_LOCK(b)              \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->lock, 0); \
	} while (0)

#define EVHTTP_THREAD_UNLOCK(b)              \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->lock, 0); \
	} while (0)


unsigned long
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

static void
thread_process(evutil_socket_t fd, short which, void *arg)
{
	struct evhttp_thread *evthread = arg;

	char buf[1];

	if (recv(fd, buf, 1, 0) != 1) {
		fprintf(stderr, "Can't read from libevent pipe\n");
		return;
	}

	switch (buf[0]) {
	case 'c': {
		struct evhttp *http = evthread->http;
		struct event_base *base = evthread->base;
		struct evhttp_socket *evsoc;

		EVHTTP_THREAD_LOCK(evthread);

		while ((evsoc = TAILQ_FIRST(&evthread->sockets)) != NULL) {
			
			evhttp_get_request(http, evsoc->nfd, &evsoc->addr, evsoc->addrlen, NULL);

			TAILQ_REMOVE(&evthread->sockets, evsoc, next);

			mm_free(evsoc);
		}
		
		EVHTTP_THREAD_UNLOCK(evthread);

	} break;
	default:
		break;
	}
}

static void
thread_cleanup(struct evhttp_thread *evthread)
{
	struct evhttp_socket *evsoc;
	struct timeval msec10 = {0, 10 * 1000};

	if (NULL == evthread)
		return;

	if (evthread->base)
		event_base_loopbreak(evthread->base);

	while (evthread->thread_id != 0)
		evutil_usleep_(&msec10);

	if (evthread->notify_receive_fd > 0) {
		evutil_closesocket(evthread->notify_receive_fd);
		evthread->notify_receive_fd = 0;
	}

	if (evthread->notify_send_fd > 0) {
		evutil_closesocket(evthread->notify_send_fd);
		evthread->notify_send_fd = 0;
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

	//EVHTTP_THREAD_LOCK(evthread);
	while ((evsoc = TAILQ_FIRST(&evthread->sockets)) != NULL) {
		TAILQ_REMOVE(&evthread->sockets, evsoc, next);
		mm_free(evsoc);
	}
	//EVHTTP_THREAD_UNLOCK(evthread);

	if (evthread->lock) {
		EVTHREAD_FREE_LOCK(evthread->lock, EVTHREAD_LOCKTYPE_READWRITE);
		evthread->lock = NULL;
	}
}

static 
bool thread_setup(struct evhttp_thread *evthread, struct evhttp *http)
{
	struct evhttp_cb *http_cb;
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

	evthread->base = event_init();
	if (!evthread->base) {
		fprintf(stderr, "Can't allocate event base\n");
		goto error;
	}

	evthread->http = evhttp_new(evthread->base);
	if (!evthread->http) {
		fprintf(stderr, "Can't allocate event http\n");
		goto error;
	}

	TAILQ_FOREACH (http_cb, &http->callbacks, next) {
		evhttp_set_cb(
			evthread->http, http_cb->what, http_cb->cb, http_cb->cbarg);
	}

	evhttp_set_gencb(evthread->http, http->gencb, http->gencbarg);

	evhttp_set_max_body_size(evthread->http, http->default_max_body_size);

	/* Listen for notifications from other threads */
	evthread->notify_event =
		event_new(evthread->base, evthread->notify_receive_fd,
			EV_READ | EV_PERSIST, thread_process, evthread);
	event_base_set(evthread->base, evthread->notify_event);
	if (event_add(evthread->notify_event, 0) == -1) {
		fprintf(stderr, "Can't monitor libevent notify pipe\n");
		goto error;
	}

	TAILQ_INIT(&evthread->sockets);
	EVTHREAD_ALLOC_LOCK(evthread->lock, EVTHREAD_LOCKTYPE_READWRITE);
	
	return true;

error:
	thread_cleanup(evthread);

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
evhttp_thread_pool_new(struct evhttp *http, int nthreads)
{
	int i;
	struct evhttp_thread_pool *evpool = NULL;

	if ((evpool = mm_calloc(1, sizeof(struct evhttp_thread_pool))) == NULL) {
		event_warn("%s: calloc failed", __func__);
		goto error;
	}

	if (nthreads <= 0 || nthreads > MAX_THREAD_COUNT)
		nthreads = MAX_THREAD_COUNT;

	evpool->http = http;
	evpool->thread_idx = 0;
	evpool->thread_cnt = nthreads;
	evpool->threads = mm_calloc(nthreads, sizeof(struct evhttp_thread));
	if (!evpool->threads) {
		fprintf(stderr, "Can't allocate thread descriptors\n");
		goto error;
	}

	for (i = 0; i < nthreads; i++) {
		memset(&evpool->threads[i], 0, sizeof(struct evhttp_thread));
	}

	for (i = 0; i < nthreads; i++) {
		if (!thread_setup(&evpool->threads[i], http))
			goto error;
	}

	/* Create threads after we've done all the libevent setup. */
	for (i = 0; i < nthreads; i++) {
		evpool->threads[i].thread_id =
			sys_os_create_thread(http_worker, &evpool->threads[i]);
		if (evpool->threads[i].thread_id == 0)
			goto error;
	}

	return evpool;

error:
	evhttp_thread_pool_free(evpool);

	return NULL;
}

void
evhttp_thread_pool_free(struct evhttp_thread_pool *evpool)
{
	int i;

	if (NULL == evpool)
		return;

	if (evpool->threads) {
		for (i = 0; i < evpool->thread_cnt; i++) {
			thread_cleanup(evpool->threads + i);
		}
		mm_free(evpool->threads);
	}

	mm_free(evpool);
}

void
evhttp_thread_dispatch_socket(struct evhttp_thread_pool *evpool, 
	evutil_socket_t nfd, struct sockaddr *addr, int addrlen)
{
	char buf[1];
	struct evhttp_socket *evsoc = NULL;
	struct evhttp_thread *evthread = NULL;

	if ((evsoc = mm_calloc(1, sizeof(struct evhttp_socket))) == NULL) {
		event_warn("%s: calloc failed", __func__);
		goto error;
	}

	evsoc->nfd = nfd;
	evsoc->addrlen = addrlen;
	memcpy(&evsoc->addr, addr, addrlen);

	evthread = evpool->threads + (evpool->thread_idx % evpool->thread_cnt);
	evpool->thread_idx++;

	EVHTTP_THREAD_LOCK(evthread);
	TAILQ_INSERT_TAIL(&evthread->sockets, evsoc, next);
	EVHTTP_THREAD_UNLOCK(evthread);

	buf[0] = 'c';
	if (send(evthread->notify_send_fd, buf, 1, 0) != 1) {
		perror("Writing to thread notify pipe");
		goto error;
	}

	return;

error:
	evutil_closesocket(nfd);

	if (evsoc)
		mm_free(evsoc);
}


int
evhttp_thread_get_connection_count(struct evhttp_thread_pool *evpool)
{
	int i, cnt = 0;

	if (evpool == NULL)
		return cnt;

	for (i = 0; i < evpool->thread_cnt; i++) {
		cnt += evhttp_get_connection_count(evpool->threads[i].http);
	}

	return cnt;
}
