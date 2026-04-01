#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define MAX_SENSORS 50

// Estructura para almacenar el estado actual de cada sensor en RAM
typedef struct {
    char id[32];
    char valor[16];
} sensor_t;

sensor_t db[MAX_SENSORS];
int sensor_count = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int socket;
    struct sockaddr_in address;
    FILE *log_file;
} client_t;

void log_event(FILE *fp, char *ip, int port, char *rx, char *tx) {
    time_t now = time(NULL);
    char *ts = ctime(&now);
    ts[strlen(ts) - 1] = '\0';
    fprintf(fp, "[%s] %s:%d | RX: %s | TX: %s\n", ts, ip, port, rx, tx);
    fflush(fp);
    printf("[%s] %s:%d | RX: %s\n", ts, ip, port, rx);
}

void update_db(char *msg) {
    char temp[1024];
    strcpy(temp, msg);
    strtok(temp, "|"); // Salta "DATA"
    char *id = strtok(NULL, "|");
    char *val = strtok(NULL, "|");

    if (id && val) {
        pthread_mutex_lock(&lock);
        int found = 0;
        for (int i = 0; i < sensor_count; i++) {
            if (strcmp(db[i].id, id) == 0) {
                strcpy(db[i].valor, val);
                found = 1;
                break;
            }
        }
        if (!found && sensor_count < MAX_SENSORS) {
            strcpy(db[sensor_count].id, id);
            strcpy(db[sensor_count].valor, val);
            sensor_count++;
        }
        pthread_mutex_unlock(&lock);
    }
}

void *handle_client(void *arg) {
    client_t *cli = (client_t *)arg;
    char buffer[1024], response[1024], ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cli->address.sin_addr, ip, INET_ADDRSTRLEN);
    int port = ntohs(cli->address.sin_port);

    while (recv(cli->socket, buffer, sizeof(buffer), 0) > 0) {
        if (strncmp(buffer, "DATA", 4) == 0) {
            update_db(buffer);
            strcpy(response, "ACK_DATA");
        } else if (strncmp(buffer, "GET_STATE", 9) == 0) {
            pthread_mutex_lock(&lock);
            response[0] = '\0';
            for (int i = 0; i < sensor_count; i++) {
                strcat(response, db[i].id);
                strcat(response, ":");
                strcat(response, db[i].valor);
                strcat(response, ";");
            }
            pthread_mutex_unlock(&lock);
        } else {
            strcpy(response, "ACK_UNKNOWN");
        }
        
        log_event(cli->log_file, ip, port, buffer, response);
        send(cli->socket, response, strlen(response), 0);
        memset(buffer, 0, sizeof(buffer));
    }
    close(cli->socket);
    free(cli);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) return 1;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {AF_INET, htons(atoi(argv[1])), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 10);
    FILE *log_f = fopen(argv[2], "a");

    while (1) {
        socklen_t al = sizeof(addr);
        int cli_sd = accept(server_fd, (struct sockaddr *)&addr, &al);
        client_t *c = malloc(sizeof(client_t));
        c->socket = cli_sd; c->address = addr; c->log_file = log_f;
        pthread_t t;
        pthread_create(&t, NULL, handle_client, c);
        pthread_detach(t);
    }
    return 0;
}