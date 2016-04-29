#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <semaphore.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>																				
#include <sys/time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "socket_poll.h"
#include "common.h"
#include <stdint.h>
#include "socket_server.h"

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
#define SOCKET_TYPE_LISTEN 		(3)
#define SOCKET_TYPE_PACCEPT		(4)/*accept返回，未加入epoll管理*/
#define SOCKET_TYPE_CONNECTING  (5)
#define SOCKET_TYPE_CONNECTED 	(6)	
#define SOCKET_TYPE_HALFCLOSE   (7)
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
	struct sockaddr_in6 v6;
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

		struct socket * socket_node = &(handle->slot[id % MAX_SOCKET]);
		if(SOCKET_TYPE_INVALID == socket_node->type)
		{
			if (__sync_bool_compare_and_swap(&socket_node->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) 
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
		struct socket * socket_node =  &(new_handle->slot[i]);
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
	dbg_printf("socket_node->type===%d\n",socket_node->type);
	if(SOCKET_TYPE_RESERVE != socket_node->type)
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
	snprintf(port,16,"%d",request->port);
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

	dbg_printf("request->id===%d\n",request->id);
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




static int socket_server_send_buffer(void * server_handle,struct socket * s,struct socket_message * ret_msg)
{
	if(NULL==server_handle || NULL==s || NULL==ret_msg)
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;

	while(s->head)
	{
		struct write_buffer * tmp = s->head;
		for(; ;)
		{
			int sz = write(s->fd,tmp->ptr,tmp->sz);
			if(sz < 0 )
			{
				switch(errno)
				{
					case EINTR:
						continue;
					case EAGAIN:
						return(-1);
				}
				socket_server_force_close(handle,s,ret_msg);
				return(SOCKET_CLOSE);
			}

			s->wb_size -= sz;
			if(sz != tmp->sz)
			{
				tmp->ptr += sz;
				tmp->sz -= sz;
				return(-1);
			}
			break;
		}

		s->head = tmp->next;
		free(tmp->buffer);
		tmp->buffer = NULL;
		
		free(tmp);
		tmp = NULL;

	}


	s->tail = NULL;
	/*应用层发送缓冲区数据发完，取消关注可写事件*/
	sp_write(handle->event_fd, s->fd, s, false);

	/*半关闭状态socket不可再调用send_buffer，如果发现这样做，直接强制关闭*/
	if (s->type == SOCKET_TYPE_HALFCLOSE)
	{
		socket_server_force_close(handle, s, ret_msg);
		return SOCKET_CLOSE;
	}


	return(-1);
	
}



/*增加一个发送buffer节点,n表示从该buffer的第几字节有效*/
static int socket_server_append_buffer(struct socket *s, struct request_send * request,int n)
{

	if(NULL==s || NULL==request)
	{
		dbg_printf("check the param!\n");
		return(0);
	}

	struct write_buffer * new_buf = calloc(1,sizeof(*new_buf));
	if(NULL == new_buf)
	{
		dbg_printf("calloc is fail!\n");
		return(-1);
	}
	new_buf->buffer = request->buffer;
	new_buf->ptr = request->buffer + n;
	new_buf->sz = request->sz - n;
	new_buf->next = NULL;

	s->wb_size += new_buf->sz;
	if(NULL==s->head)
	{
		s->head=s->tail=new_buf;
	}
	else
	{
		s->tail->next = new_buf;
		s->tail = new_buf;
	}
	
	return(0);
}



static int socket_server_send_socket(void *server_handle,struct request_send * request, struct socket_message *ret_msg)
{
	if(NULL==server_handle || NULL==request || NULL == ret_msg)
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	int id = request->id;
	struct socket_server * handle = (struct socket_server *)server_handle;

	struct socket * socket_node = &(handle->slot[id % MAX_SOCKET]);
	if(socket_node->id != request->id )
	{
		dbg_printf("id is not peer!\n");
		return(-1);

	}
	if(SOCKET_TYPE_INVALID==socket_node->type || SOCKET_TYPE_HALFCLOSE==socket_node->type || SOCKET_TYPE_PACCEPT==socket_node->type)
	{
		dbg_printf("the sockt type is not right!\n");
		return(-1);
	}

	if(NULL == socket_node->head)
	{

		int n = write(socket_node->fd, request->buffer, request->sz);
		if (n<0)
		{
			switch(errno)
			{
			case EINTR:
			case EAGAIN:	
				n = 0;
				break;
			default:
				dbg_printf("socket-server: write to %d (fd=%d) error.",id,socket_node->fd);
				socket_server_force_close(handle,socket_node,ret_msg);
				return SOCKET_CLOSE;
			}
		}
		if(n == request->sz)
		{
			free(request->buffer);
			request->buffer = NULL;
			return(-1);
		}
		else  /*没有写完*/
		{
			socket_server_append_buffer(socket_node, request, n);
			sp_write(handle->event_fd, socket_node->fd, socket_node, true);/*加入监控,当其可写的时候再通知*/	
			return(-1);
		}


	}
	else
	{
		socket_server_append_buffer(socket_node, request, 0);
		return(-1);
	
	}
	return(-1);
}



static  int socket_server_listen_socket(void *server_handle,struct request_listen * request,struct socket_message * ret_msg)
{
	if(NULL==server_handle || NULL==request || NULL==ret_msg)
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	int id = request->id;
	int listen_fd =request->fd;
	struct socket_server * handle = (struct socket_server *)server_handle;

	dbg_printf("socket_server_listen_socket:\n");
	struct socket * socket_node = socket_server_new_fd(handle,id,listen_fd,request->opaque,false);
	if(NULL == socket_node )
	{
		dbg_printf("socket_server_new_fd is fail!\n");
		close(listen_fd);
		ret_msg->opaque = request->opaque;
		ret_msg->id = id;
		ret_msg->ud = 0;
		ret_msg->data = NULL;
		handle->slot[id % MAX_SOCKET].type = SOCKET_TYPE_INVALID;
		return(-1);
	}
	socket_node->type = SOCKET_TYPE_PLISTEN;
	return(-1);
	
}


static int socket_server_close_socket(void *server_handle,struct request_close * request,struct socket_message * ret_msg)
{

	int ret = -1;
	if(NULL==server_handle || NULL==request || NULL==ret_msg)
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	
	struct socket_server * handle = (struct socket_server *)server_handle;
	
	int id = request->id;
	struct socket * socket_node =  &(handle->slot[id % MAX_SOCKET]);

	if(SOCKET_TYPE_INVALID==socket_node->type || id != socket_node->id)
	{
		ret_msg->id = id;
		ret_msg->opaque = request->opaque;
		ret_msg->ud = 0;
		ret_msg->data = NULL;
		return SOCKET_CLOSE;
	}

	if(NULL != socket_node->head)
	{
		ret = socket_server_send_buffer(handle,socket_node,ret_msg);		
		if(ret != 0)return(ret);
	}


	if(NULL == socket_node->head)/*数据发送完毕*/
	{
		socket_server_force_close(handle,socket_node,ret_msg);
		ret_msg->id = id;
		ret_msg->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	socket_node->type = SOCKET_TYPE_HALFCLOSE;
	return(0);
	
}




// 将stdin、stdout这种类型的文件描述符加入epoll管理，这种类型称之为SOCKET_TYPE_BIND

static int socket_server_bind_socket(void *server_handle,struct request_bind * request,struct socket_message * ret_msg)
{
	if(NULL==server_handle || NULL==request || NULL==ret_msg)
	{
		dbg_printf("please check the param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;
	int id = request->id;
	ret_msg->id = id;
	ret_msg->opaque = request->opaque;
	ret_msg->ud = 0;
	dbg_printf("socket_server_bind_socket\n");
	struct socket * socket_node = socket_server_new_fd(handle,id,request->fd,request->opaque,true);
	if(NULL == socket_node)
	{
		dbg_printf("socket_server_new_fd is fail !\n");
		ret_msg->data = NULL;
		return(-1);
	}
	sp_nonblocking(request->fd);
	ret_msg->data="bind";
	return(0);
}





static int socket_server_start_socket(void *server_handle,struct request_start * request,struct socket_message * ret_msg)
{
	if(NULL==server_handle || NULL==request || NULL==ret_msg)
	{
		dbg_printf("please check the param!\n");
		return(-1);
	}

	int id = request->id;
	struct socket_server * handle = (struct socket_server *)server_handle;
	ret_msg->id = request->id;
	ret_msg->ud = 0;
	ret_msg->data = NULL;
	ret_msg->opaque = request->opaque;
	struct socket * socket_node = &(handle->slot[id % MAX_SOCKET]);
	if(SOCKET_TYPE_INVALID==socket_node->type || id != socket_node->id)
	{
		dbg_printf("not peer !\n");
		return(-1);
	}

	if(SOCKET_TYPE_PACCEPT==socket_node->type || SOCKET_TYPE_PLISTEN==socket_node->type)
	{

		if(sp_add(handle->event_fd,socket_node->fd,socket_node))
		{
			socket_node->type = SOCKET_TYPE_INVALID;
			return(-1);
		}
		/*在这里重新设定了套接字的类型,由预备变成正式*/
		socket_node->type = (socket_node->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		socket_node->opaque = request->opaque;
		ret_msg->data = "start";
	}

	return(-1);
}




static int socket_server_read_pipe(int pipe_fd,void * buffer,int sz)
{
	for(; ; )
	{
		int n = read(pipe_fd,buffer,sz);
		if(n < 0)
		{
			if(EINTR==errno)continue;
			else
				return(-1);
		}
		else
		{
			return(n);
		}
	}
	return(0);
}



static int socket_server_has_cmd(void *server_handle)
{
	if(NULL==server_handle)
	{
		dbg_printf("check the param!\n");
		return(-1);
	}
	struct socket_server * handle = (struct socket_server *)server_handle;
	struct timeval tv = {0,0};
	int retval;
	FD_SET(handle->recvctrl_fd, &handle->rfds); 

	retval = select(handle->recvctrl_fd+1, &handle->rfds, NULL, NULL, &tv);
	return(retval);
}



static int socket_server_ctrl_cmd(struct socket_server *ss, struct socket_message *result)
{
	int fd = ss->recvctrl_fd;
	unsigned char buffer[256];
	unsigned char header[2];
	socket_server_read_pipe(fd, header, sizeof(header));
	int type = header[0];
	int len = header[1];
	socket_server_read_pipe(fd, buffer, len);
	switch (type) 
	{
		case 'S':
			dbg_printf("S\n");
			return socket_server_start_socket(ss,(struct request_start *)buffer, result);
		case 'B':
			dbg_printf("B\n");
			return socket_server_bind_socket(ss,(struct request_bind *)buffer, result);
		case 'L':
			dbg_printf("L\n");
			return socket_server_listen_socket(ss,(struct request_listen *)buffer, result);
		case 'K':
			dbg_printf("K\n");
			return socket_server_close_socket(ss,(struct request_close *)buffer, result); 
		case 'O':
			dbg_printf("O\n");
			return socket_server_open_socket(ss, (struct request_open *)buffer, result, false);
		case 'X':
			dbg_printf("X\n");
			result->opaque = 0;
			result->id = 0;
			result->ud = 0;
			result->data = NULL;
			return SOCKET_EXIT;
		case 'D':

			dbg_printf("D\n");
			return socket_server_send_socket(ss, (struct request_send *)buffer, result);
		default:
			dbg_printf("socket-server: Unknown ctrl %c.\n",type);
			return -1;
	};

	return 0;
}



static int socket_server_forward_message(struct socket_server *ss, struct socket *s, struct socket_message * result)
{
	int sz = s->size;
	char * buffer = malloc(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0)
	{
		free(buffer);
		switch(errno) 
		{
		case EINTR:
			break;
		case EAGAIN:
			dbg_printf("socket-server: EAGAIN capture.\n");
			break;
		default:
			socket_server_force_close(ss, s, result);	
			return SOCKET_ERROR;
		}
		return -1;
	}

	
	if(n==0) 
	{	
		free(buffer);
		socket_server_force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	if (s->type == SOCKET_TYPE_HALFCLOSE)
	{
		free(buffer);
		return -1;
	}

	if (n == sz)
	{
		s->size *= 2;
	}
	else if (sz > MIN_READ_BUFFER && n*2 < sz) 
	{
		s->size /= 2;	
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}


static int socket_server_report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) 
{
	int error;
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error)
	{  
		socket_server_force_close(ss,s, result);
		return SOCKET_ERROR;
	} 
	else
	{
		s->type = SOCKET_TYPE_CONNECTED; 
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		sp_write(ss->event_fd, s->fd, s, false);	// 连接成功，取消关注可写事件
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if(getpeername(s->fd, &u.s, &slen) == 0)
		{
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

/*listen 监听到连接请求*/
static int socket_server_report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0)
	{
		return 0;
	}
	int id = socket_server_alloc_id(ss);  /*分配一个套接字*/	
	if (id < 0)
	{		
		close(client_fd); /*分配失败，则进行关闭*/
		return 0;
	}
	socket_server_keep_alive(client_fd);/*关闭套接字的活性*/
	sp_nonblocking(client_fd);	/*设置为非阻塞*/
	dbg_printf("socket_server_report_accept\n");
	struct socket *ns = socket_server_new_fd(ss, id, client_fd, s->opaque, false);
	if (ns == NULL)
	{
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT; /*处于准备接收阶段*/
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) 
	{
		result->data = ss->buffer;
	}

	return 1;
}



int  socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more)
{
	for (;;) 
	{
		dbg_printf("1\n");
		if (ss->check_ctrl)
		{	
			dbg_printf("2\n");
			if (socket_server_has_cmd(ss))
			{
				dbg_printf("3\n");
				int type = socket_server_ctrl_cmd(ss, result);
				dbg_printf("type==%d\n",type);
				if (type != -1)
					return type;
				else
					continue;
			} 
			else 
			{ 
				dbg_printf("4\n");
				ss->check_ctrl = 0;
			}
		}

		
		if (ss->event_index == ss->event_n)
		{
			dbg_printf("5\n");
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->check_ctrl = 1;
			if (more)
			{
				*more = 0;
			}
			ss->event_index = 0;
			if (ss->event_n <= 0)
			{
				ss->event_n = 0;
				return -1;
			}
		}
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL)
		{
			// dispatch pipe message at beginning
			continue;
		}
		switch (s->type) 
		{
		
		case SOCKET_TYPE_CONNECTING: 
			dbg_printf("SOCKET_TYPE_CONNECTING\n");
			return socket_server_report_connect(ss, s, result);
			
		case SOCKET_TYPE_LISTEN:
			dbg_printf("SOCKET_TYPE_LISTEN\n");
			if (socket_server_report_accept(ss, s, result)) 
			{
				return SOCKET_ACCEPT;
			} 
			break;
		case SOCKET_TYPE_INVALID:
			dbg_printf("socket-server: invalid socket\n");
			break;
		default:
			if (e->write)
			{	
				dbg_printf("there is data to write!\n");
				int type = socket_server_send_buffer(ss, s, result);	
				if (type == -1)
					break;

				return type;
				
				
			}
			if (e->read)
			{
				
				dbg_printf("there is data to read!\n");
				int type = socket_server_forward_message(ss, s, result);
				dbg_printf("type==%d data==%s\n",type,result->data);
				if (type == -1)
					break;
				return type;
			
			}
			break;
		}
	}
}




static void socket_server_send_request(struct socket_server *ss, struct request_package *request, char type, int len)
{
	request->header[6] = (unsigned char)type;
	request->header[7] = (unsigned char)len;
	for (;;)
	{
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);
		if (n<0) 
		{
			if (errno != EINTR) 
			{
				dbg_printf("socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		return;
	}
}



static int socket_server_open_request(struct socket_server *ss, struct request_package *req, unsigned int opaque, const char *addr, int port)
{
	int len = strlen(addr);
	if (len + sizeof(req->u.open) > 256)
	{
		dbg_printf("socket-server : Invalid addr %s.\n",addr);
		return 0;
	}
	int id = socket_server_alloc_id(ss);
	dbg_printf("the id is %d\n",id);
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}


int socket_server_connect(struct socket_server *ss, unsigned int opaque, const char * addr, int port)
{
	struct request_package request;
	int len = socket_server_open_request(ss, &request, opaque, addr, port);
	socket_server_send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}



int  socket_server_block_connect(struct socket_server *ss, unsigned int opaque, const char * addr, int port)
{
	struct request_package request;
	struct socket_message result;
	socket_server_open_request(ss, &request, opaque, addr, port);
	int ret = socket_server_open_socket(ss, &request.u.open, &result, true);
	if (ret == SOCKET_OPEN)
	{
		return result.id;
	}
	else 
	{
		return -1;
	}
}


int socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz)
{
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID)
	{
		return -1;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	socket_server_send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}

void socket_server_exit(struct socket_server *ss)
{
	struct request_package request;
	socket_server_send_request(ss, &request, 'X', 0);
}



void socket_server_close(struct socket_server *ss, unsigned int opaque, int id)
{
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;
	socket_server_send_request(ss, &request, 'K', sizeof(request.u.close));
}




static int socket_server_do_listen(const char * host, int port, int backlog)
{

	unsigned int addr = INADDR_ANY;
	if (host[0]) 
	{
		addr=inet_addr(host);
		dbg_printf("%s\n",host);
		
	}
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) 
	{
		dbg_printf("socket is fail!\n");
		return -1;
	}

	int reuse = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) 
	{
		dbg_printf("setsockopt is fail!\n");
		goto _failed;
	}

	struct sockaddr_in my_addr;
	memset(&my_addr, 0, sizeof(struct sockaddr_in));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = addr;
	
	if (bind(listen_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
	{
		dbg_printf("bind is fail!\n");
		goto _failed;
	}
	
	if (listen(listen_fd, backlog) == -1) 
	{
		dbg_printf("listen is fail!\n");
		goto _failed;
	}
	return listen_fd;
_failed:
	close(listen_fd);
	return -1;
}




int  socket_server_listen(struct socket_server *ss, unsigned int opaque, const char * addr, int port, int backlog)
{
	int fd = socket_server_do_listen(addr, port, backlog);
	if (fd < 0) 
	{
		dbg_printf("socket_server_do_listen is fail!\n");
		return -1;
	}
	struct request_package request;
	int id = socket_server_alloc_id(ss);

	dbg_printf("id=====%d\n",id);
	
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;
	socket_server_send_request(ss, &request, 'L', sizeof(request.u.listen)); 
	return id;
}




int socket_server_bind(struct socket_server *ss, unsigned int opaque, int fd) {
	struct request_package request;
	int id = socket_server_alloc_id(ss);
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	socket_server_send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void socket_server_start(struct socket_server *ss, unsigned int opaque, int id)
{
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	socket_server_send_request(ss, &request, 'S', sizeof(request.u.start)); /*启动*/
}



