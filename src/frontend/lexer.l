%option noyywrap nounput noinput
%{
#include <cstdlib>
#include <cstring>
#include "parser.tab.hpp"
%}

%%

[ \t\r\n]+        ;
"//"[^\n]*        ;
"/*"([^*]|\*+[^*/])*\*+"/" ;

"const"           return CONST;
"int"             return INT;
"void"            return VOID;
"if"              return IF;
"else"            return ELSE;
"while"           return WHILE;
"break"           return BREAK;
"continue"        return CONTINUE;
"return"          return RETURN;

"<="              return LE;
">="              return GE;
"=="              return EQ;
"!="              return NE;
"&&"              return LAND;
"||"              return LOR;

[0-9]+            { yylval.ival = static_cast<int>(std::strtol(yytext, nullptr, 10)); return NUMBER; }
[_A-Za-z][_A-Za-z0-9]* { yylval.str = strdup(yytext); return ID; }

.                 return yytext[0];

%%
