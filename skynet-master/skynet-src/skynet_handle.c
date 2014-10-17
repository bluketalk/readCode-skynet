#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4

struct handle_name {
	char * name;
	uint32_t handle;
};

struct handle_storage {
	struct rwlock lock;

	uint32_t harbor;
	uint32_t handle_index;
	int slot_size;
	struct skynet_context ** slot;
	
	int name_cap;
	int name_count;
	struct handle_name *name;
};

static struct handle_storage *H = NULL;

uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;
		for (i=0;i<s->slot_size;i++) {
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK; //与前24倍 ，也就是取出值在 0 ~ HANDLE_MASK 之间值
			int hash = handle & (s->slot_size-1);// s->slot_size-1 =３， 与前两位，也就是取出值 0~3
			if (s->slot[hash] == NULL) {
				s->slot[hash] = ctx;
				s->handle_index = handle + 1;

				rwlock_wunlock(&s->lock);
                
                /*
                 0000 0001 0000 0000 0000 0000 0000 0000
                 |
                 0000 0000 0000 0000 0000 0000 0000 0001
                 =
                 0000 0001 0000 0000 0000 0000 0000 0001
                 */
				handle |= s->harbor;
				skynet_context_init(ctx, handle);
				return handle;
			}
		}
        //原有 s->slot 存储已满，重新申请一块两倍大小的空间存储handle
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);
		struct skynet_context ** new_slot = malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
        //将原来空间的内容拷贝到新申请的空间中
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);//为了能与出前0到s->slot_size之前的数，所以s->slot_size必须以2的倍数增长
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
        //释放原来的空间
		free(s->slot);
        //重新指定到新的空间
		s->slot = new_slot;
        //重新指定大小
		s->slot_size *= 2;
	}
}

void
skynet_handle_retire(uint32_t handle) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		skynet_context_release(ctx);
		s->slot[hash] = NULL;
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;
	}

	rwlock_wunlock(&s->lock);
}

void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			rwlock_runlock(&s->lock);
			if (ctx != NULL) {
				++n;
				skynet_handle_retire(skynet_context_handle(ctx));
			}
		}
		if (n==0)
			return;
	}
}

struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
		s->name_cap *= 2;
		struct handle_name * n = malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		free(s->name);
		s->name = n;
	} else {
		int i;
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	char * result = strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	struct handle_storage * s = malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;//为何是4？
	s->slot = malloc(s->slot_size * sizeof(struct skynet_context *));//skynet_context ** slot; 相当于指针数组
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));
    //初始化读写锁
	rwlock_init(&s->lock);
    /*我们最终允许 255 个 skynet 节点部署在不同的机器上协作。每个 skynet 节点有不同的 id 。这里被称为 harbor id 。这个是独立指定，人为管理分配的（也可以写一个中央服务协调分配）。每个消息包产生的时候，skynet 框架会把自己的 harbor id 编码到源地址的高 8 位。这样，系统内所有的服务模块，都有不同的地址了。从数字地址，可以轻易识别出，这个消息是远程消息，还是本地消息。
     
     0000 0000 0000 0000 0000 0000 0000 0001
     << HANDLE_REMOTE_SHIFT
    　＝
     0000 0001 0000 0000 0000 0000 0000 0000
     */
	// reserve 0 for system
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;//harbor id 编码到源地址的高 8 位，0xff =２５５；
	s->handle_index = 1;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

