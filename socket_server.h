#ifndef _socket_server_h
#define _socket_server_h



#define SOCKET_DATA 0		// �����ݵ���
#define SOCKET_CLOSE 1		// ���ӹر�
#define SOCKET_OPEN 2		// ���ӽ������������߱����������Ѽ��뵽epoll��
#define SOCKET_ACCEPT 3		// �������ӽ�������accept�ɹ������������׽��֣���δ���뵽epoll
#define SOCKET_ERROR 4		// ��������
#define SOCKET_EXIT 5		// �˳��¼�


struct socket_message 
{
	int id;
	unsigned int opaque;	
	int ud;		
	char * data;
};

struct socket_server;
void * socket_server_create_handle(void);
int socket_server_release_handle(void * server_handle);
void socket_server_start(struct socket_server *ss, unsigned int opaque, int id);


#endif  /*_socket_server_h*/