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

#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "util-internal.h"
#include "../mm-internal.h"
#include "../compat/sys/queue.h"

#define MAX_OUTPUT (512*1024)

struct bufferevent_connection {
	TAILQ_ENTRY(bufferevent_connection) next;

	struct bufferevent *bev;
};
TAILQ_HEAD(bufferevent_connectionq, bufferevent_connection) connections; /* queue of new connections */

static void
readcb(struct bufferevent *bev, void *ctx)
{
	struct bufferevent_connection *bev_conn;
	struct evbuffer *src, *dst;
	size_t len;

	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);

	TAILQ_FOREACH (bev_conn, &connections, next) {
		dst = bufferevent_get_output(bev_conn->bev);
		evbuffer_add_buffer_reference(dst, src);
	}

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
				//bufferevent_free(bev_conn->bev);
				TAILQ_REMOVE(&connections, bev_conn, next);
				mm_free(bev_conn);
				break;
			}
		}
	}

	bufferevent_free(bev);
}

static void
accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *p)
{
	struct event_base *base = p; 
	struct bufferevent *b_in;
	
	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	if (!b_in) {
		evutil_closesocket(fd);
		return;
	}

	bufferevent_setcb(b_in, NULL, NULL, eventcb, NULL);
	bufferevent_enable(b_in, EV_READ);

	struct bufferevent_connection *bev_conn = mm_calloc(1, sizeof(struct bufferevent_connection));
	if (!bev_conn) {
		event_warn("%s: calloc failed", __func__);
		bufferevent_free(b_in);
		return;
	}

	bev_conn->bev = b_in;
	TAILQ_INSERT_TAIL(&connections, bev_conn, next);
}

static void
syntax(void)
{
	fputs("Syntax:\n", stderr);
	fputs("   forward-proxy <listen-on-addr> <connect-to-addr>\n", stderr);
	fputs("Example:\n", stderr);
	fputs("   forward-proxy 127.0.0.1:8888 1.2.3.4:80\n", stderr);

	exit(1);
}

int
main(int argc, char **argv)
{
	struct event_base *base = NULL;
	struct bufferevent *b_out = NULL;
	struct evconnlistener *listener = NULL;
	struct sockaddr_storage listen_on_addr;
	struct sockaddr_storage connect_to_addr;
	int socklen, connect_to_addrlen, i;

#ifdef _WIN32
	{
		WORD wVersionRequested;
		WSADATA wsaData;
		wVersionRequested = MAKEWORD(2, 2);
		(void) WSAStartup(wVersionRequested, &wsaData);
	}
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("signal()");
		return 1;
	}
#endif

	if (argc < 3)
		syntax();

	for (i=1; i < argc; ++i) {
		if (!strcmp(argv[i], "-s")) {
			//use_ssl = 1;
		} else if (!strcmp(argv[i], "-W")) {
			//use_wrapper = 0;
		} else if (argv[i][0] == '-') {
			syntax();
		} else
			break;
	}

	if (i+2 != argc)
		syntax();

	memset(&listen_on_addr, 0, sizeof(listen_on_addr));
	socklen = sizeof(listen_on_addr);
	if (evutil_parse_sockaddr_port(argv[i],
		(struct sockaddr*)&listen_on_addr, &socklen)<0) {
		int p = atoi(argv[i]);
		struct sockaddr_in *sin = (struct sockaddr_in*)&listen_on_addr;
		if (p < 1 || p > 65535)
			syntax();
		sin->sin_port = htons(p);
		sin->sin_addr.s_addr = htonl(0x7f000001);
		sin->sin_family = AF_INET;
		socklen = sizeof(struct sockaddr_in);
	}

	memset(&connect_to_addr, 0, sizeof(connect_to_addr));
	connect_to_addrlen = sizeof(connect_to_addr);
	if (evutil_parse_sockaddr_port(argv[i+1],
		(struct sockaddr*)&connect_to_addr, &connect_to_addrlen)<0)
		syntax();

	TAILQ_INIT(&connections);

	base = event_base_new();
	if (!base) {
		perror("event_base_new()");
		return 1;
	}

	b_out = bufferevent_socket_new(
		base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
	if (!b_out) {
		fprintf(stderr, "Couldn't open listener.\n");
		event_base_free(base);
		return EXIT_FAILURE;
	}
	if (bufferevent_socket_connect(b_out, (struct sockaddr *)&connect_to_addr,
			connect_to_addrlen) < 0) {
		perror("bufferevent_socket_connect");
		bufferevent_free(b_out);
		event_base_free(base);
		return EXIT_FAILURE;
	}

	bufferevent_setcb(b_out, readcb, NULL, eventcb, b_out);
	bufferevent_enable(b_out, EV_READ);

	listener = evconnlistener_new_bind(base, accept_cb, base,
	    LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
	    -1, (struct sockaddr*)&listen_on_addr, socklen);
	if (! listener) {
		fprintf(stderr, "Couldn't open listener.\n");
		bufferevent_free(b_out);
		event_base_free(base);
		return EXIT_FAILURE;
	}

	event_base_dispatch(base);

	evconnlistener_free(listener);
	event_base_free(base);

#ifdef _WIN32
	WSACleanup();
#endif

	return EXIT_SUCCESS;
}
