%error-verbose

%{
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>

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
%}

%union
{
	char *text;
	int num;
};

%token SEMICOLON STRING NUMBER

%%

statements: 
		    | statements statement SEMICOLON;
statement: ;
