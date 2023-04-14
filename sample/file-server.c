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

#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <getopt.h>
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

#ifdef _WIN32
#ifndef stat
#define stat _stati64
#endif
#ifndef fstat
#define fstat _fstati64
#endif
#ifndef open
#define open _open
#endif
#ifndef close
#define close _close
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#endif /* _WIN32 */

static char *send_filename;
static int use_print_debug = 0;
static int use_thread_pool = 0;
static int use_delay_send = 0;
static int single_send_length = 1024;
static struct timeval tv_send = {1, 0};
static struct event *send_timer = NULL;

struct bufferevent_connection {
	TAILQ_ENTRY(bufferevent_connection) next;

	struct event_base *base;
	struct bufferevent *bev;
};

struct file_server {
	TAILQ_HEAD(bufferevent_connectionq, bufferevent_connection)
	connections; /* queue of new connections */
	void *lock;

	int connection_cnt;

	ev_off_t offset;
	ev_off_t length;
} server;


#define BEV_CONNECT_LOCK() EVLOCK_LOCK(server.lock, 0)
#define BEV_CONNECT_UNLOCK() EVLOCK_UNLOCK(server.lock, 0)

static void
init_server()
{
	server.connection_cnt = 0;
	server.lock = NULL;
	TAILQ_INIT(&server.connections);
	if (use_thread_pool)
		EVTHREAD_ALLOC_LOCK(server.lock, EVTHREAD_LOCKTYPE_READWRITE);
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
static void write_cb(struct bufferevent *bev, void *arg);

static ev_off_t
get_file_length(const char *filename)
{
	int fd = -1;
	struct stat st;
	st.st_size = -1;

	if ((fd = open(filename, O_RDONLY)) < 0) {
		perror("open");
		goto err;
	}

	if (fstat(fd, &st) < 0) {
		/* Make sure the length still matches, now that we
		 * opened the file :/ */
		perror("fstat");
		goto err;
	}

err:
	if (fd >= 0)
		close(fd);

	return st.st_size;
}

static void
send_file(struct bufferevent *bev, const char *filename, ev_off_t offset,
	ev_off_t length)
{
	int fd = -1;
	//struct stat st;

	if ((fd = open(filename, O_RDONLY)) < 0) {
		perror("open");
		goto err;
	}

#if 0
	if (-1 == length) {
		if (fstat(fd, &st) < 0) {
			/* Make sure the length still matches, now that we
			 * opened the file :/ */
			perror("fstat");
			goto err;
		}
		length = st.st_size;
	}
#endif

	evbuffer_add_file(bufferevent_get_output(bev), fd, offset, length);

	return;

err:
	if (fd >= 0)
		close(fd);
}

static struct bufferevent_connection *
add_bufferevent_connection(struct event_base *base, evutil_socket_t fd)
{
	struct bufferevent_connection *bev_conn = NULL;

	bev_conn = mm_calloc(1, sizeof(struct bufferevent_connection));
	if (!bev_conn) {
		event_warn("%s: calloc failed", __func__);
		goto err;
	}

	// 如果在不同的线程中使用bev， 需要加入 BEV_OPT_DEFER_CALLBACKS
	// |BEV_OPT_UNLOCK_CALLBACKS， 因为在不同的线程中回调函数会unlock后调用,
	// 才不会在另一个线程中和自己的锁冲突，造成互锁
	bev_conn->base = base;
	bev_conn->bev = bufferevent_socket_new(base, fd,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS |
			BEV_OPT_UNLOCK_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev_conn->bev) {
		event_warn("%s: bufferevent socket new failed", __func__);
		goto err;
	}

	bufferevent_setcb(bev_conn->bev, NULL, write_cb, event_cb, NULL);
	bufferevent_enable(bev_conn->bev, EV_WRITE);

	BEV_CONNECT_LOCK();
	TAILQ_INSERT_TAIL(&server.connections, bev_conn, next);
	server.connection_cnt++;
	BEV_CONNECT_UNLOCK();

	if (use_print_debug) {
		bev_print(bev_conn->bev, "connect");
		fprintf(stdout, "client count: %d\n", server.connection_cnt);
	}

	if (!use_delay_send)
		send_file(bev_conn->bev, send_filename, 0, -1);

	return bev_conn;

err:
	if (bev_conn)
		mm_free(bev_conn);

	evutil_closesocket(fd);

	return NULL;
}

static void
read_cb(struct bufferevent *bev, void *arg)
{
	
}

static void
write_cb(struct bufferevent *bev, void *arg)
{
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0) {
		if (use_print_debug)
			bev_print(bev, "flushed");

		if (!use_delay_send)
			send_file(bev, send_filename, 0, -1);
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

	// remove client from queue
	BEV_CONNECT_LOCK();
	TAILQ_FOREACH (bev_conn, &server.connections, next) {
		if (bev_conn->bev == bev) {
			TAILQ_REMOVE(&server.connections, bev_conn, next);
			mm_free(bev_conn);
			server.connection_cnt--;
			break;
		}
	}
	BEV_CONNECT_UNLOCK();

	if (use_print_debug)
		bev_print(bev, "disconnect");

	if (use_print_debug)
		fprintf(stdout, "client count: %d\n", server.connection_cnt);

	bufferevent_free(bev);
}

static void
time_cb(evutil_socket_t fd, short event, void *arg)
{
	struct event_base *base = arg;
	struct bufferevent_connection *bev_conn;
	ev_off_t length = single_send_length;

	if (!TAILQ_EMPTY(&server.connections))
	{
		if (server.offset + length > server.length)
			length = server.length - server.offset;

		BEV_CONNECT_LOCK();
		TAILQ_FOREACH (bev_conn, &server.connections, next) {
			send_file(bev_conn->bev, send_filename, server.offset, length);
		}
		BEV_CONNECT_UNLOCK();

		server.offset += length;
		if (server.offset >= server.length)
			server.offset = 0;
	}

	if (send_timer)
		evtimer_add(send_timer, &tv_send);
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
print_usage(FILE *out, const char *prog, int exit_code)
{
	fprintf(out,
		"Syntax: %s [ OPTS ] <port> <file>\n"
		" -t      - user thread pool\n"
		" -d      - delay send\n"
		" -l      - single send length\n"
		" -T      - delay millisecond send\n"
		" -v      - verbosity, enables libevent debug logging too\n",
		prog);
	exit(exit_code);
}

int
main(int argc, char **argv)
{
	struct event_config *cfg = NULL;
	struct event_base *base = NULL;
	struct eveasy_thread_pool *pool = NULL;
	struct evconnlistener *listener = NULL;
	struct event *signal_event = NULL;
	struct sockaddr_in sin = {0};
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

	int opt;
	while ((opt = getopt(argc, argv, "htvdl:T:")) != -1) {
		switch (opt) {
		case 't':
			use_thread_pool = 1;
			break;
		case 'v':
			use_print_debug = 1;
			break;
		case 'd':
			use_delay_send = 1;
			break;
		case 'l':
			single_send_length = atoi(optarg);
			break;
		case 'T':
		{
			int ms = atoi(optarg);
			tv_send.tv_sec = ms / 1000;
			tv_send.tv_usec = (ms % 1000) * 1000;
		}
			break;
		case 'h':
			print_usage(stdout, argv[0], 0);
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", opt);
			break;
		}
	}

	if (optind >= argc || (argc - optind) < 2) {
		print_usage(stdout, argv[0], 1);
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(atoi(argv[optind++]));

	send_filename = argv[optind++];

	server.offset = 0;
	server.length = get_file_length(send_filename);
	if (server.length == -1) {
		fprintf(stderr, "Couldn't get file length: %s\n", send_filename);
		goto err;
	}

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
	event_config_set_flag(
		cfg, EVENT_BASE_FLAG_STARTUP_IOCP | EVENT_BASE_FLAG_INHERIT_IOCP);
#else
#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
	evthread_use_pthreads();
#endif
#endif

	init_server();

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

	if (use_thread_pool) {
		listener = evconnlistener_new_bind(base, accept_socket_cb, pool,
			LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
			-1, (struct sockaddr *)&sin, sizeof(sin));
	} else {
		listener = evconnlistener_new_bind(base, accept_socket_cb, base,
			LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
			-1, (struct sockaddr *)&sin, sizeof(sin));
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

	send_timer = evtimer_new(base, time_cb, base);
	if (!send_timer) {
		fprintf(stderr, "Could not create/add a send event!\n");
		goto err;
	}

	if (use_delay_send)
		evtimer_add(send_timer, &tv_send);

	event_base_dispatch(base);

	ret = EXIT_SUCCESS;

err:
	if (pool)
		eveasy_thread_pool_free(pool);
	if (cfg)
		event_config_free(cfg);
	if (listener)
		evconnlistener_free(listener);
	if (send_timer)
		evtimer_del(send_timer);
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
