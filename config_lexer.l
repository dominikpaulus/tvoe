%option never-interactive case-insensitive yylineno warn nodefault nounput

%{
#define YY_NO_INPUT
#include "config_parser.h"

extern const char *conffile;

int init_lexer(void)
{
	yyin = fopen(conffile, "r");
	if (yyin == NULL) {
		fprintf(stderr, "Unable to open config file %s: %s\n", conffile,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	return 0;
}
%}

%%

\"[^"\n]+[\"\n]	yytext[yyleng-1] = 0; yylval.text = yytext+1; return STRING;
yes|no			yylval.num=!strcmp(yytext,"yes"); return YESNO;
[0-9]+			yylval.num=atoi(yytext); return NUMBER;

http-listen	return HTTPLISTEN;
frontend	return FRONTEND;
adapter		return ADAPTER;
lof1		return LOF1;
lof2		return LOF2;
slof		return SLOF;
channels	return CHANNELSCONF;
logfile		return LOGFILE;
use_syslog	return USESYSLOG;
loglevel	return LOGLEVEL;

;			return SEMICOLON;
[ \t\r\n]+		;
#.*			;

.			return yytext[0];
