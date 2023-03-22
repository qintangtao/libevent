/*
  This example code shows how to write an (optionally encrypting) SSL proxy
  with Libevent's bufferevent layer.

  XXX It's a little ugly and should probably be cleaned up.
 */

// Get rid of OSX 10.7 and greater deprecation warnings.
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#endif

#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "evthread-internal.h"
#include "util-internal.h"
#include "mm-internal.h"
#include "compat/sys/queue.h"
#include "easy_thread.h"

static int use_thread_pool = 0;

struct bufferevent_connection {
	TAILQ_ENTRY(bufferevent_connection) next;

	struct event_base *base;
	struct bufferevent *bev;
};
TAILQ_HEAD(bufferevent_connectionq, bufferevent_connection)
connections; /* queue of new connections */
void *connection_lock = NULL;

#define BEV_CONNECT_LOCK()                   \
	do {                                     \
		if (connection_lock)                 \
			EVLOCK_LOCK(connection_lock, 0); \
	} while (0)

#define BEV_CONNECT_UNLOCK()                   \
	do {                                       \
		if (connection_lock)                   \
			EVLOCK_UNLOCK(connection_lock, 0); \
	} while (0)

static void
readcb(struct bufferevent *bev, void *ctx)
{
	struct bufferevent_connection *bev_conn;
	struct evbuffer *src, *dst;
	size_t len;

	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);

	BEV_CONNECT_LOCK();
	TAILQ_FOREACH (bev_conn, &connections, next) {
		dst = bufferevent_get_output(bev_conn->bev);
		evbuffer_add_buffer_reference(dst, src);
	}
	BEV_CONNECT_UNLOCK();

	evbuffer_drain(src, len);
}

static void
eventcb(struct bufferevent *bev, short events, void *ctx)
{
	struct bufferevent *b_out = ctx;
	struct bufferevent_connection *bev_conn;

	if (events & BEV_EVENT_EOF) {
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		printf("Got an error on the connection: %s\n",
			strerror(errno)); /*XXX win32*/
	} else if (events & BEV_EVENT_TIMEOUT) {
		printf("Connection timeout.\n");
	} else {
		return;
	}

	BEV_CONNECT_LOCK();
	if (bev == b_out) {
		// close all clients;
		while ((bev_conn = TAILQ_FIRST(&connections)) != NULL) {
			bufferevent_free(bev_conn->bev);
			TAILQ_REMOVE(&connections, bev_conn, next);
			mm_free(bev_conn);
		}
	} else {
		// remove client from queue
		TAILQ_FOREACH (bev_conn, &connections, next) {
			if (bev_conn->bev == bev) {
				// bufferevent_free(bev_conn->bev);
				TAILQ_REMOVE(&connections, bev_conn, next);
				mm_free(bev_conn);
				break;
			}
		}
	}
	BEV_CONNECT_UNLOCK();

	bufferevent_free(bev);
}

static struct bufferevent_connection *
bufferevent_connection_add(struct event_base *base, evutil_socket_t fd)
{
	struct bufferevent_connection *bev_conn = NULL;

	bev_conn = mm_calloc(1, sizeof(struct bufferevent_connection));
	if (!bev_conn) {
		event_warn("%s: calloc failed", __func__);
		goto err;
	}

	bev_conn->base = base;
	bev_conn->bev = bufferevent_socket_new(base, fd,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev_conn->bev) {
		event_warn("%s: bufferevent socket new failed", __func__);
		goto err;
	}

	bufferevent_setcb(bev_conn->bev, NULL, NULL, eventcb, NULL);
	// bufferevent_settimeout(bev_conn->bev, 0, 0);
	bufferevent_enable(bev_conn->bev, EV_READ);

	BEV_CONNECT_LOCK();
	TAILQ_INSERT_TAIL(&connections, bev_conn, next);
	BEV_CONNECT_UNLOCK();

	return bev_conn;

err:
	if (bev_conn)
		mm_free(bev_conn);

	return NULL;
}

static void
accept_socket_cb(struct evconnlistener *listener, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg)
{
	if (use_thread_pool) {
		struct eveasy_thread_pool *pool = arg;
		eveasy_thread_pool_assign(pool, fd, sa, socklen);
	} else	{
		struct event_base *base = arg;

		if (!bufferevent_connection_add(base, fd))
			evutil_closesocket(fd);
	}
}

static void
new_conn_cb(struct eveasy_thread *evthread, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg)
{
	struct eveasy_thread_pool *pool = arg;
	struct event_base *base = eveasy_thread_get_base(evthread);

	if (!bufferevent_connection_add(base, fd))
		evutil_closesocket(fd);
}

static void
syntax(void)
{
	fputs("Syntax:\n", stderr);
	fputs("   forward-proxy [-t] <listen-on-addr> <connect-to-addr>\n", stderr);
	fputs("Example:\n", stderr);
	fputs("   forward-proxy 127.0.0.1:8888 1.2.3.4:80\n", stderr);

	exit(1);
}

int
main(int argc, char **argv)
{
	struct eveasy_thread_pool *pool = NULL;
	struct event_config *cfg = NULL;
	struct event_base *base = NULL;
	struct bufferevent *bev = NULL;
	struct evconnlistener *listener = NULL;
	struct sockaddr_storage listen_on_addr;
	struct sockaddr_storage connect_to_addr;
	int socklen, connect_to_addrlen, i;
	int ret = EXIT_FAILURE;

#ifdef _WIN32
	{
		WORD wVersionRequested;
		WSADATA wsaData;
		wVersionRequested = MAKEWORD(2, 2);
		(void)WSAStartup(wVersionRequested, &wsaData);
	}
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("signal()");
		return 1;
	}
#endif

	if (argc < 3)
		syntax();

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-t")) {
			use_thread_pool = 1;
		} else if (!strcmp(argv[i], "-W")) {
			// use_wrapper = 0;
		} else if (argv[i][0] == '-') {
			syntax();
		} else
			break;
	}

	if (i + 2 != argc)
		syntax();

	memset(&listen_on_addr, 0, sizeof(listen_on_addr));
	socklen = sizeof(listen_on_addr);
	if (evutil_parse_sockaddr_port(
			argv[i], (struct sockaddr *)&listen_on_addr, &socklen) < 0) {
		int p = atoi(argv[i]);
		struct sockaddr_in *sin = (struct sockaddr_in *)&listen_on_addr;
		if (p < 1 || p > 65535)
			syntax();
		sin->sin_port = htons(p);
		sin->sin_addr.s_addr = htonl(0x7f000001);
		sin->sin_family = AF_INET;
		socklen = sizeof(struct sockaddr_in);
	}

	memset(&connect_to_addr, 0, sizeof(connect_to_addr));
	connect_to_addrlen = sizeof(connect_to_addr);
	if (evutil_parse_sockaddr_port(argv[i + 1],
			(struct sockaddr *)&connect_to_addr, &connect_to_addrlen) < 0)
		syntax();

	TAILQ_INIT(&connections);
	if (use_thread_pool)
		EVTHREAD_ALLOC_LOCK(connection_lock, EVTHREAD_LOCKTYPE_READWRITE);

	cfg = event_config_new();
	if (!cfg) {
		fprintf(stderr, "Couldn't create an config: exiting\n");
		goto err;
	}

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
#ifdef EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
	evthread_use_windows_threads();
	event_config_set_num_cpus_hint(cfg, si.dwNumberOfProcessors);
#endif
	event_config_set_flag(cfg, EVENT_BASE_FLAG_STARTUP_IOCP);
#else
#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
	evthread_use_pthreads();
#endif
#endif

	base = event_base_new_with_config(cfg);
	if (!base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		goto err;
	}

	if (use_thread_pool) {
		pool = eveasy_thread_pool_new(cfg, 128);
		if (!pool) {
			fprintf(stderr, "Couldn't create an eveasy_thread_pool: exiting\n");
			goto err;
		}

		eveasy_thread_pool_set_conncb(pool, new_conn_cb, pool);
	}

	event_config_free(cfg);
	cfg = NULL;

	bev = bufferevent_socket_new(base, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev) {
		fprintf(stderr, "Couldn't open listener.\n");
		goto err;
	}
	if (bufferevent_socket_connect(
			bev, (struct sockaddr *)&connect_to_addr, connect_to_addrlen) < 0) {
		perror("bufferevent_socket_connect");
		goto err;
	}

	bufferevent_setcb(bev, readcb, NULL, eventcb, bev);
	bufferevent_enable(bev, EV_READ);

	if (use_thread_pool) {
		listener = evconnlistener_new_bind(base, accept_socket_cb, pool,
			LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
			-1, (struct sockaddr *)&listen_on_addr, socklen);
	} else {
		listener = evconnlistener_new_bind(base, accept_socket_cb, base,
			LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
			-1, (struct sockaddr *)&listen_on_addr, socklen);
	}

	if (!listener) {
		fprintf(stderr, "Couldn't open listener.\n");
		goto err;
	}

	event_base_dispatch(base);

	ret = EXIT_SUCCESS;

err:
	if (bev)
		bufferevent_free(bev);
	if (pool)
		eveasy_thread_pool_free(pool);
	if (cfg)
		event_config_free(cfg);
	if (listener)
		evconnlistener_free(listener);
	if (base)
		event_base_free(base);

#ifdef _WIN32
	WSACleanup();
#endif

	return ret;
}
