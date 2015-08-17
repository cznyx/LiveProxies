#pragma once

#include "IPv6Map.h"
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <pthread.h>

#define WEB_SOCKETS_MAX_PIECE_COUNT 32

typedef struct _WEB_SOCKET_UNFINISHED_PACKET {
	struct bufferevent *buffEvent;
	uint8_t *data;
	uint64_t dataLen;
	uint8_t pieceCount;
	struct event *timeout;
} WEB_SOCKET_UNFINISHED_PACKET;

typedef enum _WEBSOCKET_SERVER_COMMANDS {
	WEBSOCKET_SERVER_COMMAND_SIZE_UPROXIES = 0x01,
	WEBSOCKET_SERVER_COMMAND_SIZE_PROXIES = 0x02
} WEBSOCKET_SERVER_NOTIFICATION_COMMANDS;

typedef struct _WEB_SOCKET_SUBSCRIBED_CLIENT {
	struct bufferevent *buffEvent;
	WEBSOCKET_SERVER_NOTIFICATION_COMMANDS subscriptions;
	struct event *timer;
} WEB_SOCKET_SUBSCRIBED_CLIENT;

typedef enum _WEBSOCKET_OPCODES {
	WEBSOCKET_OPCODE_CONTINUATION = 0x00,
	WEBSOCKET_OPCODE_UNICODE = 0x01,
	WEBSOCKET_OPCODE_BINARY = 0x02,
	WEBSOCKET_OPCODE_CLOSE = 0x08,
	WEBSOCKET_OPCODE_PING = 0x09,
	WEBSOCKET_OPCODE_PONG = 0x0A
} WEBSOCKET_OPCODES;

pthread_mutex_t WebSocketSubscribedClientsLock;
WEB_SOCKET_SUBSCRIBED_CLIENT **WebSocketSubscribedClients;
size_t WebSocketSubscribedClientsSize;

pthread_mutex_t WebSocketUnfinishedPacketsLock;
WEB_SOCKET_UNFINISHED_PACKET **WebSocketUnfinishedPackets;
size_t WebSocketUnfinishedPacketsSize;

void WebsocketSwitch(struct bufferevent *BuffEvent, char *Buff);
void WebsocketLanding(struct bufferevent *BuffEvent, uint8_t *Buff, uint64_t BuffLen);
void WebsocketTimeout(struct bufferevent *BuffEvent, short Event, void *Ctx);
void WebsocketClientTimeout(struct bufferevent *BuffEvent, short Event, void *Ctx);
void WebsocketClientPing(evutil_socket_t fd, short Event, void *BuffEvent);