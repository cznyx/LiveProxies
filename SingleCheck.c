#define _GNU_SOURCE

#include "SingleCheck.h"
#include "ProxyLists.h"
#include "Global.h"
#ifdef __linux__
	#include <netinet/in.h>
	#include <netdb.h>
	#include <arpa/inet.h>
#elif defined _WIN32 || defined _WIN64
	#include <Winsock2.h>
	#include <Ws2tcpip.h>
	#include <windows.h>

int WSAAPI getnameinfo(
	const struct sockaddr FAR *sa,
	socklen_t                 salen,
	char FAR                  *host,
	DWORD                     hostlen,
	char FAR                  *serv,
	DWORD                     servlen,
	int                       flags
	);

typedef struct _addrinfo {
	int             ai_flags;
	int             ai_family;
	int             ai_socktype;
	int             ai_protocol;
	size_t          ai_addrlen;
	char            *ai_canonname;
	struct sockaddr  *ai_addr;
	struct addrinfo  *ai_next;
} ADDRINFOA, *PADDRINFOA;

int WSAAPI getaddrinfo(
	PCSTR      pNodeName,
	PCSTR      pServiceName,
	const ADDRINFOA  *pHints,
	PADDRINFOA *ppResult
	);

void WSAAPI freeaddrinfo(
	struct addrinfo *ai
	);

#endif

#include <string.h>
#include <event2/bufferevent.h>
#include "ProxyRequest.h"
#include "Logger.h"
#include "Config.h"

#include "DNS.h"

void PageRequest(PROXY *In, void *FinishedCallback, char *Page, void *Ex)
{
	bool success = false;
	UNCHECKED_PROXY *UProxy = UProxyFromProxy(In);
	UProxy->singleCheckCallback = FinishedCallback;
	UProxy->singleCheckCallbackExtraData = Ex;
	UProxy->pageTarget = strdup(Page);
	if (strncmp(UProxy->pageTarget, "https://", 8) == NULL && !ProxyIsSSL(UProxy->type)) {
		switch (UProxy->type) {
			case PROXY_TYPE_HTTP: {
				UProxy->type = PROXY_TYPE_HTTPS;
				break;
			}
			case PROXY_TYPE_SOCKS4: {
				UProxy->type = PROXY_TYPE_SOCKS4_TO_SSL;
				break;
			}
			case PROXY_TYPE_SOCKS4A: {
				UProxy->type = PROXY_TYPE_SOCKS4A_TO_SSL;
				break;
			}
			case PROXY_TYPE_SOCKS5: {
				UProxy->type = PROXY_TYPE_SOCKS5_TO_SSL;
				break;
			}
		}
	}
	if (strncmp(UProxy->pageTarget, "http://", 7) == NULL && ProxyIsSSL(UProxy->type)) {
		switch (UProxy->type) {
			case PROXY_TYPE_HTTPS: {
				UProxy->type = PROXY_TYPE_HTTP;
				break;
			}
			case PROXY_TYPE_SOCKS4_TO_SSL: {
				UProxy->type = PROXY_TYPE_SOCKS4;
				break;
			}
			case PROXY_TYPE_SOCKS4A_TO_SSL: {
				UProxy->type = PROXY_TYPE_SOCKS4A;
				break;
			}
			case PROXY_TYPE_SOCKS5_TO_SSL: {
				UProxy->type = PROXY_TYPE_SOCKS5;
				break;
			}
		}
	}

	UProxyAdd(UProxy);
	RequestAsync(UProxy);
}

void Recheck(PROXY *In, void *FinishedCallback, void *Ex)
{
	bool success = false;
	UNCHECKED_PROXY *UProxy = UProxyFromProxy(In);
	UProxy->singleCheckCallback = FinishedCallback;
	UProxy->singleCheckCallbackExtraData = Ex;
	UProxy->targetIPv4 = GlobalIp4;
	UProxy->targetIPv6 = GlobalIp6;
	UProxy->targetPort = UProxy->type == PROXY_TYPE_SOCKS5_WITH_UDP ? ServerPortUDP : (ProxyIsSSL(UProxy->type) ? SSLServerPort : ServerPort);

	UProxyAdd(UProxy);
	In->rechecking = true;
	RequestAsync(UProxy);
}

char *ReverseDNS(IPv6Map *In)
{
	struct hostent *hent = NULL;
	char *ret = zalloc(NI_MAXHOST);
	IP_TYPE type = GetIPType(In);

	struct sockaddr *raw = IPv6MapToRaw(In, 0); {
		size_t sLen = type == IPV4 ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

		if (getnameinfo(type == IPV4 ? (struct sockaddr_in*)raw : (struct sockaddr_in6*)raw, sLen, ret, NI_MAXHOST, NULL, NULL, NI_NAMEREQD) == 0) {
			free(raw);
			ret = realloc(ret, (strlen(ret) * sizeof(char)) + 1);
			return ret;
		}

	} free(raw);
	free(ret);
	return NULL;
}

void SpamhausZENAsyncStage2(struct dns_cb_data *data)
{
	if (data->addr_len <= 0) {
		if (data->context != NULL) {
			bufferevent_write((struct bufferevent*)(((DNS_LOOKUP_ASYNC_EX*)data->context)->object), "cln", 3 * sizeof(char));
			((DNS_LOOKUP_ASYNC_EX*)data->context)->resolveDone = true;
		}
		if (data->error != DNS_DOES_NOT_EXIST)
			Log(LOG_LEVEL_WARNING, "Failed to lookup spamhaus DNS (%d)", data->error);
		return;
	}

	DNS_LOOKUP_ASYNC_EX *ex = (DNS_LOOKUP_ASYNC_EX*)data->context;
	struct bufferevent *buffEvent = (struct bufferevent*)ex->object;

	uint8_t addrData = ((uint8_t*)(data->addr))[3];
	switch (addrData) {
		case 2:
			bufferevent_write(buffEvent, "sbl", 3 * sizeof(char));
			break;
		case 3:
			bufferevent_write(buffEvent, "css", 3 * sizeof(char));
			break;
		case 10:
		case 11:
			bufferevent_write(buffEvent, "pbl", 3 * sizeof(char));
			break;
		default:
			bufferevent_write(buffEvent, "xbl", 3 * sizeof(char));
	}

end:
	bufferevent_flush(buffEvent, EV_WRITE, BEV_FINISHED);
	BufferEventFreeOnWrite(buffEvent);
	ex->resolveDone = true;
}

void HTTP_BLAsyncStage2(struct dns_cb_data *data)
{
	if (data->addr_len <= 0) {
		if (data->context != NULL) {
			bufferevent_write((struct bufferevent*)(((DNS_LOOKUP_ASYNC_EX*)data->context)->object), "1\r\nContent-Type: text/html\r\n\r\nl", 31 * sizeof(char));
			((DNS_LOOKUP_ASYNC_EX*)data->context)->resolveDone = true;
		}
		if (data->error != DNS_DOES_NOT_EXIST)
			Log(LOG_LEVEL_WARNING, "Failed to lookup HTTP_BL DNS (%d)", data->error);
		return;
	}

	DNS_LOOKUP_ASYNC_EX *ex = (DNS_LOOKUP_ASYNC_EX*)data->context;
	struct bufferevent *buffEvent = (struct bufferevent*)ex->object;

	uint8_t days = ((uint8_t*)(data->addr))[1];
	uint8_t score = ((uint8_t*)(data->addr))[2];
	HTTPBL_CROOK_TYPE crookType = ((uint8_t*)(data->addr))[3];

	if (crookType == 0) {
		bufferevent_write(buffEvent, "1\r\nContent-Type: text/html\r\n\r\nl", 31 * sizeof(char));
		goto end;
	}

	char body[8];
	memset(body, 0, 8 * sizeof(char));

	if ((crookType & HTTPBL_CROOK_TYPE_COMMENT_SPAMMER) == HTTPBL_CROOK_TYPE_COMMENT_SPAMMER)
		strcat(body, "c");
	if ((crookType & HTTPBL_CROOK_TYPE_HARVESTER) == HTTPBL_CROOK_TYPE_HARVESTER)
		strcat(body, "h");
	if ((crookType & HTTPBL_CROOK_TYPE_SUSPICIOUS) == HTTPBL_CROOK_TYPE_SUSPICIOUS)
		strcat(body, "s");
	if (crookType == HTTPBL_CROOK_TYPE_CLEAN)
		strcat(body, "l");

	char sScore[4];
	sprintf(sScore, "%d", score);
	strcat(body, sScore);

	char sBodyLen[3];
	sprintf(sBodyLen, "%d", strlen(body) * sizeof(char));

	bufferevent_write(buffEvent, sBodyLen, strlen(sBodyLen) * sizeof(char));
	bufferevent_write(buffEvent, "\r\nContent-Type: text/html\r\n\r\n", 29 * sizeof(char));
	bufferevent_write(buffEvent, body, strlen(body) * sizeof(char));

end:
	bufferevent_flush(buffEvent, EV_WRITE, BEV_FINISHED);
	bufferevent_free(buffEvent);
	ex->resolveDone = true;
}

void SpamhausZENAsync(IPv6Map *In, struct bufferevent *BuffEvent)
{
	IP_TYPE type = GetIPType(In);

	char *name = zalloc(type == IPV4 ? 32 : 81); {
		if (type == IPV4) {
			uint8_t a, b, c, d;
			uint8_t *bytes = ((uint8_t*)&(In->Data[3]));
			a = bytes[0];
			b = bytes[1];
			c = bytes[2];
			d = bytes[3];
			sprintf((char*)(name), "%d.%d.%d.%d.zen.spamhaus.org", d, c, b, a);
		} else {
			uint8_t *data = (uint8_t*)(In->Data);
			for (size_t x = IPV6_SIZE;x >= 0;x++) {
				char format[2];
				sprintf(format, "%x.", data[x]);
				strcat((char*)(name), format);
			}
			strcat((char*)(name), ".zen.spamhaus.org");
		}

		DNSResolveAsync(BuffEvent, name, false, SpamhausZENAsyncStage2);
	} free(name);
}

void HTTP_BLAsync(IPv6Map *In, char *AccessKey, struct bufferevent *BuffEvent)
{
	if (AccessKey[0] == 0x00) {
		bufferevent_write(BuffEvent, "1\r\nContent-Type: text/html\r\n\r\nN", 31 * sizeof(char));
		bufferevent_flush(BuffEvent, EV_WRITE, BEV_FINISHED);
		bufferevent_free(BuffEvent);
		return;
	}

	IP_TYPE type = GetIPType(In);

	char *name = zalloc(type == IPV4 ? 33 + strlen(AccessKey) : 82 + strlen(AccessKey)); {
		if (type == IPV4) {
			uint8_t a, b, c, d;
			uint8_t *bytes = ((uint8_t*)&(In->Data[3]));
			a = bytes[0];
			b = bytes[1];
			c = bytes[2];
			d = bytes[3];
			sprintf((char*)(name), "%s.%d.%d.%d.%d.dnsbl.httpbl.org", AccessKey, d, c, b, a);
		} else {
			uint8_t *data = (uint8_t*)In->Data;
			for (size_t x = IPV6_SIZE;x >= 0;x++) {
				char format[2];
				sprintf(format, "%x.", data[x]);
				strcat((char*)(name), format);
			}
			strcat((char*)(name), ".dnsbl.httpbl.org");
		}

		DNSResolveAsync(BuffEvent, name, false, HTTP_BLAsyncStage2);
	} free(name);
}

SPAMHAUS_ZEN_ANSWER SpamhausZEN(IPv6Map *In)
{
	struct addrinfo *servinfo;
	IP_TYPE type = GetIPType(In);

	char *query = zalloc(type == IPV4 ? 33 : 82); {
		if (type == IPV4) {
			uint8_t a, b, c, d;
			uint8_t *bytes = ((uint8_t*)&(In->Data[3]));
			a = bytes[0];
			b = bytes[1];
			c = bytes[2];
			d = bytes[3];
			sprintf(query, "%d.%d.%d.%d.zen.spamhaus.org", d, c, b, a);
		} else {
			uint8_t *data = (uint8_t*)In->Data;
			for (size_t x = IPV6_SIZE;x >= 0;x++) {
				char format[2];
				sprintf(format, "%x.", data[x]);
				strcat(query, format);
			}
			strcat(query, ".zen.spamhaus.org");
		}

		if (getaddrinfo(query, NULL, NULL, &servinfo) != 0)
			return SPAMHAUS_ZEN_ANSWER_CLEAN;
	} free(query);

	struct sockaddr_in *addr = (struct sockaddr_in*)(servinfo->ai_addr);
	uint8_t data = ((uint8_t*)(&(addr->sin_addr.s_addr)))[3];
	freeaddrinfo(servinfo);
	switch (data) {
		case 2:
			return SPAMHAUS_ZEN_ANSWER_SBL;
			break;
		case 3:
			return SPAMHAUS_ZEN_ANSWER_CSS;
			break;
		case 10:
		case 11:
			return SPAMHAUS_ZEN_ANSWER_PBL;
			break;
		default:
			return SPAMHAUS_ZEN_ANSWER_XBL;
	}
}

void HTTP_BL(IPv6Map *In, char *AccessKey, HTTPBL_ANSWER OUT *Out)
{
	struct addrinfo *servinfo;
	IP_TYPE type = GetIPType(In);

	char *query = zalloc(type == IPV4 ? 34 + strlen(AccessKey) : 83 + strlen(AccessKey)); {
		if (type == IPV4) {
			uint8_t a, b, c, d;
			uint8_t *bytes = ((uint8_t*)&(In->Data[3]));
			a = bytes[0];
			b = bytes[1];
			c = bytes[2];
			d = bytes[3];
			sprintf(query, "%s.%d.%d.%d.%d.dnsbl.httpbl.org", AccessKey, d, c, b, a);
		} else {
			uint8_t *data = (uint8_t*)(In->Data);
			for (size_t x = IPV6_SIZE;x >= 0;x++) {
				char format[2];
				sprintf(format, "%x.", data[x]);
				strcat(query, format);
			}
			strcat(query, ".dnsbl.httpbl.org");
		}

		if (getaddrinfo(query, NULL, NULL, &servinfo) != 0) {
			Out->crookType = HTTPBL_CROOK_TYPE_CLEAN;
			return;
		}
	} free(query);

	struct sockaddr_in *addr = (struct sockaddr_in*)(servinfo->ai_addr);
	Out->days = ((uint8_t*)(&(addr->sin_addr.s_addr)))[1];
	Out->score = ((uint8_t*)(&(addr->sin_addr.s_addr)))[2];
	Out->crookType = ((uint8_t*)(&(addr->sin_addr.s_addr)))[3];
	if (Out->crookType == 0)
		Out->crookType = HTTPBL_CROOK_TYPE_CLEAN;
	freeaddrinfo(servinfo);
}