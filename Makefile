all:
	gcc  -I./   socket_server.c   test_client.c      -o  client  -lpthread
	gcc  -I./   socket_server.c   test_server.c      -o  server  -lpthread

clean:
	-rm -rf  client  server