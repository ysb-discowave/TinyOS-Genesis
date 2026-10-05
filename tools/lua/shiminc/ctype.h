/* TinyOS 裸机 shim：ctype.h（只实现 Lua 用到的几个） */
#ifndef TINYOS_SHIM_CTYPE_H
#define TINYOS_SHIM_CTYPE_H

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int ispunct(int c);
int iscntrl(int c);
int isgraph(int c);
int isblank(int c);
int tolower(int c);
int toupper(int c);

#endif
