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
static int acquire_frontend(struct tune s);

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
	int users;			/**< Current user refcount */
	pthread_t thread;	/**< Handle for tuning thread */
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

/* libevent callback for data on dvr fd */
static void dvr_callback(evutil_socket_t fd, short int flags, void *arg) {
	struct frontend *fe = (struct frontend *) arg;
	unsigned char buf[1024 * 188];
	int n = read(fd, buf, sizeof(buf));
	if(n < 0) {
		logger(LOG_ERR, "Invalid read on frontend: %s",
				strerror(errno));
		return;
	}
	handle_input(fe->mpeg_handle, buf, n);
}

int subscribe_to_frontend(struct tune s) {
	GList *it = used_fe;
	logger(LOG_DEBUG, "Subscribing to frontend, freq: %d", s.dvbs.frequency);
	// Check whether we have already tuned to this transponder
	while(it) {
		struct tune in = ((struct frontend *) it->data)->in;
		if(in.dvbs.delivery_system == s.dvbs.delivery_system &&
				in.dvbs.symbol_rate == s.dvbs.symbol_rate &&
				in.dvbs.frequency == s.dvbs.frequency &&
				in.dvbs.polarization == s.dvbs.polarization) {
			((struct frontend *) it->data)->users++;
			logger(LOG_DEBUG, "Frontend already known. New user count: %d",
					((struct frontend *) it->data)->users);
			return 0;
		}
		it = g_list_next(it);
	}
	logger(LOG_DEBUG, "Acquiring new frontend");
	// We don't, acquire a new tuner
	return acquire_frontend(s);
}

/*
 * Thread handling FE parameter setting
 */
static void *tune_to_fe(void *arg) {
	pthread_detach(pthread_self());
	struct frontend *fe = arg;
	struct tune s = fe->in;
	/* Tune to transponder */
	{
		struct dtv_property p[8];
		struct dtv_properties cmds;
		p[0].cmd = DTV_CLEAR;
		p[1].cmd = DTV_DELIVERY_SYSTEM;		p[1].u.data = s.dvbs.delivery_system;
		p[2].cmd = DTV_SYMBOL_RATE;			p[2].u.data = s.dvbs.symbol_rate;
		p[3].cmd = DTV_INNER_FEC;			p[3].u.data = FEC_AUTO;
		p[4].cmd = DTV_INVERSION;			p[4].u.data = INVERSION_AUTO;
		p[5].cmd = DTV_FREQUENCY;			p[5].u.data = get_frequency(s.dvbs.frequency, fe->lnb);
		p[6].cmd = DTV_VOLTAGE;				p[6].u.data = s.dvbs.polarization ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13;
		p[7].cmd = DTV_TUNE;				p[7].u.data = 0;
		cmds.num = 8;
		cmds.props = p;
		if(ioctl(fe->fe_fd, FE_SET_PROPERTY, &cmds) < 0) {
			logger(LOG_ERR, "Failed to tune frontend %d/%d to freq %d, sym	%d",
					fe->adapter, fe->frontend, get_frequency(p[5].u.data,
					fe->lnb), s.dvbs.symbol_rate);
			// TODO
	//		goto fail;
		}
	}
	/* Now wait for the tuning to be successful */
	struct dvb_frontend_event ev;
	do {
		if(ioctl(fe->fe_fd, FE_GET_EVENT, &ev) < 0) {
			logger(LOG_ERR, "Failed to get event from frontend %d/%d: %s",
					fe->adapter, fe->frontend, strerror(errno));
			// TODO
		}
	} while(!(ev.status & FE_HAS_LOCK) && !(ev.status & FE_TIMEDOUT));
	if(ev.status & FE_TIMEDOUT) {
		logger(LOG_ERR, "Timed out waiting for lock on frontend %d/%d",
				fe->adapter, fe->frontend);
		// TODO
	}
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
			// TODO
	//		goto fail;
		}
	}
	return NULL;
}

/* Tune to a new, previously unknown transponder */
int acquire_frontend(struct tune s) {
	GList *f = g_list_first(idle_fe);
	if(!f)
		return -1;
	struct frontend *fe = (struct frontend *) (f->data);
	idle_fe = g_list_remove(idle_fe, f->data);
	fe->dmx_fd = fe->dvr_fd = fe->fe_fd = 0;
	fe->event = NULL;
	fe->in = s;
	fe->users = 1;

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
	struct timeval tv = { 30, 0 }; // 30s timeout
	if(event_add(ev, &tv)) {
		logger(LOG_ERR, "Adding frontend to libevent failed.");
		goto fail;
	}
	fe->event = ev;

	/* Register this transponder with the MPEG-TS handler */
	fe->mpeg_handle = register_transponder();
	if(!fe->mpeg_handle) {
		logger(LOG_ERR, "Initialization of MPEG handling module failed.");
		goto fail;
	}

	used_fe = g_list_append(used_fe, fe);

	/* Start tuning thread */
	if((errno = pthread_create(&fe->thread, NULL, tune_to_fe,
					fe)) < 0) {
		logger(LOG_ERR, "pthread_create() failed: %s", strerror(errno));
		goto fail;
	}

	logger(LOG_DEBUG, "Registering frontend succeeded, returning");

	return 0;
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
	idle_fe = g_list_append(idle_fe, f);
	return -2;
}

void release_frontend(struct tune s) {
	GList *it = used_fe;
	struct frontend *fe;
	// Reduce user count
	while(it) {
		fe = (struct frontend *) (it->data);
		struct tune in = fe->in;
		if(in.dvbs.delivery_system == s.dvbs.delivery_system &&
				in.dvbs.symbol_rate == s.dvbs.symbol_rate &&
				in.dvbs.frequency == s.dvbs.frequency &&
				in.dvbs.polarization == s.dvbs.polarization) {
			fe->users--;
			if(fe->users) // Still users present. Nothing to do;
				return;
			break;
		}
		it = g_list_next(it);
	}
	if(!it)	{ // No tuned transponder found?
		logger(LOG_NOTICE, "release_frontend() on unknown tuner");
		return;
	}
	logger(LOG_DEBUG, "Last user on frontend %d/%d exited, closing FE",
			fe->adapter, fe->frontend);
	// Last user on transponder removed, release frontend
	event_del(fe->event);
	event_free(fe->event);
	close(fe->fe_fd);
	close(fe->dmx_fd);
	close(fe->dvr_fd);
	unregister_transponder(fe->mpeg_handle);
	used_fe = g_list_remove_link(used_fe, it);
	idle_fe = g_list_append(idle_fe, fe);
}

void add_frontend(int adapter, int frontend, struct lnb l) {
	struct frontend *fe = g_slice_alloc0(sizeof(struct frontend));
	fe->lnb = l;
	fe->adapter = adapter;
	fe->frontend = frontend;
	idle_fe = g_list_append(idle_fe, fe);
}
