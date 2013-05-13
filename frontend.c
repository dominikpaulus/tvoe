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

static GList *idle_fe, *used_fe;

struct tune_data {
	struct tune in;
	struct lnb lnb;
	struct event *event;
	int adapter, frontend;
	int fe_fd, dmx_fd, dvr_fd;
};

static int get_frequency(int freq, struct lnb l) {
	return 0; // TODO
}

static void dvr_callback(evutil_socket_t fd, short int flags, void *arg) {
	
}

int acquire_frontend(struct tune s) {
	GList *f = g_list_first(idle_fe);
	if(!f)
		return -1;
	idle_fe = g_list_remove(idle_fe, f->data);
	struct tune_data *fe = (struct tune_data *) f->data;
	fe->dmx_fd = fe->dvr_fd = fe->fe_fd = 0;
	fe->in = s;

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
		p[1].cmd = DTV_DELIVERY_SYSTEM;		p[1].u.data = SYS_DVBS2;
		p[2].cmd = DTV_SYMBOL_RATE;			p[2].u.data = s.dvbs.symbol_rate;
		p[3].cmd = DTV_INNER_FEC;			p[3].u.data = FEC_AUTO;
		p[4].cmd = DTV_INVERSION;			p[4].u.data = INVERSION_AUTO;
		p[5].cmd = DTV_FREQUENCY;			p[5].u.data = get_frequency(s.dvbs.frequency, fe->lnb);
		p[6].cmd = DTV_VOLTAGE;				p[6].u.data = SEC_VOLTAGE_13;
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
	fe->dvr_fd = open(path, O_RDONLY);
	if(!fe->dvr_fd) {
		logger(LOG_CRIT, "Failed to open dvr device for frontend %d/%d",
				fe->adapter, fe->frontend);
		goto fail;
	}
	logger(LOG_DEBUG, "Successfully opened frontend! :-)");

	struct event *ev = event_new(NULL, fe->dvr_fd, EV_READ, dvr_callback, fe);
	struct timeval tv = { 30, 0 }; // 30s timeout
	if(event_add(ev, &tv)) {
		logger(LOG_CRIT, "Adding frontend to libevent failed.");
		goto fail;
	}
	fe->event = ev;

	used_fe = g_list_append(used_fe, fe);

	return 0;
fail:
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

}

void add_frontend(int adapter, int frontend, struct lnb l) {

}

void remove_frontend(int adapter, int frontend) {

}

#if 0
	int fd = open("/dev/dvb/adapter7/frontend0", O_RDWR);
	if(fd == -1) {
		perror("open()");
		return EXIT_FAILURE;
	}
	printf("Open frontend: Success.\n");
	struct dvb_frontend_info info;
	if(ioctl(fd, FE_GET_INFO, &info) < 0) {
		perror("FE_GET_INFO on frontend failed");
		return EXIT_FAILURE;
	}
	printf("FE_GET_INFO: Success. Returned struct:\n");
	printf("\tName: %s\n\tMin freq: %d\n\tMax freq: %d\n\tType: %d\n",
			info.name, info.frequency_min, info.frequency_max, info.type);
	struct dtv_properties cmd;
	struct dtv_property props[2];
	props[0].cmd = DTV_DELIVERY_SYSTEM;
	props[1].cmd = DTV_API_VERSION;
	cmd.props = props;
	cmd.num = 2;
	if(ioctl(fd, FE_GET_PROPERTY, &cmd) < 0) {
		perror("FE_GET_INFO on frontend failed");
		return EXIT_FAILURE;
	}
	printf("FE_GET_PROPERTY: Success. Returned data:\n");
	printf("\tDelivery system: %d\n\tAPI version: %d\n", props[0].u.data,
			props[1].u.data);
	printf("Now trying to tune to ZDF HD.");
	fflush(stderr);
	struct dtv_property p[9];
	struct dtv_properties cmds;
	p[0].cmd = DTV_CLEAR;
	p[1].cmd = DTV_DELIVERY_SYSTEM;		p[1].u.data = SYS_DVBS2;
	p[2].cmd = DTV_SYMBOL_RATE;			p[2].u.data = 22000 * 1000;
	p[3].cmd = DTV_INNER_FEC;			p[3].u.data = FEC_AUTO;
	p[4].cmd = DTV_INVERSION;			p[4].u.data = INVERSION_AUTO;
	p[5].cmd = DTV_FREQUENCY;			p[5].u.data = 1597000; // 11362 * 1000 - 10600000
	p[6].cmd = DTV_VOLTAGE;				p[6].u.data = SEC_VOLTAGE_13;
//	p[7].cmd = DTV_TONE;				p[7].u.data = ;
	p[7].cmd = DTV_TUNE;				p[7].u.data = 0;
	cmds.num = 8;
	cmds.props = p;
	if(ioctl(fd, FE_SET_PROPERTY, &cmds) < 0) {
		perror("FE_SET_PROPERTY on frontend failed");
		return EXIT_FAILURE;
	}
	printf(" Success!\n");
	//close(fd);
	printf("Setting kernel demuxer to passthrough\n");
	int fd2 = open("/dev/dvb/adapter7/demux0", O_RDWR);
	if(fd2 < 0) {
		perror("open()");
		return EXIT_FAILURE;
	}
	struct dmx_pes_filter_params par;
	par.pid = 0; //0x2000;
	par.input = DMX_IN_FRONTEND;
	par.output = DMX_OUT_TS_TAP;
	par.pes_type = DMX_PES_OTHER;
	par.flags = DMX_IMMEDIATE_START;
	if(ioctl(fd2, DMX_SET_PES_FILTER, &par) < 0) {
		perror("ioctl() for setting filter");
		return EXIT_FAILURE;
	}
	if(ioctl(fd2, DMX_START) < 0) {
		perror("ioctl(): Enable filter");
		return EXIT_FAILURE;
	}
	//close(fd2);
	printf("Opening dvr device: ");
	int fd3 = open("/dev/dvb/adapter7/dvr0", O_RDONLY | O_NONBLOCK);
	if(fd3 < 0) {
		perror("open()");
		return EXIT_FAILURE;
	}
	printf("Ok.\n");
	printf("Initializing libdvbpsi: ");
	psi = dvbpsi_new(NULL, DVBPSI_MSG_DEBUG);
	psi2 = dvbpsi_new(NULL, DVBPSI_MSG_DEBUG);
	if(psi == NULL || psi2 == NULL) {
		fprintf(stderr, "dvbpsi_new() failed\n");
		return EXIT_FAILURE;
	}
	if(!dvbpsi_pat_attach(psi, pat_handler, NULL)) {
		fprintf(stderr, "dvbpsi_pat_attach() failed\n");
		return EXIT_FAILURE;
	}
	if(!dvbpsi_pmt_attach(psi2, 11150, pmt_handler, NULL)) {
		fprintf(stderr, "dvbpsi_pmt_attach() failed\n");
		return EXIT_FAILURE;
	}
	printf("Now waiting for input\n");
	fd_set readfds, writefds, errorfds;
	int pkt = 0, un = 0, err = 0, pat = 0;
	//int fd4 = open("tmpfile", O_CREAT | O_RDWR);
	//if(fd4 < 0)
	//	perror("open()");
	while(1) {
		FD_ZERO(&readfds);
		FD_SET(fd3, &readfds);
		if(select(fd3+1, &readfds, NULL, NULL, NULL) < 0) {
			perror("select()");
			return EXIT_FAILURE;
		}
		if(!FD_ISSET(fd3, &readfds))
			// wtf?
			continue;
		char buf[128*188];
		int rd;
		if((rd = read(fd3, buf, sizeof(buf))) < 0) {
			perror("read()");
			return EXIT_FAILURE;
		}
		if(rd % 188 != 0)
			un++;
		for(int i=0; i+188<rd; i+=188) {
			dvbpsi_packet_push(psi, buf + i);
			dvbpsi_packet_push(psi2, buf + i);
		}
		struct mpeg_ts *head = (struct mpeg_ts*) buf;
		if(head->sync != 0x47)
			err++;
		if(head->pid == 0)
			pat++;
		/*
		if(buf[2] == 0)
			pat++;
		*/
		/*
		if(((struct test *) (buf+1))->pid == 0)
			pat++;
		*/
		//printf("PID: %d\n", head->pid);
		printf("\rReceived packets: %d (unaligned: %d, error: %d, pat: %d)", pkt++, un, err, pat);
		fflush(stdout);
		//write(fd4, buf, rd);
	}
}
#endif
