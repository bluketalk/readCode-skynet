#ifndef skynet_socket_server_h
#define skynet_socket_server_h

#include <stdint.h>

// socket_server_poll返回的socket消息类型
#define SOCKET_DATA 0    // data 到来
#define SOCKET_CLOSE 1   // close conn
#define SOCKET_OPEN 2    // conn ok
#define SOCKET_ACCEPT 3  // 被动连接建立 (Accept返回了连接的fd 但是未加入epoll来管理)
#define SOCKET_ERROR 4   // error
#define SOCKET_EXIT 5    // exit

struct socket_server;

struct socket_message {
	int id;     //fd
	uintptr_t opaque;   //handle
	int ud;	// for accept, ud is listen id ; for data, ud is size of data 
	char * data;    //data
};

struct socket_server * socket_server_create();
void socket_server_release(struct socket_server *);
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);

void socket_server_exit(struct socket_server *);
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);
void socket_server_start(struct socket_server *, uintptr_t opaque, int id);

// return -1 when error
int socket_server_send(struct socket_server *, int id, const void * buffer, int sz);

// ctrl command below returns id
int socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);
int socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);
int socket_server_bind(struct socket_server *, uintptr_t opaque, int fd);

int socket_server_block_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);

#endif
