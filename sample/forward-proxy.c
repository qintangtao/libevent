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

static int use_print_debug = 0;
static int use_thread_pool = 0;

struct bufferevent_connection {
	TAILQ_ENTRY(bufferevent_connection) next;

	struct event_base *base;
	struct bufferevent *bev;
};

struct forward_proxy {
	TAILQ_HEAD(bufferevent_connectionq, bufferevent_connection) connections; /* queue of new connections */
	void *lock;

	int connection_cnt ;
} proxy;


#define BEV_CONNECT_LOCK()                   \
	do {                                     \
		if (proxy.lock)                 \
			EVLOCK_LOCK(proxy.lock, 0); \
	} while (0)

#define BEV_CONNECT_UNLOCK()                   \
	do {                                       \
		if (proxy.lock)						\
			EVLOCK_UNLOCK(proxy.lock, 0);	\
	} while (0)

static void 
init_proxy()
{
	proxy.connection_cnt = 0;
	proxy.lock = NULL;
	TAILQ_INIT(&proxy.connections);
	if (use_thread_pool)
		EVTHREAD_ALLOC_LOCK(proxy.lock, EVTHREAD_LOCKTYPE_READWRITE);
}

static void
conn_print(struct bufferevent *bev, const char *TAG)
{
	struct sockaddr_storage ss;
	evutil_socket_t fd = bufferevent_getfd(bev);
	ev_socklen_t socklen = sizeof(ss);
	char addrbuf[128];
	void *inaddr;
	const char *addr;
	uint16_t got_port = -1;

	memset(&ss, 0, sizeof(ss));
	if (getsockname(fd, (struct sockaddr *)&ss, &socklen)) {
		perror("getsockname() failed");
		return;
	}

	if (ss.ss_family == AF_INET) {
		got_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
		inaddr = &((struct sockaddr_in *)&ss)->sin_addr;
	} else if (ss.ss_family == AF_INET6) {
		got_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
		inaddr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
	}

	addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf, sizeof(addrbuf));
	if (addr) {
		printf("%s on %s:%d\n", TAG, addr, got_port);
	} else {
		fprintf(stderr, "evutil_inet_ntop failed\n");
	}
}

static void
readcb(struct bufferevent *bev, void *ctx)
{
	struct bufferevent_connection *bev_conn;
	struct evbuffer *src, *dst;
	size_t len;

	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);

	BEV_CONNECT_LOCK();
	TAILQ_FOREACH (bev_conn, &proxy.connections, next) {
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
		if (use_print_debug)
			fprintf(stdout, "Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		if (use_print_debug)
			fprintf(stdout, "Got an error on the connection: %s\n",
			strerror(errno)); /*XXX win32*/
	} else if (events & BEV_EVENT_TIMEOUT) {
		if (use_print_debug)
			fprintf(stdout, "Connection timeout.\n");
	} else {
		return;
	}

	if (use_print_debug)
		conn_print(bev, "disconnect");

	BEV_CONNECT_LOCK();
	if (bev == b_out) {
		// close all clients;
		while ((bev_conn = TAILQ_FIRST(&proxy.connections)) != NULL) {
			bufferevent_free(bev_conn->bev);
			TAILQ_REMOVE(&proxy.connections, bev_conn, next);
			mm_free(bev_conn);
			proxy.connection_cnt--;
		}
	} else {
		// remove client from queue
		TAILQ_FOREACH (bev_conn, &proxy.connections, next) {
			if (bev_conn->bev == bev) {
				// bufferevent_free(bev_conn->bev);
				TAILQ_REMOVE(&proxy.connections, bev_conn, next);
				mm_free(bev_conn);
				proxy.connection_cnt--;
				break;
			}
		}
	}
	BEV_CONNECT_UNLOCK();

	if (use_print_debug)
		fprintf(stdout, "client count: %d\n", proxy.connection_cnt);

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
	TAILQ_INSERT_TAIL(&proxy.connections, bev_conn, next);
	proxy.connection_cnt++;
	BEV_CONNECT_UNLOCK();

	if (use_print_debug) {
		conn_print(bev_conn->bev, "connect");
		fprintf(stdout, "client count: %d\n", proxy.connection_cnt);
	}

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
	fputs("   forward-proxy [-t] [-p] <listen-on-addr> <connect-to-addr>\n", stderr);
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
		} else if (!strcmp(argv[i], "-p")) {
			use_print_debug = 1;
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

	init_proxy();

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
