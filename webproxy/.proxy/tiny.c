/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

int main(int argc, char **argv) {
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2) {  // 입력 인자가 2개가 아니면
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  /* 듣기 소켓 오픈 
   * argv는 main 함수가 받은 각각의 인자들
   * argv[1]은 ./tiny 8000 명령어를 입력할 때 8000 (우리가 입력하는 port 번호), 듣기 식별자 return
   */
  printf("현재 port(argv[1]):%s\n", argv[1]); 
  listenfd = Open_listenfd(argv[1]);  // 8000 포트에 연결 요청을 받을 준비가 된 듣기 식별자 return
  while (1) {
    clientlen = sizeof(clientaddr);
    /* server는 accept함수를 호출해서 client로부터의 연결 요청을 기다린다.
     * client 소켓은 server 소켓의 주소를 알고 있으니까
     * client에서 server로 넘어올 때 add 정보를 가지고 올 것이라고 가정
     * accept 함수는 연결되면 식별자 connfd를 return한다.
     */
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);  // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);   // line:netp:tiny:doit, connfd로 트랜잭션 수행
    Close(connfd);  // line:netp:tiny:close, connfd로 자신 쪽의 연결 끝 닫기
  }
}

/* client의 요청 라인을 확인하고 정적, 동적 콘텐츠를 확인하고 return */
void doit(int fd) {  // fd는 connfd라는 것이 중요
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  /* Read request line and headers */
  // 요청 라인을 읽고 분석한다.
  // 식별자 fd를 rio_t 타입의 읽기 버퍼(rio)와 연결
  // 한 개의 빈 버퍼를 설정하고, 이 버퍼와 한 개의 오픈한 파일 식별자를 연결
  Rio_readinitb(&rio, fd);
  // 다음 텍스트 줄을 파일 rio에서 읽고, 이를 메모리 위치 buf로 복사하고, 텍스트 라인을 NULL로 종료시킨다.
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers:\n");
  printf("%s", buf);  // 요청된 라인을 printf로 보여준다. (최초 요청 라인 : GET/HTTP/1.1)
  sscanf(buf, "%s %s %s", method, uri, version);  // buf의 내용을 method, uri, version이라는 문자열에 저장한다.
  if (!(strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0)) {  // GET, HEAD 메소드만 지원한다. (두 문자가 같으면 0)
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");  // 다른 메소드가 들어올 경우, 에러를 출력하고
    return;  // main으로 return시킨다.
  }
  read_requesthdrs(&rio);  // 요청 라인을 제외한 요청 헤더를 출력한다.

  /* Parse URI from GET request */
  // URI를 filename과 CGI argument string으로 parse하고
  // request가 정적인지 동적인지 확인하는 flag를 return한다. (1이면 정적 컨텐츠)
  is_static = parse_uri(uri, filename, cgiargs);  // uri 내용을 바탕으로 filename, cgiargs가 채워진다.
  if (stat(filename, &sbuf) < 0) {  // disk에 파일이 없다면 filename을 sbuf에 넣는다. 종류, 크기 등이 sbuf에 저장된다. 성공시 0 실패시 -1
    clienterror(fd, filename, "404", "Not found", "Tiny couldn’t find this file");
    return;
    }

  if (is_static) { /* Serve static content */
    // S_ISREG : 파일 종류 확인 - 일반(regular) 파일인지 판별
    // S_IRUSR : 읽기 권한을 가지고 있는지 판별
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {  // 일반 파일이 아니거나 읽기 권한이 없으면
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn’t read the file");  // 읽을 수 없다고 출력한다.
      return;
    }
    serve_static(fd, filename, sbuf.st_size, method);  // fd : connfd 정적 컨텐츠를 client에게 제공
  }
  else { /* Serve dynamic content */
    // 파일이 실행 가능한지 검증
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn’t run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs, method);  // 실행 가능하다면 동적 컨텐츠를 client에게 제공
  }
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
// tiny는 request header의 정보를 하나도 사용하지 않는다.
// 요청 라인 한 줄, 요청 헤더 여러 줄을 받는다.
// 요청 라인은 저장해주고, 요청 헤더들은 그냥 출력한다.
void read_requesthdrs(rio_t *rp) {
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  // 한 줄을 읽어들인다. ('\n'을  만나면 break되는 식으로)
  // strcmp(str1, str2) 같은 경우는 0을 반환 -> 이 경우만 탈출
  // buf가 '\r\n'이 되는 경우는 모든 줄을 읽고 나서 마지막 줄에 도착한 경우이다.
  // 헤더의 마지막 줄은 비어있다.
  while(strcmp(buf, "\r\n")) {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);  // 한줄 한줄 읽은 것을 출력한다.
  }
  return;
}

// tiny는 정적 컨텐츠를 위한 홈 디렉토리가 자신의 현재 디렉토리이고
// 실행 파일의 홈 디렉토리는 /cgi-bin이라고 가정한다.
int parse_uri(char *uri, char *filename, char *cgiargs) {
  char *ptr;
  // strstr (대상 문자열, 검색할 문자열) -> 검색된 문자열(대상 문자열) 뒤에 모든 문자열이 나오게 된다.
  // uri에서 "cgi-bin"이라는 문자열이 없으면, static content
  if (!strstr(uri, "cgi-bin")) { /* Static content */
    strcpy(cgiargs, "");  // cgiargs 인자 string을 지운다.
    strcpy(filename, ".");  // 상대 리눅스 경로 이름으로 변환 ex) '.'
    strcat(filename, uri);  // 상대 리눅스 경로 이름으로 변환 ex) '.' + '/index.html'
    if (uri[strlen(uri)-1] == '/')  // uri가 '/'문자로 끝난다면
      strcat(filename, "home.html");  // 기본 파일 이름인 home.html을 추가한다. -> 11.10 과제에서 adder.html로 변경
    return 1;
  }
  // uri에서 "cgi-bin"이라는 문자열이 존재한다면
  // 모든 CGI 인자들을 추출한다.
  // index : 첫 번째 인자에서 두 번째 인자를 찾는다. 찾으면 문자의 위치 포인터를 못 찾으면 NULL을 반환
  else { /* Dynamic content */ 
    ptr = index(uri, '?');
    if (ptr) {
      strcpy(cgiargs, ptr+1);
      *ptr = '\0';
    }
    else
      strcpy(cgiargs, "");  // 없으면 빈 칸
    // 나머지 URI 부분을 상대 리눅스 파일 이름으로 변환
    strcpy(filename, ".");
    strcat(filename, uri);
    return 0;
  }
}
// static 컨텐츠를 요청하면 서버가 disk에서 파일을 찾아서 메모리 영역으로 복사하고, 복사한 것을 client fd로 복사
void serve_static(int fd, char *filename, int filesize, char *method) {
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client */
  get_filetype(filename, filetype);  // 5개 중에 무슨 파일 형식인지 검사해서 filetype을 채워넣는다.
  // client에 응답 줄과 헤더를 보낸다.
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);  // while을 한 번 돌면 close가 되고, 새로 연결하더라도 새로 connect하므로 close가 default가 된다.
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);  // 여기 \r\n 빈 줄 하나가 헤더 종료 표시
  Rio_writen(fd, buf, strlen(buf));  // buf에서 strlen(buf) 바이트 만큼 fd로 전송한다.
  printf("Response headers:\n");
  printf("%s", buf);

  if (strcasecmp(method, "HEAD") == 0)
    return;

  /* Send response body to client */
  // open(열려고 하는 대상 파일의 이름, 파일을 열 때 적용되는 열기 옵션, 파일을 열 때의 접근 권한 설명)
  // return file descriptor
  // O_RDONLY : 읽기 전용으로 파일 열기
  // 즉, filename의 파일을 읽기 전용으로 열어서 식별자를 받아온다.
  srcfd = Open(filename, O_RDONLY, 0);
  // 요청한 파일을 disk에서 가상 메모리 영역으로 mapping한다.
  // mmap을 호출하면 파일 srcfd의 첫 번째 filesize 바이트를 주소 srcp에서 시작하는 사적 읽기-허용 가상 메모리 영역으로 mapping
  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  
  // mmap 대신 malloc 사용 -> 빈 칸에 사용해야 하므로 빈 공간에 메모리를 읽어야한다
  srcp = (char *)Malloc(filesize);  // 메모리 할당
  Rio_readn(srcfd, srcp, filesize);  // 파일을 읽어서 메모리에 저장
  // 파일을 메모리로 mapping한 후 더 이상 이 식별자는 필요가 없으므로 닫기
  Close(srcfd);
  // 실제로 파일을 client에게 전송
  // rio_writen 함수는 주소 srcp에서 시작하는 filesize를 client의 연결 식별자 fd로 복사
  Rio_writen(fd, srcp, filesize);  // client에게 데이터를 보낸다.

  Free(srcp);  //메모리 해제
}

/*
 * get_filetype - Derive file type from filename
 */

void get_filetype(char *filename, char *filetype) {
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4"))  // MP4 비디오 파일 추
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs, char *method) {
  char buf[MAXLINE], *emptylist[] = { NULL };

  /* Return first part of HTTP response */
  // 클라이언트에 성공을 알려주는 응답 라인을 보내는 것으로 시작
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  if (Fork() == 0) { /* Child */
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1);
    setenv("REQUEST_METHOD", method, 1);
    Dup2(fd, STDOUT_FILENO); /* Redirect stdout to client */
    Execve(filename, emptylist, environ); /* Run CGI program */
  }
  Wait(NULL); /* Parent waits for and reaps child */
}