#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	uint32_t source; //消息发送方地址（handle）
	int session;   //消息session ，用以标记消息
	void * data;   //消息内容
	size_t sz;      //消息大小
};

struct message_queue;

struct message_queue * skynet_globalmq_pop(void);

struct message_queue * skynet_mq_create(uint32_t handle);
void skynet_mq_mark_release(struct message_queue *q);
int skynet_mq_release(struct message_queue *q);
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);
void skynet_mq_lock(struct message_queue *q, int session);
void skynet_mq_unlock(struct message_queue *q);

void skynet_mq_force_push(struct message_queue *q);
void skynet_mq_pushglobal(struct message_queue *q);

void skynet_mq_init();

#endif
