#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>





#define MAX_CLIENTS 10
#define MAX_ELEMENT_SIZE 10*(1<<20) //10MB
#define MAX_CACHE_SIZE 200*(1<<20) //200MB
typedef struct cache_element cache_element;

/*
Defining the element stored in the cache
which has come from the server
*/
struct cache_element {
    char* data;
    int   len;
    char* url;
    time_t lru_time_track;
    cache_element *next;
};

//function definitions
cache_element find_cache_element(char *url);
int add_cache_element(char *url, char *data, int len);
void delete_cache_element(char *url);

//defining port number for proxy server
int port_number = 8080;
int proxy_server_socket;
pthread_t thread_id[MAX_CLIENTS];
sem_t semaphore;
pthread_mutex_t lock; 

cache_element *cache_head = NULL;
int cache_size;

int checkHTTPversion(char *version){
    if(strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0){
        return 1;
    }
    return -1;
}

void sendErrorMessage(int client_socket, int error_code){
    char *error_message;
    switch(error_code){
        case 400:
            error_message = "HTTP/1.1 400 Bad Request\r\n\r\n";
            break;
        case 404:
            error_message = "HTTP/1.1 404 Not Found\r\n\r\n";
            break;
        case 500:
            error_message = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
            break;
        default:
            error_message = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
            break;
    }
    send(client_socket, error_message, strlen(error_message), 0);
}

int connectRemoteServer(char *host_addr,int port_num){
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(remoteSocket < 0){
        printf("Error creating socket\n");
        return -1;
    }
    struct hostent *host = gethostbyname(host_addr);
    if(host == NULL){
        fprintf(stderr,"No such host exist\n");
        return -1;
    }
    struct socketaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);

    if(connect(remoteSocket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        fprintf(stderr,"Error connecting to remote server\n");
        return -1;
    }
    return remoteSocket;

}

int handle_request(int clinetSocketId,ParsedRequest *request,char* tempReq){
    char *buffer = (char *)malloc(sizeof(char) * 4096);
    bzero(buffer, sizeof(buffer));
    strcpy(buffer, "GET ");
    strcat(buffer, request->path);
    trcat(buffer, " ");
    strcat(buffer, " request->version");
    strcat(buffer, "\r\n");

    size_t len = strlen(buffer);

    if(ParsedHeader_set(request,"Connection", "close") < 0){
        printf("Error setting connection header\n");
        return -1;
    }

    if(ParsedHeader_get(request, "Host") == NULL){
        if(ParsedHeader_set(request, "Host", request->host) < 0){
            printf("Error setting host header\n");
            return -1;
        }
    }

    if(ParsedHeader_unparse_headers(request, buffer+len, sizeof(buffer)-len) < 0){
        printf("Error unparse headers\n");
        return -1;  
    }

    int server_port = 80
    if(request->port != NULL){
        server_port = atoi(request->port);
    }   
    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if(remoteSocketID < 0){
        printf("Error connecting to remote server\n");
        return -1;
    }
    int bytes_send_server = send(remoteSocketID, buffer, sizeof(buffer), 0);
    bzero(buffer, sizeof(buffer));
    
    bytes_send_server = recv(remoteSocketID, buffer, sizeof(buffer), 0);    
    char *tempBuffer = (char *)malloc(sizeof(char) * (bytes_send_server + 1));
    bzero(tempBuffer, sizeof(tempBuffer));
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    while(bytes_send_server > 0){
        bytes_send_server = send(clinetSocketId, buffer, sizeof(buffer), 0);
        for(int i=0;i<bytes_send_server/sizeof(char);i++){
            temp_buffer[temp_buffer_index] = buffer[i];
            temp_buffer_index++;
        }
        temp_buffer_size+=temp_buffer_size;
        temp_buffer = (char *)realloc(tempBuffer, sizeof(char) * (temp_buffer_size + 1));
        bzero(buffer, sizeof(buffer));
        if(bytes_send_server < 0){
            printf("Error receiving data from server\n");
            return -1;
        }
        bytes_send_server = recv(remoteSocketID, buffer, sizeof(buffer)-1, 0);
    }
    temp_buffer[temp_buffer_index] = '\0';
    free(buffer);
    add_cache_element(request->path, temp_buffer, temp_buffer_index);
    free(tempBuffer);
    close(remoteSocketID);
    return 0;
}

void *handle_client(void *socketNew){
    sem_wait(&semaphore); //wait for semaphore
    int p;
    sem_getvalue(&semaphore, &p);
    printf("Semaphore value: %d\n", p);
    int client_socket = *(int *)socketNew;
    int bytes_send_client,len;

    char buffer[4096];
    bzero(buffer, sizeof(buffer));
    bytes_send_client = recv(client_socket, buffer, sizeof(buffer), 0);

    while(bytes_send_client>0){
        len = strlen(buffer);
        if(strstr(buffer,"\r\n\r\n") == NULL){
            bytes_send_client = recv(client_socket, buffer+len, sizeof(buffer)-len, 0);
        }
        else{
            break;
        }
    }

    char *tempRequest = (char *)malloc(sizeof(char) * (bytes_send_client + 1));
    bzero(tempRequest, sizeof(tempRequest));

    for(int i=0;i<strlen(buffer);i++){
        tempRequest[i] = buffer[i];
    }
    struct cache_element *cache = find_cache_element(tempRequest);
    if(cache!=NULL){
        int size = cache->len/sizeof(char);
        int pos = 0;
        char response[4096];
        while(pos<size){
            bzero(response, sizeof(response));
            for(int i=0;i<4096;i++){
                response[i] = cache->data[i];
                pos++;
            }
            send(client_socket, response, sizeof(response), 0);
        }
        printf("Data retrieved from the cache\n");
        printf("Cache URL: %s\n", response);
    }
    else if(bytes_send_client>0){
        len = strlen(buffer);
        ParsedRequest *request = ParsedRequest_create();

        if(ParsedRequest_parse(request,buffer,len) < 0){
            printf("Error parsing request\n");
        }else{
            bzero(buffer, sizeof(buffer));
            if(!strcmp(request->method,"GET")){
                if(request->host && request->path && checkHTTPversion(request->version)==1){
                    bytes_send_client = handle_request(request,client_socket);
                    if(bytes_send_client < 0){
                        sendErrorMessage(client_socket, 500);
                    }
                    else{
                        sendErrorMessage(client_socket, 200);
                    }     
            }
            else{
                printf("This code does not support any method apart from GET\n");
            }
        }
        ParsedRequest_destroy(request);
    }
}
    else if(bytes_send_client == 0){
        printf("Client disconnected\n");
    }
    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);
    free(buffer);
    sem_post(&semaphore); //signal semaphore
    sem.getvalue(&semaphore, &p);
    printf("Semaphore post value: %d\n", p);
    free(tempRequest);
    return NULL;
}

int main(int argc, char *argv[]) {

    int client_socket,client_len;
    struct sockaddr_in server_addr, client_addr;
    sem_init(&semaphore,0,MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    if(argc == 2){
        port_number = atoi(argv[1]);
    }
    else{
        printf("Too few arguments\n");
        exit(1);
    }


    printf("Starting Proxy Server on port %d\n", port_number);

    //creating a socket for the proxy server
    proxy_server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if(proxy_server_socket < 0){
        printf("Error creating socket\n");
        exit(1);
    }
    int reuse = 1;

    if(setsockopt(proxy_server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0){
        printf("Error setting socket options\n");
        exit(1);
    }



    //setting up the server address
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_number);

    //binding the socket to the server address
    if(bind(proxy_server_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0){
        printf("Error binding socket\n");
        exit(1); 
    }
    printf("Binding successful\n");
    
    //listening for incoming connections
    int listen_status = listen(proxy_server_socket, 10);
    if(listen_status < 0){
        printf("Error listening\n");
        exit(1);
    }

    //accepting incoming connections
    int i = 0;
    int connected_socketId[MAX_CLIENTS];

    while(1){
        bzero(&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);
        client_socket = accept(proxy_server_socket, (struct sockaddr *) &client_addr, &client_len);
        if(client_socket < 0){
            printf("Error accepting connection\n");
            continue;
        }
        else{
            connected_socketId[i] = client_socket;
        }

        struct sockaddr_in *client = (struct sockaddr_in *)&client_addr;
        struct in_addr *ip = &client->sin_addr;
        char st[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip, st, INET_ADDRSTRLEN);
        printf("Client connected from %s:%d\n", st, ntohs(client->sin_port));

        //creating a thread for each client
        pthread_create(&thread_id[i], NULL, (void *)handle_client, (void *)&connected_socketId[i]);
        i++;
    }
    //closing the socket
    close(proxy_server_socket);
    return 0;
     
}

cache_element find_cache_element(char *url){
    cache_element *site = NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Cache lock value: %d\n", temp_lock_val);
    if(head!=NULL){
        site = head;
        while(site!=NULL){
            if(!strcmp(site->url, url)){
                printf("LRU time track before: %ld\n", site->lru_time_track);
                printf("\n URL found\n");
                site->lru_time_track = time(NULL);
                printf("LRU time track after: %ld\n", site->lru_time_track);
                break;
            }
            site = site->next;
        }
    }else{
        printf("URL not found\n");
    }
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Cache unlock value: %d\n", temp_lock_val);
    return site;
}

int add_cache_element(char *url, char *data, int size){
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Cache lock value: %d\n", temp_lock_val);
    int element_size = size+1+strlen(url)*sizeof(cache_element);
    if(element_size<MAX_ELEMENT_SIZE){
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Cache unlock value: %d\n", temp_lock_val);
        return 0;
    }else{
        while(cache_size+element_size>MAX_ELEMENT_SIZE){
            remove_cache_element();
        }
        cache_element *new_element = (cache_element *)malloc(sizeof(cache_element));
        new_element->data = (char *)malloc(sizeof(char) * (size+1));
        strcpy(new_element->data, data);
        new_element->len = size;
        new_element->url = (char *)malloc(sizeof(char) * (strlen(url)+1));
        strcpy(new_element->url, url);
        new_element->lru_time_track = time(NULL);
        new_element->next = head;
        head = element;
        cache_size += element_size;
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Cache unlock value: %d\n", temp_lock_val);
        return 1;
    }
    return 0;
}

void delete_cache_element(char *url){
    cache_element *p;
    cache_element *q;
    cache_element *temp;

    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Cache lock Acquire value: %d\n", temp_lock_val);
    if(head!=NULL){
        for(q=head,p=head,temp=head;q->next!=NULL;q=q->next){
            if((q->next)->lru_time_track < q->lru_time_track){
                temp = q->next;
                p=q;
            }
         }
         if(temp==head){
            head = head->next;
         }else{
            p->next = temp->next;
         }
         cache_size -= (temp->len+strlen(temp->url)+sizeof(cache_element));
         free(temp->data);
         free(temp->url);
         free(temp);
    }
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Cache unlock value: %d\n", temp_lock_val);
}