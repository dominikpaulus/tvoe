#include <stdint.h>
#include <stdbool.h> // dvbpsi requires these... m(
#include <stdlib.h>
#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/psi.h>
#include <dvbpsi/pat.h>
#include <dvbpsi/descriptor.h>
#include <dvbpsi/pmt.h>
#include <glib.h>
#include "mpeg.h"
#include "log.h"

struct pmt_handle {
	dvbpsi_t *handler;
	int pmt_id, sid;
};
struct mpeg_handle {
	dvbpsi_t *pat;
	GList *pmts;
};

static bool pmt_handled[0x2000];
static int sid_map[0x2000];

/*
 * Process a new parsed PMT. Map PIDs to corresponding SIDs.
 * @param p Pointer to struct pmt_handle
 */
static void pmt_handler(void *p, dvbpsi_pmt_t *pmt) {
	logger(LOG_DEBUG, "New PMT found");
}

/*
 * Process a new parsed PAT on input stream. Add PMT parsers for all referenced
 * channels, if necessary.
 * @param p Pointer to struct mpeg_handle
 */
static void pat_handler(void *p, dvbpsi_pat_t *pat) {
	logger(LOG_DEBUG, "New PAT found");
	struct mpeg_handle *h = (struct mpeg_handle *) p;
	dvbpsi_pat_program_t *p_program = pat->p_first_program;
	// Iterate over PAT entries
	while(p_program) {
		logger(LOG_DEBUG, "PAT entry: Channel %d PMT is at PID %d", p_program->i_number, p_program->i_pid);
		/*
		 * PAT entries for PID 0 are special PIDs for NIT tables and other
		 * stuff. We skip them for now, TODO: Handle and forward them
		 * appropiately.
		 */
		if(p_program->i_number == 0) { // NIT and other stuff
			p_program = p_program->p_next;
			continue;
		}
		sid_map[p_program->i_pid] = p_program->i_number;
		/*
		 * Map PMT PID to corresponding SID. Add parser to handle PMTs for this
		 * SID.
		 */
		if(!pmt_handled[p_program->i_pid]) {
			dvbpsi_t *handle = dvbpsi_new(NULL, DVBPSI_MSG_DEBUG); // TODO
			if(!handle) {
				logger(LOG_CRIT, "dvbpsi_new() failed");
				exit(EXIT_FAILURE); // TODO
			}

			// Keep track of installed PMT handlers
			struct pmt_handle *pmt = g_slice_alloc(sizeof(struct pmt_handle));
			pmt->handler = handle;
			pmt->pmt_id = p_program->i_pid;
			pmt->sid = p_program->i_number;
			h->pmts = g_list_append(h->pmts, pmt);

			if(!dvbpsi_pmt_attach(handle, p_program->i_number, pmt_handler, pmt)) {
				logger(LOG_CRIT, "dvbpsi_pmt_attach() failed");
				exit(EXIT_FAILURE); // TODO
			}
			pmt_handled[p_program->i_pid] = true;
		}
		p_program = p_program->p_next;
	}
	logger(LOG_DEBUG, "PAT parsing finished");
	dvbpsi_pat_delete(pat);
	return;
}

void handle_input(void *ptr, unsigned char *data, size_t len) {
	struct mpeg_handle *handle = (struct mpeg_handle *) ptr;
	int i;
	if(len % 188) {
		logger(LOG_NOTICE, "Unabligned MPEG-TS packets received, dropping.");
		return;
	}
	for(i=0; i+188<len; i+=188) {
		dvbpsi_packet_push(handle->pat, data+i);
		GList *h = handle->pmts;
		while(h) {
			struct pmt_handle *he = (struct pmt_handle *) h->data;
			dvbpsi_packet_push(he->handler, data+i);
			h = g_list_next(h);
		}
	}
}

void *register_transponder(struct tune s) {
	struct mpeg_handle *h = g_slice_alloc0(sizeof(struct mpeg_handle));
	h->pat = dvbpsi_new(NULL, DVBPSI_MSG_DEBUG);
	if(!h->pat) {
		logger(LOG_CRIT, "dvbpsi_new() failed");
		return NULL;
	}
	if(!dvbpsi_pat_attach(h->pat, pat_handler, h))  {
		logger(LOG_CRIT, "Failed to attach libdvbpsi PAT decoder");
		dvbpsi_delete(h->pat);
		return NULL;
	}
	return h;
}

void unregister_transponder(void *handle) {

}
