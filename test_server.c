#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "socket_server.h"


#undef  DBG_ON
#undef  FILE_NAME
#define 	DBG_ON  	(0x01)
#define 	FILE_NAME 	"main:"



int main(void)
{

	/*创建服务端*/
	struct socket_server* ss = socket_server_create_handle();
	if(NULL == ss)
	{
		dbg_printf("socket_server_create_handle is fail!\n");
		return(-1);
	}


	int listen_id = socket_server_listen(ss, 100, "", 8888, 32);
	
	socket_server_start(ss, 200, listen_id);

	// 事件循环
	struct socket_message result;
	for (;;) {
		int type = socket_server_poll(ss, &result, NULL);
		// DO NOT use any ctrl command (socket_server_close , etc. ) in this thread.
		switch (type) {
		case SOCKET_EXIT:
			goto EXIT_LOOP;
		case SOCKET_DATA:
			dbg_printf("message [id=%d] size=%d\n",result.id, result.ud);
			socket_server_send(ss, result.id, result.data, result.ud);
			//free(result.data);
			break;
		case SOCKET_CLOSE:
			dbg_printf("close [id=%d]\n",result.id);
			break;
		case SOCKET_OPEN:
			dbg_printf("open [id=%d] %s\n",result.id,result.data);
			break;
		case SOCKET_ERROR:
			dbg_printf("error [id=%d]\n",result.id);
			break;
		case SOCKET_ACCEPT:
			dbg_printf("accept [id=%d %s] from [%d]\n", result.ud, result.data, result.id);
			socket_server_start(ss, 300, result.ud);
			break;
		}
	}

EXIT_LOOP:
	socket_server_release_handle(ss);
	return 0;
}

