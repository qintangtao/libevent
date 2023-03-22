/*
  A trivial static http webserver using Libevent's evhttp.

  This is not the best code in the world, and it does some fairly stupid stuff
  that you would never want to do in a production webserver. Caveat hackor!

 */

/* Compatibility for possible missing IPv6 declarations */
#include "../util-internal.h"
#include "../evthread-internal.h"
#include "../compat/sys/queue.h"
#include "http_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <getopt.h>
#include <io.h>
#include <fcntl.h>
#ifndef S_ISDIR
#define S_ISDIR(x) (((x)&S_IFMT) == S_IFDIR)
#endif
#else /* !_WIN32 */
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif /* _WIN32 */
#include <signal.h>

#ifdef EVENT__HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef EVENT__HAVE_AFUNIX_H
#include <afunix.h>
#endif

#include <event2/event.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#ifdef _WIN32
#include <event2/thread.h>
#endif /* _WIN32 */

#ifdef EVENT__HAVE_NETINET_IN_H
#include <netinet/in.h>
#ifdef _XOPEN_SOURCE_EXTENDED
#include <arpa/inet.h>
#endif
#endif

#ifdef _WIN32
#ifndef stat
#define stat _stat
#endif
#ifndef fstat
#define fstat _fstat
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


char uri_root[512];

static const struct table_entry {
	const char *extension;
	const char *content_type;
} content_type_table[] = {
	{"txt", "text/plain"},
	{"c", "text/plain"},
	{"h", "text/plain"},
	{"html", "text/html"},
	{"htm", "text/htm"},
	{"css", "text/css"},
	{"gif", "image/gif"},
	{"jpg", "image/jpeg"},
	{"jpeg", "image/jpeg"},
	{"png", "image/png"},
	{"pdf", "application/pdf"},
	{"ps", "application/postscript"},
	{"js", "application/javascript"},
	{"xml", "application/xml"},
	{"json", "application/json"},
	{NULL, NULL},
};

// application/x-www-form-urlencoded
// Content-Type=multipart/form-data;
// boundary=----WebKitFormBoundaryEbKBn4s3ZCuW5a4Y

struct options {
	int port;
	int iocp;
	int verbose;
	int max_body_size;

	int unlink;
	const char *unixsock;
	const char *bind;
	const char *docroot;
};

/* Try to guess a good content-type for 'path' */
static const char *
guess_content_type(const char *path)
{
	const char *last_period, *extension;
	const struct table_entry *ent;
	last_period = strrchr(path, '.');
	if (!last_period || strchr(last_period, '/'))
		goto not_found; /* no exension */
	extension = last_period + 1;
	for (ent = &content_type_table[0]; ent->extension; ++ent) {
		if (!evutil_ascii_strcasecmp(ent->extension, extension))
			return ent->content_type;
	}

not_found:
	return "application/misc";
}

/* Callback used for the /dump URI, and for every non-GET request:
 * dumps all information to stdout and gives back a trivial 200 ok */
static void
dump_request_cb(struct evhttp_request *req, void *arg)
{
	struct evhttp_thread_pool *evpool = arg;
	const char *cmdtype;
	struct evkeyvalq *headers;
	struct evkeyval *header;
	struct evbuffer *buf;
	struct evbuffer *evb = NULL;
	struct evhttp_uri *decoded = NULL;
	const char *uri = evhttp_request_get_uri(req);
	const char *path;
	const char *query;
	struct timeval lasttime;
	struct evkeyvalq params;
	struct evkeyval *param;

	TAILQ_INIT(&params);

	/* Decode the URI */
	decoded = evhttp_uri_parse(uri);
	if (!decoded) {
		printf("It's not a good URI. Sending BADREQUEST\n");
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
		return;
	}

	path = evhttp_uri_get_path(decoded);
	query = evhttp_uri_get_query(decoded);

	evhttp_parse_query_str(query, &params);

	switch (evhttp_request_get_command(req)) {
	case EVHTTP_REQ_GET:
		cmdtype = "GET";
		break;
	case EVHTTP_REQ_POST:
		cmdtype = "POST";
		break;
	case EVHTTP_REQ_HEAD:
		cmdtype = "HEAD";
		break;
	case EVHTTP_REQ_PUT:
		cmdtype = "PUT";
		break;
	case EVHTTP_REQ_DELETE:
		cmdtype = "DELETE";
		break;
	case EVHTTP_REQ_OPTIONS:
		cmdtype = "OPTIONS";
		break;
	case EVHTTP_REQ_TRACE:
		cmdtype = "TRACE";
		break;
	case EVHTTP_REQ_CONNECT:
		cmdtype = "CONNECT";
		break;
	case EVHTTP_REQ_PATCH:
		cmdtype = "PATCH";
		break;
	default:
		cmdtype = "unknown";
		break;
	}

	evb = evbuffer_new();

	evbuffer_add_printf(evb, "<!DOCTYPE html>\n"
							 "<html>\n <head>\n"
							 "  <meta charset='utf-8'>\n"
							 "  <title>dump</title>\n"
							 " </head>\n"
							 " <body>\n");

	printf("Received a %s request for %s\nHeaders:\n", cmdtype, uri);


	evbuffer_add_printf(
		evb, " <h1>Received a %s request for %s\n</h1>\n", cmdtype, uri);

	evbuffer_add_printf(evb, " <h3>Headers:</h3>\n");

	headers = evhttp_request_get_input_headers(req);
	for (header = headers->tqh_first; header; header = header->next.tqe_next) {
		printf("  %s: %s\n", header->key, header->value);
		evbuffer_add_printf(
			evb, "  <p>%s: %s</p>\n", header->key, header->value);
	}

	evbuffer_add_printf(evb, " <h3>Parameters:</h3>\n");

	TAILQ_FOREACH (param, &params, next) {
		evbuffer_add_printf(evb, "  <p>%s: %s</p>\n", param->key, param->value);
	}

	evbuffer_add_printf(evb, " <h3>Others:</h3>\n");
	evbuffer_add_printf(evb, "  <p>%s: %s</p>\n", "uri", uri);
	evbuffer_add_printf(evb, "  <p>%s: %s</p>\n", "path", path);
	evbuffer_add_printf(evb, "  <p>%s: %s</p>\n", "query", query);
	evbuffer_add_printf(
		evb, "  <p>%s: %d</p>\n", "thread id", EVTHREAD_GET_ID());

	if (evpool) {
		evbuffer_add_printf(evb, "  <p>%s: %d</p>\n", "connection count",
			evhttp_thread_pool_get_connection_count(evpool));
	}
	
	evutil_gettimeofday(&lasttime, NULL);
	evbuffer_add_printf(evb, "  <p>%s: %016llx</p>\n", "response time",
		lasttime.tv_sec * 1000 * 1000 + lasttime.tv_usec);

	evbuffer_add_printf(evb, "</body></html>\n");

	evhttp_add_header(
		evhttp_request_get_output_headers(req), "Content-Type", "text/html");

	buf = evhttp_request_get_input_buffer(req);
	puts("Input data: <<<");
	while (evbuffer_get_length(buf)) {
		int n;
		char cbuf[128];
		n = evbuffer_remove(buf, cbuf, sizeof(cbuf));
		if (n > 0)
			(void)fwrite(cbuf, 1, n, stdout);
	}
	puts(">>>");

	evhttp_send_reply(req, 200, "OK", evb);

	evhttp_clear_headers(&params);

	if (evb)
		evbuffer_free(evb);
}

/* This callback gets invoked when we get any http request that doesn't match
 * any other callback.  Like any evhttp server callback, it has a simple job:
 * it must eventually call evhttp_send_error() or evhttp_send_reply().
 */
static void
send_document_cb(struct evhttp_request *req, void *arg)
{
	struct evbuffer *evb = NULL;
	struct options *o = arg;
	const char *uri = evhttp_request_get_uri(req);
	struct evhttp_uri *decoded = NULL;
	const char *path;
	char *decoded_path;
	char *whole_path = NULL;
	size_t len;
	int fd = -1;
	struct stat st;

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
		dump_request_cb(req, arg);
		return;
	}

	printf("Got a GET request for <%s>\n", uri);

	/* Decode the URI */
	decoded = evhttp_uri_parse(uri);
	if (!decoded) {
		printf("It's not a good URI. Sending BADREQUEST\n");
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
		return;
	}

	/* Let's see what path the user asked for. */
	path = evhttp_uri_get_path(decoded);
	if (!path)
		path = "/";

	/* We need to decode it, to see what path the user really wanted. */
	decoded_path = evhttp_uridecode(path, 0, NULL);
	if (decoded_path == NULL)
		goto err;
	/* Don't allow any ".."s in the path, to avoid exposing stuff outside
	 * of the docroot.  This test is both overzealous and underzealous:
	 * it forbids aceptable paths like "/this/one..here", but it doesn't
	 * do anything to prevent symlink following." */
	if (strstr(decoded_path, ".."))
		goto err;

	len = strlen(decoded_path) + strlen(o->docroot) + 2;
	if (!(whole_path = malloc(len))) {
		perror("malloc");
		goto err;
	}
	evutil_snprintf(whole_path, len, "%s/%s", o->docroot, decoded_path);

	if (stat(whole_path, &st) < 0) {
		goto err;
	}

	/* This holds the content we're sending. */
	evb = evbuffer_new();

	if (S_ISDIR(st.st_mode)) {
		/* If it's a directory, read the comments and make a little
		 * index page */
#ifdef _WIN32
		HANDLE d;
		WIN32_FIND_DATAA ent;
		char *pattern;
		size_t dirlen;
#else
		DIR *d;
		struct dirent *ent;
#endif
		const char *trailing_slash = "";

		if (!strlen(path) || path[strlen(path) - 1] != '/')
			trailing_slash = "/";

#ifdef _WIN32
		dirlen = strlen(whole_path);
		pattern = malloc(dirlen + 3);
		memcpy(pattern, whole_path, dirlen);
		pattern[dirlen] = '\\';
		pattern[dirlen + 1] = '*';
		pattern[dirlen + 2] = '\0';
		d = FindFirstFileA(pattern, &ent);
		free(pattern);
		if (d == INVALID_HANDLE_VALUE)
			goto err;
#else
		if (!(d = opendir(whole_path)))
			goto err;
#endif

		evbuffer_add_printf(evb,
			"<!DOCTYPE html>\n"
			"<html>\n <head>\n"
			"  <meta charset='utf-8'>\n"
			"  <title>%s</title>\n"
			"  <base href='%s%s'>\n"
			" </head>\n"
			" <body>\n"
			"  <h1>%s</h1>\n"
			"  <ul>\n",
			decoded_path, /* XXX html-escape this. */
			path,		  /* XXX html-escape this? */
			trailing_slash, decoded_path /* XXX html-escape this */);
#ifdef _WIN32
		do {
			const char *name = ent.cFileName;
#else
		while ((ent = readdir(d))) {
			const char *name = ent->d_name;
#endif
			evbuffer_add_printf(evb, "    <li><a href=\"%s\">%s</a>\n", name,
				name); /* XXX escape this */
#ifdef _WIN32
		} while (FindNextFileA(d, &ent));
#else
		}
#endif
		evbuffer_add_printf(evb, "</ul></body></html>\n");
#ifdef _WIN32
		FindClose(d);
#else
		closedir(d);
#endif
		evhttp_add_header(evhttp_request_get_output_headers(req),
			"Content-Type", "text/html");
	} else {
		/* Otherwise it's a file; add it to the buffer to get
		 * sent via sendfile */
		const char *type = guess_content_type(decoded_path);
		if ((fd = open(whole_path, O_RDONLY)) < 0) {
			perror("open");
			goto err;
		}

		if (fstat(fd, &st) < 0) {
			/* Make sure the length still matches, now that we
			 * opened the file :/ */
			perror("fstat");
			goto err;
		}

		ev_off_t offset = 0;
		ev_off_t length = st.st_size;

		/*
			Range ���õĸ�ʽ�����¼��������
			Range:bytes=0-1024 ����ʾ������Ǵӿ�ͷ����1024�ֽڵ����ݣ�
			Range:bytes=1025-2048 ����ʾ������Ǵӵ�1025��2048�ֽڷ�Χ�����ݣ�
			Range:bytes=-2000 ����ʾ����������2000�ֽڵ����ݣ�
			Range:bytes=1024-
		   ����ʾ������Ǵӵ�1024�ֽڿ�ʼ���ļ��������ֵ����ݣ�
			Range:bytes=0-0,-1 ��ʾ������ǵ�һ�������һ���ֽ� ��
			Range:bytes=1024-2048,2049-3096,3097-4096
		   ����ʾ������Ƕ���ֽڷ�Χ�� Content-Range Content-Range ������Ӧ����
		   Range �����󡣷������Ὣ Content-Range ��������Ӧ��ͷ������ʽ���£�

			Content-Range:bytes(unit first byte pos)-[last byte pos]/[entity
		   length]

			�����ĸ�ʽ�������£�

			Content-Range:bytes 2048-4096/10240

			����� 2048-4096 ��ʾ��ǰ���͵����ݷ�Χ�� 10240 ��ʾ�ļ��ܴ�С��
		*/
		const char *range =
			evhttp_find_header(evhttp_request_get_input_headers(req), "Range");
		if (range != NULL) {
			printf("Range: <%s>\n", range);

			ev_off_t offset2 = 0;
			sscanf(range, "bytes=%lld-%lld", &offset, &offset2);
			if (offset == 0 && offset2 == 0)
				sscanf(range, "bytes=%lld-", &offset);

			if (offset2 > offset)
				length = offset2 - offset + 1;
		}

		if (offset < 0)
			offset = 0;

		if (offset + length > st.st_size)
			length = st.st_size - offset;

		printf("offset:%lld, length:%lld\n", offset, length);

		evhttp_add_header(
			evhttp_request_get_output_headers(req), "Content-Type", type);

		if (range) {
			char content_range[512];
			evutil_snprintf(content_range, 512, "bytes %d-%d/%d", offset,
				offset + length - 1, st.st_size);
			evhttp_add_header(evhttp_request_get_output_headers(req),
				"Content-Range", content_range);
		}

		// evbuffer_add_file(evb, fd, 0, st.st_size);
		evbuffer_add_file(evb, fd, offset, length);
	}

	evhttp_send_reply(req, HTTP_OK, "OK", evb);
	goto done;
err:
	evhttp_send_error(req, HTTP_NOTFOUND, NULL);
	if (fd >= 0)
		close(fd);
done:
	if (decoded)
		evhttp_uri_free(decoded);
	if (decoded_path)
		free(decoded_path);
	if (whole_path)
		free(whole_path);
	if (evb)
		evbuffer_free(evb);
}

static void
print_usage(FILE *out, const char *prog, int exit_code)
{
	fprintf(out,
		"Syntax: %s [ OPTS ] <docroot>\n"
		" -p      - port\n"
		" -U      - bind to unix socket\n"
		" -H      - address to bind (default: 0.0.0.0)\n"
		" -u      - unlink unix socket before bind\n"
		" -I      - IOCP\n"
		" -m      - max body size\n"
		" -v      - verbosity, enables libevent debug logging too\n",
		prog);
	exit(exit_code);
}
static struct options
parse_opts(int argc, char **argv)
{
	struct options o;
	int opt;

	memset(&o, 0, sizeof(o));

	while ((opt = getopt(argc, argv, "hp:U:m:uIvH:")) != -1) {
		switch (opt) {
		case 'p':
			o.port = atoi(optarg);
			break;
		case 'U':
			o.unixsock = optarg;
			break;
		case 'u':
			o.unlink = 1;
			break;
		case 'I':
			o.iocp = 1;
			break;
		case 'm':
			o.max_body_size = atoi(optarg);
			break;
		case 'v':
			++o.verbose;
			break;
		case 'H':
			o.bind = optarg;
			break;
		case 'h':
			print_usage(stdout, argv[0], 0);
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", opt);
			break;
		}
	}

	if (optind >= argc || (argc - optind) > 1) {
		print_usage(stdout, argv[0], 1);
	}
	o.docroot = argv[optind];

	return o;
}

static void
do_term(int sig, short events, void *arg)
{
	struct event_base *base = arg;
	event_base_loopbreak(base);
	fprintf(stderr, "Got %i, Terminating\n", sig);
}

static int
display_listen_sock(struct evhttp_bound_socket *handle)
{
	struct sockaddr_storage ss;
	evutil_socket_t fd;
	ev_socklen_t socklen = sizeof(ss);
	char addrbuf[128];
	void *inaddr;
	const char *addr;
	int got_port = -1;

	fd = evhttp_bound_socket_get_fd(handle);
	memset(&ss, 0, sizeof(ss));
	if (getsockname(fd, (struct sockaddr *)&ss, &socklen)) {
		perror("getsockname() failed");
		return 1;
	}

	if (ss.ss_family == AF_INET) {
		got_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
		inaddr = &((struct sockaddr_in *)&ss)->sin_addr;
	} else if (ss.ss_family == AF_INET6) {
		got_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
		inaddr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
	}
#ifdef EVENT__HAVE_STRUCT_SOCKADDR_UN
	else if (ss.ss_family == AF_UNIX) {
		printf("Listening on <%s>\n", ((struct sockaddr_un *)&ss)->sun_path);
		return 0;
	}
#endif
	else {
		fprintf(stderr, "Weird address family %d\n", ss.ss_family);
		return 1;
	}

	addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf, sizeof(addrbuf));
	if (addr) {
		printf("Listening on %s:%d\n", addr, got_port);
		evutil_snprintf(
			uri_root, sizeof(uri_root), "http://%s:%d", addr, got_port);
	} else {
		fprintf(stderr, "evutil_inet_ntop failed\n");
		return 1;
	}

	return 0;
}

/* Listener callback when a connection arrives at a server. */
static void
accept_socket_cb(struct evconnlistener *listener, evutil_socket_t nfd,
	struct sockaddr *peer_sa, int peer_socklen, void *arg)
{
#if 1
	struct evhttp_thread_pool *pool = arg;

	evhttp_thread_pool_assign(pool, nfd, peer_sa, peer_socklen);
#else
	struct evhttp_bound_socket *bound = arg;

	struct evhttp *http = bound->http;

	struct bufferevent *bev = NULL;
	if (bound->bevcb)
		bev = bound->bevcb(http->base, bound->bevcbarg);

	evhttp_get_request(http, nfd, peer_sa, peer_socklen, bev);
#endif
}

int
main(int argc, char **argv)
{
	struct evhttp_thread_pool *pool = NULL;
	struct event_config *cfg = NULL;
	struct event_base *base = NULL;
	struct evhttp *http = NULL;
	struct evhttp_bound_socket *handle = NULL;
	struct evconnlistener *lev = NULL;
	struct event *term = NULL;
	struct options o = parse_opts(argc, argv);
	int ret = 0;
	int i, nthreads;


#ifdef _WIN32
	{
		WORD wVersionRequested;
		WSADATA wsaData;
		wVersionRequested = MAKEWORD(2, 2);
		WSAStartup(wVersionRequested, &wsaData);
	}
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		ret = 1;
		goto err;
	}
#endif

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	/** Read env like in regress */
	if (o.verbose || getenv("EVENT_DEBUG_LOGGING_ALL"))
		event_enable_debug_logging(EVENT_DBG_ALL);

	cfg = event_config_new();
#ifdef _WIN32
	if (o.iocp) {
#ifdef EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		evthread_use_windows_threads();
		event_config_set_num_cpus_hint(cfg, si.dwNumberOfProcessors);
#endif
		event_config_set_flag(cfg, EVENT_BASE_FLAG_STARTUP_IOCP);
	}
#else

#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
	evthread_use_pthreads();
#endif

#endif

	base = event_base_new_with_config(cfg);
	if (!base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		ret = 1;
	}
	//event_config_free(cfg);
	//cfg = NULL;

	/* Create a new evhttp object to handle requests. */
	http = evhttp_new(base);
	if (!http) {
		fprintf(stderr, "couldn't create evhttp. Exiting.\n");
		ret = 1;
	}
#if 0
	evhttp_set_timeout(http, 30);

	/* The /dump URI will dump all requests to stdout and say 200 ok. */
	evhttp_set_cb(http, "/dump", dump_request_cb, http);

	/* We want to accept arbitrary requests, so we need to set a "generic"
	 * cb.  We can also add callbacks for specific paths. */
	evhttp_set_gencb(http, send_document_cb, &o);
	if (o.max_body_size)
		evhttp_set_max_body_size(http, o.max_body_size);
#endif

	if (o.unixsock) {
#ifdef EVENT__HAVE_STRUCT_SOCKADDR_UN
		struct sockaddr_un addr;

		if (o.unlink && (unlink(o.unixsock) && errno != ENOENT)) {
			perror(o.unixsock);
			ret = 1;
			goto err;
		}

		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, o.unixsock);

		lev = evconnlistener_new_bind(base, NULL, NULL, LEV_OPT_CLOSE_ON_FREE,
			-1, (struct sockaddr *)&addr, sizeof(addr));
		if (!lev) {
			perror("Cannot create listener");
			ret = 1;
			goto err;
		}

		handle = evhttp_bind_listener(http, lev);
		if (!handle) {
			fprintf(stderr, "couldn't bind to %s. Exiting.\n", o.unixsock);
			ret = 1;
			goto err;
		}
#else  /* !EVENT__HAVE_STRUCT_SOCKADDR_UN */
		fprintf(stderr, "-U is not supported on this platform. Exiting.\n");
		ret = 1;
		goto err;
#endif /* EVENT__HAVE_STRUCT_SOCKADDR_UN */
	} else {
		handle = evhttp_bind_socket_with_handle(http, o.bind, o.port);
		if (!handle) {
			fprintf(
				stderr, "couldn't bind to %s:%d. Exiting.\n", o.bind, o.port);
			ret = 1;
			goto err;
		}
	}

#if 0
	{
		struct sockaddr_un addr;
		struct timeval msec10 = {5, 0};

		pool = evhttp_thread_pool_new(cfg, 128);

		evhttp_thread_pool_assign(
			pool, EVUTIL_INVALID_SOCKET, (struct sockaddr *)&addr, sizeof(addr));
		
		evutil_usleep_(&msec10);

		evhttp_thread_pool_free(pool);
	}
#endif


	// create http thread pool
	pool = evhttp_thread_pool_new(cfg, 128);
	if (pool) {
		struct evhttp *http;

		nthreads = evhttp_thread_pool_get_thread_count(pool);
		for (i = 0; i < nthreads; i++) {
			http = evhttp_thread_get_http(evhttp_thread_pool_get_thread(pool, i));
			if (http) {

				evhttp_set_timeout(http, 30);

				/* The /dump URI will dump all requests to stdout and say 200
				 * ok. */
				evhttp_set_cb(http, "/dump", dump_request_cb, pool);

				/* We want to accept arbitrary requests, so we need to set a
				 * "generic"
				 * cb.  We can also add callbacks for specific paths. */
				evhttp_set_gencb(http, send_document_cb, &o);
				if (o.max_body_size)
					evhttp_set_max_body_size(http, o.max_body_size);
			}
		}

		// reset connlistener cb
		evconnlistener_set_cb(
			evhttp_bound_socket_get_listener(handle), accept_socket_cb, pool);
	}

	if (display_listen_sock(handle)) {
		ret = 1;
		goto err;
	}

	term = evsignal_new(base, SIGINT, do_term, base);
	if (!term)
		goto err;
	if (event_add(term, NULL))
		goto err;

	event_base_dispatch(base);

#ifdef _WIN32
	WSACleanup();
#endif

err:
	if (cfg)
		event_config_free(cfg);
	if (http)
		evhttp_free(http);
	if (term)
		event_free(term);
	if (base)
		event_base_free(base);
	if (pool)
		evhttp_thread_pool_free(pool);

	return ret;
}