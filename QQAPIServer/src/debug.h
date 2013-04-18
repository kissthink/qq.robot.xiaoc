#ifndef _DEBUG_H
#define _DEBUG_H

#include <stdio.h>
#include <errno.h>
#include <assert.h>

#define RELEASE

#ifndef RELEASE

#define DBG(args ...) \
	print_error( __FILE__, (char*)__func__, __LINE__, ##args )
#define OPS(args ...) \
    print_msg( __FILE__, (char*)__func__, __LINE__, ##args )

#else

#define DBG(args ...)
#define OPS(args ...)

#endif

#define MSG	printf

void print_error(char* file, char* function, int line, const char *fmt, ...);
void print_msg(char* file, char* function, int line, const char *fmt, ...);
void hex_dump( unsigned char * buf, int len );
void debug_term_on();
void debug_term_off();
void debug_file_on();
void debug_file_off();
void debug_set_dir(char* str);
void api_ops_on();
void api_ops_off();

#endif //_DEBUG_H

