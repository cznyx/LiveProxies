#include "Base64.h"
#include "Logger.h"
#include "IPv6Map.h"
#include "ProxyLists.h"
#include "Global.h"
#include "GeoIP.h"
#include "WServer.h"
#include "ProxyRequest.h"
#include "ProxyRemove.h"
#include "Interface.h"
#include "Config.h"
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <pcre.h>
#include <assert.h>
#include <event2/bufferevent_ssl.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <evhtp.h>
#include <assert.h>

static const char *GetCountryByIPv6Map(IPv6Map *In)
{
	if (GetIPType(In) == IPV4) {
		if (GeoIPDB == NULL)
			GeoIPDB = GeoIP_open("/usr/local/share/GeoIP/GeoIP.dat", GEOIP_MEMORY_CACHE);
		assert(GeoIPDB != NULL);
	} else {
		if (GeoIPDB6 == NULL)
			GeoIPDB6 = GeoIP_open("/usr/local/share/GeoIP/GeoIPv6.dat", GEOIP_MEMORY_CACHE);
		assert(GeoIPDB6 != NULL);
	}

	const char *ret;
	if (GetIPType(In) == IPV4) {
		ret = GeoIP_country_code_by_ipnum(GeoIPDB, In->Data[3]);
	} else {
		geoipv6_t ip;
		memcpy(ip.s6_addr, In->Data, IPV6_SIZE);
		ret = GeoIP_country_code_by_ipnum_v6(GeoIPDB6, ip);
	}

	return ret == NULL ? "--" : ret;
}

static void ProxyCheckLanding(struct bufferevent *BuffEvent, char *Buff)
{
	// Loose proxy is automatically free'd by EVWrite called timeout

	UNCHECKED_PROXY *UProxy = NULL;
	PROXY *proxy;

	char *lpKeyIndex, *lpKeyEnd;
	size_t buffLen = strlen(Buff);

	/* Get UProxy pointer */ {
		lpKeyIndex = strstr(Buff, "LPKey: ");
		if (lpKeyIndex == NULL)
			goto free;

		lpKeyEnd = strstr(lpKeyIndex + 8, "\r\n");
		if (lpKeyEnd == NULL)
			lpKeyEnd = strchr(lpKeyIndex + 8, '\n');

		if (lpKeyIndex == NULL || lpKeyEnd == NULL || lpKeyEnd - (lpKeyIndex + 8) < 512 / 8 || lpKeyIndex - Buff + 8 <= buffLen)
			goto free;

		char *keyRaw = lpKeyIndex + 8;

		char *key;
		size_t len; // trash
		if (!Base64Decode(keyRaw, &key, &len))
			goto free;

		pthread_mutex_lock(&lockUncheckedProxies); {
			for (size_t x = 0; x < sizeUncheckedProxies; x++) {
				if (memcmp(key, uncheckedProxies[x]->hash, 512 / 8) == 0)
					UProxy = uncheckedProxies[x];
			}
		} pthread_mutex_unlock(&lockUncheckedProxies);
		free(key);

		if (UProxy == NULL)
			goto free;

		pthread_mutex_lock(&(UProxy->processing));
	} /* End get UProxy pointer */

	/* Process headers */ {
		if (UProxy->associatedProxy == NULL) {
			proxy = malloc(sizeof(PROXY));
			proxy->ip = malloc(sizeof(IPv6Map));
			memcpy(proxy->ip->Data, UProxy->ip->Data, IPV6_SIZE);

			proxy->port = UProxy->port;
			proxy->type = UProxy->type;
			proxy->country = GetCountryByIPv6Map(proxy->ip);
			proxy->liveSinceMs = proxy->lastCheckedMs = GetUnixTimestampMilliseconds();
			proxy->failedChecks = 0;
			proxy->httpTimeoutMs = GetUnixTimestampMilliseconds() - UProxy->requestTimeHttpMs;
			proxy->timeoutMs = GetUnixTimestampMilliseconds() - UProxy->requestTimeMs;
			proxy->rechecking = false;
			proxy->retries = 0;
			proxy->successfulChecks = 1;
			proxy->anonymity = ANONYMITY_NONE;
		}

		bool anonMax = true;

		char *host = GetHost(GetIPType(proxy->ip), ProxyIsSSL(proxy->type));

		char *hostIndex = strstr(Buff, "Host: ");
		if (hostIndex == NULL)
			goto freeProxy;

		char *hostEnd = strstr(hostIndex + 7, "\r\n");
		if (hostEnd == NULL)
			hostEnd = strchr(hostIndex + 7, '\n');

		if (strncmp(hostIndex, host, hostEnd - hostIndex) != 0)
			goto freeProxy;

		if ((lpKeyEnd[0] == '\r' && Buff + buffLen - 4 != lpKeyEnd) || (lpKeyEnd[0] == '\n' && Buff + buffLen - 2 != lpKeyEnd))
			anonMax = false;
		else {
			/*
			GET /prxchk HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Connection: Close\r\n"
			"Cache-Control: max-age=0\r\n"
			"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,**;q = 0.8\r\n"
			"User-Agent: LiveProxies Proxy Checker %s (tetyys.com)\r\n"
			"DNT: 1\r\n"
			"Accept-Encoding: gzip, deflate, sdch\r\n"
			"Accept-Language: en-US,en;q=0.8\r\n"
			"LPKey:
			*/
			size_t hostLen = hostEnd - hostIndex;
			hostIndex[0] = '%';
			hostIndex[1] = 's';

			char *hostEndEnd = hostEnd[0] == '\r' ? hostEnd + 2 : hostEnd + 1;

			char *cpyBuff = malloc(buffLen - (hostEndEnd - Buff)); {
				memcpy(cpyBuff, hostEndEnd, buffLen - (hostEndEnd - Buff));
				memcpy(hostIndex + 2, cpyBuff, buffLen - (hostEndEnd - Buff));
			} free(cpyBuff);

			lpKeyIndex = strstr(Buff, "LPKey: ");
			assert(lpKeyIndex != NULL);
			lpKeyIndex[8] = 0x00;

			if (strcmp(Buff, RequestString) != 0)
				anonMax = false;

			buffLen = strlen(Buff); // recompute
		}

		if (!anonMax) {
			int subStrVec[256];
			for (size_t i = 0;i < 2;i++) {
				int regexRet = pcre_exec(i == 0 ? ipv4Regex : ipv6Regex, i == 0 ? ipv4RegexEx : ipv6RegexEx, Buff, buffLen, 0, 0, subStrVec, 256);

				if (regexRet != PCRE_ERROR_NOMATCH) {
					if (regexRet < 0) {
						Log(LOG_LEVEL_ERROR, "Couldn't execute PCRE %s regex", (i == 0 ? "IPv4" : "IPv6"));
						pthread_mutex_unlock(&(UProxy->processing));
						goto freeProxy;
					} else {
						char *foundIpStr;
						for (size_t x = 0; x < regexRet; x++) {
							pcre_get_substring(Buff, subStrVec, regexRet, x, &(foundIpStr));
							IPv6Map *foundIp = StringToIPv6Map(foundIpStr); {
								if (foundIp != NULL && IPv6MapCompare(i == 0 ? GlobalIp4 : GlobalIp6, foundIp)) {
									Log(LOG_LEVEL_DEBUG, "WServer: switch to transparent proxy on (type %d)", UProxy->type);
									proxy->anonymity = ANONYMITY_TRANSPARENT;
									free(foundIp);
									break;
								}
							} free(foundIp);
						}
						pcre_free_substring(foundIpStr);
					}
				}
			}

			if (proxy->anonymity == ANONYMITY_NONE)
				proxy->anonymity = ANONYMITY_ANONYMOUS;
		} else
			proxy->anonymity = ANONYMITY_MAX;

		UProxy->checkSuccess = true;

		if (UProxy->associatedProxy == NULL) {
			Log(LOG_LEVEL_DEBUG, "WServer: Final proxy add type %d anonimity %d", proxy->type, proxy->anonymity);
			ProxyAdd(proxy);
		} else
			UProxySuccessUpdateParentInfo(UProxy);

	} /* End process headers */

	pthread_mutex_unlock(&(UProxy->processing));

	/* Output */ {
		bufferevent_write(BuffEvent, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", 47);
		bufferevent_flush(BuffEvent, EV_WRITE, BEV_FINISHED);
	}
	goto free;
freeProxy:
	free(proxy->ip);
	free(proxy);
free:
	free(Buff);
	bufferevent_free(BuffEvent);
}

static void ServerLanding(struct bufferevent *BuffEvent, char *Buff)
{
	Log(LOG_LEVEL_DEBUG, "WServer landing at your services!");
	UNCHECKED_PROXY *UProxy = NULL;

	/* Page dispatch */ {
		if (Buff[0] != 'G') {
			// wait a sec we don't have any POST or any other handlers
			goto free;
		}

		char *path;
		char *firstIndex = strchr(Buff, ' ');

		if (firstIndex == NULL)
			goto free;

		char *secondIndex = strchr(firstIndex + 1, ' ');

		if (secondIndex == NULL)
			goto free;

		size_t pathLen = secondIndex - (firstIndex + 1);

		path = malloc(pathLen * sizeof(char) + 1);
		memcpy(path, firstIndex + 1, pathLen * sizeof(char) + 1);

		if (strncmp(path, "/prxchk", pathLen) == 0)
			ProxyCheckLanding(BuffEvent, Buff);
		if (strncmp(path, "/iface", pathLen) == 0)
			InterfaceWeb(BuffEvent, Buff);
		if (strncmp(path, "/ifaceu", pathLen) == 0)
			InterfaceWebUnchecked(BuffEvent, Buff);
		if (pathLen > 13 && strncmp(path, "/iface/check", 13) == 0)
			InterfaceWebUnchecked(BuffEvent, Buff);
	} /* End page dispatch */

free:
	free(Buff);
	bufferevent_free(BuffEvent);
}

typedef enum _SERVER_TYPE {
	HTTP4,
	HTTP6,
	SSL4,
	SSL6
} SERVER_TYPE;

static void HTTPRead(struct bufferevent *BuffEvent, void *Ctx)
{
	struct evbuffer *evBuff = bufferevent_get_input(BuffEvent);
	size_t len = evbuffer_get_length(evBuff);

	char buff[4];
	evbuffer_copyout_from(evBuff, len - 4, buff, 4);

	bool valid = false;
	for (size_t x = 0;x < 2;x++) {
		if (buff[2 * x] == '\r' && buff[(2 * x) + 1] == '\n')
			valid = true;
	}
	if (!valid) {
		evbuffer_copyout_from(evBuff, len - 2, buff, 2);
		if (buff[0] != '\n' || buff[1] != '\n') {
			return;
		}
	}

	char *out = malloc(len + 1);
	evbuffer_remove(evBuff, out, len);
	out[len] = 0x00;

	ServerLanding(BuffEvent, out);
}

static void ServerEvent(struct bufferevent *BuffEvent, short Event, void *Ctx)
{
	if (Event & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
		bufferevent_free(BuffEvent);
}


static void ServerAccept(struct evconnlistener *List, evutil_socket_t Fd, struct sockaddr *Address, int Socklen, void *Ctx)
{
	SERVER_TYPE type = (SERVER_TYPE)Ctx;
	struct event_base *base = evconnlistener_get_base(List);

	switch (type) {
		case HTTP4:
		case HTTP6:
		{
			struct bufferevent *bev = bufferevent_socket_new(base, Fd, BEV_OPT_CLOSE_ON_FREE);

			bufferevent_setcb(bev, HTTPRead, NULL, ServerEvent, NULL);
			bufferevent_enable(bev, EV_READ | EV_WRITE);
			break;
		}
		case SSL4:
		case SSL6:
		{
			struct bufferevent *bev = bufferevent_openssl_socket_new(base, Fd, SSL_new(levServerSSL), BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);

			bufferevent_setcb(bev, HTTPRead, NULL, ServerEvent, NULL);
			bufferevent_enable(bev, EV_READ | EV_WRITE);
			break;
		}
		default:
			assert(0);
			break;
	}
}

static struct evconnlistener *LevConnListenerBindCustom(struct event_base *Base, evconnlistener_cb Cb, void *Arg, IPv6Map *Ip, uint16_t Port)
{
	struct evconnlistener *listener;
	evutil_socket_t fd;
	int on = 1;
	int family = GetIPType(Ip) == IPV4 ? AF_INET : AF_INET6;
	int flags = LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_REUSEABLE_PORT | LEV_OPT_DEFERRED_ACCEPT;

	fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd == -1)
		return NULL;

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void*)&on, sizeof(on)) < 0)
		goto err;

	if (evutil_make_listen_socket_reuseable(fd) < 0)
		goto err;

	if (evutil_make_listen_socket_reuseable_port(fd) < 0)
		goto err;

	if (evutil_make_tcp_listen_socket_deferred(fd) < 0)
		goto err;

	if (family == AF_INET6) {
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&on, sizeof(on)) < 0)
			goto err;
	}

	struct sockaddr *sin = IPv6MapToRaw(Ip, Port); {
		if (bind(fd, sin, family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0) {
			free(sin);
			goto err;
		}
	} free(sin);

	listener = evconnlistener_new(Base, Cb, Arg, flags, -1, fd);
	if (!listener)
		goto err;

	return listener;
err:
	evutil_closesocket(fd);
	return NULL;
}

void WServerBaseSSL()
{
	levServerBaseSSL = event_base_new();

	IPv6Map map;
	memset(&map, 0, IPV6_SIZE);

	if (GlobalIp4 != NULL) {
		map.Data[2] = 0xFFFF0000;

		levServerListSSL4 = LevConnListenerBindCustom(levServerBaseSSL, ServerAccept, SSL4, &map, SSLServerPort);
		if (!levServerListSSL4) {
			Log(LOG_LEVEL_ERROR, "Failed to listen on %d (SSL4)\n", ServerPort);
			exit(EXIT_FAILURE);
		}
	}
	if (GlobalIp6 != NULL) {
		memset(&map, 0, IPV6_SIZE);

		levServerListSSL6 = LevConnListenerBindCustom(levServerBaseSSL, ServerAccept, SSL6, &map, SSLServerPort);
		if (!levServerListSSL6) {
			Log(LOG_LEVEL_ERROR, "Failed to listen on %d (SSL6)\n", ServerPort);
			exit(EXIT_FAILURE);
		}
	}

	event_base_dispatch(levServerBase);
}

void WServerBase()
{
	levServerBase = event_base_new();

	IPv6Map map;
	memset(&map, 0, IPV6_SIZE);

	if (GlobalIp4 != NULL) {
		map.Data[2] = 0xFFFF0000;

		levServerList4 = LevConnListenerBindCustom(levServerBaseSSL, ServerAccept, HTTP4, &map, ServerPort);
		if (!levServerList4) {
			Log(LOG_LEVEL_ERROR, "Failed to listen on %d (HTTP4)\n", ServerPort);
			exit(EXIT_FAILURE);
		}
	}
	if (GlobalIp6 != NULL) {
		memset(&map, 0, IPV6_SIZE);

		levServerList6 = LevConnListenerBindCustom(levServerBaseSSL, ServerAccept, HTTP6, &map, ServerPort);
		if (!levServerList6) {
			Log(LOG_LEVEL_ERROR, "Failed to listen on %d (HTTP6)\n", ServerPort);
			exit(EXIT_FAILURE);
		}
	}

	event_base_dispatch(levServerBase);
}

static void WServerUDP(int hSock)
{
	char buff[512 / 8];
	struct sockaddr_in remote;
	socklen_t len;
	size_t size;

	for (;;) {
		len = sizeof(remote);
		size = recvfrom(hSock, buff, 512 / 8, 0, (struct sockaddr *)&remote, &len);
		if (size != 512 / 8)
			continue;

		UNCHECKED_PROXY *UProxy = NULL;

		IPv6Map *ip = RawToIPv6Map(&remote); {
			pthread_mutex_lock(&lockUncheckedProxies); {
				for (size_t x = 0; x < sizeUncheckedProxies; x++) {
					if (memcmp(buff, uncheckedProxies[x]->hash, 512 / 8) == 0 && IPv6MapCompare(ip, uncheckedProxies[x]->ip)) {
						UProxy = uncheckedProxies[x];
						pthread_mutex_lock(&(UProxy->processing));
					}
				}
			} pthread_mutex_unlock(&lockUncheckedProxies);
		} free(ip);

		PROXY *proxy;

		if (UProxy->associatedProxy == NULL) {
			proxy = malloc(sizeof(PROXY));
			proxy->ip = malloc(sizeof(IPv6Map));
			memcpy(proxy->ip->Data, UProxy->ip->Data, IPV6_SIZE);

			proxy->port = UProxy->port;
			proxy->type = UProxy->type;
			proxy->country = GetCountryByIPv6Map(proxy->ip);
			proxy->liveSinceMs = proxy->lastCheckedMs = GetUnixTimestampMilliseconds();
			proxy->failedChecks = 0;
			proxy->httpTimeoutMs = GetUnixTimestampMilliseconds() - UProxy->requestTimeHttpMs;
			proxy->timeoutMs = GetUnixTimestampMilliseconds() - UProxy->requestTimeMs;
			proxy->rechecking = false;
			proxy->retries = 0;
			proxy->successfulChecks = 1;
			proxy->anonymity = ANONYMITY_NONE;
		}

		UProxy->checkSuccess = true;

		if (UProxy->associatedProxy == NULL) {
			Log(LOG_LEVEL_DEBUG, "WServerUDP: Final proxy");
			ProxyAdd(proxy);
		} else {
			UProxySuccessUpdateParentInfo(UProxy);
			if (UProxy->singleCheck != NULL)
				pthread_mutex_unlock(UProxy->singleCheck);
		}

		pthread_mutex_unlock(&(UProxy->processing));
	}
}

void WServerUDP4()
{
	int hSock;
	struct sockaddr_in local;

	hSock = socket(AF_INET, SOCK_DGRAM, 0);
	int yes = 1;
	setsockopt(hSock, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
	setsockopt(hSock, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));

	bzero(&local, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(ServerPortUDP);
	bind(hSock, (struct sockaddr *)&local, sizeof(local));

	pthread_t wServerUDP;
	int status = pthread_create(&wServerUDP, NULL, (void*)WServerUDP, hSock);
	if (status != 0) {
		Log(LOG_LEVEL_ERROR, "WServerUDP thread creation error, return code: %d\n", status);
		return status;
	}
	pthread_detach(wServerUDP);
}

void WServerUDP6()
{
	int hSock;
	struct sockaddr_in local;

	hSock = socket(AF_INET6, SOCK_DGRAM, 0);
	int yes = 1;
	setsockopt(hSock, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
	setsockopt(hSock, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));
	setsockopt(hSock, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&yes, sizeof(yes));

	bzero(&local, sizeof(local));
	local.sin_family = AF_INET6;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(ServerPortUDP);
	bind(hSock, (struct sockaddr *)&local, sizeof(local));

	pthread_t wServerUDP;
	int status = pthread_create(&wServerUDP, NULL, (void*)WServerUDP, hSock);
	if (status != 0) {
		Log(LOG_LEVEL_ERROR, "WServerUDP thread creation error, return code: %d\n", status);
		return status;
	}
	pthread_detach(wServerUDP);
}