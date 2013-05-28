%error-verbose

%{
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "http.h"
#include "frontend.h"
#include "channels.h"

extern FILE *yyin;
extern int yylineno;
extern int yylex(void);
extern char *logfile;
extern int use_syslog;
extern int loglevel;

/* Temporary variables needed while parsing */
static struct lnb l;
static int adapter = -1;

void yyerror(const char *str)
{
	fprintf(stderr, "Parse error on line %d: %s\n", yylineno, str);
	exit(EXIT_FAILURE);
}

static void parse_error(char *text, ...) 
{
	static char error[1024];
	va_list args;
	va_start(args, text);
	vsnprintf(error, sizeof(error), text, args);
	va_end(args);
	yyerror(error);
}

int yywrap()
{
	fclose(yyin);
        return 1;
}

void init_parser() {
}

%}

%union
{
	char *text;
	int num;
};

%token<text> STRING
%token<num> NUMBER
%token<num> YESNO
%token SEMICOLON HTTPLISTEN FRONTEND ADAPTER LOF1 LOF2 SLOF CHANNELSCONF
%token LOGFILE USESYSLOG LOGLEVEL

%%

statements: 
		    | statements statement SEMICOLON;
statement: http | frontend | channels | logfile | syslog | loglevel;

loglevel: LOGLEVEL NUMBER {
	loglevel = $2;
	if(loglevel < 0 || loglevel > 5) {
		parse_error("Loglevel must be between 0 and 5.");
		exit(EXIT_FAILURE);
	}
}


logfile: LOGFILE STRING {
	logfile = strdup($2);
}

syslog: USESYSLOG YESNO {
	use_syslog = $2;
}

http: HTTPLISTEN NUMBER {
	struct evhttp_bound_socket *handle = evhttp_bind_socket_with_handle(httpd, "::", $2);
	if(handle == NULL) {
		fprintf(stderr, "Unable to bind to port %d. Exiting\n", $2);
		exit(EXIT_FAILURE);
	}
}

channels: CHANNELSCONF STRING {
	if(parse_channels($2)) {
		parse_error("parse_channels() failed");
		exit(EXIT_FAILURE);
	}
}

frontend: FRONTEND '{' frontendoptions '}' {
	if(adapter == -1)
		parse_error("frontend block needs an adapter number");
	/* Default Universal LNB */
	if(!l.lof1)
		l.lof1 = 9750000;
	if(!l.lof2)
		l.lof2 = 10600000;
	if(!l.slof)
		l.slof = 11700000;
	frontend_add(adapter, 0, l);
	adapter = -1;
}
frontendoptions: | frontendoptions frontendoption;
frontendoption: adapter | lof1 | lof2 | slof;
adapter: ADAPTER NUMBER SEMICOLON {
	adapter = $2;
}
lof1: LOF1 NUMBER SEMICOLON {
	l.lof1 = $2;
}
lof2: LOF2 NUMBER SEMICOLON {
	l.lof2 = $2;
}
slof: SLOF NUMBER SEMICOLON {
	l.slof = $2;
}
