#include <exception>
#include <errno.h>
#include <string.h>
#include "conn.h"
#include "log.h"
#include "fdwrapper.h"

/*
首先谁是客户端与服务端：
理论上：整个环节应该是主机服务器即负载均衡服务器与逻辑服务器连接，然后再使用客户端 (nc localhost 8080) 进行连接
*/
conn::conn()
{
    m_srvfd = -1;
    m_clt_buf = new char[ BUF_SIZE ];				// 客户端缓冲区
    if( !m_clt_buf )
    {
        throw std::exception();
    }
    m_srv_buf = new char[ BUF_SIZE ];				// 服务端缓冲区
    if( !m_srv_buf )
    {
        throw std::exception();
    }
    reset();
}

conn::~conn()
{
    delete [] m_clt_buf;
    delete [] m_srv_buf;
}

//初始化客户端地址 
void conn::init_clt( int sockfd, const sockaddr_in& client_addr )				//客户端socket 地址
{
    m_cltfd = sockfd;               // 客户端fd
    m_clt_address = client_addr;    //  客户端address
}

//初始化服务器端地址
void conn::init_srv( int sockfd, const sockaddr_in& server_addr )				//服务器端socket地址
{
    m_srvfd = sockfd;               // 服务端fd
    m_srv_address = server_addr;    // 服务端address
}

//重置读写缓冲
void conn::reset()
{
    m_clt_read_idx = 0;
    m_clt_write_idx = 0;
    m_srv_read_idx = 0;
    m_srv_write_idx = 0;
    m_srv_closed = false;
    m_cltfd = -1;
    memset( m_clt_buf, '\0', BUF_SIZE );    // 重置缓冲区
    memset( m_srv_buf, '\0', BUF_SIZE );
}

//从客户端读入的信息写入m_clt_buf
RET_CODE conn::read_clt()
{
    int bytes_read = 0;
    while( true )
    {
        if( m_clt_read_idx >= BUF_SIZE )			   //如果读入的数据大于BUF_SIZE    	
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the client read buffer is full, let server write" );
            // 信息满了，需要将信息写入服务端
            // 把从客户端读入m_clt_buf的内容写入服务端 (正常情况)
            return BUFFER_FULL;
        }

        bytes_read = recv( m_cltfd, m_clt_buf + m_clt_read_idx, BUF_SIZE - m_clt_read_idx, 0 );			//因为存在分包的问题（recv所读入的并非是size的大小），因此我们根据recv的返回值进行循环读入，直到读满m_clt_buf或者recv的返回值为0（数据被读完）
        if ( bytes_read == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )					// 非阻塞情况下： EAGAIN表示没有数据可读，请尝试再次调用,而在阻塞情况下，如果被中断，则返回EINTR;  EWOULDBLOCK等同于EAGAIN
            {
                break;
            }
            return IOERR;
        }
        else if ( bytes_read == 0 )  //连接被关闭
        {
            return CLOSED;
        }

        m_clt_read_idx += bytes_read;   //移动读下标
    }
    return ( ( m_clt_read_idx - m_clt_write_idx ) > 0 ) ? OK : NOTHING;     //当读下标大于写下标时代表正常
}

//从服务端读入的信息写入m_srv_buf
RET_CODE conn::read_srv()
{
    int bytes_read = 0;
    while( true )
    {
        if( m_srv_read_idx >= BUF_SIZE )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the server read buffer is full, let client write" );
            // 信息满了
            // 服务端读入m_srv_buf的内容写入客户端 (正常情况)
            return BUFFER_FULL;
        }

        /*
        //因为存在分包的问题（recv所读入的并非是size的大小），
        因此我们根据recv的返回值进行循环读入，直到读满m_clt_buf或者recv的返回值为0（数据被读完）
        */
        bytes_read = recv( m_srvfd, m_srv_buf + m_srv_read_idx, BUF_SIZE - m_srv_read_idx, 0 );
        if ( bytes_read == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return IOERR;
        }
        else if ( bytes_read == 0 )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the server should not close the persist connection" );
            return CLOSED;
        }

        m_srv_read_idx += bytes_read;
    }
    return ( ( m_srv_read_idx - m_srv_write_idx ) > 0 ) ? OK : NOTHING;
}

// 客户端读入m_clt_buf的内容写入服务端
RET_CODE conn::write_srv()
{
    int bytes_write = 0;
    while( true )
    {
        /*
        正常情况下，缓冲区写满才会进行发送，因此应该是read_idx >= wirte_idx，结合send函数中第三个参数理解
        当 read_idx <= write_idx 代表缓冲区为空了
        */
        if( m_clt_read_idx <= m_clt_write_idx )
        {
            m_clt_read_idx = 0;
            m_clt_write_idx = 0;
            return BUFFER_EMPTY;    
        }

        bytes_write = send( m_srvfd, m_clt_buf + m_clt_write_idx, m_clt_read_idx - m_clt_write_idx, 0 );
        if ( bytes_write == -1 )    // 发送过程出现错误
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                return TRY_AGAIN;
            }
            log( LOG_ERR, __FILE__, __LINE__, "write server socket failed, %s", strerror( errno ) );
            return IOERR;
        }
        else if ( bytes_write == 0 )
        {
            return CLOSED;  // 发送完毕
        }

        m_clt_write_idx += bytes_write;
    }
}

//把从服务端读入m_srv_buf的内容写入客户端
RET_CODE conn::write_clt()
{
    int bytes_write = 0;
    while( true )
    {
        /*
        //此处为一个循环写入,防止分包引起的问题,当写指针>读指针的时候说明已经将buffer内的数据完全写入sockfd当中
        */
        if( m_srv_read_idx <= m_srv_write_idx )  
        {
            m_srv_read_idx = 0;
            m_srv_write_idx = 0;
            return BUFFER_EMPTY;
        }

        bytes_write = send( m_cltfd, m_srv_buf + m_srv_write_idx, m_srv_read_idx - m_srv_write_idx, 0 );
        if ( bytes_write == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                return TRY_AGAIN;
            }
            log( LOG_ERR, __FILE__, __LINE__, "write client socket failed, %s", strerror( errno ) );
            return IOERR;
        }
        else if ( bytes_write == 0 )
        {
            return CLOSED;
        }
        m_srv_write_idx += bytes_write;
    }
}
