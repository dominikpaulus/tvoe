#include <glib.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <event.h>
#include <event2/http.h>
#include <unistd.h>
#include <assert.h>
#include "frontend.h"
#include "log.h"
#include "mpeg.h"
#include "http.h"
#include "tvoe.h"

/* Client buffer size: Set by config parser */
#define TS_SIZE 188
#define CLIENTBUF 8192 * TS_SIZE

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

	/* For input line reading */
	int readpending, readoff;
	char buf[512];

	char clientname[INET6_ADDRSTRLEN];
	void *mpeg_handle;
	bool timeout;
	bool shutdown;

	/* Client output buffer and read/insert position */
	char writebuf[CLIENTBUF];
	int cb_inptr, cb_outptr, fill;
};

void http_add_channel(const char *name, int sid, struct tune t) {
	char text[128];
	snprintf(text, sizeof(text), "/by-sid/%d", sid);
	struct url *u = g_slice_alloc(sizeof(struct url));
	u->t = t;
	u->text = strdup(text);
	urls = g_slist_prepend(urls, u);
}

static void terminate_client(struct client *c) {
	logger(LOG_INFO, "[%s] Terminating connection", c->clientname);
	event_del(c->readev);
	event_del(c->writeev);
	event_free(c->readev);
	event_free(c->writeev);
	close(c->fd);
	if(c->mpeg_handle)
		mpeg_unregister(c->mpeg_handle);
	g_slice_free1(sizeof(struct client), c);
}

void client_timeout(evutil_socket_t sock, short event, void *p) {
	struct client *c = (struct client *) p;
	terminate_client(c);
}

static void client_senddata(void *p, uint8_t *buf, uint16_t bufsize) {
	struct client *c = (struct client *) p;
	if(c->timeout)
		return;
	if(c->fill + bufsize > CLIENTBUF) {
		logger(LOG_INFO, "[%s] Client buffer overrun, terminating connection", c->clientname);
		/* Schedule client disconnect in main control flow. */
		event_base_once(evbase, -1, EV_TIMEOUT, client_timeout, c, NULL);
		c->timeout = true;
		return;
	}
	/* Insert data into client ringbuffer */
	if(CLIENTBUF - c->cb_inptr <= bufsize) {
		/* Wraparound */
		int chunk_a = CLIENTBUF - c->cb_inptr;
		memcpy(c->writebuf + c->cb_inptr, buf, chunk_a);
		int chunk_b = bufsize - chunk_a;
		memcpy(c->writebuf, buf + chunk_a, chunk_b);
		c->cb_inptr = chunk_b;
	} else {
		/* Fits directly */
		memcpy(c->writebuf + c->cb_inptr, buf, bufsize);
		c->cb_inptr += bufsize;
	}
	c->fill += bufsize;
	event_add(c->writeev, NULL);
}

static void handle_readev(evutil_socket_t fd, short events, void *p) {
	//logger(LOG_DEBUG, "readev() called");
	struct client *c = (struct client *) p;
	int ret = recv(fd, c->buf + c->readoff, sizeof(c->buf) - c->readoff - 1, 0);
	if(ret < 0)
		logger(LOG_INFO, "Read error (disconnecting client): %s", strerror(errno));
	if(ret > 0) {
		c->readoff += ret;
		c->buf[c->readoff] = 0;
	}
	/* Read error, terminated connection or no proper client request */
	if(ret <= 0) {
		logger(LOG_INFO, "[%s] Read error: %s", c->clientname, strerror(errno));
		terminate_client(c);
		return;
	}
	if(c->readoff == sizeof(c->buf) - 1) {
		logger(LOG_INFO, "[%s] Client request has exceeded input buffer size", c->clientname);
		const char *response = "HTTP/1.1 400 Maximum request size exceeded\r\n\r\n";
		client_senddata(c, (void*) response, strlen(response));
		c->shutdown = true;
		return;
	}
	/* Read request, if already finished */
	if(!strchr(c->buf, '\n')) {
		/* Partial read - wait for remaining line */
		return;
	}
	/*
	 * We only read at most one line from the client. Ignore
	 * any additional data sent, and remove the read event.
	 */
	event_del(c->readev);
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
	/* Find matching SID/URL and add client to callback list */
	logger(LOG_INFO, "[%s] GET %s", c->clientname, url);
	for(GSList *it = urls; it != NULL; it = g_slist_next(it)) {
		struct url *u = it->data;
		if(strcmp(u->text, url))
			continue;
		logger(LOG_DEBUG, "Found requested URL");
		/* Register this client with the MPEG module */
		if(!(c->mpeg_handle = mpeg_register(u->t, client_senddata, (void (*) (void *)) terminate_client, c))) {
			logger(LOG_NOTICE, "HTTP: Unable to fulfill request: mpeg_register() failed");
			const char *response = "HTTP/1.1 503 No tuner available to fulfil your request\r\n\r\n";
			client_senddata(c, (void*) response, strlen(response));
			c->shutdown = true;
			return;
		}
		const char *response = "HTTP/1.1 200 OK\r\n\r\n";
		client_senddata(c, (void*) response, strlen(response));
		return;
	}
	logger(LOG_INFO, "Client %s requested invalid URL %s, terminating connection", c->clientname, url);
	terminate_client(c);
}

static int min(int a, int b) {
	return a < b ? a : b;
}
static void handle_writeev(evutil_socket_t fd, short events, void *p) {
	/* Send buffered data to client */
	struct client *c = (struct client *) p;
	int tosend = min(c->fill, CLIENTBUF - c->cb_outptr);
	ssize_t res = send(fd, c->writebuf + c->cb_outptr, tosend, 0);
	if(res < 0) {
		if(errno == EAGAIN)
			return;
		logger(LOG_INFO, "[%s] Send error, terminating connection (%s)", c->clientname, strerror(errno));
		terminate_client(c);
		return;
	}
	c->cb_outptr += res;
	c->fill -= res;
	if(c->cb_outptr == CLIENTBUF)
		c->cb_outptr = 0;
	if(c->fill)
		event_add(c->writeev, NULL);
	else if(c->shutdown) /* Socket is in shutdown state and all data has already been sent */
		terminate_client(c);
}

void http_connect_cb(evutil_socket_t sock, short foo, void *p) {
	logger(LOG_DEBUG, "New connection on socket");
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	int clientsock = accept(sock, (struct sockaddr *) &addr, &addrlen);
	if(clientsock < 0) {
		logger(LOG_INFO, "accept() returned %d: %s", clientsock, strerror(errno));
		return;
	}
	evutil_make_socket_nonblocking(clientsock);
	struct client *c = g_slice_alloc(sizeof(struct client));
	c->readpending = 0;
	c->readoff = 0;
	c->cb_inptr = c->cb_outptr = c->fill = 0;
	c->timeout = false;
	c->shutdown = false;
	c->fd = clientsock;
	c->mpeg_handle = NULL;
	int ret = getnameinfo((struct sockaddr *) &addr, addrlen, c->clientname, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST) < 0;
	if(ret < 0) {
		logger(LOG_ERR, "getnameinfo() failed: %s", gai_strerror(ret));
		c->clientname[0] = 0;
	}
	c->readev = event_new(evbase, clientsock, EV_READ | EV_PERSIST, handle_readev, c);
	if(!c->readev) {
		logger(LOG_ERR, "Unable to allocate new event: event_new() returned NULL");
		g_slice_free1(sizeof(struct client), c);
		close(clientsock);
		return;
	}
	c->writeev = event_new(evbase, clientsock, EV_WRITE, handle_writeev, c);
	if(!c->writeev) {
		logger(LOG_ERR, "Unable to allocate new event: event_new() returned NULL");
		event_free(c->readev);
		g_slice_free1(sizeof(struct client), c);
		close(clientsock);
		return;
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
	if(event_assign(&httpd, evbase, listenSock, EV_PERSIST | EV_READ | EV_WRITE, http_connect_cb, NULL) < 0) {
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
