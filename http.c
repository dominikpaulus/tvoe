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
	if(c->mpeg_handle)
		mpeg_unregister(c->mpeg_handle);
	g_slice_free1(sizeof(struct client), c);
}

static void client_senddata(void *p, uint8_t *buf, uint16_t bufsize) {
	struct client *c = (struct client *) p;
	if(c->fill + bufsize > CLIENTBUF) {
		logger(LOG_INFO, "[%s] Client buffer overrun, terminating connection", c->clientname);
		terminate_client(c);
		return;
	}
	if(CLIENTBUF - c->cb_inptr <= bufsize) {
		/* Wraparound */
		int a = CLIENTBUF - c->cb_inptr;
		memcpy(c->writebuf + c->cb_inptr, buf, a);
		int b = bufsize - a;
		memcpy(c->writebuf, buf + a, b);
		c->cb_inptr = b;
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
	if(ret <= 0 || c->readoff == sizeof(c->buf) - 1) {
		if(ret < 0)
			logger(LOG_INFO, "[%s] Read error: %s", c->clientname, strerror(errno));
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
	logger(LOG_DEBUG, "[%s] Requesting %s", c->clientname, url);
	for(GSList *it = urls; it != NULL; it = g_slist_next(it)) {
		struct url *u = it->data;
		if(strcmp(u->text, url))
			continue;
		logger(LOG_DEBUG, "Found requested URL");
		/* Register this client with the MPEG module */
		if(!(c->mpeg_handle = mpeg_register(u->t, client_senddata, (void (*) (void *)) terminate_client, c))) {
			logger(LOG_NOTICE, "HTTP: Unable to fulfill request: mpeg_register() failed");
			/* TODO: Send meaningful reply */
			terminate_client(c);
			//evhttp_send_reply(req, HTTP_SERVUNAVAIL, "No available tuner", NULL);
		}
		return;
	}
	logger(LOG_INFO, "Client %s requested invalid URL %s, terminating connection", c->clientname, url);
	terminate_client(c);
}

static int min(int a, int b) {
	return a < b ? a : b;
}
static void handle_writeev(evutil_socket_t fd, short events, void *p) {
	struct client *c = (struct client *) p;
	int tosend = min(c->fill, CLIENTBUF - c->cb_outptr);
	ssize_t res = send(fd, c->writebuf + c->cb_outptr, tosend, 0);
	if(res < 0) {
		if(errno == EAGAIN)
			return;
		logger(LOG_NOTICE, "[%s] Send error, terminating connection (%s)", c->clientname, strerror(errno));
		terminate_client(c);
		return;
	}
	c->cb_outptr += res;
	c->fill -= res;
	if(c->cb_outptr == CLIENTBUF)
		c->cb_outptr = 0;
	if(c->fill)
		event_add(c->writeev, NULL);
}

void http_connect_cb(evutil_socket_t sock, short foo, void *p) {
	//logger(LOG_DEBUG, "New connection on socket");
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
	c->cb_inptr = c->cb_outptr = c->fill = 0;
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
