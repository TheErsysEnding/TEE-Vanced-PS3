// tls-transport - BearSSL HTTPS plumbing over a raw BSD socket (see tls-transport.h).

#include "tls-transport.h"
#include "bearssl.h"
#include "dbg.h"
#include "thread.h"   // resolver lock

#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // strtoull, atoi

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/sys_time.h>     // sys_time_get_current_time (wall clock for cert-date validation)
#include <sys/random_number.h>

// the bundled root CAs (Google Trust Services + Let's Encrypt), generated in trust-anchors.c
extern const br_x509_trust_anchor TLS_TRUST_ANCHORS[];
extern const size_t TLS_TRUST_ANCHORS_COUNT;

#define TLS_PORT          443
#define SOCKET_TIMEOUT    10       // seconds; bounds a stalled recv/send (also the dead-connection detector)
#define DAYS_0AD_TO_EPOCH 719528   // days from 0000-01-01 to 1970-01-01 (BearSSL's day count is from 0 AD)
#define SECONDS_PER_DAY   86400     // split the unix clock into BearSSL's (days, seconds-of-day) time
#define HEAD_MAX          8192     // response header block cap; larger is treated as a malformed response

// (TEE Vanced V3.0.0 Test 12) No SO_RCVBUF here. Test 10 and 11 asked for 256 KB a connection, and on the console
// every connection then failed after about 256 KB ("recv failed", the offsets 262144 bytes apart); each was replaced
// at once, ten a second, until the closed ones still held by the system left none: "socket() failed", the stream
// gave out (the Elden Ring video froze at 17:12 with its sound running on) and webMAN's FTP stopped answering too.
// RPCS3 never showed it - the PC's network stack holds its sockets. The console's default is what works.

struct TlsConn {
   int                    socketHandle;
   br_ssl_client_context  client;
   br_x509_minimal_context x509;
   br_sslio_context       io;
   int64_t                remaining;     // body bytes left per Content-Length; -1 when close-delimited
   int                    chunked;       // body arrives in chunks, each with its own size line, instead
   int64_t                chunkLeft;     // bytes left in the chunk being served
   int                    chunkedEnd;    // the zero-sized chunk that ends a chunked body has been read
   const unsigned char   *leftover;      // body bytes read while parsing the head (points into head)
   int                    leftoverLen;
   char                   host[256];     // remembered for connection reuse (pool key)
   int                    keepAlive;     // this request asked the server to keep the connection open
   unsigned char          head[HEAD_MAX];
   unsigned char          ioBuffer[BR_SSL_BUFSIZE_BIDI];
};

// section: socket transport

// gethostbyname returns a pointer to shared static storage, so resolve+copy is serialised: several worker
// threads (the thumbnail fetchers) open connections at once. Created once by initTlsResolver at startup.
static sys_lwmutex_t resolveLock;
static int           resolveLockReady;
static sys_lwmutex_t timeLock;      // (V3.0.0 Test 12) certTimeNow's one network-time fetch
void initTlsResolver(void)  { if (!resolveLockReady) { createLock(&resolveLock); createLock(&timeLock); resolveLockReady = 1; } }
static void lockResolve(void)   { if (resolveLockReady) lock(&resolveLock); }
static void unlockResolve(void) { if (resolveLockReady) unlock(&resolveLock); }

static int readSocket(int handle, void *buffer, int length)
{
   int got = recv(handle, buffer, length, 0);
   return got <= 0 ? -1 : got;
}

static int writeSocket(int handle, const void *data, int length)
{
   int sent = send(handle, data, length, 0);
   return sent <= 0 ? -1 : sent;
}

static void closeSocket(int handle) { socketclose(handle); }

// resolve host and open a TCP connection to port 443. returns the socket, or -1.
static int connectHost(const char *host, int port)
{
   struct sockaddr_in address;
   memset(&address, 0, sizeof address);
   address.sin_family = AF_INET;
   address.sin_port = htons((uint16_t)port);

   // resolve and copy the address under the lock: gethostbyname's result is shared static storage that the
   // next thread's call overwrites, so we must be done reading it before releasing.
   lockResolve();
   struct hostent *resolved = gethostbyname(host);
   int resolvedOk = resolved && resolved->h_addr_list[0];
   if (resolvedOk) memcpy(&address.sin_addr, resolved->h_addr_list[0], sizeof address.sin_addr);
   unlockResolve();
   if (!resolvedOk) { logError("[tls] dns failed for %s\n", host); return -1; }

   int socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (socketHandle < 0) { logError("[tls] socket() failed\n"); return -1; }

   struct timeval timeout = { SOCKET_TIMEOUT, 0 };
   setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
   setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

   if (connect(socketHandle, (struct sockaddr *)&address, sizeof address) < 0) {
      logError("[tls] connect failed to %s\n", host);
      socketclose(socketHandle);
      return -1;
   }
   return socketHandle;
}

// section: the channel under TLS

static const TlsChannel socketChannel = { connectHost, readSocket, writeSocket, closeSocket };
static const TlsChannel *boundChannel;

void bindTlsChannel(const TlsChannel *channel) { boundChannel = channel; }

static const TlsChannel *getTlsChannel(void) { return boundChannel ? boundChannel : &socketChannel; }

static int readChannel(void *context, unsigned char *buffer, size_t length)
{
   return getTlsChannel()->read(*(int *)context, buffer, (int)length);
}

static int writeChannel(void *context, const unsigned char *data, size_t length)
{
   return getTlsChannel()->write(*(int *)context, data, (int)length);
}

// section: the time certificates are checked against (V3.0.0 Test 12)
//
// BearSSL checks a certificate's notBefore/notAfter against the time it is handed, and that was the console's clock:
// a console whose clock is off - a flat clock battery puts it in the year 2000 - failed every certificate, so
// nothing loaded. The console's clock is still used whenever it is plausible: not before the day this build was made
// and at most TIME_PLAUSIBLE_SECONDS after it. Otherwise the time is a server's: the "Date" of one plain-HTTP HEAD
// (no certificate to check on it), fetched once a run and kept as an offset to the console's clock. The console's
// clock itself is never changed. Should the fetch fail, it is the console's clock, as before.
#define TIME_HOST              "clients3.google.com"
#define TIME_PLAUSIBLE_SECONDS (3ll * 366 * SECONDS_PER_DAY)

// days since 1970-01-01 of a proleptic Gregorian date (month 1..12)
static int64_t daysFromCivil(int64_t year, int month, int day)
{
   year -= month <= 2;
   int64_t era = (year >= 0 ? year : year - 399) / 400;
   int64_t yoe = year - era * 400;
   int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
   int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   return era * 146097 + doe - 719468;
}

static int monthIndex(const char *name)   // "Jan" -> 1, 0 when it is no month
{
   static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
   for (int i = 0; i < 12; i++) if (strncmp(name, MONTHS + 3 * i, 3) == 0) return i + 1;
   return 0;
}

// the start of the day this file was compiled ("Nov 12 2026")
static int64_t buildEpoch(void)
{
   const char *date = __DATE__;
   int month = monthIndex(date), day = atoi(date + 4), year = atoi(date + 7);
   return month ? daysFromCivil(year, month, day) * SECONDS_PER_DAY : 0;
}

// "Thu, 12 Nov 2026 09:29:18 GMT" -> unix seconds, 0 when it does not read as that
static int64_t parseHttpDate(const char *text)
{
   const char *at = strchr(text, ',');
   if (!at) return 0;
   at++;
   int field[5];   // day, year, hour, minute, second - read as digits (no sscanf: it brings 60 KB of libc along)
   int m = 0;
   for (int i = 0; i < 5; i++) {
      while (*at == ' ' || *at == ':') at++;
      if (i == 1) {   // the month's name stands between the day and the year
         m = monthIndex(at);
         if (!m) return 0;
         at += 3;
         while (*at == ' ') at++;
      }
      if (*at < '0' || *at > '9') return 0;
      field[i] = 0;
      while (*at >= '0' && *at <= '9') field[i] = field[i] * 10 + (*at++ - '0');
   }
   int day = field[0], year = field[1], hour = field[2], minute = field[3], second = field[4];
   if (!m || year < 2020 || day < 1 || day > 31) return 0;
   return daysFromCivil(year, m, day) * SECONDS_PER_DAY + hour * 3600 + minute * 60 + second;
}

// the time a server says it is, over plain HTTP: 0 and *out on success
static int fetchNetworkTime(int64_t *out)
{
   const TlsChannel *channel = getTlsChannel();
   int handle = channel->open(TIME_HOST, 80);
   if (handle < 0) return -1;
   static const char REQUEST[] = "HEAD /generate_204 HTTP/1.1\r\nHost: " TIME_HOST "\r\nConnection: close\r\n\r\n";
   int rc = -1;
   if (channel->write(handle, REQUEST, (int)sizeof REQUEST - 1) == (int)sizeof REQUEST - 1) {
      char head[1024];
      int length = 0;
      while (length < (int)sizeof head - 1) {
         int got = channel->read(handle, head + length, (int)sizeof head - 1 - length);
         if (got <= 0) break;
         length += got;
         head[length] = '\0';
         if (strstr(head, "\r\n\r\n")) break;
      }
      head[length] = '\0';
      for (char *line = head; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
         if ((line[0] == 'D' || line[0] == 'd') && strncmp(line + 1, "ate:", 4) == 0) {
            int64_t seconds = parseHttpDate(line + 5);
            if (seconds > 0) { *out = seconds; rc = 0; }
            break;
         }
      }
   }
   channel->close(handle);
   return rc;
}

static void dateText(char *out, int cap, int64_t seconds)   // "2026-11-12", for the log
{
   int64_t z = seconds / SECONDS_PER_DAY + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
   int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
   int day = (int)(doy - (153 * mp + 2) / 5 + 1), month = (int)(mp < 10 ? mp + 3 : mp - 9);
   snprintf(out, cap, "%04d-%02d-%02d", (int)(yoe + era * 400 + (month <= 2)), month, day);
}

static int64_t certTimeNow(void)
{
   sys_time_sec_t nowSeconds = 0;
   sys_time_nsec_t nowNanoseconds = 0;
   sys_time_get_current_time(&nowSeconds, &nowNanoseconds);
   int64_t now = (int64_t)nowSeconds, built = buildEpoch();
   if (!built || (now >= built && now <= built + TIME_PLAUSIBLE_SECONDS)) return now;

   static int timeState;        // 0 not asked yet, 1 a server's offset known, 2 no server answered
   static int64_t timeOffset;   // the server's time minus the console's
   if (resolveLockReady) lock(&timeLock);
   if (timeState == 0) {
      int64_t network = 0;
      char consoleDay[16], serverDay[16];
      dateText(consoleDay, sizeof consoleDay, now);
      if (fetchNetworkTime(&network) == 0) {
         timeOffset = network - now;
         timeState = 1;
         dateText(serverDay, sizeof serverDay, network);
         logWarn("[tls] the console's clock says %s, a server %s: certificates are checked at the server's time\n",
                 consoleDay, serverDay);
      } else {
         timeState = 2;
         logWarn("[tls] the console's clock says %s and no server gave the time: certificates are checked against "
                 "the console's clock\n", consoleDay);
      }
   }
   int64_t result = timeState == 1 ? now + timeOffset : now;
   if (resolveLockReady) unlock(&timeLock);
   return result;
}

// section: connection setup

TlsConn *openTlsConn(const char *host)
{
   int socketHandle = getTlsChannel()->open(host, TLS_PORT);
   if (socketHandle < 0) return NULL;

   TlsConn *conn = malloc(sizeof *conn);
   if (!conn) { getTlsChannel()->close(socketHandle); return NULL; }
   conn->socketHandle = socketHandle;
   conn->remaining = -1;
   conn->leftover = NULL;
   conn->leftoverLen = 0;
   conn->keepAlive = 0;
   strncpy(conn->host, host, sizeof conn->host - 1);
   conn->host[sizeof conn->host - 1] = '\0';

   br_ssl_client_init_full(&conn->client, &conn->x509, TLS_TRUST_ANCHORS, TLS_TRUST_ANCHORS_COUNT);

   // BearSSL has no built-in clock, so hand the X.509 engine the current UTC time; without it the cert's
   // notBefore/notAfter can't be checked and validation fails with BR_ERR_X509_TIME_UNKNOWN. (Test 12) the console's
   // clock when it is plausible, a server's otherwise (certTimeNow)
   int64_t nowSeconds = certTimeNow();
   br_x509_minimal_set_time(&conn->x509, (uint32_t)(nowSeconds / SECONDS_PER_DAY) + DAYS_0AD_TO_EPOCH, (uint32_t)(nowSeconds % SECONDS_PER_DAY));

   unsigned char seed[32];
   if (sys_get_random_number(seed, sizeof seed) != 0) { logError("[tls] no entropy\n"); closeTlsConn(conn); return NULL; }
   br_ssl_engine_inject_entropy(&conn->client.eng, seed, sizeof seed);

   br_ssl_engine_set_buffer(&conn->client.eng, conn->ioBuffer, sizeof conn->ioBuffer, 1);
   br_ssl_client_reset(&conn->client, host, 0);   // host also sets SNI
   br_sslio_init(&conn->io, &conn->client.eng, readChannel, &conn->socketHandle, writeChannel, &conn->socketHandle);
   return conn;
}

int getLastTlsError(TlsConn *conn) { return br_ssl_engine_last_error(&conn->client.eng); }

int splitHttpsUrl(const char *url, char *hostOut, int hostCap, const char **pathOut)
{
   if (strncmp(url, "https://", 8) != 0) { logError("[tls] not an https url: %s\n", url); return -1; }
   const char *hostStart = url + 8;
   const char *pathStart = strchr(hostStart, '/');
   int hostLen = pathStart ? (int)(pathStart - hostStart) : (int)strlen(hostStart);
   if (hostLen <= 0 || hostLen >= hostCap) return -1;
   memcpy(hostOut, hostStart, hostLen);
   hostOut[hostLen] = '\0';
   *pathOut = pathStart ? pathStart : "/";
   return 0;
}

void closeTlsConn(TlsConn *conn)
{
   if (!conn) return;
   getTlsChannel()->close(conn->socketHandle);
   free(conn);
}

// section: request

static int writeStr(TlsConn *conn, const char *text) { return br_sslio_write_all(&conn->io, text, strlen(text)); }

int sendTlsRequest(TlsConn *conn, const char *method, const char *httpVersion, int keepAlive, const char *host,
                   const char *path, const char *extraHeaders, const void *body, int bodyLen)
{
   conn->keepAlive = keepAlive;
   // written piecewise, not into a fixed buffer: googlevideo videoplayback urls run to ~2 KB and must not
   // be capped. BearSSL coalesces these small writes into TLS records; the first write drives the handshake.
   if (writeStr(conn, method) || writeStr(conn, " ") || writeStr(conn, path) || writeStr(conn, " ") ||
       writeStr(conn, httpVersion) || writeStr(conn, "\r\nHost: ") || writeStr(conn, host) || writeStr(conn, "\r\n"))
      return -1;
   if (extraHeaders && *extraHeaders && writeStr(conn, extraHeaders)) return -1;
   // a body needs its length, and so does a bodyless write method: PUT/POST/PATCH without a
   // Content-Length is ambiguous to the server (Drive's final, empty upload chunk is one).
   int hasBody = body && bodyLen > 0;
   if (hasBody || (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)) {
      char contentLength[40];
      snprintf(contentLength, sizeof contentLength, "Content-Length: %d\r\n", hasBody ? bodyLen : 0);
      if (writeStr(conn, contentLength)) return -1;
   }
   if (writeStr(conn, keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n")) return -1;
   if (hasBody && br_sslio_write_all(&conn->io, body, bodyLen) != 0) return -1;
   return br_sslio_flush(&conn->io) != 0 ? -1 : 0;
}

// case-insensitive substring test (for the response Connection header).
static int containsCi(const char *haystack, const char *lowerNeedle)
{
   for (; *haystack; haystack++) {
      int i = 0;
      while (lowerNeedle[i]) {
         char c = haystack[i];
         if (c >= 'A' && c <= 'Z') c += 32;
         if (c != lowerNeedle[i]) break;
         i++;
      }
      if (!lowerNeedle[i]) return 1;
   }
   return 0;
}

const char *getTlsHost(TlsConn *conn) { return conn->host; }

// can this connection be handed back to a pool and reused for the next request to the same host? only if
// we asked to keep it, the whole body was consumed (so the socket sits exactly at the next response), and
// the server agreed (HTTP/1.0 keep-alive is opt-in: the server must echo Connection: keep-alive).
int isTlsReusable(TlsConn *conn)
{
   if (!conn->keepAlive || conn->remaining != 0) return 0;
   char connection[48];
   if (getTlsHeader(conn, "connection", connection, sizeof connection) != 0) return 0;
   return containsCi(connection, "keep-alive");
}

// section: response parsing

// index just past the "\r\n\r\n" header/body break in buf[0..len), or -1 if not present yet.
static int findBodyStart(const unsigned char *buf, int len)
{
   for (int i = 0; i + 3 < len; i++)
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return i + 4;
   return -1;
}

// value of a header line (case-insensitive name match), or NULL. `headers` is NUL-terminated header text.
static const char *findHeader(const char *headers, const char *name)
{
   int nameLen = (int)strlen(name);
   for (const char *line = headers; line && *line; ) {
      int match = 1;
      for (int i = 0; i < nameLen; i++) {
         char a = line[i], b = name[i];
         if (a >= 'A' && a <= 'Z') a += 32;
         if (a != b) { match = 0; break; }
      }
      if (match && line[nameLen] == ':') {
         const char *value = line + nameLen + 1;
         while (*value == ' ') value++;
         return value;
      }
      const char *newline = strchr(line, '\n');
      line = newline ? newline + 1 : NULL;
   }
   return NULL;
}

int readTlsHead(TlsConn *conn, int *status, uint64_t *totalSize)
{
   if (status) *status = 0;
   if (totalSize) *totalSize = 0;

   // read until the blank line that ends the head (may pull the first body bytes in with it)
   int headLen = 0, bodyStart = -1;
   while (headLen < HEAD_MAX) {
      int got = br_sslio_read(&conn->io, conn->head + headLen, HEAD_MAX - headLen);
      if (got <= 0) break;
      headLen += got;
      if ((bodyStart = findBodyStart(conn->head, headLen)) >= 0) break;
   }
   if (bodyStart < 0) return -1;   // the caller logs the outcome (stale-reuse retry vs real failure)

   conn->head[bodyStart - 2] = '\0';   // terminate the header text at the blank line's first \r\n
   const char *headers = (const char *)conn->head;

   if (status) {
      const char *space = strchr(headers, ' ');
      if (strncmp(headers, "HTTP/1.", 7) == 0 && space) *status = atoi(space + 1);
   }

   // body length: Content-Length delimits this response; Content-Range's "/total" gives the full resource.
   // a server that starts sending before it knows the length (youtube's media replies) sends the body in
   // chunks instead, each behind its own size line, which recvTls strips back out.
   const char *length = findHeader(headers, "content-length");
   const char *encoding = findHeader(headers, "transfer-encoding");
   conn->chunked = encoding && strncmp(encoding, "chunked", 7) == 0;
   conn->chunkLeft = 0;
   conn->chunkedEnd = 0;
   conn->remaining = length ? (int64_t)strtoull(length, NULL, 10) : -1;
   if (totalSize) {
      const char *range = findHeader(headers, "content-range");
      const char *slash = range ? strchr(range, '/') : NULL;
      if (slash) *totalSize = strtoull(slash + 1, NULL, 10);
      else if (conn->remaining > 0) *totalSize = (uint64_t)conn->remaining;
   }

   conn->leftover = conn->head + bodyStart;
   conn->leftoverLen = headLen - bodyStart;
   return 0;
}

int getTlsHeader(TlsConn *conn, const char *name, char *out, int cap)
{
   const char *value = findHeader((const char *)conn->head, name);
   if (!value) return -1;
   int i = 0;
   while (value[i] && value[i] != '\r' && value[i] != '\n' && i < cap - 1) { out[i] = value[i]; i++; }
   out[i] = '\0';
   return 0;
}

// pulls up to cap bytes of the raw stream, the bytes read while parsing the head first. >0 bytes,
// 0 the connection ended cleanly, -1 it failed.
static int64_t readRaw(TlsConn *conn, void *buffer, uint64_t cap)
{
   if (conn->leftoverLen > 0) {
      uint64_t n = (uint64_t)conn->leftoverLen < cap ? (uint64_t)conn->leftoverLen : cap;
      memcpy(buffer, conn->leftover, (size_t)n);
      conn->leftover += n;
      conn->leftoverLen -= (int)n;
      return (int64_t)n;
   }
   int got = br_sslio_read(&conn->io, buffer, (size_t)cap);
   if (got > 0) return got;
   return getLastTlsError(conn) == BR_ERR_OK ? 0 : -1;
}

static int hexValue(unsigned char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

// each chunk is introduced by its size in hex on a line of its own, and a zero size ends the body. the
// line may carry a ";extension" tail, and the blank line closing the previous chunk's data is skipped
// here rather than tracked separately. 0 when a size was read, -1 on a broken or malformed stream.
static int readChunkSize(TlsConn *conn)
{
   int64_t size = 0;
   int sawDigit = 0, inExtension = 0;
   for (;;) {
      unsigned char c;
      int64_t got = readRaw(conn, &c, 1);
      if (got <= 0) return -1;
      if (c == '\n') {
         if (!sawDigit) { inExtension = 0; continue; }   // the empty line after the previous chunk's data
         break;
      }
      if (c == '\r' || inExtension) continue;
      int value = hexValue(c);
      if (value < 0) { inExtension = 1; continue; }
      size = size * 16 + value;
      sawDigit = 1;
   }
   conn->chunkLeft = size;
   conn->chunkedEnd = (size == 0);
   return 0;
}

// serves body bytes out of a chunked stream, so callers never see the framing.
static int64_t recvChunked(TlsConn *conn, void *buffer, uint64_t cap)
{
   if (conn->chunkedEnd) return 0;
   if (conn->chunkLeft == 0 && readChunkSize(conn) != 0) return -1;
   if (conn->chunkedEnd) return 0;

   uint64_t want = cap < (uint64_t)conn->chunkLeft ? cap : (uint64_t)conn->chunkLeft;
   int64_t got = readRaw(conn, buffer, want);
   if (got <= 0) return -1;   // a chunk that stops short is a broken body, not the end of one
   conn->chunkLeft -= got;
   return got;
}

int64_t recvTls(TlsConn *conn, void *buffer, uint64_t cap)
{
   if (cap == 0) return 0;
   if (conn->chunked) return recvChunked(conn, buffer, cap);

   // serve the bytes already read past the head first
   if (conn->leftoverLen > 0) {
      uint64_t n = (uint64_t)conn->leftoverLen < cap ? (uint64_t)conn->leftoverLen : cap;
      memcpy(buffer, conn->leftover, (size_t)n);
      conn->leftover += n;
      conn->leftoverLen -= (int)n;
      if (conn->remaining > 0) conn->remaining -= n;
      return (int64_t)n;
   }

   if (conn->remaining == 0) return 0;   // whole Content-Length delivered

   uint64_t want = cap;
   if (conn->remaining > 0 && (uint64_t)conn->remaining < want) want = (uint64_t)conn->remaining;
   int got = br_sslio_read(&conn->io, buffer, (size_t)want);
   if (got > 0) {
      if (conn->remaining > 0) conn->remaining -= got;
      return got;
   }
   // br_sslio_read <= 0: the connection ended (or errored). For a close-delimited body (no Content-Length,
   // remaining < 0) the close IS the end of the body -> EOF, even on a bare TCP FIN with no TLS close_notify
   // (common for HTTP/1.0 responses; BearSSL flags the missing close_notify as an error, but the HTTP framing
   // is the close). For a length-delimited body a graceful close is EOF, anything else is a real transport error.
   if (conn->remaining < 0) return 0;
   return getLastTlsError(conn) == BR_ERR_OK ? 0 : -1;
}
