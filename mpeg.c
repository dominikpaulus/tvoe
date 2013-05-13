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

struct mpeg_handle {
	dvbpsi_t *pat, *pmt;
};

static bool pmt_handled[0x2000+1];

static void pat_handler(void *p, dvbpsi_pat_t *pat) {

}

static void pmt_handler(void *p, dvbpsi_pmt_t *pmt) {

}

void *register_transponder(struct tune s) {
	struct mpeg_handle *h = g_malloc(sizeof(struct mpeg_handle));
	h->pat = dvbpsi_new(NULL, DVBPSI_MSG_DEBUG);
	if(!h->pat) {
		logger(LOG_CRIT, "dvbpsi_new() failed");
		return NULL;
	}
	/*
	h->pmt = dvbpsi_new(NULL, DVBPSI_MSG_DEBUG);
	if(!h->pmt) {
		logger(LOG_CRIT, "dvbpsi_new() failed");
		dvbpsi_delete(h->pat);
		return NULL;
	}
	*/
	if(!dvbpsi_pat_attach(h->pat, pat_handler, NULL))  {
//			!dvbpsi_pmt_attach(h->pmt, pmt_handler, NULL)) {
		logger(LOG_CRIT, "Failed to attach libdvbpsi PAT decoder");
		dvbpsi_delete(h->pat);
		dvbpsi_delete(h->pmt);
		return NULL;
	}
	return h;
}

void unregister_transponder(void *handle) {

}
