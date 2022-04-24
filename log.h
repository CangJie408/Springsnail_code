#ifndef LOG_H
#define LOG_H

#include <syslog.h>
#include <cstdarg>					//可变参数

/*
设置日志的级别
宏定义：LOG_DEBUG 来自头文件 syslog.h
*/
void set_loglevel( int log_level = LOG_DEBUG );  

// printf: 输出日志信息
void log( int log_level, const char* file_name, int line_num, const char* format, ... );

#endif
