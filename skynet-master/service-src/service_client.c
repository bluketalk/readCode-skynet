#include "skynet.h"
#include "skynet_socket.h"

#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct client {
	int id;     //socket连接的 fd
};

static int
_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	assert(sz <= 65535);
	struct client * c = ud;
	// tmp will be free by skynet_socket.
	// see skynet_src/socket_server.c : send_socket()
	uint8_t *tmp = malloc(sz + 2);
	tmp[0] = (sz >> 8) & 0xff;
	tmp[1] = sz & 0xff;
	memcpy(tmp+2, msg, sz);
	skynet_socket_send(context, c->id, tmp, (int)sz+2);

	return 0;
}

int
client_init(struct client *c, struct skynet_context *ctx, const char * args) {
	int id = 0;
	sscanf(args, "%d",&id);
	c->id = id;
	skynet_callback(ctx, c, _cb);

	return 0;
}

struct client *
client_create(void) {
	struct client *c = malloc(sizeof(*c));
	memset(c,0,sizeof(*c));
	return c;
}

void
client_release(struct client *c) {
	free(c);
}
