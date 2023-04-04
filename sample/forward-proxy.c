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
#include <signal.h>

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
#include <event2/easy_thread.h>

#include "evthread-internal.h"
#include "util-internal.h"
#include "mm-internal.h"
#include "compat/sys/queue.h"

static struct sockaddr_storage connect_to_addr;
static int connect_to_addrlen;
static int use_print_debug = 0;
static int use_thread_pool = 0;
static struct timeval tv_read = {30, 0};
static struct timeval tv_connect = {5, 0};
static struct event *connect_timer = NULL;

struct bufferevent_connection {
	TAILQ_ENTRY(bufferevent_connection) next;

	struct event_base *base;
	struct bufferevent *bev;
};

struct forward_proxy {
	TAILQ_HEAD(bufferevent_connectionq, bufferevent_connection)
	connections; /* queue of new connections */
	void *lock;

	int connection_cnt;
} proxy;


#define BEV_CONNECT_LOCK() EVLOCK_LOCK(proxy.lock, 0)
#define BEV_CONNECT_UNLOCK() EVLOCK_UNLOCK(proxy.lock, 0)

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
bev_print(struct bufferevent *bev, const char *TAG)
{
	struct sockaddr_storage ss;
	evutil_socket_t fd = bufferevent_getfd(bev);
	ev_socklen_t socklen = sizeof(ss);
	char addrbuf[128];
	void *inaddr;
	const char *addr;
	uint16_t got_port = -1;

	memset(&ss, 0, sizeof(ss));
	if (getpeername(fd, (struct sockaddr *)&ss, &socklen)) {
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

static void event_cb(struct bufferevent *bev, short events, void *arg);
static void read_cb(struct bufferevent *bev, void *arg);

static struct bufferevent_connection *
add_bufferevent_connection(struct event_base *base, evutil_socket_t fd)
{
	struct bufferevent_connection *bev_conn = NULL;

	bev_conn = mm_calloc(1, sizeof(struct bufferevent_connection));
	if (!bev_conn) {
		event_warn("%s: calloc failed", __func__);
		goto err;
	}

	// 如果在不同的线程中使用bev， 需要加入 BEV_OPT_DEFER_CALLBACKS |BEV_OPT_UNLOCK_CALLBACKS，
	// 因为在不同的线程中回调函数会unlock后调用, 才不会在另一个线程中和自己的锁冲突，造成互锁
	bev_conn->base = base;
	bev_conn->bev = bufferevent_socket_new(base, fd,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS |
			BEV_OPT_UNLOCK_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev_conn->bev) {
		event_warn("%s: bufferevent socket new failed", __func__);
		goto err;
	}

	bufferevent_setcb(bev_conn->bev, NULL, NULL, event_cb, NULL);
	bufferevent_enable(bev_conn->bev, EV_WRITE);

	BEV_CONNECT_LOCK();
	TAILQ_INSERT_TAIL(&proxy.connections, bev_conn, next);
	proxy.connection_cnt++;
	BEV_CONNECT_UNLOCK();

	if (use_print_debug) {
		bev_print(bev_conn->bev, "connect");
		fprintf(stdout, "client count: %d\n", proxy.connection_cnt);
	}

	return bev_conn;

err:
	if (bev_conn)
		mm_free(bev_conn);

	evutil_closesocket(fd);

	return NULL;
}

static struct bufferevent *
create_bufferevent_socket(struct event_base *base)
{
	struct bufferevent *bev = NULL;

	bev = bufferevent_socket_new(base, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev) {
		fprintf(stderr, "Couldn't open listener.\n");
		goto err;
	}
	if (bufferevent_socket_connect(
			bev, (struct sockaddr *)&connect_to_addr, connect_to_addrlen) < 0) {
		perror("bufferevent socket connect");
		goto err;
	}

	bufferevent_setcb(bev, read_cb, NULL, event_cb, bev);
	bufferevent_set_timeouts(bev, &tv_read, NULL);
	bufferevent_enable(bev, EV_READ);

	return bev;

err:
	if (bev)
		bufferevent_free(bev);

	return NULL;
}

static void
read_cb(struct bufferevent *bev, void *arg)
{
	struct bufferevent_connection *bev_conn;
	struct evbuffer *src;//, *dst, *tmp;
	size_t len;
	char buff[1024];

	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);

#if 1

	while ((len = evbuffer_remove(src, buff, 1024)) > 0) {
		BEV_CONNECT_LOCK();
		bev_print(bev, "start read");
		TAILQ_FOREACH (bev_conn, &proxy.connections, next) {
			bufferevent_write(bev_conn->bev, buff, len);
		}
		bev_print(bev, "end read");
		BEV_CONNECT_UNLOCK();
	}

#else

	tmp = evbuffer_new();
	if (tmp) {
		evbuffer_add_buffer(tmp, src);

		BEV_CONNECT_LOCK();
		TAILQ_FOREACH (bev_conn, &proxy.connections, next) {
			dst = bufferevent_get_output(bev_conn->bev);
			evbuffer_add_buffer_reference(dst, tmp);
		}
		BEV_CONNECT_UNLOCK();

		evbuffer_free(tmp);
	} else {
		evbuffer_drain(src, len);
	}
#endif
}

static void
write_cb(struct bufferevent *bev, void *arg)
{
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0) {
		printf("flushed answer\n");
	}
}

static void
event_cb(struct bufferevent *bev, short events, void *arg)
{
	struct bufferevent *b_out = arg;
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


	if (bev == b_out) {
		// delay to connect
		if (connect_timer)
			evtimer_add(connect_timer, &tv_connect);
	} else {
		// remove client from queue
		BEV_CONNECT_LOCK();
		bev_print(bev, "start close");
		TAILQ_FOREACH (bev_conn, &proxy.connections, next) {
			if (bev_conn->bev == bev) {
				TAILQ_REMOVE(&proxy.connections, bev_conn, next);
				mm_free(bev_conn);
				proxy.connection_cnt--;
				break;
			}
		}
		bev_print(bev, "end close");
		BEV_CONNECT_UNLOCK();

		if (use_print_debug)
			bev_print(bev, "disconnect");

		if (use_print_debug)
			fprintf(stdout, "client count: %d\n", proxy.connection_cnt);
	}

	bufferevent_free(bev);
}

static void
time_cb(evutil_socket_t fd, short event, void *arg)
{
	struct event_base *base = arg;

	if (!create_bufferevent_socket(base)) {
		// delay to connect
		if (connect_timer)
			evtimer_add(connect_timer, &tv_connect);
	}
}

static void
accept_socket_cb(struct evconnlistener *listener, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg)
{
	if (use_thread_pool) {
		struct eveasy_thread_pool *pool = arg;
		eveasy_thread_pool_assign(pool, fd, sa, socklen);
	} else {
		struct event_base *base = arg;
		add_bufferevent_connection(base, fd);
	}
}

static void
easyconn_cb(struct eveasy_thread *evthread, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg)
{
	struct event_base *base = eveasy_thread_get_base(evthread);
	add_bufferevent_connection(base, fd);
}

static void
signal_cb(evutil_socket_t sig, short events, void *arg)
{
	struct event_base *base = arg;
	struct timeval delay = {2, 0};

	printf("Caught an interrupt signal; exiting cleanly in two seconds.\n");

	event_base_loopexit(base, &delay);
}

static void
syntax(void)
{
	fputs("Syntax:\n", stderr);
	fputs("   forward-proxy [-t] [-p] <listen-on-addr> <connect-to-addr>\n",
		stderr);
	fputs("Example:\n", stderr);
	fputs("   forward-proxy 127.0.0.1:8888 1.2.3.4:80\n", stderr);

	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	struct event_config *cfg = NULL;
	struct event_base *base = NULL;
	struct bufferevent *bev = NULL;
	struct eveasy_thread_pool *pool = NULL;
	struct evconnlistener *listener = NULL;
	struct event *signal_event = NULL;
	struct sockaddr_storage listen_on_addr;
	int socklen, i;
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

	cfg = event_config_new();
	if (!cfg) {
		fprintf(stderr, "Couldn't create an config: exiting\n");
		goto err;
	}

#ifdef _WIN32
#ifdef EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
	evthread_use_windows_threads();
#endif
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	event_config_set_num_cpus_hint(cfg, si.dwNumberOfProcessors);
	//event_config_set_flag(
	//	cfg, EVENT_BASE_FLAG_STARTUP_IOCP | EVENT_BASE_FLAG_INHERIT_IOCP);
#else
#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
	evthread_use_pthreads();
#endif
#endif

	init_proxy();

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

		eveasy_thread_pool_set_conncb(pool, easyconn_cb, NULL);
	}

	event_config_free(cfg);
	cfg = NULL;

	bev = create_bufferevent_socket(base);
	if (!bev) {
		goto err;
	}

	connect_timer = evtimer_new(base, time_cb, base);

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

	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);
	if (!signal_event || event_add(signal_event, NULL) < 0) {
		fprintf(stderr, "Could not create/add a signal event!\n");
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
	if (connect_timer)
		evtimer_del(connect_timer);
	if (signal_event)
		event_free(signal_event);
	if (base)
		event_base_free(base);

#ifdef _WIN32
	WSACleanup();
#endif

	printf("done\n");

	return ret;
}
