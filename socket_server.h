#ifndef _socket_server_h
#define _socket_server_h



#define SOCKET_DATA 0		// 有数据到来
#define SOCKET_CLOSE 1		// 连接关闭
#define SOCKET_OPEN 2		// 连接建立（主动或者被动，并且已加入到epoll）
#define SOCKET_ACCEPT 3		// 被动连接建立（即accept成功返回已连接套接字）但未加入到epoll
#define SOCKET_ERROR 4		// 发生错误
#define SOCKET_EXIT 5		// 退出事件




#endif  /*_socket_server_h*/