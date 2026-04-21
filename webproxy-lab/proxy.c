#include <stdio.h>
#include "csapp.h"

#define MAX_HEADER_SIZE (8 * MAXLINE)

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

/*
 * TODO: 전체 구현 큰 흐름
 * 1. main에서 포트를 받아 listening socket을 연다.
 * 2. accept loop로 클라이언트 연결을 계속 받는다.
 * 3. 연결 하나를 처리하는 함수에서:
 *    - 요청 라인(method, uri, version) 읽기
 *    - URI에서 host / port / path 파싱
 *    - 서버로 보낼 새 HTTP/1.0 요청 만들기
 *    - origin server와 연결
 *    - 서버 응답을 클라이언트로 relay
 *
 * TODO: 시작 순서 추천
 * 1. 멀티스레드 없이 단일 연결 프록시를 먼저 완성한다.
 * 2. GET 요청만 처리되게 만든다.
 * 3. Host, User-Agent, Connection, Proxy-Connection 헤더를 올바르게 맞춘다.
 * 4. 응답 전달이 안정화되면 그 다음에 thread를 추가한다.
 *
 * TODO: 구현하면서 helper 함수로 분리하면 좋은 후보
 * - parse_uri(...)
 * - read_requesthdrs(...)
 * - build_http_header(...)
 * - doit(int connfd)
 * - forward_response(...)
 */

int build_http_header(char *request, size_t request_size, char *path, char *host,
                      char *host_hdr, char *other_hdrs, int has_host);
int append_header(char *dst, size_t dst_size, size_t *used, char *src);
void *worker(void *arg);
void doit(int fd);
int parse_uri(char *uri, char *host, char *port, char *path);
int read_requesthdrs(rio_t *rp, char *host_hdr, size_t host_hdr_size,
                     char *other_hdrs, size_t other_hdrs_size, int *has_host);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  pthread_t tid;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]);
  Signal(SIGPIPE, SIG_IGN);

  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);

    int *connfd_cp = Malloc(sizeof(int));
    *connfd_cp = connfd;
    Pthread_create(&tid, NULL, worker, connfd_cp);
    Pthread_detach(tid);
    // doit(connfd);
    // Close(connfd);
  }
  return 0;
}

void *worker(void *arg) {
  int connfd = *((int *)arg);
  Free(arg);

  doit(connfd);
  Close(connfd);
  return NULL;
}

void doit(int fd) {
  // 1. 요청 읽고, 파싱
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char host[MAXLINE], port[MAXLINE], path[MAXLINE];
  char request[MAX_HEADER_SIZE];
  int parsed;
  int request_error = 0;

  rio_t client_rio, server_rio;
  ssize_t n;
  int serverfd;

  char host_hdr[MAX_HEADER_SIZE];
  char other_hdrs[MAX_HEADER_SIZE];
  int has_host = 0;

  rio_readinitb(&client_rio, fd);
  if (rio_readlineb(&client_rio, buf, MAXLINE) <= 0) {
    return;
  }

  printf("%s", buf);
  // 요청 라인에서 GET, /..., HTTP/1.0 을 빠르게 분리하기 위한 도구
  parsed = sscanf(buf, "%s %s %s", method, uri, version);
  if (parsed != 3) {
    clienterror(fd, "Bad Request", "400", "Bad Request", "Tiny does not accept this request");
    return;
  }

  // 지금은 GET 만 요청을 받을 수 있음. -> 그 외의 method는 if문 안에 들어가서 실행 불가 판정
  if (strcasecmp(method, "GET")) {
    clienterror(fd, method, "501", "Not Implemented", "Tiny does not implement this method");
    return;
  }
  if (parse_uri(uri, host, port, path) == -1) {
    clienterror(fd, method, "501", "Not Implemented", "Tiny does not implement this method");
    return;
  }

  if (read_requesthdrs(&client_rio, host_hdr, sizeof(host_hdr),
                       other_hdrs, sizeof(other_hdrs), &has_host) < 0) {
    return;
  }

  // 2. 클라이언트가 되어서 서버 연결
  serverfd = Open_clientfd(host, port);
  if (serverfd < 0) {
    clienterror(fd, method, "502", "Bad Gateway", "Tiny can't enter this Gateway");
    return;
  }
  rio_readinitb(&server_rio, serverfd);

  // 3. 요청 전달
  // snprintf(request, MAXLINE, "GET %s HTTP/1.0\r\n", path);
  // snprintf(request + strlen(request), MAXLINE - strlen(request), "Host: %s\r\n", host);
  // snprintf(request + strlen(request), MAXLINE - strlen(request), "%s", user_agent_hdr);
  // snprintf(request + strlen(request), MAXLINE - strlen(request), "Connection: close\r\n");
  // snprintf(request + strlen(request), MAXLINE - strlen(request), "Proxy-Connection: close\r\n");
  // snprintf(request + strlen(request), MAXLINE - strlen(request), "\r\n");
  request_error = build_http_header(request, sizeof(request), path, host,
                                    host_hdr, other_hdrs, has_host);

  if (request_error) {
    Close(serverfd);
    return;
  }

  printf("---- origin request ----\n%s\n", request);

  if (rio_writen(serverfd, request, strlen(request)) != (ssize_t)strlen(request)) {
    Close(serverfd);
    return;
  }

  // 4. 응답 전달
  while ((n = rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
    if (rio_writen(fd, buf, n) != n) {
      break;
    }
  }
  Close(serverfd);
}

int build_http_header(char *request, size_t request_size, char *path, char *host,
                      char *host_hdr, char *other_hdrs, int has_host) {
  size_t used = 0;
  int written;

  written = snprintf(request, request_size, "GET %s HTTP/1.0\r\n", path);
  if (written < 0 || (size_t)written >= request_size) {
    return -1;
  }
  used = written;

  if (has_host) {
    if (append_header(request, request_size, &used, host_hdr) < 0) {
      return -1;
    }
  } else {
    written = snprintf(request + used, request_size - used, "Host: %s\r\n", host);
    if (written < 0 || (size_t)written >= request_size - used) {
      return -1;
    }
    used += written;
  }

  if (append_header(request, request_size, &used, (char *)user_agent_hdr) < 0) {
    return -1;
  }
  if (append_header(request, request_size, &used, "Connection: close\r\n") < 0) {
    return -1;
  }
  if (append_header(request, request_size, &used, "Proxy-Connection: close\r\n") < 0) {
    return -1;
  }
  if (append_header(request, request_size, &used, other_hdrs) < 0) {
    return -1;
  }
  if (append_header(request, request_size, &used, "\r\n") < 0) {
    return -1;
  }

  return 0;
}

int append_header(char *dst, size_t dst_size, size_t *used, char *src) {
  size_t len = strlen(src);

  if (*used + len >= dst_size) {
    return -1;
  }
  memcpy(dst + *used, src, len);
  *used += len;
  dst[*used] = '\0';
  return 0;
}

int parse_uri(char *uri, char *host, char *port, char *path) {
  char *hostbegin;
  char *hostend;
  char *pathbegin;
  int len;

  /*
   * proxy용 parse_uri
   *
   * 입력 예:
   *   http://example.com:8080/index.html
   *
   * 출력 예:
   *   host = "example.com"
   *   port = "8080"
   *   path = "/index.html"
   *
   * 예외 처리:
   *   - port가 없으면 기본값 "80"
   *   - path가 없으면 기본값 "/"
   */

  /* 1. uri가 "http://"로 시작하는지 확인한다. */
  /*    - 아니면 실패를 반환한다. */
  if (strncasecmp(uri, "http://", 7) != 0)
    return -1;

  /* 2. host가 시작하는 위치를 잡는다. */
  /*    - "http://" 바로 뒤가 host 시작 위치다. */
  hostbegin = uri + 7;

  /* 3. host 뒤에 path가 시작되는 '/'를 찾는다. */
  /*    - 있으면 그 위치부터 path다. */
  /*    - 없으면 path는 "/"로 설정한다. */
  pathbegin = strchr(hostbegin, '/');

  /* 4. path를 path 버퍼에 저장한다. */
  /*    - '/'를 찾았으면 그 위치부터 끝까지 복사한다. */
  /*    - 못 찾았으면 "/"를 저장한다. */

  /* 5. host[:port] 부분의 길이를 계산한다. */
  /*    - '/'가 있으면 host 시작부터 '/' 직전까지 길이 */
  /*    - '/'가 없으면 host 시작부터 문자열 끝까지 길이 */
  if (pathbegin) {
    strcpy(path, pathbegin);
    len = pathbegin - hostbegin;
  } else {
    strcpy(path, "/");
    len = strlen(hostbegin);
  }

  /* 6. host[:port] 부분만 host 버퍼에 먼저 복사한다. */
  /*    - 아직은 "example.com:8080" 같은 형태일 수 있다. */
  /*    - 마지막에 문자열 끝('\0')도 꼭 붙인다. */
  strncpy(host, hostbegin, len);
  host[len] = '\0';

  /* 7. host 안에 ':'가 있는지 찾아본다. */
  /*    - ':'가 있으면 host와 port를 분리한다. */
  /*    - ':' 앞은 host, ':' 뒤는 port다. */

  /* 8. ':'가 없으면 port를 기본값 "80"으로 설정한다. */
  hostend = strchr(host, ':');
  if (hostend) {
    *hostend = '\0';
    strcpy(port, hostend + 1);
  } else {
    strcpy(port, "80");
  }

  /* 9. 정상적으로 파싱이 끝났으면 성공 값을 반환한다. */
  return 0;
}

int read_requesthdrs(rio_t *rp, char *host_hdr, size_t host_hdr_size,
                     char *other_hdrs, size_t other_hdrs_size, int *has_host) {
  char buf[MAXLINE];
  ssize_t n;
  size_t used;

  host_hdr[0] = '\0';
  other_hdrs[0] = '\0';
  *has_host = 0;

  // rp -> rio_pointer 
  // rio_t -> 소켓이나 파일에서 안전하게 읽고 쓰기 위한 버퍼 구조체
  while ((n = rio_readlineb(rp, buf, MAXLINE)) > 0) {
    // "\r\n"을 만날 때까지 계속 한 줄씩 읽는다.
    if (!strcmp(buf, "\r\n")) {
      break;
    }

    if (strncasecmp(buf, "Host:", 5) == 0) {
      // host 처리
      if (strlen(buf) >= host_hdr_size) {
        return -1;
      }
      strcpy(host_hdr, buf);
      *has_host = 1;
    } else if (strncasecmp(buf, "Connection:", 11) == 0) {
      // 무시
    } else if (strncasecmp(buf, "Proxy-Connection:", 17) == 0) {
      // 무시
    } else if (strncasecmp(buf, "User-Agent:", 11) == 0) {
      // 무시
    } else {
      // other_hdrs에 추가
      used = strlen(other_hdrs);
      if (append_header(other_hdrs, other_hdrs_size, &used, buf) < 0) {
        return -1;
      }
    }
  }

  if (n < 0) {
    return -1;
  }
  return 0;
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
  char buf[MAXLINE], body[MAXBUF];
  size_t used = 0;
  int written;

  written = snprintf(body + used, sizeof(body) - used, "<html><title>Tiny Error</title>");
  if (written < 0 || (size_t)written >= sizeof(body) - used) {
    return;
  }
  used += written;
  written = snprintf(body + used, sizeof(body) - used, "<body bgcolor=ffffff>\r\n");
  if (written < 0 || (size_t)written >= sizeof(body) - used) {
    return;
  }
  used += written;
  written = snprintf(body + used, sizeof(body) - used, "%s: %s\r\n", errnum, shortmsg);
  if (written < 0 || (size_t)written >= sizeof(body) - used) {
    return;
  }
  used += written;
  written = snprintf(body + used, sizeof(body) - used, "<p>%s: %s\r\n", longmsg, cause);
  if (written < 0 || (size_t)written >= sizeof(body) - used) {
    return;
  }
  used += written;
  written = snprintf(body + used, sizeof(body) - used, "<hr><em>The Tiny Web server</em>\r\n");
  if (written < 0 || (size_t)written >= sizeof(body) - used) {
    return;
  }
  used += written;

  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  if (rio_writen(fd, buf, strlen(buf)) < 0) {
    return;
  }
  sprintf(buf, "Content-type: text/html\r\n");
  if (rio_writen(fd, buf, strlen(buf)) < 0) {
    return;
  }
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  if (rio_writen(fd, buf, strlen(buf)) < 0) {
    return;
  }
  rio_writen(fd, body, strlen(body));
}
