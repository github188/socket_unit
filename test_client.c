#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "common.h"
#include "socket_server.h"

#undef  DBG_ON
#undef  FILE_NAME
#define 	DBG_ON  	(0x01)
#define 	FILE_NAME 	"main:"







static void * _poll(void * ud) 
{
	struct socket_server *ss = ud;
	struct socket_message result;
	for (;;) 
	{
		int type = socket_server_poll(ss, &result, NULL);
		switch (type)
		{
		case SOCKET_EXIT:
			return NULL;
		case SOCKET_DATA:
			printf("message [id=%d] size=%d\n",result.id, result.ud);
			free(result.data);
			break;
		case SOCKET_CLOSE:
			printf("close [id=%d]\n",result.id);
			break;
		case SOCKET_OPEN:
			printf("open [id=%d] %s\n",result.id,result.data);
			break;
		case SOCKET_ERROR:
			printf("error [id=%d]\n",result.id);
			break;
		case SOCKET_ACCEPT:
			printf("accept [id=%d %s] from [%d]\n",result.ud, result.data, result.id);
			socket_server_start(ss, 300, result.ud);
			break;
		}
	}
}

int main(void)
{
	struct socket_server* ss = socket_server_create_handle();
	if(NULL == ss)
	{
		dbg_printf("socket_server_create_handle is fail!\n");
		return(-1);

	}

	pthread_t pid;
	pthread_create(&pid, NULL, _poll, (void*)ss);

	
	int conn_id = socket_server_connect(ss, 100, "127.0.0.1", 8888);



	char buf[1024] = {0};
	while (fgets(buf, sizeof(buf), stdin) != NULL)
	{
		if (strncmp(buf, "quit", 4) == 0)
			break;
		buf[strlen(buf)-1] = '\n';		// È¥³ý\n
		char* sendbuf = (char*)malloc(sizeof(buf)+1);
		memcpy(sendbuf, buf, strlen(buf)+1);
		socket_server_send(ss, conn_id, sendbuf, strlen(sendbuf));
	}
	socket_server_exit(ss);
	pthread_join(pid, NULL);
	socket_server_release_handle(ss);
	return 0;
}

