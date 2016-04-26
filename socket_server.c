#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"


#undef  DBG_ON
#undef  FILE_NAME
#define 	DBG_ON  	(0x01)
#define 	FILE_NAME 	"socket_server.c:"


#define MAX_INFO	    (128u)
#define MAX_EVENT		(64u)
#define MAX_SOCKET_P	(16u)
#define MIN_READ_BUFFER (64u)
#define MAX_SOCKET		(1 << MAX_SOCKET_P)	



#define SOCKET_TYPE_INVALID	 	(0)
#define SOCKET_TYPE_RESERVE		(1)
#define SOCKET_TYPE_PLISTEN		(2)/*监听套接字，未加入epoll管理*/
#define SOCKET_TYPE_PACCEPT		(3)/*accept返回，未加入epoll管理*/
#define SOCKET_TYPE_CONNECTING  (4)
#define SOCKET_TYPE_CONNECTED 	(5)	
#define SOCKET_TYPE_BIND 		(8)	/*其它类型的文件描述符，比如stdin,stdout等*/



struct write_buffer 
{
	struct write_buffer * next;
	char * ptr; /*指向未发送字节区的首地址*/
	int sz;/*未发送的字节数*/
	void * buffer;/*发送缓冲区*/
};


/*对套接字的抽象*/

struct socket
{
	int fd; /*套接字文件描述符*/
	int id;/*应用层维护的与fd对应的id*/
	int type;/*套接字类型*/
	int size;
	int wb_size;/*发送缓冲区中未发送的字节数*/
	unsigned int opaque;  /*保存服务句柄*/
	struct write_buffer * head;
	struct write_buffer  * tail;
};



struct socket_server
{
	int recvctrl_fd;/*管道的读取端，用于接收控制命令*/
	int sendctrl_fd;/*管道的写入端，用于发送控制命令*/
	int check_ctrl;/*是否检查控制命令*/
	poll_fd event_fd; /*epoll fd*/
	volatile int alloc_id; /*用于分配id*/
	int event_n;  /*epoll_wait返回的事件个数*/
	int event_index; /*当前处理的事件序号*/
	struct event ev[MAX_EVENT]; /*用于epoll wait*/
	struct socket slot[MAX_SOCKET];/*socket池*/
	char buffer[MAX_INFO]; /*用于保存临时数据*/
	fd_set rfds;	/*用于select*/
};


struct socket_message 
{
	int id;
	unsigned int opaque;	
	int ud;		
	char * data;
};





struct request_open
{
	int id;
	int port;
	unsigned int opaque;
	char host[1];
};


struct request_send
{
	int id;
	int sz;
	char * buffer;
};


struct request_close
{
	int id;
	unsigned int opaque;
};


struct request_listen
{
	int id;
	int fd;
	unsigned int opaque;
	char host[1];

};


struct request_bind
{
	int id;
	int fd;
	unsigned int opaque;
};


struct request_start
{
	int id;
	unsigned int opaque;
};




struct request_package
{
	unsigned char header[8];
	union
	{
		char buff[256];
		struct request_open open;
		struct request_send send;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
	}u;

	unsigned char dummy[256];

};


union sockaddr_all
{
	struct sockaddr s;
	struct sockaddr_in v4;
	struct scokaddr_in6 v6;
};


static int socket_server_keep_alive(int fd)
{
	if(fd < 0 )
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	int value = 1;
	setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,(void *)&value , sizeof(value));
	return(0);
}





static int socket_server_alloc_id(void * server_handle)
{
	if(NULL == server_handle)
	{
		dbg_printf("please check the param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;
	int i = 0;
	for(i=0;i<MAX_SOCKET;++i)
	{
		int id = __sync_add_and_fetch(&(handle->alloc_id),1);
		if(id < 0)
		{
			id = __sync_and_and_fetch(&(handle->alloc_id),0x7fffffff);
		}

		struct socket * socket_node = handle->slot[id % MAX_SOCKET];
		if(SOCKET_TYPE_INVALID == socket_node->type)
		{
			if (__sync_bool_compare_and_swap(&handle->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) 
			{
					return id;
			} 
			else
			{

				--i;
			}
		}
	}

	return(-1);
}




void * socket_server_create_handle(void)
{

	struct socket_server *  new_handle = calloc(1,sizeof(*new_handle));
	if(NULL == new_handle)
	{
		dbg_printf("calloc is fail!\n");
		return(NULL);
	}
	

	int fd[2] = {-1,-1};
	poll_fd efd = -1;

	efd = sp_create();
	if(sp_invalid(efd))
	{
		dbg_printf("sp_create is fail!\n");
		goto fail;
	}

	if(pipe(fd))
	{
		dbg_printf("pipe is fail!\n");
		goto fail;
	}

	if(sp_add(efd,fd[0],NULL))
	{
		dbg_printf("sp_add is fail!\n");
		goto fail;

	}
	
	int i = 0;
	new_handle->event_fd = efd;
	new_handle->recvctrl_fd = fd[0];
	new_handle->sendctrl_fd = fd[1];
	new_handle->check_ctrl = 1;
	for(i=0;i<MAX_SOCKET;++i)
	{
		struct socket * socket_node =  &new_handle->slot[i];
		socket_node->type = SOCKET_TYPE_INVALID;
		socket_node->head = NULL;
		socket_node->tail = NULL;
	}

	new_handle->alloc_id = 0;
	new_handle->event_n = 0;
	new_handle->event_index = 0;


	return(new_handle);
fail:

	if(efd > 0 )
	{
		sp_release(efd);
	}

	if(fd[0]>0)
	{
		close(fd[0]);
		fd[0] = -1;
	}

	if(fd[1]>0)
	{
		close(fd[1]);
		fd[1] = -1;
	}

	if(NULL != new_handle)
	{
		free(new_handle);
		new_handle = NULL;
	}

	return(NULL);
	
}



static int socket_server_force_close(void *server_handle,struct socket * s,struct socket_message * result)
{
	if(NULL == server_handle || NULL==s || NULL==result)
	{
		dbg_printf("please check the  param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;
	result->id = s->id;
	result->ud = 0;
	result->opaque = s->opaque;
	result->data = NULL;

	if(SOCKET_TYPE_INVALID == s->type)
	{
		dbg_printf("the socket has been release !\n");
		return(-1);
	}

	struct write_buffer * wb = s->head;
	while(wb)
	{
		struct write_buffer * tmp = wb;
		wb = wb->next;

		if(NULL != tmp)
		{
			if(NULL != tmp->buffer)
			{
				free(tmp->buffer);
				tmp->buffer = NULL;
			}
			free(tmp);
			tmp = NULL;
		}
	}

	/*如果加入了epoll管理*/
	if(SOCKET_TYPE_PLISTEN != s->type && SOCKET_TYPE_PACCEPT == s->type)
	{
		sp_del(handle->event_fd,s->fd); /*从管理中删除*/
	}

	/*如果不是其它类型的文件描述符*/
	if(SOCKET_TYPE_BIND != s->type)
	{
		close(s->fd);
		s->fd = -1;
	}

	s->type = SOCKET_TYPE_INVALID;

	return(0);

}


int socket_server_release_handle(void * server_handle)
{
	if(NULL == server_handle)
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;
	int i = 0;
	struct socket_message ret_msg;
	for(i=0;i<MAX_SOCKET;++i)
	{
		socket_server_force_close(handle,&(handle->slot[i]),&ret_msg);	
	}

	
	if(handle->recvctrl_fd > 0)
	{
		close(handle->recvctrl_fd);
		handle->recvctrl_fd = -1;
	}
	
	if(handle->sendctrl_fd > 0)
	{
		close(handle->recvctrl_fd);
		handle->recvctrl_fd = -1;
	}

	sp_release(handle->event_fd);
	free(server_handle);
	server_handle = NULL;
	return(0);

}



static struct socket * socket_server_new_fd(void * server_handle,int id,int fd,unsigned int opaque,bool add)
{
	if(NULL==server_handle)
	{
		dbg_printf("check the param!\n");
		return(NULL);
	}

	struct socket_server * handle = (struct socket_server *)server_handle;
	struct socket * socket_node = &(handle->slot[id % MAX_SOCKET]);
	if(SOCKET_TYPE_INVALID != socket_node->type)
	{
		dbg_printf("the socket has been used !\n");
		return(NULL);
	}

	socket_node->fd = fd;
	socket_node->id = id;
	socket_node->opaque = opaque;
	socket_node->wb_size = 0;
	socket_node->tail = NULL;
	socket_node->head = NULL;
	socket_node->size = MIN_READ_BUFFER;
	if(add)
	{
		if(0 != sp_add(handle->event_fd,fd,socket_node))
		{
			socket_node->type = SOCKET_TYPE_INVALID;
			return(NULL);
		}
	}

	return(socket_node);

}




static int socket_server_open_socket(void * server_handle,struct request_open * request,struct socket_message * ret_msg,bool blocking)
{
	if(NULL==server_handle || NULL==request || NULL==ret_msg )
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;

	
	ret_msg->id = request->id;
	ret_msg->opaque = request->opaque;
	ret_msg->data = NULL;
	ret_msg->ud = 0;


	struct socket * socket_node = NULL;
	int status = -1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr  = NULL;
	char port[16] = {0};
	snpritnf(port,16,"%d",request->port);
	memset(&ai_hints,0,sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	status = getaddrinfo(request->host,port,&ai_hints,&ai_list);
	if(0 != status)
	{
		goto fail;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next )
	{
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 )
		{
			continue;
		}
		socket_server_keep_alive(sock);
		if (!blocking)
		{
			sp_nonblocking(sock);
		}
		status = connect(sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS)
		{
			close(sock);
			sock = -1;
			continue;
		}
		if (blocking) 
		{
			sp_nonblocking(sock);
		}
		break;
	}

	if (sock < 0) 
	{
		goto fail;
	}

	socket_node = socket_server_new_fd(handle, request->id, sock, request->opaque, true);
	if (socket_node == NULL)
	{
		close(sock);
		goto fail;
	}

	if(0 == status)
	{
		socket_node->type = SOCKET_TYPE_CONNECTED;
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (AF_INET==ai_ptr->ai_family) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, handle->buffer, sizeof(handle->buffer))) 
		{
			ret_msg->data = handle->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	}
	else
	{
		socket_node->type = SOCKET_TYPE_CONNECTING;	
		/*非阻塞套接字尝试连接中，必需将关注其可写事件，稍后epoll才能捕获到连接出错了还是成功了*/
		sp_write(handle->event_fd, socket_node->fd, socket_node, true);

	}
	freeaddrinfo( ai_list );
	return(0);

fail:

	freeaddrinfo( ai_list );
	handle->slot[request->id % MAX_SOCKET].type = SOCKET_TYPE_INVALID;  /*归还套接字槽 */
	return(-1);

}















