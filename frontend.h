#ifndef __INCLUDED_GETSH_FRONTEND
#define __INCLUDED_GETSH_FRONTEND

struct tune {
	unsigned int type;
//	union {
		struct {
			unsigned int delivery_system;
			unsigned int symbol_rate;
			unsigned int frequency;
			unsigned int voltage;
		} dvbs;
//	};
};

struct lnb {
	int lof1, lof2, slof;
};

int acquire_frontend(struct tune s);
void release_frontend(struct tune s);
void add_frontend(int adapter, int frontend, struct lnb l);
void remove_frontend(int adapter, int frontend);

#endif
