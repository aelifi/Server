#include "hw5-pthread.h"
#include "hw5.h"

static const char ping_request[] = "GET /ping HTTP/1.1\r\n\r\n";
static const char ping_header[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n";
static const char ping_body[] = "pong";

static const char OK200[] = "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n";

static const char echo_request[] = "GET /echo HTTP/1.1\r\n";

static const char write_request[] = "POST /write HTTP/1.1\r\n";
static const char read_request[] = "GET /read HTTP/1.1\r\n";

static const char file_request[]  = "GET /%s HTTP/1.1\r\n";

static int BUFSIZE = 7;
static char buffer[1024] = "<empty>";

static const char BR400[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
static const char NF404[] = "HTTP/1.1 404 Not Found\r\n\r\n";

static const char stat_request[] = "GET /stats HTTP/1.1\r\n\r\n";

static const char content_request[] = "Content-Length: %d";

static int NREQUESTS = 0;
static int NHEADERS = 0;
static int NBODYS = 0;
static int NERRORS = 0;
static int NERROR_BYTES = 0;

static const char stat_body[] = "Requests: %d\nHeader bytes: %d\nBody bytes: %d\nErrors: %d\nError bytes: %d";

sem_t lock;
sem_t items;
sem_t slots;
pthread_mutex_t stats_Lock;
pthread_mutex_t write_Lock;

int clients[100];
int in = 0;
int out = 0;

int Socket(int namespace, int style, int protocol);
void Bind(int sockfd, struct sockaddr *server, socklen_t length);
void Listen(int sockfd, int qlen);
int Accept(int sockfd, struct sockaddr *addr, socklen_t *length_ptr);

static void send_response(int connfd, char header[1024], char body[1024], int HSIZE, int BSIZE){
    int head_sent = send_fully(connfd, header, HSIZE, 0);
    int body_sent =  send_fully(connfd, body, BSIZE, 0);

    pthread_mutex_lock(&stats_Lock);
    NHEADERS += HSIZE;
    NBODYS += BSIZE;
    NREQUESTS += 1;  
    pthread_mutex_unlock(&stats_Lock);

}

static void send_error(int connfd, const char error[]){
    int HSIZE = strlen(error);
    send_fully(connfd, error, HSIZE, 0);

    pthread_mutex_lock(&stats_Lock);
    NERRORS += 1;
    NERROR_BYTES += HSIZE;
    pthread_mutex_unlock(&stats_Lock);
}

static void handle_ping(int connfd){
    char header[1024];
    char body[1024];
    int HSIZE = 0;
    int BSIZE = 0;

    HSIZE = strlen(ping_header);
    memcpy(header, ping_header, HSIZE);

    BSIZE = strlen(ping_body);
    memcpy(body, ping_body, BSIZE);
    send_response(connfd, header, body, HSIZE, BSIZE);   
}

static void handle_echo(int connfd, char* request){
    char header[1024];
    char body[1024];
    int HSIZE = 0;
    int BSIZE = 0;

    char* start = strstr(request, "\r\n");
    start += 2;
    char* end = strstr(request, "\r\n\r\n");

    if(end == NULL){
        end = request + 1024;
    }
    
    *end = '\0';

    BSIZE = strlen(start);
    memcpy(body, start, BSIZE);

    HSIZE = snprintf(header, sizeof(header), OK200, BSIZE);

    send_response(connfd, header, body, HSIZE, BSIZE); 
}

static void handle_read(int connfd, char* request){
    char header[1024];
    char body[1024];
    int HSIZE = 0;
    int BSIZE = 0;

    pthread_mutex_lock(&write_Lock);
    BSIZE = BUFSIZE;
    memcpy(body, buffer, BSIZE);
    pthread_mutex_unlock(&write_Lock);

    HSIZE = snprintf(header, sizeof(header), OK200, BSIZE);

    send_response(connfd, header, body, HSIZE, BSIZE);
}

static void handle_write(int connfd, char* request){
    char header[1024];
    char body[1024];
    int HSIZE = 0;
    int BSIZE = 0;

    char* body_start = strstr(request, "\r\n\r\n");
    body_start += 4;

    char * end = strtok_r(request, "\r\n", &request);
    end = strtok_r(NULL, "\r\n", &request);

    int length = 0;
    while(end != NULL){
        if(sscanf(end, content_request, &length) != 0)
            break;

        end = strtok_r(NULL, "\r\n", &request);
    }

    if(length > 1024)
        length = 1024;

    pthread_mutex_lock(&write_Lock);
    BUFSIZE = length;
    memcpy(buffer, body_start, BUFSIZE);
    pthread_mutex_unlock(&write_Lock);

    pthread_mutex_lock(&write_Lock);
    BSIZE = BUFSIZE;
    memcpy(body, buffer, BUFSIZE);
    pthread_mutex_unlock(&write_Lock);

    HSIZE = snprintf(header, sizeof(header), OK200, BSIZE);
    send_response(connfd, header, body, HSIZE, BSIZE);
}

static void handle_file(int connfd, char* request){
    char header[1024];
    char body[1024];
    int HSIZE = 0;
    int BSIZE = 0;

    static char file_path[128];
    int found = sscanf(request, file_request, file_path);
    assert(found > 0);

    int fd = open(file_path, O_RDONLY);
    if(fd < 0){
        send_error(connfd, NF404);
        close(connfd);
        return;
    }
    
    struct stat buf;
    if(fstat(fd, &buf) < 0){
        perror("fstat");
        exit(1);
    }
    int fsize = buf.st_size;
    HSIZE = snprintf(header, sizeof(header), OK200, fsize);
    send_fully(connfd, header, HSIZE, 0);

    pthread_mutex_lock(&stats_Lock);
    NHEADERS += HSIZE;
    pthread_mutex_unlock(&stats_Lock);

    int files_read = 0;
    int files_sent = 0;
    int total_files = 0;
    while(total_files < fsize){
        files_read = read(fd, body, sizeof(body));
        files_sent = 0;
        files_sent = send_fully(connfd, body, files_read, 0);
    
        while(files_sent != files_read){
            files_sent += send_fully(connfd, body + files_sent, files_read - files_sent, 0);  
        }
        total_files += files_sent;

        pthread_mutex_lock(&stats_Lock);
        NBODYS += files_sent;
        pthread_mutex_unlock(&stats_Lock);
    }  
    pthread_mutex_lock(&stats_Lock);
    NREQUESTS += 1;
    pthread_mutex_unlock(&stats_Lock);

    close(fd);
}

static void handle_stat(int connfd){
    char header[1024];
    char body[1024];
    int HSIZE = 0;
    int BSIZE = 0;

    pthread_mutex_lock(&stats_Lock);
    BSIZE = snprintf(body, sizeof(body), stat_body, NREQUESTS, NHEADERS, NBODYS, NERRORS, NERROR_BYTES);
    HSIZE = snprintf(header, sizeof(header), OK200, BSIZE);
    pthread_mutex_unlock(&stats_Lock);

    send_response(connfd, header, body, HSIZE, BSIZE);
}

static void* handle_request(void* fd){
    pthread_detach(pthread_self());

    while(1){
        sem_wait(&items);
        sem_wait(&lock);
        int connfd = clients[out];
        out = (out + 1) % 10;
        sem_post(&lock);
        sem_post(&slots);

        char request[2048];
        int amt = recv_http_request(connfd, request, sizeof(request), 0);
        
        if(amt == 0){
            close(connfd);
        }
        else if(!strncmp(request, ping_request, strlen(ping_request))){
            handle_ping(connfd);
        } else if(!strncmp(request, echo_request, strlen(echo_request))){
            handle_echo(connfd, request);
        } else if(!strncmp(request, write_request, strlen(write_request))){
            handle_write(connfd, request);
        } else if(!strncmp(request, read_request, strlen(read_request))){
            handle_read(connfd, request);
        } else if(!strncmp(request, stat_request, strlen(stat_request))){
            handle_stat(connfd);
        } else if(!strncmp(request, "GET ", 4)){
            handle_file(connfd, request);
        } else{
            send_error(connfd, BR400);
        }
        close(connfd);
    }
    return NULL;
}

static int open_listenfd(int port){
    int listenfd = Socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &(server.sin_addr));

    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    Bind(listenfd, (struct sockaddr*)&server, sizeof(server));

    Listen(listenfd, 10);
    return listenfd;
}

int create_server_socket(int port, int threads) {
    int listenfd = open_listenfd(port);
    sem_init(&lock, 0, 1);
    sem_init(&items, 0, 0);
    sem_init(&slots, 0, 10);
    pthread_mutex_init(&write_Lock, NULL);
    pthread_mutex_init(&stats_Lock, NULL);
    
    pthread_t tid[threads];
    for(int i = 0; i < threads; i++){
        pthread_create(&tid[i], NULL, handle_request, NULL);
    }
    
    return listenfd;
}

void accept_client(int server_socket) {
    static struct sockaddr_in client;
    static socklen_t client_size;

    memset(&client, 0, sizeof(client));
    memset(&client_size, 0, sizeof(client_size));

    int connfd = Accept(server_socket, (struct sockaddr*)&client, &client_size);

    sem_wait(&slots);
    sem_wait(&lock);
    clients[in] = connfd;
    in = (in + 1) % 10;
    sem_post(&lock);
    sem_post(&items);
}

int Socket(int namespace, int style, int protocol){
    int sockfd = socket(namespace, style, protocol);
    if(sockfd < 0){
        perror("socket");
        exit(1);
    }
    return sockfd;
}

void Bind(int sockfd, struct sockaddr *server, socklen_t length){
    if(bind(sockfd, server, length) < 0){
        perror("bind");
        exit(1);
    }
}

void Listen(int sockfd, int qlen){
    if(listen(sockfd, qlen) < 0){
        perror("listen");
        exit(1);
    }
}

int Accept(int sockfd, struct sockaddr *addr, socklen_t *length_ptr){
    int newfd = accept(sockfd, addr, length_ptr);
    if(newfd < 0){
        perror("accept");
        exit(1);
    }
    return newfd;
}