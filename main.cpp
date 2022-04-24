#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <vector>

#include "log.h"
#include "conn.h"
#include "mgr.h"
#include "processpool.h"

using std::vector;

static const char* version = "1.0"; // 静态变量就是唯一的，防止多次创建

static void usage( const char* prog )
{
    log( LOG_INFO, __FILE__, __LINE__,  "usage: %s [-h] [-v] [-f config_file]", prog );				
}
//ANSI C标准中几个标准预定义宏
// __FILE__ :在源文件中插入当前原文件名
// __LINE__ ：在源文件中插入当前源代码行号
// _DATE_: 在源文件插入当前的编译日期
// _TIME_: 在源文件中插入的编译时间
// _STDC_ : 当要求程序严格遵循ANSI C 标准时刻标示被赋值为1
// _cplusplus: 当编写C++程序时该标识符被定义
int main( int argc, char* argv[] )
{
    char cfg_file[1024];						//配置文件
    memset( cfg_file, '\0', 100 );              // 将cfd_file的前100个字节初始化为 \0
    int option;
    // xvf是可选参数
    while ( ( option = getopt( argc, argv, "f:xvh" ) ) != -1 )					//getopt函数用来分析命令行参数
    {
        switch ( option )
        {
            case 'x':
            {
                set_loglevel( LOG_DEBUG );										//log.cpp
                break;
            }
            case 'v':  //版本信息
            {
                log( LOG_INFO, __FILE__, __LINE__, "%s %s", argv[0], version );
                return 0;
            }
            case 'h':  //帮助 help
            {
                usage( basename( argv[ 0 ] ) ); // basename 返回文件名，而不是argv[0]整个文件的绝对路径
                return 0;
            }
            case 'f':
            {   
                // optarg 是 -f 后边的 config_file
                memcpy( cfg_file, optarg, strlen( optarg ) );		// cfg_file  此时的内容是 config.xml  比如运行时：./main -f config.xml
                break;
            }
            case '?':  //无效的参数或者缺少参数的选项值
            {
                log( LOG_ERR, __FILE__, __LINE__, "un-recognized option %c", option );
                usage( basename( argv[ 0 ] ) );
                return 1;
            }
        }
    }    

    // // cfg_file = config.xml
    if( cfg_file[0] == '\0' ) 
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "please specifiy the config file" );
        return 1;
    }
    int cfg_fd = open( cfg_file, O_RDONLY );			//打开配置文件 config.xml， cfg_fd是config.xml文件的文件描述符
    if( !cfg_fd )
    {
        // strerror(errno) 可以解析出errro的具体错误类型
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
    struct stat ret_stat;   // stat 展示文件状况  man stat
    if( fstat( cfg_fd, &ret_stat ) < 0 )            // fstat()用来将参数fildes 所指的文件状态
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
    char* buf = new char [ret_stat.st_size + 1];        
    memset( buf, '\0', ret_stat.st_size + 1 );
    ssize_t read_sz = read( cfg_fd, buf, ret_stat.st_size );			//buf此时的内容是config.xml文件的内容, 
    if ( read_sz < 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
    vector< host > balance_srv;							//host在前面的mgr.h文件中定义   一个是负载均衡服务器
    vector< host > logical_srv;                         //一个是逻辑服务器
    host tmp_host;
    memset( tmp_host.m_hostname, '\0', 1024 );          
    char* tmp_hostname;
    char* tmp_port;
    char* tmp_conncnt;
    bool opentag = false;
    char* tmp = buf;				//此时tem指向config.xml文件的内容
    char* tmp2 = NULL;
    char* tmp3 = NULL;
    char* tmp4 = NULL;
    
    /*
    对config.xml文件的解析
    logical_srv 保存了logical_host信息即网易云服务器
    balance_srv 保存了listen里的信息即本机服务器 (本机服务器做一个中转服务器)
    通过本机服务器 (负载均衡服务器) 去找 真正的逻辑服务器
    */
    while( tmp2 = strpbrk( tmp, "\n" ) )	//在源字符串tmp中找出最先含有搜索字符串"\n"中任一字符的位置并返回，若没找到则返回空指针		
    {    
        *tmp2++ = '\0';             
        if( strstr( tmp, "<logical_host>" ) )		//strstr(str1,str2)函数用于判断字符串str2是否是str1的子串。如果是，则该函数返回str2在str1首次出现的地址，否则返回null
        {
            if( opentag )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            opentag = true;         // 开始读一个host
        }
        else if( strstr( tmp, "</logical_host>" ) )								// 有/	符号	
        {
            if( !opentag )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            // tmp_host.m_hostname = 115.236.121.4
            logical_srv.push_back( tmp_host );
            memset( tmp_host.m_hostname, '\0', 1024 );
            opentag = false;        // 结束读一个host
        }
        else if( tmp3 = strstr( tmp, "<name>" ) )  
        {
            tmp_hostname = tmp3 + 6;				//将tmp_hostname指针指向<name>后面的IP地址的首个地址     <name> 字符串的大小为6
            tmp4 = strstr( tmp_hostname, "</name>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';                           // 将字符串手动结束
            memcpy( tmp_host.m_hostname, tmp_hostname, strlen( tmp_hostname ) );
        }
        else if( tmp3 = strstr( tmp, "<port>" ) )
        {
            tmp_port = tmp3 + 6;
            tmp4 = strstr( tmp_port, "</port>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';                           // 将字符串手动结束
            tmp_host.m_port = atoi( tmp_port );
        }
        else if( tmp3 = strstr( tmp, "<conns>" ) )
        {
            tmp_conncnt = tmp3 + 7;                 //  <conns>的字符大小为7
            tmp4 = strstr( tmp_conncnt, "</conns>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            tmp_host.m_conncnt = atoi( tmp_conncnt );
        }
        else if( tmp3 = strstr( tmp, "Listen" ) )       // 对于第一行 Listen 127.0.0.1:8080
        {
            tmp_hostname = tmp3 + 6;
            tmp4 = strstr( tmp_hostname, ":" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4++ = '\0';
            tmp_host.m_port = atoi( tmp4 );
            memcpy( tmp_host.m_hostname, tmp3, strlen( tmp3 ) );
            balance_srv.push_back( tmp_host );
            memset( tmp_host.m_hostname, '\0', 1024 );
        }
        tmp = tmp2;
    }

    // 两种服务器都不能为空
    if( balance_srv.size() == 0 || logical_srv.size() == 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
        return 1;
    }

    // 只有一个主机地址即负载均衡服务器
    const char* ip = balance_srv[0].m_hostname;			//balance_srv数组里只有一个元素
    int port = balance_srv[0].m_port;

    // listenfd 是主机服务器socket 即 127.0.0.1 8080负载均衡的服务器
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
 
    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int reuse = 1;
    ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));    // 开启端口复用即主动断开连接时避免等待2MSL时间
    assert(ret != -1);

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    /*
    使用网易云的两个服务器的host (IP+Port+Conn) 创建一个进程池 
    */
    processpool< conn, host, mgr >* pool = processpool< conn, host, mgr >::create( listenfd, logical_srv.size() );
    if( pool )
    {   
        // logical_src是一个vector数组，里边保存的是网易云网站的两个服务器
        
        /*
        从这里开始，父进程与子进程都会执行下列的代码
        */
        pool->run( logical_srv ); 
        delete pool;
    }

    close( listenfd );
    return 0;
}
