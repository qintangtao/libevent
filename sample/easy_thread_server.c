#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#ifndef _WIN32
#include <ws2tcpip.h>
#include <netinet/in.h>
#ifdef _XOPEN_SOURCE_EXTENDED
#include <arpa/inet.h>
#endif
#include <sys/socket.h>
#include<unistd.h>
#endif

#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/easy_thread.h>

#include "evthread-internal.h"


#define CONN_TIMEOUT_READ 30
#define CONN_TIMEOUT_WRITE 0

#pragma pack(push)
#pragma pack(1)
typedef struct _RPC_PACKET {
	char magic[5];
	unsigned char major_version;
	unsigned char minor_version;
	unsigned char major_cmd;
	unsigned char minor_cmd;
	unsigned char state;
	unsigned int length;
} RPC_PACKET;
#pragma pack(pop)
#define RPC_MAGIC "BRRCP"
#define RPC_MAJOR_VERSION 0x01
#define RPC_MINOR_VERSION 0x01

static int use_thread_pool = 1;
static uint64_t print_index = 0;

const char *send_msg = "asdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasfasdfasdfasdfasf";

#define LOOP_SEND

static void
hexdump(const unsigned char *ptr, int len)
{
	int i;
	for (i = 0; i < len; ++i)
		printf("%02x ", ptr[i]);
	printf("\n");
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
		printf("[%lld] %s on %s:%d threadid:%d\n", print_index++, TAG, addr,
			got_port,
			EVTHREAD_GET_ID());
	} else {
		fprintf(stderr, "evutil_inet_ntop failed\n");
	}
}

static void
read_cb(struct bufferevent *bev, void *user_data)
{
#if 1
	struct evbuffer *input = bufferevent_get_input(bev);
	struct evbuffer *ouput = bufferevent_get_output(bev);

	evbuffer_add_buffer(ouput, input);

	bev_print(bev, "Reading");

#else
	RPC_PACKET *packet;
	uint8_t data[1024];
	ev_ssize_t total;
	ev_ssize_t size;
	struct evbuffer *body = evbuffer_new();
	struct evbuffer *input = bufferevent_get_input(bev);

	conn_print(bev, "Reading");

	while ((total = evbuffer_get_length(input)) >= sizeof(RPC_PACKET)) {

		// copy packet header
		size = evbuffer_copyout(input, data, sizeof(RPC_PACKET));
		if (size < sizeof(RPC_PACKET))
			break;

		packet = (RPC_PACKET *)data;

		if (memcmp(packet->magic, RPC_MAGIC, sizeof(packet->magic)) != 0) {
			evbuffer_drain(input, 1);
#if 0
			fprintf(stderr, "check magic: ");
			hexdump(packet->magic, sizeof(packet->magic));
#endif
			continue;
		}

		if (packet->major_version != RPC_MAJOR_VERSION ||
			packet->minor_version != RPC_MINOR_VERSION) {
			fprintf(stderr, "check version: %d-%d\n", packet->major_version,
				packet->minor_version);
			evbuffer_drain(input, sizeof(RPC_PACKET));
			continue;
		}

		// 不是一个完整的包
		if (total < (ev_ssize_t)(packet->length + sizeof(RPC_PACKET))) {
			break;
		}

		// read packet header
		size = evbuffer_remove(input, data, sizeof(RPC_PACKET));
		if (size > 0) {

			printf("major_version:%d, minor_version:%d, major_cmd:%d, "
				   "minor_cmd:%d, length:%d\n",
				packet->major_version, packet->minor_version, packet->major_cmd,
				packet->minor_cmd, packet->length);

			// read packet body
			if (packet->length > 0) {
				size = evbuffer_remove_buffer(input, body, packet->length);
				if (size > 0) {
				}
			}

			packet->state = 0;
			packet->length = 0;
			bufferevent_write(bev, (const void *)packet, sizeof(RPC_PACKET));

			// clear packet body
			size = evbuffer_get_length(body);
			if (size > 0)
				evbuffer_drain(body, size);
		}
	}

	evbuffer_free(body);
#endif
}

static void
write_cb(struct bufferevent *bev, void *user_data)
{
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0) {
		printf("flushed answer\n");
#ifdef LOOP_SEND
		bufferevent_write(bev, send_msg, strlen(send_msg));
#endif
	}
}

static void
event_cb(struct bufferevent *bev, short events, void *user_data)
{
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

	bev_print(bev, "Close");

	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
}

static struct bufferevent *
create_bufferevent_socket(struct event_base *base, evutil_socket_t fd)
{
	struct bufferevent *bev;

	bev = bufferevent_socket_new(base, fd,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev) {
		fprintf(stderr, "Couldn't new bufferevent socket.\n");
		goto err;
	}

	bufferevent_setcb(bev, read_cb, write_cb, event_cb, NULL);	
	bufferevent_settimeout(bev, CONN_TIMEOUT_READ, CONN_TIMEOUT_WRITE);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	bev_print(bev, "Connected");

#ifdef LOOP_SEND
	bufferevent_settimeout(bev, 0, 0);
	bufferevent_write(bev, send_msg, strlen(send_msg));
#endif

	return bev;

err:
	evutil_closesocket(fd);

	return NULL;
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
		create_bufferevent_socket(base, fd);
	}
}

static void
easyconn_cb(struct eveasy_thread *evthread, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg)
{
	struct event_base *base = eveasy_thread_get_base(evthread);

	create_bufferevent_socket(base, fd);
}

static void
signal_cb(evutil_socket_t sig, short events, void *arg)
{
	struct event_base *base = arg;
	struct timeval delay = {2, 0};

	printf("Caught an interrupt signal; exiting cleanly in two seconds.\n");

	event_base_loopexit(base, &delay);
}

int
main(int argc, char **argv)
{
	struct eveasy_thread_pool *pool = NULL;
	struct event_config *cfg		= NULL;
	struct event_base *base			= NULL;
	struct evconnlistener *listener = NULL;
	struct event *signal_event		= NULL;
	struct sockaddr_in sin			= {0};
	unsigned short port				= 9995;
	int ret							= EXIT_FAILURE;

#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(MAKEWORD(2, 2), &wsa_data);
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		fprintf(stderr, "Couldn't signal SIGPIP SIG_IGN\n");
		goto err;
	}
#endif

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

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

	if (use_thread_pool) {
		listener = evconnlistener_new_bind(base, accept_socket_cb, (void *)pool,
			LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
			(struct sockaddr *)&sin, sizeof(sin));
	} else {
		listener = evconnlistener_new_bind(base, accept_socket_cb, (void *)base,
			LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
			(struct sockaddr *)&sin, sizeof(sin));
	}
	if (!listener) {
		fprintf(stderr, "Could not create a listener!\n");
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
	if (pool)
		eveasy_thread_pool_free(pool);
	if (cfg)
		event_config_free(cfg);
	if (listener)
		evconnlistener_free(listener);
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

