%option never-interactive case-insensitive yylineno warn nodefault nounput

%{
#include "config_parser.h"

const char *conffilename = "foo"; // TODO

int init_lexer(void)
{
	yyin = fopen(conffilename, "r");
	if (yyin == NULL) {
		fprintf(stderr, "Unable to open config file %s: %s\n", conffilename,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	return 0;
}
%}

%%

\"[^"\n]+[\"\n]		yytext[yyleng-1] = 0; yylval.text = yytext+1; return STRING;
[0-9]+			yylval.num=atoi(yytext); return NUMBER;

;			return SEMICOLON;
[ \t\r\n]+		;
#.*			;

.			return yytext[0];
