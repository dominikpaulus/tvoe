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
struct client {
	int sid;
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
struct transponder {
	PSI_TABLE_DECLARE(current_pat);
	struct pid_info pids[MAX_PID];
};
static GSList *clients, *transponders;

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

static void send_pat(struct transponder *a, struct client *c, uint16_t sid, uint16_t pid) {

}

/*
 * Process a new parsed PMT. Map PIDs to corresponding SIDs.
 * @param p Pointer to struct pmt_handle
 */
static void pmt_handler(struct transponder *a, uint16_t pid, uint8_t *section) {
//	uint16_t sid = pmt_get_program(section);
	int j;

	if(!pmt_validate(section)) {
		logger(LOG_NOTICE, "Invalid PMT received on PID %u", pid);
		free(section);
		return;
	}

	/*
	if(a->pids[pid].current_pmt &&
			psi_compare(a->pids[pid].current_pmt, section)) {
		free(section);
		return;
	}
	logger(LOG_DEBUG, "New PMT for SID %d found", pid);
	*/

	uint8_t *es;
	for(j = 0; (es = pmt_get_es(section, j)); j++) {
		GSList *it = a->pids[pid].callback; // PMT callbacks
		// Register callbacks for all clients that subscribed to this
		// program on all PIDs referenced by it
		for(; it != NULL; it = g_slist_next(it)) {
			// ES callbacks
			GSList *it2 = a->pids[pmtn_get_pid(es)].callback;
			for(; it2 != NULL; it2 = g_slist_next(it2)) {
				if(it2->data == it->data)
					break;
			}
			if(it2) // Client already registered
				continue;
			a->pids[pmtn_get_pid(es)].callback =
				g_slist_prepend(a->pids[pmtn_get_pid(es)].callback, it->data);
		}
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
static void pat_handler(struct transponder *a, uint16_t pid, uint8_t *section) {
	PSI_TABLE_DECLARE(new_pat);
	uint8_t last_section;
	int i;

	if(!pat_validate(section)) {
		logger(LOG_NOTICE, "Invalid PAT received on PID %u", pid);
		free(section);
		return;
	}

	psi_table_init(new_pat);
	if(!psi_table_section(new_pat, section)) {
		psi_table_free(new_pat);
		return;
	}

	/*
	// Don't re-parse already known PATs
	if(psi_table_validate(a->current_pat) &&
			psi_table_compare(a->current_pat, new_pat)) {
		psi_table_free(new_pat);
		psi_table_init(new_pat);
		return;
	}
	logger(LOG_DEBUG, "New PAT found");
	*/

	last_section = psi_table_get_lastsection(new_pat);
	for(i = 0; i <= last_section; i++) {
		uint8_t *cur = psi_table_get_section(new_pat, i);
		const uint8_t *program;
		int j;

		/* 
		 * For every proram in this PAT, check whether we have clients
		 * that request it. Add callbacks for them, if necessary.
		 */
		for(j = 0; (program = pat_get_program(cur, j)); j++) {
			GSList *it;
			uint16_t cur_sid = patn_get_program(program);

			for(it = clients; it != NULL; it = g_slist_next(it)) {
				struct client *c = it->data;
				if(c->sid != cur_sid)
					continue;

				// Send new PAT to this client
				send_pat(a, c, cur_sid, patn_get_pid(program));

				/* Check whether callback for PMT is already installed */
				GSList *it2;
				for(it2 = a->pids[patn_get_pid(program)].callback;
						it2 != NULL; it2 = g_slist_next(it2)) {
					if(it2->data == c)
						break;
				}
				if(it2) // Callback already registered
					continue;
				a->pids[patn_get_pid(program)].callback =
					g_slist_prepend(a->pids[patn_get_pid(program)].callback, c);
			}
/*			logger(LOG_DEBUG, "%d -> %d", patn_get_program(program),
					patn_get_pid(program)); */
		}
	}

	psi_table_free(a->current_pat);
	psi_table_copy(a->current_pat, new_pat);

	return;
}

static void handle_section(struct transponder *a, uint16_t pid, uint8_t *section) {
	uint8_t table_pid = psi_get_tableid(section);
	if(!psi_validate(section)) {
		logger(LOG_NOTICE, "Invalid section on PID %u\n", pid);
		free(section);
		return;
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
	struct transponder *a = ptr;
	struct evbuffer *out = evbuffer_new();

	int i;
	if(len % TS_SIZE) {
		logger(LOG_NOTICE, "Unaligned MPEG-TS packets received, dropping.");
		evbuffer_free(out);
		return;
	}

	for(i=0; i+TS_SIZE < len; i+=TS_SIZE) {
		uint16_t pid = ts_get_pid(data + i);
		uint8_t *cur = data + i;
		GSList *it;

		if(pid >= MAX_PID - 1)
			continue;

		// Send packet to clients
		//logger(LOG_DEBUG, "%d", pid);
		for(it = a->pids[pid].callback; it != NULL; it = g_slist_next(it)) {
			struct client *c = it->data;
			evbuffer_add(out, cur, TS_SIZE);
			c->cb(out, c->ptr);
			//logger(LOG_DEBUG, "Packet out");
		}

		// Mostly c&p from biTstream examples

		if(ts_check_duplicate(ts_get_cc(cur), a->pids[i].last_cc) ||
				!ts_has_payload(cur))
			continue;
		if(ts_check_discontinuity(ts_get_cc(cur), a->pids[i].last_cc))
			psi_assemble_reset(&a->pids[i].psi_buffer, &a->pids[i].psi_buffer_used);

		a->pids[i].last_cc = ts_get_cc(cur);

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

	evbuffer_free(out);
}

void *register_client(unsigned int sid, void (*cb) (struct evbuffer *, void *), void *ptr) {
	struct client *scb = g_slice_alloc(sizeof(struct client));
	//GSList *it;
	scb->cb = cb;
	scb->ptr = ptr;
	scb->sid = sid;
	clients = g_slist_prepend(clients, scb);

/*
	for(it = transponders; it != NULL; it = g_slist_next(it)) {
		struct transponder *a = it->data;
		uint8_t *prog;

		if(!psi_table_validate(a->current_pat) ||
				!pat_table_find_program(a->current_pat, sid))
			continue;
		uint16_t i = patn_get_pid(pat_table_find_program(a->current_pat, sid));
		a->pids[i].callback = g_slist_prepend(a->pids[i].callback, scb);
		logger(LOG_DEBUG, "Assigned! PID: %d", i);
	}
*/
	return scb;
}

void unregister_client(void *ptr) {

	//struct callback scb = { cb, ptr };
	//callbacks[sid] = g_slist_remove(callbacks[sid], &scb);
}

void *register_transponder(struct tune s) {
	struct transponder *h = g_slice_alloc(sizeof(struct transponder));
	int i;
	for(i = 0; i < MAX_PID; i++) {
		h->pids[i].refcnt = 0;
		h->pids[i].last_cc = -1;
		h->pids[i].callback = NULL;
		h->pids[i].current_pmt = NULL;
		psi_assemble_init(&h->pids[i].psi_buffer, &h->pids[i].psi_buffer_used);
	}
	psi_table_init(h->current_pat);
	transponders = g_slist_prepend(transponders, h);
	return h;
}

void unregister_transponder(void *handle) {
	struct transponder *h = handle;
	int i;
	for(i = 0; i < MAX_PID; i++) {
		psi_assemble_reset(&h->pids[i].psi_buffer, &h->pids[i].psi_buffer_used);
	}
	psi_table_free(h->current_pat);
	transponders = g_slist_remove(transponders, h);
	g_slice_free1(sizeof(struct transponder), h);
}
