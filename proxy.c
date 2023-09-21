#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

typedef struct web_cache {
    char addr[MAXLINE];  // cache된 web의 주소값
    int content_length;  // cache된 content의 길이
    char *ptr_cache;  // cache된 content가 저장된 주소값
    struct web_cache *prev;  // 이전 cache
    struct web_cache *next;  // 다음 cache
} web_cache;

web_cache *first_cache;
web_cache *last_cache;
int cache_size = 0;

void write_cache(web_cache *web);
void read_cache(web_cache *web);
void send_cache(web_cache *web, int clientfd);
web_cache *find_cache(char *uri);

void *thread(void *vargp);
void doit(int clientfd);
void parse_uri(char *uri, char *host, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void transfer_request(rio_t *rio_request, int serverfd, char *method, char *path, char *buf_request);
void transfer_response(rio_t *rio_response, int clientfd, char *uri, char *buf_response);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv) {
  pthread_t tid;
  int listenfd, *clientfd;
  char clienthost[MAXLINE], clientport[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  signal(SIGPIPE, SIG_IGN);

  first_cache = (web_cache *)calloc(1, sizeof(web_cache));
  last_cache = (web_cache *)calloc(1, sizeof(web_cache));

  printf("%s", user_agent_hdr);

  /* Check command line args */
  if (argc != 2) {  // 입력 인자가 2개가 아니면
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  
  printf("현재 port(argv[1]):%s\n", argv[1]);

  listenfd = Open_listenfd(argv[1]);  // 입력 받은 포트에 연결 요청을 받을 준비가 된 socket 생성
  
  while (1) {
    clientlen = sizeof(clientaddr);
    clientfd = Malloc(sizeof(int));
    *clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, clienthost, MAXLINE, clientport, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", clienthost, clientport);
    Pthread_create(&tid, NULL, thread, clientfd);
  }

  return 0;
}

/* uri를 통해 cache가 있는지 확인 */
web_cache *find_cache(char *uri) {
    if(! first_cache)  // cache된 web이 아예 없으면
        return NULL;
    web_cache *current = first_cache;
    while (strcmp(current->addr, uri)) {
        if (!current->next)
            return NULL;
        current = current->next;
        if (!strcmp(current->addr, uri))
            return current;
    }
    return current;
}

/* cache된 내용을 proxy에서 client로 바로 전송 */
void send_cache(web_cache *web, int clientfd) {
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer:Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n\r\n", buf, web->content_length);
    Rio_writen(clientfd, buf, strlen(buf));  // 헤더 파일 전송
    Rio_writen(clientfd, web->ptr_cache, web->content_length);  // content 전송
}

/* 읽은 cache를 리스트의 가장 앞으로 이동 */
void read_cache(web_cache *web) {
    if (web == first_cache)
        return;

    if (web->next) {
        web_cache *previous_web = web->prev;
        web_cache *next_web = web->next;
        if (previous_web)
            web->prev->next = next_web;
        web->next->prev = previous_web;
    } else {
        web->prev->next = NULL;
    }

    web->next = first_cache;
    first_cache = web;
}

/* 새로 작성한 cache를 리스트의 가장 앞으로 이동 */
void write_cache(web_cache *web) {
    cache_size += web->content_length;

    while (cache_size > MAX_CACHE_SIZE) {
        cache_size -= last_cache->content_length;
        last_cache = last_cache->prev;
        free(last_cache->next);
        last_cache->next = NULL;
    }

    if (!first_cache)
        last_cache = web;
    
    if (first_cache) {
        web->next = first_cache;
        first_cache->prev = web;
    }
    first_cache = web;
}

void *thread(void *vargp) {
  int clientfd = *((int *)vargp);
  Pthread_detach(pthread_self());
  Free(vargp);
  doit(clientfd);
  Close(clientfd);
  return NULL;
}

void doit(int clientfd) {
  int serverfd;
  char buf_request[MAXLINE], buf_response[MAXLINE];
  char method[MAXLINE], uri[MAXLINE], version[MAXLINE], host[MAXLINE], port[MAXLINE], path[MAXLINE];
  rio_t rio_request, rio_response;

  /* client가 보낸 request line과 header 읽기 */
  // client fd를 rio_t 타입의 읽기 버퍼(request_rio)와 연결
  // 한 개의 빈 버퍼를 설정, 이 버퍼와 오픈한 cliend fd를 연결
  Rio_readinitb(&rio_request, clientfd);
  // request_rio를 읽고, 이를 메모리 위치 request_buf에 복사, 텍스트 라인을 NULL로 종료
  Rio_readlineb(&rio_request, buf_request, MAXLINE);
  Rio_readlineb(&rio_request, buf_response, MAXLINE);
  printf("Request headers:\n %s\n", buf_request);  // 요청된 라인 출력

  // 요청 라인 parsing을 통해 'method, uri, host, port, path' 찾기
  sscanf(buf_request, "%s %s %s", method, uri, version);
  parse_uri(uri, host, port, path);

  /* cache된 정보가 있다면 cache 정보를 전송 */
  web_cache *cached_web = find_cache(uri);
  if (cached_web) {
      send_cache(cached_web, clientfd);
      read_cache(cached_web);
      return;
  }

  // 지원하지 않는 method에 대한 예외 처리
  if (!(strcasecmp(method, "HEAD") == 0 || strcasecmp(method, "GET") == 0)) {
    clienterror(clientfd, method, "501", "Not implemented", "Proxy does not implement this method");
    return;
  }

  // server 쪽 소켓 생성
  serverfd = Open_clientfd(host, port);
  if (serverfd < 0) {
    clienterror(serverfd, method, "502", "Bad Gateway", "Failed to establish connection with the server");
    return;
  }

  // printf("uri : %s\n", uri);
  Rio_readinitb(&rio_response, serverfd);  // server로부터 response를 받을 소켓 생성
  transfer_request(&rio_request, serverfd, method, path, buf_request);
  transfer_response(&rio_response, clientfd, uri, buf_response);
  
  Close(serverfd);
}

/* uri에서 host, port, path를 parsing */
void parse_uri(char *uri, char *host, char *port, char *path) {
  char *ptr_host = strstr(uri, "//") ? strstr(uri, "//") + 2 : uri;
  char *ptr_port = strchr(ptr_host, ':');
  char *ptr_path = strchr(ptr_host, '/');
  strcpy(path, ptr_path);

  if (ptr_port) {   // port가 있다면
    strncpy(port, ptr_port+1, ptr_path - ptr_port - 1);
    strncpy(host, ptr_host, ptr_port - ptr_host);
  } else {  // port가 없다면
    strcpy(port, "5000");
    strncpy(host, ptr_host, ptr_path - ptr_host);
  }
  // printf("host : %s\n", host);
  // printf("port : %s\n", port);
  // printf("path : %s\n", path);
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body */
  // sprintf는 출력하는 결과 값을 변수에 저정하게 해주는 기능이 있다.
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}

/* client로부터 받은 request를 proxy가 server로 전송 */
void transfer_request(rio_t *rio_request, int serverfd, char *method, char*path, char *buf_request) {
  char buf[MAXLINE];
  
  // server에 전송하기 위해 요청 라인의 형식 변경 : 'method, uri, version' -> 'method, path, HTTP/1.0'
  sprintf(buf, "%s %s %s", method, path, "HTTP/1.0\r\n");
  
  while (strcmp(buf_request, "\r\n")) {
    Rio_readlineb(rio_request, buf_request, MAXLINE);
    if (strstr(buf_request, "User-Agent"))
      sprintf(buf, "%s%s", buf, user_agent_hdr);
    else
      sprintf(buf, "%s%s", buf, buf_request);
  }

  printf("request : %s\n", buf);
  Rio_writen(serverfd, buf, strlen(buf));
}

/* server로부터 받은 response를 proxy가 client로 전송 */
void transfer_response(rio_t *rio_response, int clientfd, char *uri, char *buf_response) {
  int filesize;
  char buf[MAXLINE], *srcp;

  Rio_readlineb(rio_response, buf_response, MAXLINE);
  sprintf(buf, "");

  while(strcmp(buf_response, "\r\n")) {
    sprintf(buf, "%s%s", buf, buf_response);
    if (strstr(buf_response, "Content-length")) {
      filesize = atoi(strchr(buf_response, ':') + 2);
    }
    Rio_readlineb(rio_response, buf_response, MAXLINE);
  }
  sprintf(buf, "%s\r\n", buf);
  // printf("filesize : %d\n", filesize);
  // printf("response : %s\n", buf);
  Rio_writen(clientfd, buf, strlen(buf));
  sprintf(buf, "");
  srcp = Malloc(filesize);
  Rio_readnb(rio_response, srcp, filesize);
  Rio_writen(clientfd, srcp, filesize);

  // 받아온 정보를 cache에 새로 저장
  if (filesize <= MAX_OBJECT_SIZE) {
    web_cache *web = (web_cache *)calloc(1, sizeof(web_cache));
    web->ptr_cache = srcp;
    web->content_length = filesize;
    strcpy(web->addr, uri);
    write_cache(web);
  } else {
    free(srcp);
  }
}