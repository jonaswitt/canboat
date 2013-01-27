/*

Runs a TCP server, single threaded. It reads JSON styled NMEA 2000 records (lines)
from stdin, collects this data and sends this out on three types of TCP clients:

- Non stream JSON type get all accumulated data.
- Stream JSON type just receive exactly the same messages as this program
  receives.
- NMEA0183 stream type get those messages which this program knows how to translate
  into NMEA0183. The two letter talkers is the hexadecimal code for the NMEA2000
  sender.

(C) 2009-2013, Kees Verruijt, Harlingen, The Netherlands.

This file is part of CANboat.

CANboat is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

CANboat is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with CANboat.  If not, see <http://www.gnu.org/licenses/>.

*/


#include "common.h"
#include <signal.h>

#define PORT 2597

#define UPDATE_INTERVAL (500)       /* Every x milliseconds send the normal 'once' clients all state */

uint16_t port = PORT;

uint32_t protocol = 1;

#define SENSOR_TIMEOUT   (120)            /* Timeout when PGN messages expire (no longer retransmitted) */
#define AIS_TIMEOUT      (3600)           /* AIS messages expiration is much longer */
#define SONICHUB_TIMEOUT (3600 * 24 * 31) /* SonicHub messages expiration is basically indefinite */

static void closeStream(int i);

typedef void (*ReadHandler)(int i);
/* ... is the prototype for the following types of read-read file descriptors: */
static void handleClientRequest(int i);
static void acceptJSONClient(int i);
static void acceptNMEA0183Client(int i);

/*
 * TCP clients or servers. We keep an array of TCP sockets, indexed by a small integer.
 * We use the FD_SET bits to keep track of which sockets are active (open).
 */

typedef enum StreamType
{ SOCKET_TYPE_ANY
, CLIENT_JSON
, CLIENT_JSON_STREAM
, CLIENT_NMEA0183_STREAM
, SERVER_JSON
, SERVER_NMEA0183
, DATA_INPUT_STREAM
, DATA_OUTPUT_SINK
, DATA_OUTPUT_COPY
, DATA_OUTPUT_STREAM
, SOCKET_TYPE_MAX
} StreamType;

ReadHandler readHandlers[SOCKET_TYPE_MAX] =
{ 0
, handleClientRequest
, handleClientRequest
, 0
, acceptJSONClient
, acceptNMEA0183Client
, handleClientRequest
, 0
, 0
, 0
};

int socketIdxMin = 0;
int socketIdxMax = 0;
SOCKET socketFdMax = 0;
fd_set activeSet;
fd_set readSet;
fd_set writeSet;

typedef struct StreamInfo
{
  SOCKET         fd;
  StreamType     type;
  int64_t        timeout;
  ReadHandler    readHandler;
  char           buffer[4096]; /* Lines longer than this might get into trouble */
  size_t         len;
} StreamInfo;

StreamInfo stream[FD_SETSIZE];

const int stdinfd  = 0; /* The fd for the stdin port, this receives the analyzed stream of N2K data. */
const int stdoutfd = 1; /* Possible fd for the stdout port */

FILE * debugf;

StreamType outputType = DATA_OUTPUT_STREAM;

size_t currentAlloc = 0;
char * currentMessage = 0;
size_t currentLen = 0;

#define MIN_PGN (59391)
#define MAX_PGN (131000)
#define ACTISENSE_BEM (0x400000)
#define ACTISENSE_RNG (0x100)

#define PGN_SPACE (ACTISENSE_RNG + MAX_PGN - MIN_PGN)
#define PrnToIdx(prn) ((prn <= MAX_PGN) ? (prn - MIN_PGN) : ((prn <= ACTISENSE_BEM + ACTISENSE_RNG) ? (prn - ACTISENSE_BEM) : -1))

/*
 * We store messages and where they come from.
 *
 * the 'primary key' is the combination of the following 2 fields:
 * - src
 * - key2 (value of some field in the message, or null)
 *
 */
typedef struct
{
  uint8_t m_src;
  char * m_key2;
  time_t m_time;
  char * m_text;
} Message;

/*
 * Per PGN we keep a list of messages.
 * We use an 'indefinite' array of messages that is extended at runtime.
 */
typedef struct
{
  unsigned int p_prn;
  unsigned int p_maxSrc;
  char * p_description;
  Message p_message[];
} Pgn;

/*
 * An index from PRN to index in the data[] array. By keeping
 * the PGNs that we have seen coalesced in data[] we can loop over all
 * of them very efficiently.
 */
Pgn * pgnIdx[PGN_SPACE];

/*
 * Support for 512 different PGNs. Since this is more than there are defined
 * by the NMEA this does not need to be variable.
 * Each entry points to the location of pgnIdx[...].
 */
Pgn ** pgnList[512];
size_t maxPgnList;

static char * secondaryKeyList[] =
  { "Instance\""
  , "\"Reference\""
  , "\"Message ID\""
  , "\"User ID\""
  , "\"Proprietary ID\""
  };

static int secondaryKeyTimeout[] =
  { SENSOR_TIMEOUT
  , SENSOR_TIMEOUT
  , AIS_TIMEOUT
  , AIS_TIMEOUT
  , SENSOR_TIMEOUT
  , SENSOR_TIMEOUT
  };

/* Characters that occur between key name and value */
#define SKIP_CHARACTERS "\": "

#ifndef ARRAYSIZE
# define ARRAYSIZE(x) (sizeof(x)/sizeof(x[0]))
#endif

/*****************************************************************************************/

int64_t epoch(void)
{
  struct timeval t;

  if (gettimeofday(&t, 0))
  {
    logAbort("Error on obtaining wall clock\n");
  }
  return (int64_t) t.tv_sec * 1000 + t.tv_usec / 1000;
}

int setFdUsed(SOCKET fd, StreamType ct)
{
  int i;

  /* Find a free entry in socketFd(i) */
  for (i = 0; i <= socketIdxMax; i++)
  {
    if (stream[i].fd == -1 || stream[i].fd == fd)
    {
      break;
    }
  }

  if (i == FD_SETSIZE)
  {
    logError("Already %d active streams, ignoring new one\n", FD_SETSIZE);
    close(fd);
    return -1;
  }

  stream[i].fd = fd;
  stream[i].timeout = epoch() + UPDATE_INTERVAL;
  stream[i].type = ct;
  stream[i].readHandler = readHandlers[ct];

  FD_SET(fd, &activeSet);
  if (stream[i].readHandler)
  {
    FD_SET(fd, &readSet);
  }
  else
  {
    FD_CLR(fd, &readSet);
  }

  switch (stream[i].type)
  {
  case CLIENT_JSON:
  case CLIENT_JSON_STREAM:
  case DATA_OUTPUT_STREAM:
  case DATA_OUTPUT_COPY:
    FD_SET(fd, &writeSet);
    break;
  default:
    FD_CLR(fd, &writeSet);
  }

  socketIdxMax = CB_MAX(socketIdxMax, i);
  socketFdMax = CB_MAX(socketFdMax, fd);
  logDebug("New client %u %u..%u fd=%d fdMax=%d\n", i, socketIdxMin, socketIdxMax, fd, socketFdMax);

  return i;
}

static void closeStream(int i)
{
  logDebug("closeStream(%d)\n", i);

  close(stream[i].fd);
  FD_CLR(stream[i].fd, &activeSet);
  FD_CLR(stream[i].fd, &readSet);
  FD_CLR(stream[i].fd, &writeSet);

  stream[i].fd = -1; /* Free for re-use */
  if (i == socketIdxMax)
  {
    socketIdxMax--;
    socketFdMax = -1;
    for (; i >= 0; i--)
    {
      socketFdMax = CB_MAX(socketFdMax, stream[i].fd);
    }
  }
  logDebug("closeStream(%d) IdMax=%u FdMax=%d\n", i, socketIdxMax, socketFdMax);
}

# define INITIAL_ALLOC    8192
# define NEXT_ALLOC       4096
# define MAKE_SPACE(x) \
    { if (remain < (size_t)(x)) \
    { \
      alloc += NEXT_ALLOC; \
      state = realloc(state, alloc); \
      remain += NEXT_ALLOC; \
    } }

# define INC_L_REMAIN \
    {l = l + strlen(state + l); remain = alloc - l; }

static char * getFullStateJSON(void)
{
  char separator = '{';
  size_t alloc = INITIAL_ALLOC;
  char * state;
  int i, s;
  Pgn * pgn;
  size_t remain = alloc;
  size_t l;
  time_t now = time(0);

  state = malloc(alloc);
  for (l = 0, i = 0; i < maxPgnList; i++)
  {
    pgn = *pgnList[i];
    MAKE_SPACE(100 + strlen(pgn->p_description));
    snprintf(state + l, remain, "%c\"%u\":\n  {\"description\":\"%s\"\n"
            , separator
            , pgn->p_prn
            , pgn->p_description
            );
    INC_L_REMAIN;

    for (s = 0; s < pgn->p_maxSrc; s++)
    {
      Message * m = &pgn->p_message[s];

      if (m->m_time >= now)
      {
        MAKE_SPACE(strlen(m->m_text) + 32);
        snprintf(state + l, remain, "  ,\"%u%s%s\":%s\n"
                , m->m_src
                , m->m_key2 ? "_" : ""
                , m->m_key2 ? m->m_key2 : ""
                , m->m_text
                );
        INC_L_REMAIN;
      }
    }
    strcpy(state + l, "  }\n");
    INC_L_REMAIN;

    separator = ',';
  }
  if (separator == ',')
  {
    strcpy(state + l, "}\n");
  }
  else
  {
    strcpy(state + l, "\n");
  }

  return state;
}

static void tcpServer(uint16_t port, StreamType st)
{
  struct sockaddr_in serverAddr;
  int sockAddrSize = sizeof(serverAddr);
  int r;
  int on = 1;
  SOCKET s;

  s = socket(PF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET)
  {
    die("Unable to open server socket");
  }
  memset((char *) &serverAddr, 0, sockAddrSize);
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(port);
  serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &on, (socklen_t) sizeof(on));

  r = bind(s, (struct sockaddr *) &serverAddr, sockAddrSize);
  if (r == INVALID_SOCKET)
  {
    die("Unable to bind server socket");
  }

  r = listen(s, 10);
  if (r == INVALID_SOCKET)
  {
    die("Unable to listen to server socket");
  }

# ifdef O_NONBLOCK
  {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
  }
# else
  {
    int ioctlOptionValue = 1;

    ioctl(s, FIONBIO, &ioctlOptionValue);
  }
# endif

  logDebug("TCP server fd=%d\n", s);
  setFdUsed(s, st);
}

static void startTcpServers(void)
{
  tcpServer(port, SERVER_JSON);
  logInfo("TCP JSON server listening on port %d\n", port);
  tcpServer(port + 1, SERVER_NMEA0183);
  logInfo("TCP NMEA0183 server listening on port %d\n", port);
}

void acceptClient(SOCKET s, StreamType ct)
{
  SOCKET r;
  struct sockaddr_in clientAddr;
  socklen_t clientAddrLen;

  for (;;)
  {
    clientAddrLen = sizeof(clientAddr);
    r = accept(s, (struct sockaddr *) &clientAddr, &clientAddrLen);
    if (r == INVALID_SOCKET)
    {
      /* No socket ready, just ignore */
      return;
    }

    /* New client found, mark it as such */
    if (setFdUsed(r, ct) < 0)
    {
      /* Too many open clients, ignore */
      return;
    }
  }
}

void acceptJSONClient(int i)
{
  acceptClient(stream[i].fd, CLIENT_JSON);
}

void acceptNMEA0183Client(int i)
{
  acceptClient(stream[i].fd, CLIENT_NMEA0183_STREAM);
}

void appendJSONMessage(char * message)
{
  size_t len = strlen(message);

  if (currentAlloc < currentLen + len)
  {
    currentMessage = realloc(currentMessage, currentAlloc + len);
    if (!currentMessage)
    {
      die("Out of memory");
    }
    currentAlloc += len;
  }
  strcpy(currentMessage + currentLen, message);
  currentLen += len;
}

/*
 * Perform immediate action upon receiving a NMEA2000 message
 */
void sendJSONStream(char * message)
{
  size_t i;
  SOCKET fd;
  size_t len = strlen(message);

  for (i = socketIdxMin; i <= socketIdxMax; i++)
  {
    fd = stream[i].fd;
    if (fd && stream[i].type == CLIENT_JSON_STREAM)
    {
      if (send(fd, message, len, 0) < (int) len)
      {
        closeStream(i); /* On short write we just close, could try better but this might block! */
      }
    }
  }
}

void writeAllClients(void)
{
  fd_set ws;
  struct timeval timeout = {0, 0};
  int r;
  int i;
  SOCKET fd;
  int64_t now = 0;
  char * state = 0;

  if (socketIdxMax >= 0)
  {
    ws = writeSet;
    r = select(socketFdMax + 1, 0, &ws, 0, &timeout);

    for (i = socketIdxMin; r > 0 && i <= socketIdxMax; i++)
    {
      fd = stream[i].fd;
      logDebug("writeAllClients i=%u fd=%d\n", i, fd);
      if (fd < 0)
      {
        continue;
      }
      if (fd > socketFdMax)
      {
        logAbort("Inconsistent: fd[%u]=%d, max=%d\n", i, fd, socketFdMax);
      }
      if (FD_ISSET(fd, &writeSet))
      {
        if (!FD_ISSET(fd, &ws))
        {
          /* We close all listening clients that can't be written to */
          closeStream(i);
        }
        else
        {
          r--;
          if (!now) now = epoch();

          switch (stream[i].type)
          {
          case CLIENT_JSON:
            if (stream[i].timeout && stream[i].timeout < now)
            {
              if (!state)
              {
                state = getFullStateJSON();
              }
              send(fd, state, strlen(state), 0);
              if (stream[i].type == CLIENT_JSON_STREAM)
              {
                stream[i].timeout = epoch() + UPDATE_INTERVAL;
              }
              else
              {
                closeStream(i);
              }
            }
            break;
          case CLIENT_JSON_STREAM:
          case DATA_OUTPUT_STREAM:
          case DATA_OUTPUT_COPY:
            if (currentLen)
            {
              send(fd, currentMessage, currentLen, 0);
            }
            break;
          default:
            break;
          }
        }
      }
    }
  }

  if (state)
  {
    free(state);
  }
  currentLen = 0;
}

void handleMessageByte(char c)
{
  static char readLine[4096], *readBegin = readLine, *s, *e = 0, *e2;
  size_t r;
  Message * m;
  int i, idx, k;
  int src, dst, prn;
  Pgn * pgn;
  time_t now;
  char * key2 = 0;
  int valid;

  if ((c != '\n') && (readBegin < readLine + sizeof(readLine)))
  {
    *readBegin++ = c;
    return;
  }
  *readBegin = 0;
  now = time(0);

  r = readBegin - readLine;
  readBegin = readLine;
  if (!strstr(readLine, "\"fields\":"))
  {
#ifdef DEBUG
    logDebug("Ignore pgn %u without fields\n", prn);
#endif
    return;
  }
  if (memcmp(readLine, "{\"timestamp", 11) != 0)
  {
    logDebug("Ignore '%s'\n", readLine);
    return;
  }
  if (memcmp(readLine + r - 2, "}}", 2) != 0)
  {
    logDebug("Ignore '%s' (end)\n", readLine);
    return;
  }
#ifdef DEBUG
  logDebug("Message :%s:\n", readLine);
#endif
  s = strstr(readLine, "\"src\":");
  if (s)
  {
    if (sscanf(s + sizeof("\"src\":"), "%u\",\"dst\":\"%u\",\"pgn\":\"%u\"", &src, &dst, &prn))
    {
#ifdef DEBUG
      logDebug("prn=%u src=%u\n", prn, src);
#endif
    }
  }
  if (!prn || !src)
  {
    return;
  }
  if (prn > MAX_PGN)
  {
    return;
  }

  /* Look for a secondary key */
  for (k = 0; k < ARRAYSIZE(secondaryKeyList); k++)
  {
    s = strstr(readLine, secondaryKeyList[k]);
    if (s)
    {
      s += strlen(secondaryKeyList[k]);
      while (strchr(SKIP_CHARACTERS, *s))
      {
        s++;
      }

      e = strchr(s, ' ');
      e2 = strchr(s, '"');
      if (!e || e2 < e)
      {
        e = e2;
      }
      if (!e)
      {
        e = s + strlen(s);
      }
      key2 = malloc(e - s + 1);
      if (!key2)
      {
        logAbort("Out of memory allocating %u bytes", e - s);
      }
      memcpy(key2, s, e - s);
      key2[e - s] = 0;
    }
  }

  appendJSONMessage(readLine);

  idx = PrnToIdx(prn);
  if (idx < 0)
  {
    logAbort("PRN %d is out of range\n", prn);
  }

  pgn = pgnIdx[idx];
  if (!pgn)
  {
    if (maxPgnList == ARRAYSIZE(pgnList))
    {
      logAbort("Too many PGNs\n");
    }

    pgn = calloc(1, sizeof(Pgn) + sizeof(Message));
    if (!pgn)
    {
      logAbort("Out of memory allocating %u bytes", sizeof(Pgn) + sizeof(Message));
    }
    pgnIdx[idx] = pgn;
    pgnList[maxPgnList++] = &pgnIdx[idx];
  }

  if (!pgn->p_description)
  {
    pgn->p_prn = prn;
    s = strstr(readLine, "\"description\":");
    if (s)
    {
      s = s + sizeof("\"description\":");
      e = strchr(s, ':');
      e2 = strchr(s, '"');
      if (!e || e2 < e)
      {
        e = e2;
      }
      if (!e)
      {
        logDebug("Cannot find end of description in %s\n", s);
        return;
      }
      logDebug("New PGN '%.*s'\n", e - s, s);
      pgn->p_description = malloc(e - s + 1);
      if (!pgn->p_description)
      {
        logAbort("Out of memory allocating %u bytes", e - s);
      }
      memcpy(pgn->p_description, s, e - s);
      pgn->p_description[e - s] = 0;
    }
  }

  /* Find existing key */
  for (i = 0; i < pgn->p_maxSrc; i++)
  {
    if (pgn->p_message[i].m_src == src)
    {
      if (pgn->p_message[i].m_key2)
      {
        if (key2 && strcmp(pgn->p_message[i].m_key2, key2) == 0)
        {
          break;
        }
      }
      else
      {
        break;
      }
    }
  }

  /* Reuse expired key ? */
  if (i == pgn->p_maxSrc)
  {
    for (i = 0; i < pgn->p_maxSrc; i++)
    {
      if (pgn->p_message[i].m_time < now)
      {
        pgn->p_message[i].m_src = (uint8_t) src;
        if (pgn->p_message[i].m_key2)
        {
          free(pgn->p_message[i].m_key2);
        }
        pgn->p_message[i].m_key2 = key2;
        key2 = 0;
        break;
      }
    }
  }

  /* Create new key */
  if (i == pgn->p_maxSrc)
  {
    size_t newSize;

    pgn->p_maxSrc++;
    newSize = sizeof(Pgn) + pgn->p_maxSrc * sizeof(Message);
    pgn = realloc(pgnIdx[idx], newSize);
    if (!pgn)
    {
      logAbort("Out of memory allocating %u bytes", newSize);
    }
    pgnIdx[idx] = pgn;
    pgn->p_message[i].m_src = (uint8_t) src;
    pgn->p_message[i].m_key2 = key2;
    key2 = 0;
    pgn->p_message[i].m_text = 0;
  }

  m = &pgn->p_message[i];
  if (m->m_text)
  {
    if (strlen(m->m_text) < r)
    {
      m->m_text = realloc(m->m_text, r + 1);
    }
  }
  else
  {
    m->m_text = malloc(r + 1);
  }
  if (!m->m_text)
  {
    logAbort("Out of memory allocating %u bytes", r + 1);
  }
  strcpy(m->m_text, readLine);

  if (prn == 126996)
  {
    valid = AIS_TIMEOUT;
  }
  else if (prn == 130816)
  {
    valid = SONICHUB_TIMEOUT;
  }
  else
  {
    valid = secondaryKeyTimeout[k];
  }
  m->m_time = now + valid;
}

void handleClientRequest(int i)
{
  ssize_t r;
  char * p;

  r = read(stream[i].fd, stream[i].buffer + stream[i].len, sizeof(stream[i].buffer) - 1 - stream[i].len);

  if (r <= 0)
  {
    if (stream[i].fd == stdinfd)
    {
      logAbort("Error on reading stdin\n");
    }
    if (stream[i].fd == stdoutfd)
    {
      logAbort("Error on writing stdout\n");
    }
    closeStream(i);
  }
  while (r > 0)
  {
    stream[i].buffer[r] = 0;
    stream[i].len += r;
    p = strchr(stream[i].buffer, '\n');
    if (p)
    {
      p++;
      if (strstr(stream[i].buffer, "-\n"))
      {
        stream[i].type = CLIENT_JSON_STREAM;
        stream[i].len = 0;
        return;
      }
      logDebug("Write client request to %d msg='%1.*s'\n", stdoutfd, p - stream[i].buffer);
      /* Send output to stdout */
      if (stream[stdoutfd].type == DATA_OUTPUT_STREAM)
      {
        write(stdoutfd, stream[i].buffer, p - stream[i].buffer);
      }
      else if (stream[stdoutfd].type == DATA_OUTPUT_COPY)
      {
        /* Feed it into the NMEA2000 message handler */
        size_t j;

        for (j = 0; j < p - stream[i].buffer; j++)
        {
          handleMessageByte(stream[i].buffer[j]);
        }
        if (strlen(p))
        {
          memcpy(stream[i].buffer, p, strlen(p + 1));
        }
        stream[i].len -= p - stream[i].buffer;
        r -= p - stream[i].buffer;
      }
      /* Else just drop the data on the floor */
    }
    else
    {
      r = 0;
    }
  }
}

void checkReadEvents(void)
{
  fd_set rs;
  struct timeval timeout = {1, 0};
  int r;
  size_t i;
  SOCKET fd;

  logDebug("checkReadEvents maxfd = %d\n", socketFdMax);

  rs = readSet;

  r = select(socketFdMax + 1, &rs, 0, 0, &timeout);

  for (i = socketIdxMin; r > 0 && i <= socketIdxMax; i++)
  {
    fd = stream[i].fd;

    if (fd && FD_ISSET(fd, &rs))
    {
      (stream[i].readHandler)(i);
      r--;
    }
  }
}

void doServerWork(void)
{
  for (;;)
  {
    /* Do a range of non-blocking operations */
    checkReadEvents();       /* Process incoming requests on all clients */
    writeAllClients();        /* Check any timeouts on clients */
  }
}

int main (int argc, char **argv)
{
  struct sigaction sa;

  setProgName(argv[0]);

  FD_ZERO(&activeSet);
  FD_ZERO(&readSet);
  FD_ZERO(&writeSet);

  setFdUsed(stdinfd, DATA_INPUT_STREAM);
  setFdUsed(stdoutfd, DATA_OUTPUT_STREAM);

  while (argc > 1)
  {
    if (strcasecmp(argv[1], "-d") == 0)
    {
      setLogLevel(LOGLEVEL_DEBUG);
    }
    else if (strcasecmp(argv[1], "-q") == 0)
    {
      setLogLevel(LOGLEVEL_ERROR);
    }
    else if (strcasecmp(argv[1], "-o") == 0)
    {
      setFdUsed(stdoutfd, DATA_OUTPUT_COPY);
    }
    else if (strcasecmp(argv[1], "-r") == 0)
    {
      setFdUsed(stdoutfd, DATA_OUTPUT_SINK);
    }
    else if (strcasecmp(argv[1], "-p") == 0)
    {
      if (argc > 2)
      {
        unsigned int uPort;

        if (sscanf(argv[2], "%u", &uPort))
        {
          port = (uint16_t) uPort;
        }
        argc--, argv++;
      }
    }
    else
    {
      fprintf(stderr, "usage: n2kd [-d] [-o] [-p <port>] [-r]\n\n"COPYRIGHT);
      exit(1);
    }
    argc--, argv++;
  }

  socketIdxMin = 0;
  socketIdxMax = 0;

  startTcpServers();

  /*  Ignore SIGPIPE, this will let a write to a socket that's closed   */
  /*  at the other end just fail instead of raising SIGPIPE             */
  memset( &sa, 0, sizeof( sa ) );
  sa.sa_handler = SIG_IGN;
  sigaction( SIGPIPE, &sa, 0 );

  doServerWork();

  exit(0);
}

