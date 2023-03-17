#include "server.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#ifndef _WIN32
#include <netinet/in.h>
#ifdef _XOPEN_SOURCE_EXTENDED
#include <arpa/inet.h>
#endif
#include <sys/socket.h>
#include<unistd.h>
#endif

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/thread.h>

#include "thread.h"

static void
listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *user_data)
{
	dispatch_conn_new(fd);
}

static void
signal_cb(evutil_socket_t sig, short events, void *user_data)
{
	struct event_base *base = user_data;
	struct timeval delay = {2, 0};

	printf("Caught an interrupt signal; exiting cleanly in two seconds.\n");

	event_base_loopexit(base, &delay);
}

int
server_start(unsigned short port)
{
	struct event_config *cfg		= NULL;
	struct event_base *base			= NULL;
	struct evconnlistener *listener = NULL;
	struct event *signal_event		= NULL;
	struct sockaddr_in sin			= {0};
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
	event_config_free(cfg);
	cfg = NULL;

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

	listener = evconnlistener_new_bind(base, listener_cb, (void *)base,
		LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1, (struct sockaddr *)&sin, sizeof(sin));
	if (!listener) {
		fprintf(stderr, "Could not create a listener!\n");
		goto err;
	}

	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);
	if (!signal_event || event_add(signal_event, NULL) < 0) {
		fprintf(stderr, "Could not create/add a signal event!\n");
		goto err;
	}

#if 1
	thread_init(128, base);
#else
#ifdef _WIN32
	thread_init(si.dwNumberOfProcessors * 2 + 2, base);
#else
	thread_init(sysconf(_SC_NPROCESSORS_ONLN) * 2 + 2, base);
#endif
#endif

	event_base_dispatch(base);

	ret = EXIT_SUCCESS;

err:
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

