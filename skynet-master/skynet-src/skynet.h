#ifndef SKYNET_H
#define SKYNET_H

#include <stddef.h>
#include <stdint.h>

#define PTYPE_TEXT 0     //text 文本协议
#define PTYPE_RESPONSE 1    //回应包
#define PTYPE_MULTICAST 2
#define PTYPE_CLIENT 3      //client协议
#define PTYPE_SYSTEM 4      //SYSTEM ？
#define PTYPE_HARBOR 5
#define PTYPE_SOCKET 6
// don't use these id
#define PTYPE_RESERVED_0 7	
// read lualib/skynet.lua lualib/mqueue.lua
#define PTYPE_RESERVED_QUEUE 8
#define PTYPE_RESERVED_DEBUG 9
#define PTYPE_RESERVED_LUA 10   //lua 协议

#define PTYPE_TAG_DONTCOPY 0x10000      //不要复制 msg/sz 指代的数据包
#define PTYPE_TAG_ALLOCSESSION 0x20000

struct skynet_context;

void skynet_error(struct skynet_context * context, const char *msg, ...);
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);
uint32_t skynet_queryname(struct skynet_context * context, const char * name);
int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);
int skynet_sendname(struct skynet_context * context, const char * destination , int type, int session, void * msg, size_t sz);

void skynet_forward(struct skynet_context *, uint32_t destination);
int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

#endif
