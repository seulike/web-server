/*
 * server.c - A multi-threads echo server based on epoll
 */
#include "csapp.h"
#include "sbuf.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg);
void check_clients(int fd);
void *thread(void *vargp);
void addsig(int sig);
void sig_handler(int sig);
void setnonblocking(int fd);
void addfd(int epollfd, int fd);

#define MAX_EVENTS 100
#define NTHREADS 10
#define SBUFSIZE 16

sbuf_t sbuf;
int epollfd;
static int pipefd[2];

//begin the main
int main(int argc, char **argv)
{
    int listenfd, connfd, port, n, flags;
    socklen_t clientlen = sizeof(struct sockaddr_in);
    struct sockaddr_in clientaddr;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    //initial the listen port
    port = atoi(argv[1]);
    listenfd = Open_listenfd(port);

    //initial the task pool
    sbuf_init(&sbuf, SBUFSIZE);

    //create the threads
    pthread_t tid;
    int i;
    for (i = 0; i < NTHREADS; i++)
        Pthread_create(&tid, NULL, thread, NULL);

    //create pipefd to handle signal in main loop
    if (socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd) == -1) {
        perror("pipe fail");
        exit(errno);
    }
    setnonblocking(pipefd[1]);

    struct epoll_event ev, events[MAX_EVENTS];
    int conn_sock, nfds;
    epollfd = epoll_create(100);    //epollfd 应为一文件描述符

    if (epollfd == -1) {
        perror("epoll_create");
        exit(errno);
    }
    //add events to epoll
    addfd(epollfd, listenfd);
    addfd(epollfd, pipefd[0]);

    //register signal
    addsig(SIGHUP);
    addsig(SIGINT);
    addsig(SIGQUIT);
    addsig(SIGPIPE);
    //sigquit will set stopserver to kill the server
    int stopserver = 0;
    while (!stopserver) {
        /* Wait for listening/connected descriptor(s) to become ready */
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_pwait");
    //      exit(errno);
            continue;
        }
        for (n = 0; n < nfds; ++n) {
            if (events[n].data.fd == listenfd) {
                conn_sock = Accept(listenfd, (SA *)&clientaddr, &clientlen);
                if (conn_sock == -1)
                {
                    perror("accept");
                    exit(errno);
                }
                addfd(epollfd, conn_sock);
            }
            //handle the signals
            else if ((events[n].data.fd == pipefd[0])) {
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) {
                    perror("pipe read problems");
                    continue;
                }
                else {
                    int i;
                    for (i = 0; i < ret; i++) {
                        switch(signals[i]) {
                            case SIGHUP:
                                printf("SIGHUP received: %d %d ret:%d\n",signals[i], i, ret);
                                break;

                            case SIGQUIT:
                                printf("SIGQUIT received: %d %d ret:%d\n",signals[i], i, ret);
                                stopserver = 1;
                                break;
                            case SIGINT:
                                printf("SIGINT received: %d %d ret:%d\n",signals[i], i, ret);
                                break;
                            case SIGPIPE:
                                printf("SIGPIPE received: %d %d ret:%d\n",signals[i], i, ret);
                                break;
                        }
                    }

                }
            }
            /* Echo a text line from each ready connected descriptor */
            else if (events[n].data.fd > 6 && (events[n].events & EPOLLIN))  //first else{} has problems,then add the judge
                sbuf_insert(&sbuf, events[n].data.fd);

        }
    }
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
    /* $end echoserversmain */
}
//set the fd to nonblocking
void setnonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}
//add fd to epoll
void addfd(int epollfd, int fd)
{
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        printf("epoll_ctl: %d",fd);
        exit(errno);
    }
    setnonblocking(fd);
}
//signal handling
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
//register signal
void addsig(int sig)
{
    struct sigaction action;
    memset(&action, '\0', sizeof(action));

    action.sa_handler = sig_handler;
    sigfillset(&action.sa_mask);
    action.sa_flags |= SA_RESTART;

    if(sigaction(sig, &action, NULL) < 0)
        unix_error("Signal error");
}
//the thread get tasks from task poll then handle it
void *thread(void *varg)
{
    Pthread_detach(pthread_self());
    while (1) {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
        close(connfd);
    }
}
//  doit - handle one HTTP request/response transaction
void doit(int fd)
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;
    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);                   //line:netp:doit:readrequest
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }                                                    //line:netp:doit:endrequesterr
    read_requesthdrs(&rio);                              //line:netp:doit:readrequesthdrs

    /* Parse URI from GET request */
    is_static = parse_uri(uri, filename, cgiargs);       //line:netp:doit:staticcheck
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found",
                    "Tiny couldn't find this file");
        return;
    }                                                    //line:netp:doit:endnotfound
    /* Serve static content */
    if (is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, sbuf.st_size);        //line:netp:doit:servestatic
    }
    /* Serve dynamic content */
    else {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs);            //line:netp:doit:servedynamic
    }
}
/*
 * read_requesthdrs - read and parse HTTP request headers
 */
/* $begin read_requesthdrs */
void read_requesthdrs(rio_t *rp)
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
//        printf("%s", buf);
    }
    return;
}
/* $end read_requesthdrs */

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 */
/* $begin parse_uri */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {  /* Static content */ //line:netp:parseuri:isstatic
        strcpy(cgiargs, "");                             //line:netp:parseuri:clearcgi
        strcpy(filename, ".");                           //line:netp:parseuri:beginconvert1
        strcat(filename, uri);                           //line:netp:parseuri:endconvert1
        if (uri[strlen(uri)-1] == '/')                   //line:netp:parseuri:slashcheck
            strcat(filename, "home.html");               //line:netp:parseuri:appenddefault
        return 1;
    }
    else {  /* Dynamic content */                        //line:netp:parseuri:isdynamic
        ptr = index(uri, '?');                           //line:netp:parseuri:beginextract
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        }
        else
            strcpy(cgiargs, "");                         //line:netp:parseuri:endextract
        strcpy(filename, ".");                           //line:netp:parseuri:beginconvert2
        strcat(filename, uri);                           //line:netp:parseuri:endconvert2
        return 0;
    }
}
/* $end parse_uri */

/*
 * serve_static - copy a file back to the client
 */
/* $begin serve_static */
void serve_static(int fd, char *filename, int filesize)
{
    int srcfd,cnt;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */
    get_filetype(filename, filetype);       //line:netp:servestatic:getfiletype
    sprintf(buf, "HTTP/1.0 200 OK\r\n");    //line:netp:servestatic:beginserve
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);

    // Send response body to client
    srcfd = Open(filename, O_RDONLY, 0);    //line:netp:servestatic:open
    /*
        srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);//line:netp:servestatic:mmap
        Close(srcfd);                           //line:netp:servestatic:close
        Rio_writen(fd, srcp, filesize);         //line:netp:servestatic:write
        Munmap(srcp, filesize);                 //line:netp:servestatic:munmap
    */
    if (!strcmp(filetype, "text/php")) {
        sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
	    Rio_writen(fd, buf, strlen(buf));       //line:netp:servestatic:endserve
		if (fork() == 0) {
            dup2(fd, STDOUT_FILENO);
			execlp("php", "php", filename, (char *)0);
     //     execlp("/usr/bin/php", "php", "/mnt/share/sbuf/test.php", (char *)0);
		}
        Wait(NULL);
        return;
	}
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));       //line:netp:servestatic:endserve
    if ((cnt = sendfile(fd,srcfd,0,filesize)) < 0) {
        printf("fd:%d srcfd:%d", fd, srcfd);
        perror("sendfile");
        exit(errno);
    }
    close(srcfd);  //at first forget to close,cause the main loop problems
//    printf("Server sent %d bytes.\n", filesize);
}

/*
 * get_filetype - derive file type from file name
 */
void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".php"))
		strcpy(filetype, "text/php");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else
        strcpy(filetype, "text/plain");
}
/* $end serve_static */

/*
 * serve_dynamic - run a CGI program on behalf of the client
 */
/* $begin serve_dynamic */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n\r\n", buf);//in the book "\r\n" should be "\r\n\r\n"
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) { /* child */ //line:netp:servedynamic:fork
        /* Real server would set all CGI vars here */
        setenv("QUERY_STRING", cgiargs, 1); //line:netp:servedynamic:setenv
        Dup2(fd, STDOUT_FILENO);         /* Redirect stdout to client */ //line:netp:servedynamic:dup2
        execl(filename, filename, (char *)0, environ);
    //  Execve(filename, emptylist, environ); /* Run CGI program */ //line:netp:servedynamic:execve
    }
    Wait(NULL); /* Parent waits for and reaps child */ //line:netp:servedynamic:wait
}
/* $end serve_dynamic */

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
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
/* $end clienterror */

