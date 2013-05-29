#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdbool.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <glib-2.0/glib.h>
#include <event.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "frontend.h"
#include "log.h"
#include "mpeg.h"
#include "frontend.h"

static GList *idle_fe, *used_fe;
static GMutex queue_lock;

struct frontend {
	struct tune in;		/**< Associated transponder, if applicable */
	struct lnb lnb;		/**< Attached LNB */
	int adapter;		/**< Adapter number */
	int frontend;		/**< Frontend number */
	int fe_fd;			/**< File descriptor for /dev/dvb/adapterX/frontendY (O_RDONLY) */
	int dmx_fd;			/**< File descriptor for /dev/dvb/adapterX/demuxY (O_WRONLY) */
	int dvr_fd;			/**< File descriptor for /dev/dvb/adapterX/dvrY (O_RDONLY) */
	struct event *event;/**< Handle for the event callbacks on the dvr file handle */
	void *mpeg_handle;	/**< Handle for associated MPEG-TS decoder (see mpeg.c) */
};

/** Compute program frequency based on transponder frequency
 * and LNB parameters. Ripped from getstream-poempel */
static int get_frequency(int freq, struct lnb l) {
	if(freq > 2200000) { /* Frequency contains l.osc.f. */
		if(freq < l.slof)
			return freq - l.lof1;
		else
			return freq - l.lof2;
	} else
		return freq;
}

/*
 * Most ioctl() operations on the DVB frontends are asynchronous (they usually
 * return before the request is completed), but can't be expected to be
 * non-blocking. Thus, we do all the frontend parameter settings in a seperate
 * thread. Work is provided to the tuning thread using the GASyncQueue
 * work_queue.
 *
 * As the frontend is inserted into the idle_fe list by the tuner thread after
 * release, we have to synchronize access to the idle_fe queue using the
 * tune_thread lock.
 */

#define FE_WORK_TUNE 	1
#define FE_WORK_RELEASE	2
struct work {
	int action;
	struct frontend *fe;
};
static GAsyncQueue *work_queue;

/*
 * Tune previously unkown frontend
 */
static void tune_to_fe(struct frontend *fe) {
	struct tune s = fe->in;
	// TODO: Proper error handling for this function. At the moment, we just return
	// leaving this frontend idle - eventually, all clients will drop and we will
	// release this frontend...
	/* Tune to transponder */
	{
		struct dtv_property p[9];
		struct dtv_properties cmds;
		bool tone = s.dvbs.frequency > 2200000 && s.dvbs.frequency >= fe->lnb.slof;
		p[0].cmd = DTV_CLEAR;
		p[1].cmd = DTV_DELIVERY_SYSTEM;		p[1].u.data = s.dvbs.delivery_system;
		p[2].cmd = DTV_SYMBOL_RATE;			p[2].u.data = s.dvbs.symbol_rate;
		p[3].cmd = DTV_INNER_FEC;			p[3].u.data = FEC_AUTO;
		p[4].cmd = DTV_INVERSION;			p[4].u.data = INVERSION_AUTO;
		p[5].cmd = DTV_FREQUENCY;			p[5].u.data = get_frequency(s.dvbs.frequency, fe->lnb);
		p[6].cmd = DTV_VOLTAGE;				p[6].u.data = s.dvbs.polarization ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13;
		p[7].cmd = DTV_TONE;				p[7].u.data = tone ? SEC_TONE_ON : SEC_TONE_OFF;
		p[8].cmd = DTV_TUNE;				p[8].u.data = 0;
		cmds.num = 9;
		cmds.props = p;
		if(ioctl(fe->fe_fd, FE_SET_PROPERTY, &cmds) < 0) {
			// This should only fail if we get an event overflow, thus,
			// we can safely continue after this error.
			logger(LOG_ERR, "Failed to tune frontend %d/%d to freq %d, sym	%d",
					fe->adapter, fe->frontend, get_frequency(p[5].u.data,
					fe->lnb), s.dvbs.symbol_rate);
			return;
		}
	}
	/* Now wait for the tuning to be successful */
	/*
	struct dvb_frontend_event ev;
	do {
		if(ioctl(fe->fe_fd, FE_GET_EVENT, &ev) < 0) {
			logger(LOG_ERR, "Failed to get event from frontend %d/%d: %s",
					fe->adapter, fe->frontend, strerror(errno));
		}
	} while(!(ev.status & FE_HAS_LOCK) && !(ev.status & FE_TIMEDOUT));
	if(ev.status & FE_TIMEDOUT) {
		logger(LOG_ERR, "Timed out waiting for lock on frontend %d/%d",
				fe->adapter, fe->frontend);
		return;
	}
	*/
	logger(LOG_INFO, "Tuning on adapter %d/%d succeeded",
			fe->adapter, fe->frontend);
	{
		struct dmx_pes_filter_params par;
		par.pid = 0x2000;
		par.input = DMX_IN_FRONTEND;
		par.output = DMX_OUT_TS_TAP;
		par.pes_type = DMX_PES_OTHER;
		par.flags = DMX_IMMEDIATE_START;
		if(ioctl(fe->dmx_fd, DMX_SET_PES_FILTER, &par) < 0) {
			logger(LOG_ERR, "Failed to configure tmuxer on frontend %d/%d",
					fe->adapter, fe->frontend);
			return;
		}
	}
}

static void release_fe(struct frontend *fe) {
	close(fe->fe_fd);
	close(fe->dmx_fd);
	close(fe->dvr_fd);
	g_mutex_lock(&queue_lock);
	idle_fe = g_list_append(idle_fe, fe);
	g_mutex_unlock(&queue_lock);
}

/*
 * Frontend worker thread main routine
 */
static void *tune_worker(void *ptr) {
	for(;;) {
		struct work *w = g_async_queue_pop(work_queue);
		struct frontend *fe = w->fe;
		if(w->action == FE_WORK_TUNE)
			tune_to_fe(fe);
		else
			release_fe(fe);
		g_free(w);
	}
	return NULL;
}

void frontend_init(void) {
	work_queue = g_async_queue_new();
	g_mutex_init(&queue_lock);
	/* Start tuning thread */
	g_thread_new("tune_worker", tune_worker, NULL);
}

/* libevent callback for data on dvr fd */
static void dvr_callback(evutil_socket_t fd, short int flags, void *arg) {
	struct frontend *fe = (struct frontend *) arg;
	unsigned char buf[1024 * 188];

	if(flags & EV_TIMEOUT) {
		logger(LOG_ERR, "Timeout reading data from frontend %d/%d", fe->adapter,
				fe->frontend);
		mpeg_notify_timeout(fe->mpeg_handle);
		return;
	}

	int n = read(fd, buf, sizeof(buf));
	if(n < 0) {
		logger(LOG_ERR, "Invalid read on frontend %d/%d: %s",
				fe->adapter, fe->frontend, strerror(errno));
		return;
	}
	mpeg_input(fe->mpeg_handle, buf, n);
}

/* Tune to a new, previously unknown transponder */
void *frontend_acquire(struct tune s, void *ptr) {
	// Get new idle frontend from queue
	g_mutex_lock(&queue_lock);
	GList *f = g_list_first(idle_fe);
	if(!f) {
		g_mutex_unlock(&queue_lock);
		return NULL;
	}
	idle_fe = g_list_remove_link(idle_fe, f);
	g_mutex_unlock(&queue_lock);

	struct frontend *fe = (struct frontend *) (f->data);
	fe->dmx_fd = fe->dvr_fd = fe->fe_fd = 0;
	fe->event = NULL;
	fe->in = s;
	fe->mpeg_handle = ptr;

	logger(LOG_DEBUG, "Acquiring frontend %d/%d",
			fe->adapter, fe->frontend);

	char path[512];
	/* Open frontend... */
	snprintf(path, sizeof(path), "/dev/dvb/adapter%d/frontend%d", fe->adapter,
			fe->frontend);
	fe->fe_fd = open(path, O_RDWR);
	if(fe->fe_fd < 0) {
		logger(LOG_ERR, "Failed to open frontend (%d/%d): %s", fe->adapter,
				fe->frontend, strerror(errno));
		goto fail;
	}

	/* ...demuxer... */
	snprintf(path, sizeof(path), "/dev/dvb/adapter%d/demux%d", fe->adapter, fe->frontend);
	fe->dmx_fd = open(path, O_RDWR);
	if(fe->dmx_fd < 0) {
		logger(LOG_ERR, "Failed to open demuxer: %s", strerror(errno));
		goto fail;
	}

	/* ...and dvr.*/
	snprintf(path, sizeof(path), "/dev/dvb/adapter%d/dvr%d", fe->adapter, fe->frontend);
	fe->dvr_fd = open(path, O_RDONLY | O_NONBLOCK);
	if(!fe->dvr_fd) {
		logger(LOG_ERR, "Failed to open dvr device for frontend %d/%d",
				fe->adapter, fe->frontend);
		goto fail;
	}

	/* Add libevent callback for TS input */
	struct event *ev = event_new(NULL, fe->dvr_fd, EV_READ | EV_PERSIST, dvr_callback, fe);
	struct timeval tv = { 10, 0 }; // 10s timeout
	if(event_add(ev, &tv)) {
		logger(LOG_ERR, "Adding frontend to libevent failed.");
		goto fail;
	}
	fe->event = ev;

	used_fe = g_list_append(used_fe, fe);

	// Tell tuning thread to tune
	struct work *w = g_malloc(sizeof(struct work));
	w->action = FE_WORK_TUNE;
	w->fe = fe;
	g_async_queue_push(work_queue, w);

	return fe;
fail:
	if(fe->event) {
		event_del(fe->event);
		event_free(fe->event);
	}
	if(fe->fe_fd)
		close(fe->fe_fd);
	if(fe->dmx_fd)
		close(fe->dmx_fd);
	if(fe->dvr_fd)
		close(fe->dvr_fd);
	g_mutex_lock(&queue_lock);
	idle_fe = g_list_append(idle_fe, f);
	g_mutex_unlock(&queue_lock);
	return NULL;
}

void frontend_release(void *ptr) {
	struct frontend *fe = ptr;
	logger(LOG_INFO, "Releasing frontend %d/%d", fe->adapter, fe->frontend);
	event_del(fe->event);
	event_free(fe->event);
	used_fe = g_list_remove(used_fe, fe);
	struct work *w = g_malloc(sizeof(struct work));
	w->action = FE_WORK_RELEASE;
	w->fe = fe;
	g_async_queue_push(work_queue, w);
}

void frontend_add(int adapter, int frontend, struct lnb l) {
	struct frontend *fe = g_slice_alloc0(sizeof(struct frontend));
	fe->lnb = l;
	fe->adapter = adapter;
	fe->frontend = frontend;
	idle_fe = g_list_append(idle_fe, fe);
}
