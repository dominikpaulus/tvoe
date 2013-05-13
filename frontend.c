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
	struct tune in;
	struct lnb lnb;
	struct event *event;
	void *mpeg_handle;
	int adapter, frontend;
	int fe_fd, dmx_fd, dvr_fd;
	int users;
};

/* Ripped from getstream-poempel */
static int get_frequency(int freq, struct lnb l) {
	if(freq > 2200000) { /* Frequency contains l.osc.f. */
		if(freq < l.slof)
			return freq - l.lof1;
		else
			return freq - l.lof2;
	} else
		return freq;
}

static void dvr_callback(evutil_socket_t fd, short int flags, void *arg) {
	struct frontend *fe = (struct frontend *) arg;
	unsigned char buf[32 * 188];
	int n = read(fd, buf, sizeof(buf));
	handle_input(fe->mpeg_handle, buf, n);
}

int subscribe_to_frontend(struct tune s) {
	GList *it = used_fe;
	// Check whether we have already tuned to this transponder
	while(it) {
		struct tune in = ((struct frontend *) it->data)->in;
		if(in.dvbs.delivery_system == s.dvbs.delivery_system &&
				in.dvbs.symbol_rate == s.dvbs.symbol_rate &&
				in.dvbs.frequency == s.dvbs.frequency &&
				in.dvbs.polarization == s.dvbs.polarization) {
			((struct frontend *) it->data)->users++;
			return 0;
		}
		it = g_list_next(it);
	}
	// We don't, acquire a new tuner
	return acquire_frontend(s);
}

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

	char path[512];
	snprintf(path, sizeof(path), "/dev/dvb/adapter%d/frontend%d", fe->adapter,
			fe->frontend);
	fe->fe_fd = open(path, O_RDWR);
	if(fe->fe_fd < 0) {
		logger(LOG_CRIT, "Failed to open frontend (%d/%d): %s", fe->adapter,
				fe->frontend, strerror(errno));
		goto fail;
	}
	logger(LOG_DEBUG, "Successfully opened frontend %d/%d", fe->adapter, fe->frontend);

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
			logger(LOG_CRIT, "Failed to tune frontend %d/%d to freq %d, sym	%d",
					fe->adapter, fe->frontend, get_frequency(p[5].u.data,
					fe->lnb), s.dvbs.symbol_rate);
			goto fail;
		}
	}
	logger(LOG_DEBUG, "Tuning succeeded");
	logger(LOG_DEBUG, "Setting demuxer to budget mode");
	snprintf(path, sizeof(path), "/dev/dvb/adapter%d/demux%d", fe->adapter, fe->frontend);
	fe->dmx_fd = open(path, O_RDWR);
	if(fe->dmx_fd < 0) {
		logger(LOG_CRIT, "Failed to open demuxer: %s", strerror(errno));
		goto fail;
	}
	{
		struct dmx_pes_filter_params par;
		par.pid = 0x2000;
		par.input = DMX_IN_FRONTEND;
		par.output = DMX_OUT_TS_TAP;
		par.pes_type = DMX_PES_OTHER;
		par.flags = DMX_IMMEDIATE_START;
		if(ioctl(fe->dmx_fd, DMX_SET_PES_FILTER, &par) < 0) {
			logger(LOG_CRIT, "Failed to configure tmuxer on frontend %d/%d",
					fe->adapter, fe->frontend);
			goto fail;
		}
		if(ioctl(fe->dmx_fd, DMX_START) < 0) {
			logger(LOG_CRIT, "Failed to enable tmuxer on frontend %d/%d",
					fe->adapter, fe->frontend);
			goto fail;
		}
	}
	logger(LOG_DEBUG, "Opening dvr interface");
	snprintf(path, sizeof(path), "/dev/dvb/adapter%d/dvr%d", fe->adapter, fe->frontend);
	fe->dvr_fd = open(path, O_RDONLY | O_NONBLOCK);
	if(!fe->dvr_fd) {
		logger(LOG_CRIT, "Failed to open dvr device for frontend %d/%d",
				fe->adapter, fe->frontend);
		goto fail;
	}
	logger(LOG_DEBUG, "Successfully opened frontend! :-)");

	struct event *ev = event_new(NULL, fe->dvr_fd, EV_READ | EV_PERSIST, dvr_callback, fe);
	struct timeval tv = { 30, 0 }; // 30s timeout
	if(event_add(ev, &tv)) {
		logger(LOG_CRIT, "Adding frontend to libevent failed.");
		goto fail;
	}
	fe->event = ev;

	fe->mpeg_handle = register_transponder(s);
	if(!fe->mpeg_handle) {
		logger(LOG_CRIT, "Initialization of MPEG handling module failed.");
		goto fail;
	}

	used_fe = g_list_append(used_fe, fe);

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

void remove_frontend(int adapter, int frontend) {

}
