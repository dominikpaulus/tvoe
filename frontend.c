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
#include <assert.h>
#include "frontend.h"
#include "log.h"
#include "mpeg.h"
#include "frontend.h"

/* Size of demux buffer. Set by config parser, 0 means default */
size_t dmxbuf = 0;

static GList *idle_fe, *used_fe;
static GMutex queue_lock;

static void dvr_callback(evutil_socket_t fd, short int flags, void *arg);
static void fe_open_failed(evutil_socket_t fd, short int flags, void *arg);

enum fe_state {
	state_idle,			/**< Frontend is currently not in use */
	state_tuning,		/**< The frontend worker thread has queued tuning the frontend */
	state_active,		/**< Frontend is tuned and (hopefully) providing data */
	state_stale			/**< All clients have unsubscribed, but tuning operation is still pending */
};

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
	int state;			/**< Frontend currently in use */
	GMutex lock;		/**< Lock for synchronizing worker thread */
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

/************** Called in the frontend worker threads ***************/

/*
 * Open frontend descriptors
 */
static bool open_fe(struct frontend *fe) {
	/* Open frontend, demuxer and DVR output */
	char path_fe[512], path_dmx[512], path_dvr[512];
	snprintf(path_fe, sizeof(path_fe), "/dev/dvb/adapter%d/frontend%d", fe->adapter, fe->frontend);
	snprintf(path_dmx, sizeof(path_dmx), "/dev/dvb/adapter%d/demux%d", fe->adapter, fe->frontend);
	snprintf(path_dvr, sizeof(path_dvr), "/dev/dvb/adapter%d/dvr%d", fe->adapter, fe->frontend);
	if((fe->fe_fd = open(path_fe, O_RDWR | O_NONBLOCK)) < 0)
		goto fe_err;
	if((fe->dmx_fd = open(path_dmx, O_RDWR)) < 0)
		goto dmx_err;
	if((fe->dvr_fd = open(path_dvr, O_RDONLY | O_NONBLOCK)) < 0)
		goto dvr_err;

	/* Add libevent callback for TS input */
	struct event *ev = event_new(NULL, fe->dvr_fd, EV_READ | EV_PERSIST, dvr_callback, fe);
	struct timeval tv = { 3, 0 }; // 3s timeout
	if(event_add(ev, &tv)) {
		logger(LOG_ERR, "Adding frontend to libevent failed.");
		close(fe->fe_fd);
		close(fe->dmx_fd);
		close(fe->dvr_fd);
		assert(event_base_once(NULL, -1, EV_TIMEOUT, fe_open_failed, fe, NULL) != -1);
		return false;
	}
	fe->event = ev;

	return true;

dvr_err:
	close(fe->dmx_fd);
dmx_err:
	close(fe->dvr_fd);
fe_err:
	logger(LOG_ERR, "Failed to open frontend (%d/%d): %s", fe->adapter,
			fe->frontend, strerror(errno));
	/* Drop clients - schedule open_failed in main thread */
	assert(event_base_once(NULL, -1, EV_TIMEOUT, fe_open_failed, fe, NULL) != -1);
	return false;
}

/*
 * Tune previously unkown frontend
 */
static bool tune_to_fe(struct frontend *fe) {
	struct tune s = fe->in;
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
			assert(event_base_once(NULL, -1, EV_TIMEOUT, fe_open_failed, fe, NULL) != -1);
			return false;
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
	logger(LOG_INFO, "Frontend %d/%d successfully tuned",
			fe->adapter, fe->frontend);
	{
		struct dmx_pes_filter_params par = {
			.pid = 0x2000,
			.input = DMX_IN_FRONTEND,
			.output = DMX_OUT_TS_TAP,
			.pes_type = DMX_PES_OTHER,
			.flags = DMX_IMMEDIATE_START
		};
		if(ioctl(fe->dmx_fd, DMX_SET_PES_FILTER, &par) < 0) {
			logger(LOG_ERR, "Failed to configure tmuxer on frontend %d/%d",
					fe->adapter, fe->frontend);
			assert(event_base_once(NULL, -1, EV_TIMEOUT, fe_open_failed, fe, NULL) != -1);
			return false;
		}
	}
	/* Set demux buffer size, if requested */
	if(dmxbuf)
		ioctl(fe->dmx_fd, DMX_SET_BUFFER_SIZE, dmxbuf);

	return true;
}

static void release_fe(struct frontend *fe) {
	close(fe->fe_fd);
	close(fe->dmx_fd);
	close(fe->dvr_fd);
	fe->state = state_idle;
	g_mutex_lock(&queue_lock);
	idle_fe = g_list_append(idle_fe, fe);
	g_mutex_unlock(&queue_lock);
	logger(LOG_INFO, "Released frontend %d/%d", fe->adapter, fe->frontend);
}

/*
 * Frontend worker thread main routine
 */
static void *tune_worker(void *ptr) {
	for(;;) {
		struct work *w = g_async_queue_pop(work_queue);
		struct frontend *fe = w->fe;
		if(w->action == FE_WORK_TUNE) {
			/*
			 * If all clients have unsubscribed before the tuning process has
			 * finished, we have to manually schedule removal of the frontend
			 * in the main thread, as it is otherwise postponed until now.
			 *
			 * open_fe() and tune_to_fe() return false if they failed and
			 * consequently they've already scheduled the call to
			 * fe_open_failed in the main thread.
			 */
			if(open_fe(fe) && tune_to_fe(fe)) {
				g_mutex_lock(&fe->lock);
				if(fe->state == state_stale)
					assert(event_base_once(NULL, -1, EV_TIMEOUT, fe_open_failed, fe, NULL) != -1);
				else
					fe->state = state_active;
				g_mutex_unlock(&fe->lock);
			}
		} else
			release_fe(fe);
		g_slice_free(struct work, w);
	}
	return NULL;
}

/****************************** Main control flow ************************/

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

	/* We still might get spurious events from disabled frontends */
	if(fe->state != state_active)
		return;

	if(flags & EV_TIMEOUT) {
		logger(LOG_ERR, "Timeout reading data from frontend %d/%d", fe->adapter,
				fe->frontend);
		mpeg_notify_timeout(fe->mpeg_handle);
		return;
	}

	int n = read(fd, buf, sizeof(buf));
	if(n <= 0) {
		logger(LOG_ERR, "Invalid read on frontend %d/%d: %s",
				fe->adapter, fe->frontend, strerror(errno));
		return;
	}
	mpeg_input(fe->mpeg_handle, buf, n);
}

/* Tune to a new, previously unknown transponder */
void *frontend_acquire(struct tune s, void *ptr) {
	logger(LOG_INFO, "acquire()");
	// Get new idle frontend from queue
	g_mutex_lock(&queue_lock);
	GList *f = g_list_first(idle_fe);
	if(!f) {
		g_mutex_unlock(&queue_lock);
		logger(LOG_INFO, "No more free frontends in queue.");
		return NULL;
	}
	idle_fe = g_list_remove_link(idle_fe, f);
	g_mutex_unlock(&queue_lock);

	struct frontend *fe = (struct frontend *) (f->data);
	fe->in = s;
	fe->mpeg_handle = ptr;
	fe->event = NULL;

	logger(LOG_DEBUG, "Acquiring frontend %d/%d",
			fe->adapter, fe->frontend);

	used_fe = g_list_append(used_fe, fe);

	// Tell tuning thread to tune
	struct work *w = g_slice_new(struct work);
	w->action = FE_WORK_TUNE;
	w->fe = fe;
	fe->state = state_tuning;
	g_async_queue_push(work_queue, w);

	return fe;
}

void frontend_release(void *ptr) {
	struct frontend *fe = ptr;
	logger(LOG_DEBUG, "Releasing frontend %d/%d", fe->adapter, fe->frontend);
	g_mutex_lock(&fe->lock);
	if(fe->state == state_tuning) {
		fe->state = state_stale;
		g_mutex_unlock(&fe->lock);
		return;
	}
	g_mutex_unlock(&fe->lock);

	/*
	 * We might also release the frontend as a consequence of the
	 * tuning process having failed. Then, we may not touch the event,
	 * as it is not installed yet...
	 */
	if(fe->event != NULL) {
		event_del(fe->event);
		event_free(fe->event);
	}

	used_fe = g_list_remove(used_fe, fe);
	struct work *w = g_slice_new(struct work);
	w->action = FE_WORK_RELEASE;
	w->fe = fe;
	g_async_queue_push(work_queue, w);
}

static void fe_open_failed(evutil_socket_t fd, short int flags, void *arg) {
	struct frontend *fe = (struct frontend *) arg;
	if(fe->state == state_stale) {
		frontend_release(fe);
	} else {
		fe->state = state_stale;
		mpeg_notify_timeout(fe->mpeg_handle);
	}
}

void frontend_add(int adapter, int frontend, struct lnb l) {
	struct frontend *fe = g_slice_alloc0(sizeof(struct frontend));
	fe->lnb = l;
	fe->adapter = adapter;
	fe->frontend = frontend;
	fe->state = state_idle;
	g_mutex_init(&fe->lock);
	idle_fe = g_list_append(idle_fe, fe);
}
