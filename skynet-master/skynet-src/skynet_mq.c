#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_multicast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.
// 2 means message is dispatching with locked session set.
// 3 means mq is not in global mq, and locked session has been set.

#define MQ_IN_GLOBAL 1
#define MQ_DISPATCHING 2
#define MQ_LOCKED 3

struct message_queue {
	uint32_t handle;
	int cap;//消息队列的容量，即消息队列的长度
	int head;   //队列头指针
	int tail; //队列尾指针
	int lock;//消息锁
	int release;
	int lock_session;
	int in_global; //该消息队列是否在全局消息队列中
	struct skynet_message *queue; //消息队列数组
};

struct global_queue {
	uint32_t head;
	uint32_t tail;
	struct message_queue ** queue;
	bool * flag;

};

static struct global_queue *Q = NULL;

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}
#define UNLOCK(q) __sync_lock_release(&(q)->lock);

#define GP(p) ((p) % MAX_GLOBAL_MQ)

static void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	uint32_t tail = GP(__sync_fetch_and_add(&q->tail,1));
	q->queue[tail] = queue;
	__sync_synchronize();
	q->flag[tail] = true;
}

struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;
	uint32_t head =  q->head;
	uint32_t head_ptr = GP(head);
	if (head_ptr == GP(q->tail)) {
		return NULL;
	}

	if(!q->flag[head_ptr]) {
		return NULL;
	}

	__sync_synchronize();

	struct message_queue * mq = q->queue[head_ptr];
	if (!__sync_bool_compare_and_swap(&q->head, head, head+1)) {
		return NULL;
	}
	q->flag[head_ptr] = false;

	return mq;
}

struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	q->lock = 0;
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->lock_session = 0;
	q->queue = malloc(sizeof(struct skynet_message) * q->cap);

	return q;
}

static void 
_release(struct message_queue *q) {
	free(q->queue);
	free(q);
}

uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

//将最前面消息弹出队列
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head];
		ret = 0;
		if ( ++ q->head >= q->cap) {
			q->head = 0;
		}
	}

	if (ret) {
		q->in_global = 0;
	}
	
	UNLOCK(q)

	return ret;
}

static void
expand_queue(struct message_queue *q) {
    //分配原来两倍大小的空间
	struct skynet_message *new_queue = malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	//释放原来空间内存
	free(q->queue);
	q->queue = new_queue;
}

static void
_unlock(struct message_queue *q) {
	// this api use in push a unlock message, so the in_global flags must not be 0 , 
	// but the q is not exist in global queue.
	if (q->in_global == MQ_LOCKED) {
		skynet_globalmq_push(q);
		q->in_global = MQ_IN_GLOBAL;
	} else {
		assert(q->in_global == MQ_DISPATCHING);
	}
	q->lock_session = 0;
}

static void 
_pushhead(struct message_queue *q, struct skynet_message *message) {
	int head = q->head - 1;
	if (head < 0) {
		head = q->cap - 1;
	}
	if (head == q->tail) {
		expand_queue(q);
		--q->tail;
		head = q->cap - 1;
	}

	q->queue[head] = *message;
	q->head = head;

	_unlock(q);
}

void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
    //锁
	LOCK(q)
	
	if (q->lock_session !=0 && message->session == q->lock_session) {
        //将消息加入到队列最前
		_pushhead(q,message);
	} else {
		q->queue[q->tail] = *message;
        //队列+1，如果大于队列容量，重新指定到队列头部
		if (++ q->tail >= q->cap) {
			q->tail = 0;
		}
        
        //如果头指针和尾指针冲突，则表明队列已满，需要重新分配新的空间
		if (q->head == q->tail) {
			expand_queue(q);
		}

		if (q->lock_session == 0) {
			if (q->in_global == 0) {
				q->in_global = MQ_IN_GLOBAL;
                //将该消息队列加入全局消息队列
				skynet_globalmq_push(q);
			}
		}
	}
	
	UNLOCK(q)
}

void
skynet_mq_lock(struct message_queue *q, int session) {
	LOCK(q)
	assert(q->lock_session == 0);
	assert(q->in_global == MQ_IN_GLOBAL);
	q->in_global = MQ_DISPATCHING;
	q->lock_session = session;
	UNLOCK(q)
}

void
skynet_mq_unlock(struct message_queue *q) {
	LOCK(q)
	_unlock(q);
	UNLOCK(q)
}

void 
skynet_mq_init() {
	struct global_queue *q = malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	q->queue = malloc(MAX_GLOBAL_MQ * sizeof(struct message_queue *));
	q->flag = malloc(MAX_GLOBAL_MQ * sizeof(bool));
	memset(q->flag, 0, sizeof(bool) * MAX_GLOBAL_MQ);
	Q=q;
}

void 
skynet_mq_force_push(struct message_queue * queue) {
	assert(queue->in_global);
	skynet_globalmq_push(queue);
}

void 
skynet_mq_pushglobal(struct message_queue *queue) {
	LOCK(queue)
	assert(queue->in_global);
	if (queue->in_global == MQ_DISPATCHING) {
		// lock message queue just now.
		queue->in_global = MQ_LOCKED;
	}
	if (queue->lock_session == 0) {
		skynet_globalmq_push(queue);
		queue->in_global = MQ_IN_GLOBAL;
	}
	UNLOCK(queue)
}

void 
skynet_mq_mark_release(struct message_queue *q) {
	LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	UNLOCK(q)
}

static int
_drop_queue(struct message_queue *q) {
	// todo: send message back to message source
	struct skynet_message msg;
	int s = 0;
	while(!skynet_mq_pop(q, &msg)) {
		++s;
		int type = msg.sz >> HANDLE_REMOTE_SHIFT;
		if (type == PTYPE_MULTICAST) {
			assert((msg.sz & HANDLE_MASK) == 0);
			skynet_multicast_dispatch((struct skynet_multicast_message *)msg.data, NULL, NULL);
		} else {
			free(msg.data);
		}
	}
	_release(q);
	return s;
}

int 
skynet_mq_release(struct message_queue *q) {
	int ret = 0;
	LOCK(q)
	
	if (q->release) {
		UNLOCK(q)
		ret = _drop_queue(q);
	} else {
		skynet_mq_force_push(q);
		UNLOCK(q)
	}
	
	return ret;
}
