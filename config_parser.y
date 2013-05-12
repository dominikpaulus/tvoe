%error-verbose

%{
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include "http.h"

extern FILE *yyin;
extern int yylineno;

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
%token SEMICOLON HTTPLISTEN FRONTEND ADAPTER LOF1 LOF2 SLOF CHANNELSCONF

%%

statements: 
		    | statements statement SEMICOLON;
statement: http;

http: HTTPLISTEN NUMBER {
	struct evhttp_bound_socket *handle = evhttp_bind_socket_with_handle(httpd, "::", $2);
	if(handle == NULL) {
		fprintf(stderr, "Unable to bind to port %d. Exiting\n", $2);
		exit(EXIT_FAILURE);
	}
}
