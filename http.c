#include <glib.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <event.h>
#include <event2/http.h>
#include <unistd.h>
#include "frontend.h"
#include "log.h"
#include "mpeg.h"
#include "http.h"

/* Client buffer size: Set by config parser */
int clientbuf = 10485760;

/* Handle for the HTTP base used by tvoe */
//struct evhttp *httpd;
static struct event httpd;
static int listenSock;

/* List of served URLs, with member struct */
struct url {
	char *text; /* URL to be served */
	struct tune t;
};
static GSList *urls;

struct http_output {
	struct tune *t;
	void *handle;
	struct event *timer;
};

struct client {
	evutil_socket_t fd;
	struct event *readev, *writeev;
	int readpending, readoff;
	char buf[512];
	char clientname[INET6_ADDRSTRLEN];
	void *mpeg_handle;
};

// Called by libevent on connection close
static void http_closecb(struct evhttp_connection *req, void *ptr) {
	struct http_output *c = (struct http_output *) ptr;
	mpeg_unregister(c->handle);
	event_del(c->timer);
	event_free(c->timer);
	g_slice_free1(sizeof(struct http_output), ptr);
	logger(LOG_DEBUG, "Closing connection");
}

/* 
 * Invoked every second to make sure our buffers don't overflow.
 * We don't do this on every packet sent to save CPU time, thus,
 * we need a timer.
 */
static void http_check_bufsize(evutil_socket_t fd, short what, void *arg) {
	struct evhttp_connection *conn = evhttp_request_get_connection(arg);
	size_t len = evbuffer_get_length(bufferevent_get_output(evhttp_connection_get_bufferevent(conn)));
	size_t len2 = evbuffer_get_length(evhttp_request_get_output_buffer(arg));
	logger(LOG_DEBUG, "check_bufsize() called");
	logger(LOG_DEBUG, "Bufsize: %d, %d", len, len2);
	if(clientbuf >= 0 && (len > clientbuf || len2 > clientbuf)) {
		logger(LOG_ERR, "HTTP client buffer overflowed, dropping client");
		evhttp_send_reply_end(arg);
		evhttp_connection_free(conn);
		return;
	}
}

void http_timeout(void *arg) {
	evhttp_send_reply_end(arg);
}

/*
 * Invoked by libevent when new HTTP request is received
 */
static void http_callback(struct evhttp_request *req, void *ptr) {
	struct tune *t = ptr;
	void *handle;
	if(!(handle = mpeg_register(*t, (void (*) (void *, struct evbuffer *))
					evhttp_send_reply_chunk, http_timeout, req))) {
		logger(LOG_NOTICE, "HTTP: Unable to fulfill request: mpeg_register() failed");
		evhttp_send_reply(req, HTTP_SERVUNAVAIL, "No available tuner", NULL);
		return;
	}
	logger(LOG_DEBUG, "New request!");
	struct http_output *c = g_slice_new(struct http_output);
	c->handle = handle;
	c->t = t;

	evhttp_send_reply_start(req, 200, "OK");
	evhttp_connection_set_closecb(evhttp_request_get_connection(req), http_closecb, c);

	/* Add bufsize timer */
	c->timer = event_new(NULL, -1, EV_PERSIST, http_check_bufsize, req);
	struct timeval tv = { 1, 0 };
	event_add(c->timer, &tv);
}

void http_add_channel(const char *name, int sid, struct tune t) {
	char text[128];
	snprintf(text, sizeof(text), "/by-sid/%d", sid);
	struct url *u = g_slice_alloc(sizeof(struct url));
	u->t = t;
	u->text = strdup(text);
	urls = g_slist_prepend(urls, u);
}

static void terminate_client(struct client *c) {
	event_del(c->readev);
	event_del(c->writeev);
	event_free(c->readev);
	event_free(c->writeev);
	if(c->mpeg_handle)
		mpeg_unregister(c->mpeg_handle);
	g_slice_free1(sizeof(struct client), c);
}

static void handle_readev(evutil_socket_t fd, short events, void *p) {
	logger(LOG_DEBUG, "readev() called");
	struct client *c = (struct client *) p;
	int ret = recv(fd, c->buf + c->readoff, sizeof(c->buf) - c->readoff - 1, 0);
	if(ret < 0)
		logger(LOG_INFO, "Read error (disconnecting client): %s", strerror(errno));
	if(ret > 0) {
		c->readoff += ret;
		c->buf[c->readoff] = 0;
	}
	/* Read error, terminated connection or no proper client request */
	if(ret <= 0 || c->readoff == sizeof(c->buf) - 1) {
		terminate_client(c);
		return;
	}
	/* Read request, if already finished */
	if(!strchr(c->buf, '\n'))
		return;
	char *get = strtok(c->buf, " "),
		 *url = strtok(NULL, " "),
		 *http = strtok(NULL, " ");
	if(!http || !url || !get ||
			(strcmp(get, "GET") && strcmp(get, "HEAD"))) {
		logger(LOG_DEBUG, "'%s' '%s' '%s'", get, url, http);
		logger(LOG_INFO, "Invalid request from client %s, terminating connection", c->clientname);
		terminate_client(c);
		return;
	}
	/* Add client to callback list */
	logger(LOG_DEBUG, "Client %s requested URL %s", c->clientname, url);
	for(GSList *it = urls; it != NULL; it = g_slist_next(it)) {
		struct url *u = it->data;
		if(strcmp(u->text, url))
			continue;
		logger(LOG_DEBUG, "Found URL");
	}
	logger(LOG_INFO, "Client %s requested invalid URL %s, terminating connection", c->clientname, url);
	terminate_client(c);
}

static void handle_writeev(evutil_socket_t fd, short events, void *p) {
}

void http_connect_cb(evutil_socket_t sock, short foo, void *p) {
	logger(LOG_DEBUG, "New connection");
	struct sockaddr addr;
	socklen_t addrlen;
	int clientsock = accept(sock, &addr, &addrlen);
	if(clientsock < 0) {
		logger(LOG_INFO, "accept() returned %d: %s", clientsock, strerror(errno));
		return;
	}
	evutil_make_socket_nonblocking(clientsock);
	struct client *c = g_slice_alloc(sizeof(struct client));
	c->readpending = 0;
	c->readoff = 0;
	int ret = getnameinfo(&addr, addrlen, c->clientname, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST) < 0;
	if(ret < 0) {
		logger(LOG_ERR, "getnameinfo() failed: %s", gai_strerror(ret));
		c->clientname[0] = 0;
	}
	c->readev = event_new(NULL, clientsock, EV_READ | EV_PERSIST, handle_readev, c);
	if(!c->readev) {
		logger(LOG_ERR, "Unable to allocate new event: event_new() returned NULL");
		g_slice_free1(sizeof(struct client), c);
		close(clientsock);
	}
	c->writeev = event_new(NULL, clientsock, EV_WRITE | EV_PERSIST, handle_writeev, c);
	if(!c->writeev) {
		logger(LOG_ERR, "Unable to allocate new event: event_new() returned NULL");
		event_free(c->readev);
		g_slice_free1(sizeof(struct client), c);
		close(clientsock);
	}
	event_add(c->readev, NULL);
}

int http_init(uint16_t port) {
	listenSock = socket(AF_INET6, SOCK_STREAM, 0); /* Rely on bindv6only = 0 */
	if(socket < 0) {
		logger(LOG_ERR, "Unable to create listener socket: %s", strerror(errno));
		return -1;
	}
	evutil_make_socket_nonblocking(listenSock);
	{
		/* This aids debugging */
		int flag = 1;
		setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	}
	struct sockaddr_in6 n = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(port),
		.sin6_addr = in6addr_any
	};
	if(bind(listenSock, (struct sockaddr *) &n, sizeof(n)) < 0) {
		logger(LOG_ERR, "Unable to bind to port %d: %s", port, strerror(errno));
		return -2;
	}
	if(listen(listenSock, SOMAXCONN) < 0) {
		logger(LOG_ERR, "Unable to listen on already bound sock: %s", strerror(errno));
		return -3;
	}
	if(event_assign(&httpd, NULL, listenSock, EV_PERSIST | EV_READ | EV_WRITE, http_connect_cb, NULL) < 0) {
		logger(LOG_ERR, "Invalid arguments in event_assign() (this should never happen, this is a bug)");
		return -4;
	}
	if(event_add(&httpd, NULL) < 0) {
		logger(LOG_ERR, "Unable to add assigned event to event base");
		return -5;
	}
	logger(LOG_DEBUG, "Successfully created HTTP listener");
	return 0;
}
