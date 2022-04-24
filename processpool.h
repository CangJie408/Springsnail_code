#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

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
#include "fdwrapper.h"

using std::vector;

//子进程类
class process
{
public:
    process() : m_pid( -1 ){}

public:
    int m_busy_ratio;						//给每台实际处理服务器（业务逻辑服务器）分配一个加权比例
    pid_t m_pid;       //目标子进程的PID
    int m_pipefd[2];   //父进程和子进程通信用的管道 即 父进程是主机服务器，子进程是网易云服务器
};

template< typename C, typename H, typename M >
class processpool
{
private:
    processpool( int listenfd, int process_number = 8 );
public:
    // process_number是网易云网站的服务器 IP+Port+connect次数
    // static可以保证创建的实例唯一
    static processpool< C, H, M >* create( int listenfd, int process_number = 8 )
    {
        if( !m_instance )  //单例模式
        {
            m_instance = new processpool< C, H, M >( listenfd, process_number );
        }
        return m_instance;
    }
    ~processpool()
    {
        delete [] m_sub_process;
    }
    //启动进程池
    void run( const vector<H>& arg );

private:
    void notify_parent_busy_ratio( int pipefd, M* manager );  //获取目前连接数量，将其发送给父进程
    int get_most_free_srv();  //找出最空闲的服务器
    void setup_sig_pipe(); //统一事件源
    void run_parent();
    void run_child( const vector<H>& arg );

private:
    static const int MAX_PROCESS_NUMBER = 16;   //进程池允许最大进程数量
    static const int USER_PER_PROCESS = 65536;  //每个子进程最多能处理的客户数量
    static const int MAX_EVENT_NUMBER = 10000;  //EPOLL最多能处理的的事件数
    int m_process_number;  //进程池中的进程总数
    int m_idx;  //子进程在池中的序号（从0开始）
    int m_epollfd;  //当前进程的epoll内核事件表fd
    int m_listenfd;  //监听socket
    int m_stop;      //子进程通过m_stop来决定是否停止运行
    process* m_sub_process;  //保存所有子进程的描述信息
    static processpool< C, H, M >* m_instance;  //进程池静态实例
};
template< typename C, typename H, typename M >
processpool< C, H, M >* processpool< C, H, M >::m_instance = NULL;

static int EPOLL_WAIT_TIME = 5000;
static int sig_pipefd[2];  //用于处理信号的管道，以实现统一事件源,后面称之为信号管道
static void sig_handler( int sig )  // 时间处理函数，将捕获的信号通过sig_pipefd发送给调用的进程
{
    int save_errno = errno;
    int msg = sig;
    send( sig_pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

static void addsig( int sig, void( handler )(int), bool restart = true )    // 设置信号处理函数
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;  //重新调用被该信号终止的系统调用
    }
    /*
    函数sigfillset初始化set所指向的信号集，使其中所有信号的对应bit置位，
    表示该信号集的有效信号包括系统支持的所有信号
    */
    sigfillset( &sa.sa_mask ); 
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

/*
创建m_instance对象时的构造函数
*/
template< typename C, typename H, typename M >
processpool< C, H, M >::processpool( int listenfd, int process_number ) 
    : m_listenfd( listenfd ), m_process_number( process_number ), m_idx( -1 ), m_stop( false )
{
    assert( ( process_number > 0 ) && ( process_number <= MAX_PROCESS_NUMBER ) );

    /*
    根据网易云服务器的数量创建了两个process

    class process
    {
    public:
        process() : m_pid( -1 ){}   // 构造函数

    public:
        int m_busy_ratio;  // 给每台实际处理服务器（业务逻辑服务器）分配一个加权比例                             
        pid_t m_pid;       // 目标子进程的PID
        int m_pipefd[2];   // 父进程和子进程通信用的管道
    };
    */
    m_sub_process = new process[ process_number ];
    assert( m_sub_process );

    for( int i = 0; i < process_number; ++i )
    {
        /*
        socketpair函数建立一对匿名的已连接的套接字，建立的两个套接字描述符会放在sv[0]和sv[1]中。
        既可以从sv[0]写入sv[1]读出，又可以从sv[1]读入sv[0]写出，
        如果没有写入就读出则会生阻塞。用途：用来创建全双工通道，不过只局限于父子进程之间。
        */
        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd );
        assert( ret == 0 );

        /*
        fork一旦创建出来，它就会沿着此处一直往下执行，直到整个子进程结束等待父进程回收，
        也就是main函数里的pool->run( logical_srv )语句可能由父进程执行，也可能由子进程执行，
        因此下边的代码中才会由run_child 与 run->parent
        */
        m_sub_process[i].m_pid = fork();    // 创建第 i 个 子进程，父进程是整个最大的进程
        assert( m_sub_process[i].m_pid >= 0 );
        if( m_sub_process[i].m_pid > 0 )  //父进程，其实就是整个大的进程与两个网易云服务器进程之间的通信
        {
            close( m_sub_process[i].m_pipefd[1] );
            m_sub_process[i].m_busy_ratio = 0;
            continue;
        }
        else   //子进程的执行过程
        {
            close( m_sub_process[i].m_pipefd[0] );
            m_idx = i;   //该子进程对应的下标
            break;
        }
    }
}

/*
获取空闲的连接（该连接在run->child()内，初始化mgr的时候已经创建好）
*/
template< typename C, typename H, typename M >
int processpool< C, H, M >::get_most_free_srv()
{
    // m_busy_ratio：每台实际处理服务器的一个加权比例
    int ratio = m_sub_process[0].m_busy_ratio;
    int idx = 0;
    for( int i = 0; i < m_process_number; ++i )
    {
        // 谁的任务数少 (多个客户端需要连接网易云服务器，因此考虑负载) ，那谁比较空闲
        if( m_sub_process[i].m_busy_ratio < ratio )
        {
            idx = i;
            ratio = m_sub_process[i].m_busy_ratio;  // 这里的idx与ratio都是父进程中的变量
        }
    }
    return idx;
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::setup_sig_pipe()  //统一事件源
{
    m_epollfd = epoll_create( 5 );
    assert( m_epollfd != -1 );

    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );   //全双工管道
    assert( ret != -1 );

    setnonblocking( sig_pipefd[1] );  //非阻塞写
    add_read_fd( m_epollfd, sig_pipefd[0] ); //监听管道读端并设置为非阻塞

    addsig( SIGCHLD, sig_handler );  //子进程状态发生变化（退出或暂停）
    addsig( SIGTERM, sig_handler );  //终止进程,kill命令默认发送的即为SIGTERM
    addsig( SIGINT, sig_handler );   //键盘输入中断进程（Ctrl + C）
    addsig( SIGPIPE, SIG_IGN );      /*往被关闭的文件描述符中写数据时触发会使程序退出
                                       SIG_IGN可以忽略，在write的时候返回-1,
                                       errno设置为SIGPIPE*/
}

/*
arg = logical_src即网易云网站的两个服务器
*/
template< typename C, typename H, typename M >
void processpool< C, H, M >::run( const vector<H>& arg )
{
    if( m_idx != -1 )
    {
        run_child( arg );
        return;
    }
    run_parent();
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::notify_parent_busy_ratio( int pipefd, M* manager )
{
    int msg = manager->get_used_conn_cnt();
    send( pipefd, ( char* )&msg, 1, 0 );    
}

/*
arg = logical_srv 即网易云网站的两个服务器
*/
template< typename C, typename H, typename M >
void processpool< C, H, M >::run_child( const vector<H>& arg )
{
    setup_sig_pipe();   // 注册统一事件源

    int pipefd_read = m_sub_process[m_idx].m_pipefd[1]; // 网易云的第 m_idx 个服务器
    add_read_fd( m_epollfd, pipefd_read );  // 监听管道读端并设置为非阻塞

    epoll_event events[ MAX_EVENT_NUMBER ]; 

    /*
    和网易云服务端建立连接同时返回socket描述符
    此处实例化一个mgr类的对象

    一个子进程一个manager负责关闭连接
    */
    M* manager = new M( m_epollfd, arg[m_idx] ); 
    assert( manager );

    int number = 0;
    int ret = -1;

    // 子进程通过m_stop来决定是否停止运行
    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );    // 监听m_epollfd上是否有事件
        if ( ( number < 0 ) && ( errno != EINTR ) ) // 错误处理
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        if( number == 0 )           // 在Epoll_Wait_Time指定事件内没有事件到达时返回0
        {
            // 从m_freed中回收连接 (由于连接已经被关闭，因此还要调用conn2srv() )放到m_conn中
            manager->recycle_conns();
            continue;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;

            if( ( sockfd == pipefd_read ) && ( events[i].events & EPOLLIN ) )  //是父进程发送的消息（通知有新的客户连接到来）
            {
                // run->parent 有新的连接会往 m_pipefd写连接
                int client = 0;
                ret = recv( sockfd, ( char* )&client, sizeof( client ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 ) // recv失败
                {
                    continue;
                }
                else
                {
                    // 接受到了数据
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof( client_address );
                    
                    /*
                    listenfd 是主机服务器socket 即 127.0.0.1 8080负载均衡的服务器
                    */
                    int connfd = accept( m_listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                    if ( connfd < 0 )
                    {
                        log( LOG_ERR, __FILE__, __LINE__, "errno: %s", strerror( errno ) );
                        continue;
                    }
                    add_read_fd( m_epollfd, connfd );   // 将客户端文件描述符connfd上的可读事件加入内核时间表
                    C* conn = manager->pick_conn( connfd ); // 获取一个空闲的连接
                    if( !conn )
                    {
                        closefd( m_epollfd, connfd );
                        continue;
                    }
                    conn->init_clt( connfd, client_address );   // 初始化客户端信息
                    notify_parent_busy_ratio( pipefd_read, manager );
                }
            }
            //处理自身进程接收到的信号
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 ) //等收集退出的子进程，由于设置了WNOHANG因此不等待
                                {
                                    continue;
                                }
                                break;
                            }
                            case SIGTERM:  //退出该进程
                            case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )   // 有sockfd上有数据可读
            {
                 RET_CODE result = manager->process( sockfd, READ );
                 switch( result )
                 {
                     case CLOSED:
                     {
                        // 修改ratio，msg为当前任务数
                        notify_parent_busy_ratio( pipefd_read, manager );
                        break;
                     }
                     default:
                         break;
                 }
            }
            else if( events[i].events & EPOLLOUT )  // 有事件可写 (只有sockfd写缓冲满了或者某个sockfd注册了EPOLLOUT才会触发)
            {
                 RET_CODE result = manager->process( sockfd, WRITE );
                 switch( result )   // 根据返回的状态进行处理
                 {
                     case CLOSED:
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else
            {
                continue;
            }
        }
    }

    close( pipefd_read );
    close( m_epollfd );
}

/*
父进程执行 run
*/
template< typename C, typename H, typename M >
void processpool< C, H, M >::run_parent()
{
    setup_sig_pipe();

    /*
    父进程与子进程的m_epollfd是读共享，写复制，因此它们的m_epollfd是不同的
    */
    for( int i = 0; i < m_process_number; ++i )
    {
        add_read_fd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] );
    }

    add_read_fd( m_epollfd, m_listenfd );   // m_listenfd是主机服务器即balance_srv服务器的socket

    epoll_event events[ MAX_EVENT_NUMBER ];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;

            /*
            有新的客户端需要连接了 nc localhost 8080 创建客户端
            */
            if( sockfd == m_listenfd )
            {
                /*
                int i =  sub_process_counter;
                do
                {
                    if( m_sub_process[i].m_pid != -1 )
                    {
                        break;
                    }
                    i = (i+1)%m_process_number;
                }
                while( i != sub_process_counter );
                
                if( m_sub_process[i].m_pid == -1 )
                {
                    m_stop = true;
                    break;
                }
                sub_process_counter = (i+1)%m_process_number;
                */
                int idx = get_most_free_srv();  //获取空闲的连接（该连接在run->child()内，初始化mgr的时候已经创建好）
                /*
                在 run->child中
                将一个客户端与网易云服务器进行连接，主机服务器即blance_srv只是负责中转
                */
                send( m_sub_process[idx].m_pipefd[0], ( char* )&new_conn, sizeof( new_conn ), 0 ); 
                log( LOG_INFO, __FILE__, __LINE__, "send request to child %d", idx );     //通知子进程客户的连接请求
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                // 主机服务器即blance_srv收到信号通知
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    // 、一次性收到了好多的信号
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD:   // 子进程执行完毕，等待父进程回收，给父进程发送了一个SIGCHLD信号
                            {
                                pid_t pid;
                                int stat;
                                /*
                                waitpid 函数：参数 -1 意味着等待任意一个子进程，参数 WHONAG 意味着没有子进程立即返回
                                返回值：成功返回进程ID，失败返回-1
                                */
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    for( int i = 0; i < m_process_number; ++i )
                                    {
                                        if( m_sub_process[i].m_pid == pid )
                                        {
                                            /*
                                            针对某一个子进程关闭它的 m_pid 与 通信读管道
                                            */
                                            log( LOG_INFO, __FILE__, __LINE__, "child %d join", i );
                                            close( m_sub_process[i].m_pipefd[0] );
                                            m_sub_process[i].m_pid = -1;
                                        }
                                    }
                                }
                                m_stop = true;  // 子进程通过m_stop来决定是否停止运行
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    if( m_sub_process[i].m_pid != -1 )
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:   // 终止进程,kill命令默认发送的即为SIGTERM
                            case SIGINT:    // 键盘输入 (ctrl + c) 中断进程
                            {
                                log( LOG_INFO, __FILE__, __LINE__, "%s", "kill all the clild now" );
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    int pid = m_sub_process[i].m_pid;
                                    if( pid != -1 )
                                    {
                                        kill( pid, SIGTERM );   // 使用kill 杀死进程
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )   // 这里是什么信息的读
            {
                /*
                父进程和子进程通信用的管道 即 父进程是主机服务器，子进程是网易云服务器
                修改busy_ratio
                */
                int busy_ratio = 0;
                ret = recv( sockfd, ( char* )&busy_ratio, sizeof( busy_ratio ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;
                }
                for( int i = 0; i < m_process_number; ++i )
                {
                    if( sockfd == m_sub_process[i].m_pipefd[0] )
                    {
                        m_sub_process[i].m_busy_ratio = busy_ratio;
                        break;
                    }
                }
                continue;
            }
        }
    }

    for( int i = 0; i < m_process_number; ++i )
    {
        closefd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] );
    }
    close( m_epollfd );
}

#endif
