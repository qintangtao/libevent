#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32

#else
#include <unistd.h>
#endif

#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/event_compat.h>


#define ITEMS_PER_ALLOC		64
#define CONN_TIMEOUT_READ	30
#define CONN_TIMEOUT_WRITE	0

#define MAX_THREAD_COUNT	128

#ifdef _WIN32
#define mutex_handle	HANDLE
#define mutex_init(p)	p = CreateMutex(NULL, FALSE, NULL);
#define mutex_enter(p)	WaitForSingleObject(p, INFINITE);
#define mutex_leave(p)	ReleaseMutex(p);
#else
#define mutex_handle	pthread_mutex_t
#define mutex_init(p)   pthread_mutex_init(&p, NULL);
#define mutex_enter(p)	pthread_mutex_lock(&p, INFINITE);
#define mutex_leave(p)	pthread_mutex_unlock(&p);
#endif

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

typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
	evutil_socket_t sfd;
	CQ_ITEM *next;
};

typedef struct conn_queue CQ;
struct conn_queue {
	CQ_ITEM *head;
	CQ_ITEM *tail;
	mutex_handle lock;
};

typedef struct {
	int thread_id;					   /* unique ID of this thread */
	struct event_base *base;		   /* libevent handle this thread uses */
	struct event *notify_event;		   /* listen event for notify pipe */
	evutil_socket_t notify_receive_fd; /* receiving end of notify pipe */
	evutil_socket_t notify_send_fd;		/* sending end of notify pipe */
	struct conn_queue *new_conn_queue; /* queue of new connections to handle */
} LIBEVENT_THREAD;

static LIBEVENT_THREAD *threads = NULL;
static int num_threads = 0;
static int last_thread = 0;
static const char MESSAGE[] = "Hello, World!\n";

static CQ_ITEM *cqi_freelist = NULL;
static mutex_handle cqi_freelist_lock;

#ifdef _WIN32
#define I64_FMT "%I64d"
#define I64_TYP __int64
#else
#define I64_FMT "%lld"
#define I64_TYP long long int
#endif


static void
cq_init(CQ *cq)
{
	mutex_init(cq->lock);
	cq->head = NULL;
	cq->tail = NULL;
}

static CQ_ITEM *
cq_pop(CQ *cq)
{
	CQ_ITEM *item;
	mutex_enter(cq->lock);
	item = cq->head;
	if (NULL != item) {
		cq->head = item->next;
		if (NULL == cq->head)
			cq->tail = NULL;
	}
	mutex_leave(cq->lock);
	return item;
}

static void
cq_push(CQ *cq, CQ_ITEM *item)
{
	item->next = NULL;
	mutex_enter(cq->lock);
	if (NULL == cq->tail)
		cq->head = item;
	else
		cq->tail->next = item;
	cq->tail = item;
	mutex_leave(cq->lock);
}

static CQ_ITEM *
cqi_new(void)
{
	CQ_ITEM *item = NULL;
	mutex_enter(cqi_freelist_lock);
	if (cqi_freelist) {
		item = cqi_freelist;
		cqi_freelist = item->next;
	}
	mutex_leave(cqi_freelist_lock);
	if (NULL == item) {
		int i;
		/* Allocate a bunch of items at once to reduce fragmentation */
		item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
		if (NULL == item) {
			fprintf(stderr, "Allocate a bunch of items at once to reduce fragmentation \n");
			return NULL;
		}
		for (i = 2; i < ITEMS_PER_ALLOC; i++)
			item[i - 1].next = &item[i];
		mutex_enter(cqi_freelist_lock);
		item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
		cqi_freelist = &item[1];
		mutex_leave(cqi_freelist_lock);
	}
	return item;
}

static void
cqi_free(CQ_ITEM *item)
{
	mutex_enter(cqi_freelist_lock);
	item->next = cqi_freelist;
	cqi_freelist = item;
	mutex_leave(cqi_freelist_lock);
}

static void
hexdump(const unsigned char *ptr, int len)
{
	int i;
	for (i = 0; i < len; ++i)
		printf("%02x ", ptr[i]);
	printf("\n");
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
conn_readcb(struct bufferevent *bev, void *user_data)
{
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
}

static void
conn_writecb(struct bufferevent *bev, void *user_data)
{
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0) {
		printf("flushed answer\n");
		// bufferevent_free(bev);
	}
}

static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
	if (events & BEV_EVENT_EOF) {
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		printf("Got an error on the connection: %s\n",
			strerror(errno)); /*XXX win32*/
	} else if (events & BEV_EVENT_TIMEOUT) {
		printf("Connection timeout.\n");
	}

	conn_print(bev, "Close");

	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
}

static void
thread_libevent_process(evutil_socket_t fd, short which, void *arg)
{
	LIBEVENT_THREAD *me = arg;

	char buf[1];

	if (recv(fd, buf, 1, 0) != 1) {
		fprintf(stderr, "Can't read from libevent pipe\n");
		return;
	}

	switch (buf[0]) {
	case 'c': {
		struct event_base *base = me->base;
		struct bufferevent *bev;
		CQ_ITEM *item;
		evutil_socket_t fd;

		while ((item = cq_pop(me->new_conn_queue)) != NULL) {
			
			fd = item->sfd;

			// create client socket
			bev = bufferevent_socket_new(
				base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
			if (bev) {
				bufferevent_setcb(
					bev, conn_readcb, conn_writecb, conn_eventcb, NULL);
				bufferevent_settimeout(
					bev, CONN_TIMEOUT_READ, CONN_TIMEOUT_WRITE);
				bufferevent_enable(bev, EV_WRITE);
				bufferevent_enable(bev, EV_READ);
			} else {
				fprintf(stderr, "Error constructing bufferevent!");
				evutil_closesocket(fd);
			}

			cqi_free(item);
		}
	} break;
	default:
		break;
	}
}

static void
setup_thread(LIBEVENT_THREAD *me)
{
	me->base = event_init();
	if (!me->base) {
		fprintf(stderr, "Can't allocate event base\n");
		exit(EXIT_FAILURE);
	}

	/* Listen for notifications from other threads */
	me->notify_event = event_new(me->base, me->notify_receive_fd,
		EV_READ | EV_PERSIST, thread_libevent_process, me);
	event_base_set(me->base, me->notify_event);
	if (event_add(me->notify_event, 0) == -1) {
		fprintf(stderr, "Can't monitor libevent notify pipe\n");
		exit(EXIT_FAILURE);
	}

	me->new_conn_queue = malloc(sizeof(struct conn_queue)); 
	if (me->new_conn_queue == NULL) {
		perror("Failed to allocate memory for connection queue");
		exit(EXIT_FAILURE);
	}
	cq_init(me->new_conn_queue);
}

#ifdef _WIN32
static DWORD WINAPI
worker_libevent(LPVOID arg)
#else
static void *
worker_libevent(void *arg)
#endif
{
	LIBEVENT_THREAD *me = arg;

	event_base_dispatch(me->base);

#ifdef _WIN32
	return EXIT_SUCCESS;
#else
	return NULL;
#endif
}

#ifdef _WIN32
static void
create_worker(DWORD (*func)(LPVOID), void *arg)
#else
static void
create_worker(void *(*func)(void *), void *arg)
#endif
{
	LIBEVENT_THREAD *me = arg;

#ifdef _WIN32
	HANDLE hThread;
	DWORD threadid = 0;
	hThread = CreateThread(NULL, 0, func, arg, 0, &threadid);
	if (hThread == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Can't create thread: %d\n", GetLastError());
		exit(EXIT_FAILURE);
	}
#else
	int ret;
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
		fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
		exit(EXIT_FAILURE);
	}
	pthread_detach(thread);
#endif

	me->thread_id = threadid;
}

int
thread_init(unsigned int nthreads, struct event_base *main_base)
{
	int ret = EXIT_SUCCESS;
	unsigned int i;

	mutex_init(cqi_freelist_lock);
	cqi_freelist = NULL;

	if (nthreads == 0 || nthreads > MAX_THREAD_COUNT)
		nthreads = MAX_THREAD_COUNT;

	num_threads = nthreads;

	threads = calloc(nthreads, sizeof(LIBEVENT_THREAD));
	if (!threads) {
		fprintf(stderr, "Can't allocate thread descriptors\n");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < nthreads; i++) {
		evutil_socket_t fds[2];

#ifdef _WIN32
		if (evutil_socketpair(AF_INET, SOCK_STREAM, 0, fds) != 0) 
#else
		if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) 
#endif			
		{
			fprintf(stderr, "Can't create notify socket pair\n");
			exit(EXIT_FAILURE);
		}

		evutil_make_socket_nonblocking(fds[0]);
		evutil_make_socket_nonblocking(fds[1]);

		threads[i].notify_receive_fd = fds[0];
		threads[i].notify_send_fd = fds[1];

		setup_thread(&threads[i]);
	}

	/* Create threads after we've done all the libevent setup. */
	for (i = 0; i < nthreads; i++) {
		create_worker(worker_libevent, &threads[i]);
	}

	return ret;
}

void
dispatch_conn_new(evutil_socket_t fd)
{
	char buf[1];

	CQ_ITEM *item = cqi_new();
	if (!item)
	{
		fprintf(stderr, "Failed to allocate memory for connection object\n");
		evutil_closesocket(fd);
		return;
	}

	item->sfd = fd;

	int tid = (last_thread + 1) % num_threads;
	LIBEVENT_THREAD *thread = threads + tid;
	last_thread = tid;

	cq_push(thread->new_conn_queue, item);

	buf[0] = 'c';
	if (send(thread->notify_send_fd, buf, 1, 0) != 1) {
		perror("Writing to thread notify pipe");
	}
}