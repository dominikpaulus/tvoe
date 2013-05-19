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
	int sid, refcnt;
	bool deleted;
	void *ptr;
	void (*cb) (struct evbuffer *, void *);
};
struct pid_info {
	int8_t last_cc;
	uint8_t *psi_buffer;
	uint16_t psi_buffer_used;
	uint8_t *current_pmt;
	GSList *callback;
};
struct transponder {
	PSI_TABLE_DECLARE(current_pat);
	uint8_t pid0_cc;
	uint16_t tsid, nitid;
	struct pid_info pids[MAX_PID];
};
static GSList *clients, *transponders;

static void output_psi_section(struct client *c, uint8_t *section, uint16_t pid, uint8_t *cc) {
    uint16_t section_length = psi_get_length(section) + PSI_HEADER_SIZE;
    uint16_t section_offset = 0;
	struct evbuffer *ev = evbuffer_new();
    do {
        uint8_t ts[TS_SIZE];
        uint8_t ts_offset = 0;
        memset(ts, 0xff, TS_SIZE);

        psi_split_section(ts, &ts_offset, section, &section_offset);

        ts_set_pid(ts, pid);
        ts_set_cc(ts, *cc);
        (*cc)++;
        *cc &= 0xf;

        if (section_offset == section_length)
            psi_split_end(ts, &ts_offset);


		evbuffer_add(ev, ts, TS_SIZE);
		c->cb(ev, c->ptr);
    } while (section_offset < section_length);
	evbuffer_free(ev);
}

static void send_pat(struct transponder *a, struct client *c, uint16_t sid, uint16_t pid) {
	uint8_t *pat = psi_allocate();
	uint8_t *pat_n, j = 0;

    // Generate empty PAT
    pat_init(pat);
    pat_set_length(pat, 0);
    pat_set_tsid(pat, a->tsid);
    psi_set_version(pat, 0);
    psi_set_current(pat);
    psi_set_section(pat, 0);
    psi_set_lastsection(pat, 0);
    psi_set_crc(pat);
    output_psi_section(c, pat, PAT_PID, &a->pid0_cc);

    // Increase PAT version
    psi_set_version(pat, 1);
    psi_set_current(pat);
    psi_set_crc(pat);
    output_psi_section(c, pat, PAT_PID, &a->pid0_cc);

    psi_set_version(pat, 2);
    psi_set_current(pat);
    psi_set_length(pat, PSI_MAX_SIZE);

    pat_n = pat_get_program(pat, j++);
    patn_init(pat_n);
    patn_set_program(pat_n, 0);
    patn_set_pid(pat_n, a->nitid);

    pat_n = pat_get_program(pat, j++);
    patn_init(pat_n);
    patn_set_program(pat_n, sid);
    patn_set_pid(pat_n, pid);

    // Set correct PAT length
    pat_n = pat_get_program(pat, j); // Get offset of the end of last program
    pat_set_length(pat, pat_n - pat - PAT_HEADER_SIZE);
    psi_set_crc(pat);

	output_psi_section(c, pat, PAT_PID, &a->pid0_cc);

    free(pat);
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
	// Loop over all elementary streams for this SID
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
		//logger(LOG_NOTICE, "Invalid PAT received on PID %u", pid);
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

		a->tsid = pat_get_tsid(cur);

		/* 
		 * For every proram in this PAT, check whether we have clients
		 * that request it. Add callbacks for them, if necessary.
		 */
		for(j = 0; (program = pat_get_program(cur, j)); j++) {
			GSList *it;
			uint16_t cur_sid = patn_get_program(program);

			if(cur_sid == 0)
				a->nitid = patn_get_pid(program);

			for(it = clients; it != NULL; it = g_slist_next(it)) {
				struct client *c = it->data;
				if(c->sid != cur_sid && cur_sid != 0)
					continue;

				// Send new PAT to this client
				if(cur_sid != 0)
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
		//logger(LOG_NOTICE, "Invalid section on PID %u\n", pid);
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
	struct evbuffer *out;

	int i;
	if(len % TS_SIZE) {
		logger(LOG_NOTICE, "Unaligned MPEG-TS packets received, dropping.");
		return;
	}

	out = evbuffer_new();
	for(i=0; i < len; i+=TS_SIZE) {
		uint8_t *cur = data + i;
		uint16_t pid = ts_get_pid(cur);
		GSList *it;

		if(pid >= MAX_PID - 1)
			continue;

		// Mostly c&p from biTstream examples

		if(ts_check_duplicate(ts_get_cc(cur), a->pids[pid].last_cc) ||
				!ts_has_payload(cur))
			continue;
		if(ts_check_discontinuity(ts_get_cc(cur), a->pids[pid].last_cc))
			psi_assemble_reset(&a->pids[pid].psi_buffer, &a->pids[pid].psi_buffer_used);

		// Send packet to clients
		//logger(LOG_DEBUG, "%d", pid);
		for(it = a->pids[pid].callback; it != NULL; it = g_slist_next(it)) {
			//if(pid == 100)
			//	logger(LOG_DEBUG, "PID %d CC: %d", pid, ts_get_cc(cur));
			struct client *c = it->data;
			evbuffer_add(out, cur, TS_SIZE);
			c->cb(out, c->ptr);
			//logger(LOG_DEBUG, "Packet out");
		}
	
		//if(pid == 100)
		//	logger(LOG_DEBUG, "Still there");

		a->pids[pid].last_cc = ts_get_cc(cur);

		const uint8_t *payload = ts_section(cur);
		uint8_t length = data + TS_SIZE - payload;

		if(!psi_assemble_empty(&a->pids[pid].psi_buffer, &a->pids[pid].psi_buffer_used)) {
			uint8_t *section = psi_assemble_payload(&a->pids[pid].psi_buffer,
					&a->pids[pid].psi_buffer_used, &payload, &length);
			if(section)
				handle_section(a, pid, section);
		}

		payload = ts_next_section(cur);
		length = cur + TS_SIZE - payload;

		while(length) {
			uint8_t *section = psi_assemble_payload(&a->pids[pid].psi_buffer,
					&a->pids[pid].psi_buffer_used, &payload, &length);
			if(section)
				handle_section(a, pid, section);
		}
	}

	evbuffer_free(out);
}

void *register_client(unsigned int sid, void (*cb) (struct evbuffer *, void *), void *ptr) {
	struct client *scb = g_slice_alloc(sizeof(struct client));
	scb->cb = cb;
	scb->ptr = ptr;
	scb->sid = sid;
	scb->refcnt = 0;
	scb->deleted = false;
	clients = g_slist_prepend(clients, scb);
	return scb;
}

void unregister_client(void *ptr) {
	struct client *scb = ptr;
	clients = g_slist_remove(clients, scb);
	// TODO
}

void *register_transponder(void) {
	struct transponder *h = g_slice_alloc(sizeof(struct transponder));
	int i;
	for(i = 0; i < MAX_PID; i++) {
		h->pids[i].last_cc = -1;
		h->pids[i].callback = NULL;
		h->pids[i].current_pmt = NULL;
		psi_assemble_init(&h->pids[i].psi_buffer, &h->pids[i].psi_buffer_used);
	}
	h->pid0_cc = -1;
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
