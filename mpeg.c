#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/mpeg/psi/pmt_print.h>
#include <glib.h>
#include <assert.h>
#include "mpeg.h"
#include "log.h"

/* 
 * A lot of code in this file is based on biTstream examples.
 * TODO: Licensing information.
 */

// For keeping track of registered HTTP outputs
struct callback {
	void *ptr;
	void (*cb) (struct evbuffer *, void *);
};
struct pid_info {
	int refcnt;
	int8_t last_cc;
	uint8_t *psi_buffer;
	uint16_t psi_buffer_used;
	uint8_t *current_pmt;
	GSList *callback;
};
struct adapter {
	PSI_TABLE_DECLARE(current_pat);
	struct pid_info pids[MAX_PID];
};

/* Debugging help
static void print(void *unused, const char *psz_format, ...) {
	char psz_fmt[strlen(psz_format) + 2];
	va_list args;
	va_start(args, psz_format);
	strcpy(psz_fmt, psz_format);
	strcat(psz_fmt, "\n");
	vprintf(psz_fmt, args);
}
*/

/*
 * Process a new parsed PMT. Map PIDs to corresponding SIDs.
 * @param p Pointer to struct pmt_handle
 */
static void pmt_handler(struct adapter *a, uint16_t pid, uint8_t *section) {
	//uint16_t sid = pmt_get_program(section);
	int j;

	if(!pmt_validate(section)) {
		logger(LOG_NOTICE, "Invalid PMT received on PID %u", pid);
		return;
	}

	if(a->pids[pid].current_pmt &&
			psi_compare(a->pids[pid].current_pmt, section)) {
		free(section);
		return;
	}

	logger(LOG_DEBUG, "New PMT for SID %d found", pid);

	uint8_t *es;
	for(j = 0; (es = pmt_get_es(section, j)); j++) {
		//logger(LOG_DEBUG, "PID: %d", pmtn_get_pid(es));
	}

	free(a->pids[pid].current_pmt);
	a->pids[pid].current_pmt = section;

	// TODO: Add "pid" to PIDs for "sid"

	//pmt_print(section, print, NULL, NULL, NULL, 123);
}

/*
 * Process a new parsed PAT on input stream. Add PMT parsers for all referenced
 * channels, if necessary.
 * @param p Pointer to struct mpeg_handle
 */
static void pat_handler(struct adapter *a, uint16_t pid, uint8_t *section) {
	PSI_TABLE_DECLARE(new_pat);
	uint8_t last_section;
	int i;

	if(!pat_validate(section)) {
		logger(LOG_NOTICE, "Invalid PAT received on PID %u", pid);
		return;
	}

	psi_table_init(new_pat);
	if(!psi_table_section(new_pat, section)) {
		psi_table_free(new_pat);
		return;
	}
	// Don't re-parse already known PATs
	if(psi_table_validate(a->current_pat) &&
			psi_table_compare(a->current_pat, new_pat)) {
		psi_table_free(new_pat);
	}

	logger(LOG_DEBUG, "New PAT found");

	last_section = psi_table_get_lastsection(new_pat);
	for(i = 0; i <= last_section; i++) {
		uint8_t *cur = psi_table_get_section(new_pat, i);
		const uint8_t *program;
		int j;

		for(j = 0; (program = pat_get_program(cur, j)); j++) {
			logger(LOG_DEBUG, "%d -> %d", patn_get_program(program),
					patn_get_pid(program));
		}
	}

	// TODO: Doesn't work, yet.
	//psi_table_copy(a->current_pat, new_pat);
	psi_table_free(new_pat);

	return;
}

static void handle_section(struct adapter *a, uint16_t pid, uint8_t *section) {
	uint8_t table_pid = psi_get_tableid(section);
	if(!psi_validate(section)) {
		logger(LOG_NOTICE, "Invalid section on PID %u\n", pid);
	}
	switch(table_pid) {
		case PAT_TABLE_ID:
			pat_handler(a, pid, section);
			break;
		case PMT_TABLE_ID:
			pmt_handler(a, pid, section);
			break;
		default:
			free(section);
	}
}

void handle_input(void *ptr, unsigned char *data, size_t len) {
	struct adapter *a = ptr;
	int i;
	if(len % TS_SIZE) {
		logger(LOG_NOTICE, "Unaligned MPEG-TS packets received, dropping.");
		return;
	}
	for(i=0; i+TS_SIZE < len; i+=TS_SIZE) {
		uint16_t pid = ts_get_pid(data + i);
		uint8_t *cur = data + i;

		if(pid == MAX_PID - 1)
			continue;

		// Mostly c&p from biTstream examples

		if(ts_check_duplicate(ts_get_cc(cur), a->pids[i].last_cc) ||
				!ts_has_payload(cur))
			continue;
		if(ts_check_discontinuity(ts_get_cc(cur), a->pids[i].last_cc))
			psi_assemble_reset(&a->pids[i].psi_buffer, &a->pids[i].psi_buffer_used);

		const uint8_t *payload = ts_section(cur);
		uint8_t length = data + TS_SIZE - payload;

		if(!psi_assemble_empty(&a->pids[i].psi_buffer, &a->pids[i].psi_buffer_used)) {
			uint8_t *section = psi_assemble_payload(&a->pids[i].psi_buffer,
					&a->pids[i].psi_buffer_used, &payload, &length);
			if(section)
				handle_section(a, pid, section);
		}

		payload = ts_next_section(cur);
		length = cur + TS_SIZE - payload;

		while(length) {
			uint8_t *section = psi_assemble_payload(&a->pids[i].psi_buffer,
					&a->pids[i].psi_buffer_used, &payload, &length);
			if(section)
				handle_section(a, pid, section);
		}
	}
}

void register_client(unsigned int sid, void (*cb) (struct evbuffer *, void *), void *ptr) {
	/*
	struct callback *scb = g_slice_alloc(sizeof(struct callback));
	scb->cb = cb;
	scb->ptr = ptr;
	*/
	//g_hash_table_replace(callback, sid, s_slist_prepend(g_hash_table_lookup(callbacks, sid));
	//callbacks[sid] = g_slist_prepend(callbacks[sid], scb);
	return;
}

void unregister_client(unsigned int sid, void (*cb) (struct evbuffer *, void *), void *ptr) {
	//struct callback scb = { cb, ptr };
	//callbacks[sid] = g_slist_remove(callbacks[sid], &scb);
}

void *register_transponder(struct tune s) {
	struct adapter *h = g_slice_alloc(sizeof(struct adapter));
	int i;
	for(i = 0; i < MAX_PID; i++) {
		h->pids[i].refcnt = 0;
		h->pids[i].last_cc = -1;
		h->pids[i].callback = NULL;
		h->pids[i].current_pmt = NULL;
		psi_assemble_init(&h->pids[i].psi_buffer, &h->pids[i].psi_buffer_used);
	}
	psi_table_init(h->current_pat);
	return h;
}

void unregister_transponder(void *handle) {
	struct adapter *h = handle;
	int i;
	for(i = 0; i < MAX_PID; i++) {
		psi_assemble_reset(&h->pids[i].psi_buffer, &h->pids[i].psi_buffer_used);
	}
	psi_table_free(h->current_pat);
	g_slice_free1(sizeof(struct adapter), h);
}

void mpeg_init(void) {
	// Nothing here
}
