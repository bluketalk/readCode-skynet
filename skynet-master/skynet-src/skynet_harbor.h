#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

#define GLOBALNAME_LENGTH 16
#define REMOTE_MAX 256

// 可以看到，这里取高8位用来作为机器识别，而低24位用作服务节点id
// reserve high 8 bits for remote id
#define HANDLE_MASK 0xffffff   //0xffffff = 16777215    0000 0000 1111 1111 1111 1111 1111 1111
#define HANDLE_REMOTE_SHIFT 24  //2^24 = 16777216

// 消息目的skynet节点名，包含一个名字和一个32位无符号的id
struct remote_name {
	char name[GLOBALNAME_LENGTH]; //别名
	uint32_t handle;    //handle
};


struct remote_message {
	struct remote_name destination;
	const void * message;
	size_t sz;
};

// 发送消息，同时带上发送者的id
void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);
// 向master节点注册一个skynet进程
void skynet_harbor_register(struct remote_name *rname);
// 这个函数用来判断消息是来自本机器还是外部机器
int skynet_harbor_message_isremote(uint32_t handle);
// 初始化harbor
void skynet_harbor_init(int harbor);
// 启动harbor
int skynet_harbor_start(const char * master, const char *local);

#endif
