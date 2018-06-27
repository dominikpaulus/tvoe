#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include <libdvbv5/dvb-file.h>
#include "channels.h"
#include "log.h"
#include "http.h"
#include "mpeg.h"

int parse_channels(const char *file) {
	/*
	 * dvb_read_file() does not provide error reporting beyond
	 * returning NULL, so let's do some basic sanity checks
	 * before trying to open the file
	 */
	FILE *f = fopen(file, "r");
	if(!f) {
		logger(LOG_ERR, "Unable to open channels/zap file \"%s\": %s",
			strerror(errno));
		return -1;
	}
	fclose(f);
	struct dvb_file *dfile = dvb_read_file(file);
	if(!dfile) {
		/* XXX - but we can't do better at the moment. */
		logger(LOG_ERR, "Failed to parse channels file \"%s\". For more "
			"information, run tvoe in foreground and check stderr output.", file);
		return -1;
	}
	logger(LOG_INFO, "Parsed channels config (\"%s\"), importing stations", file);
	struct dvb_entry *cur = dfile->first_entry;
	int cnt;
	/*
	 * Iterate over channels in zap file
	 */
	for(cnt = 0; cur != NULL; ++cnt) {
		struct tune s = {
			.sid = cur->service_id
		};
		if(cur->sat_number > 0 || cur->freq_bpf != 0) {
			logger(LOG_ERR, "Channel \"%s\" has unsupported settings (probably DiSeqC or Unicable)",
				cur->channel);
			fprintf(stderr, "%d %d\n", cur->sat_number, cur->freq_bpf);
			goto next;
		}
		for(int i = 0; i < cur->n_props; ++i) {
			if(cur->props[i].cmd != DTV_DELIVERY_SYSTEM)
				continue;

			int sys = cur->props[i].u.data;
			if(sys == SYS_DVBS || sys == SYS_DVBS2) {
				s.delivery_system = sys;
				s.dvbs.inversion = INVERSION_AUTO;
				s.dvbs.fec = FEC_AUTO;
			} else {
				logger(LOG_ERR, "Channel \"%s\" is using unsupported delivery subsystem (%d). Currently, tvoe only supports DVB-S/S2.",
					cur->channel, sys);
				goto next;
			}
		}
		for(int i = 0; i < cur->n_props; ++i) {
			if(cur->props[i].cmd == DTV_POLARIZATION) {
				s.dvbs.polarization = cur->props[i].u.data == 1;
			} else if(cur->props[i].cmd == DTV_FREQUENCY) {
				s.dvbs.frequency = cur->props[i].u.data;
			} else if(cur->props[i].cmd == DTV_SYMBOL_RATE) {
				s.dvbs.symbol_rate = cur->props[i].u.data;
			} else if(cur->props[i].cmd == DTV_INVERSION) {
				s.dvbs.inversion = cur->props[i].u.data;
			} else if(cur->props[i].cmd == DTV_INNER_FEC) {
				s.dvbs.fec = cur->props[i].u.data;
			} else if(cur->props[i].cmd == DTV_DISEQC_MASTER) {
				logger(LOG_ERR, "Channel \"%s\": Ignoring DiSeqC commands (not supported)",
					cur->channel);
			}
		}
		http_add_channel(cur->channel, cur->service_id, s);
next:
		cur = cur->next;
	}
	logger(LOG_INFO, "Successfully imported channels from \"%s\". "
		"Added a total of %d channels.", file, cnt);
	return 0;
}
