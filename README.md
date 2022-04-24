# springsnail
一. config.xml文件解释

Listen 是主机服务器的IP与端口

logical_host的IP是百度网站服务器的地址
可以使用nslookup www.baidu.com 进行IP的查看

端口80是Http协议的端口号
第一行是负载均衡服务器的地址,下面两个则是真正的服务器,spingsnil只是起到一个中转站的作用,将客户端的连接转发给比较“闲”的服务器

config.xml的conns是连接数, 想填多少填多少

nc端模拟http报文: GET /HTTP/1.1

二. main函数解释
tinyxml这个库, 用它来解析咱们的xml文件会方便许多
将processpool的构造函数设置为私有,然后通过一个静态的方法去调用这个构造函数从而实现了一个单例模式,
也就是说无论用户用这个类去构造多少个对象,这些对象都是同一个，保证了任务只能由一个对象去实现.


三. Processpool
