#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_group.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct monitor {
	int count;  //线程数量
	struct skynet_monitor ** m;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	int sleep; //睡眠 ？
};

struct worker_parm {
	struct monitor *m;
	int id;
};

#define CHECK_ABORT if (skynet_context_total()==0) break;

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

static void *
_socket(void *p) {
	struct monitor * m = p;
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	free(m->m);
	free(m);
}

static void *
_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	for (;;) {
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void *
_timer(void *p) {
	struct monitor * m = p;
	for (;;) {
		skynet_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_cond_broadcast(&m->cond);
	return NULL;
}

static void *
_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	for (;;) {
		if (skynet_context_message_dispatch(sm)) {
			CHECK_ABORT
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		} 
	}
	return NULL;
}

/*
函数_start中，启动了多个工作线程(_worker)，一个moniter线程(_moniter)，一个定时器线程(_timer)，一个网络线程(_socket)。我们来看看这些线程都在做什么？
 worker线程：
 从全局消息队列中取一条消息队列，而后找到该消息队列的处理器(handle)，通过处理器找到对应节点的上下文，而后，更新处理器的状态值，确定其不是无穷执行的状态。接下来执行消息请求。即接口(_dispatch_message)，在该接口中会判定是否是广播消息，若不是，则执行节点的回调接口_cb(callback)。处理完毕之后，查看是否需要返回数据，若需要，则将处理结果压到目标节点的消息队列中。
 moniter线程：
 我觉得则个线程，只是判定节点是否出错了，如果出错，进行修复一下。
 timer线程：
 定时器线程我就不解释了，每个服务器系统都应该有该功能。
 socket线程：
 该线程主要是从epoll_wait的结果中读取消息，若有需要处理的消息则进行相关的处理。这里面的消息目前要说明的有三个：第一个是管道，作者把管道的读端放到了epoll中进行管理，也就是说，每次向管道中写数据，都是socket线程读取并处理的；第二个是gate产生的监听端口，也是由epoll管理，并且一旦产生了数据也是由socket处理；第三个是accept客户端的socket后，客户端发送到服务端的数据，此数据也由socket线程处理。
*/
static void
_start(int thread) {
	pthread_t pid[thread+3];

	struct monitor *m = malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	m->m = malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	create_thread(&pid[0], _monitor, m);
	create_thread(&pid[1], _timer, m);
	create_thread(&pid[2], _socket, m);

	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		create_thread(&pid[i+3], _worker, &wp[i]);
	}

	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	free_monitor(m);
}

static int
_start_master(const char * master) {
	struct skynet_context *ctx = skynet_context_new("master", master);
	if (ctx == NULL)
		return 1;
	return 0;	
}

void 
skynet_start(struct skynet_config * config) {
    //初始化group
	skynet_group_init();
    //初始化harbor
	skynet_harbor_init(config->harbor);
    //初始化handle
	skynet_handle_init(config->harbor);
    //初始化全局消息队列
	skynet_mq_init();
    //初始化全局模块
	skynet_module_init(config->module_path);
    //初始化timmer
	skynet_timer_init();
    //初始化 server socket
	skynet_socket_init();
   //启动master
	if (config->standalone) {
        printf("debug _start_master start \n");
		if (_start_master(config->standalone)) {
			fprintf(stderr, "Init fail : mater");
			return;
		}
	}
	// harbor must be init first
	if (skynet_harbor_start(config->master , config->local)) {
		fprintf(stderr, "Init fail : no master");
		return;
	}

	struct skynet_context *ctx;
	ctx = skynet_context_new("logger", config->logger);
	if (ctx == NULL) {
		fprintf(stderr,"launch logger error");
		exit(1);
	}
	ctx = skynet_context_new("localcast", NULL);
	if (ctx == NULL) {
		fprintf(stderr,"launch local cast error");
		exit(1);
	}
	ctx = skynet_context_new("snlua", "launcher");
	if (ctx) {
		skynet_command(ctx, "REG", ".launcher");
		ctx = skynet_context_new("snlua", config->start);
	}

	_start(config->thread);
	skynet_socket_free();
}

/*
 
 

 
 */

