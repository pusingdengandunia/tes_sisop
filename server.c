#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 8888
#define MAX_CLIENTS 100
#define BUFFER_SIZE 2048
#define NAME_LEN 32
#define ROOM_LEN 32
#define PID_FILE "/tmp/chat_server.pid"

typedef struct {
    int socket;
    char name[NAME_LEN];
    char room[ROOM_LEN];
    int active;
} client_t;

client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_socket;

void str_trim_lf(char *arr, int length) {
    for (int i = 0; i < length; i++) {
        if (arr[i] == '\n') {
            arr[i] = '\0';
            break;
        }
    }
}

void send_message(char *message, char *room, int sender_socket) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->active && 
            strcmp(clients[i]->room, room) == 0) {
            if (write(clients[i]->socket, message, strlen(message)) < 0) {
                perror("Write failed");
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_room_list(int client_socket, char *room) {
    char buffer[BUFFER_SIZE];
    strcpy(buffer, "\n=== Users in this room ===\n");
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->active && 
            strcmp(clients[i]->room, room) == 0) {
            strcat(buffer, "- ");
            strcat(buffer, clients[i]->name);
            strcat(buffer, "\n");
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    strcat(buffer, "========================\n");
    write(client_socket, buffer, strlen(buffer));
}

int is_name_taken(char *name, char *room) {
    int taken = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->active && 
            strcmp(clients[i]->name, name) == 0 && 
            strcmp(clients[i]->room, room) == 0) {
            taken = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return taken;
}

void add_client(client_t *client) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) {
            clients[i] = client;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(int socket) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->socket == socket) {
            clients[i]->active = 0;
            free(clients[i]);
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg) {
    char buffer[BUFFER_SIZE];
    char message[BUFFER_SIZE + NAME_LEN + 32];
    int leave_flag = 0;
    int added_to_list = 0;  // Track if client was added to global list
    
    client_t *client = (client_t *)arg;
    
    // Get client name
    write(client->socket, "Enter your name: ", 17);
    int receive = read(client->socket, buffer, BUFFER_SIZE);
    if (receive <= 0) {
        leave_flag = 1;
    } else {
        buffer[receive] = '\0';
        str_trim_lf(buffer, receive);
        strncpy(client->name, buffer, NAME_LEN - 1);
        client->name[NAME_LEN - 1] = '\0';
    }
    
    // Get room name
    if (!leave_flag) {
        write(client->socket, "Enter room name: ", 17);
        receive = read(client->socket, buffer, BUFFER_SIZE);
        if (receive <= 0) {
            leave_flag = 1;
        } else {
            buffer[receive] = '\0';
            str_trim_lf(buffer, receive);
            strncpy(client->room, buffer, ROOM_LEN - 1);
            client->room[ROOM_LEN - 1] = '\0';
        }
    }
    
    // Check duplicate name in same room
    if (!leave_flag && is_name_taken(client->name, client->room)) {
        write(client->socket, "Name already taken in this room. Disconnecting...\n", 51);
        leave_flag = 1;
    }
    
    // Add client to array only after validation passes
    if (!leave_flag) {
        add_client(client);
        added_to_list = 1;  // Mark that client is in the list
        sprintf(message, "%s has joined the room '%s'\n", client->name, client->room);
        printf("%s", message);
        send_message(message, client->room, client->socket);
        
        sprintf(buffer, "Welcome to room '%s'! Type '/users' to see who's here, '/help' for commands.\n", client->room);
        write(client->socket, buffer, strlen(buffer));
    }
    
    // Main message loop
    while (!leave_flag) {
        receive = read(client->socket, buffer, BUFFER_SIZE);
        if (receive > 0) {
            buffer[receive] = '\0';
            str_trim_lf(buffer, receive);
            
            if (strlen(buffer) > 0) {
                if (strcmp(buffer, "/users") == 0) {
                    send_room_list(client->socket, client->room);
                } else if (strcmp(buffer, "/help") == 0) {
                    sprintf(message, "\nAvailable commands:\n/users - Show users in room\n/help - Show this help\n/quit - Leave chat\n\n");
                    write(client->socket, message, strlen(message));
                } else if (strcmp(buffer, "/quit") == 0) {
                    sprintf(message, "%s has left the room\n", client->name);
                    send_message(message, client->room, client->socket);
                    leave_flag = 1;
                } else {
                    sprintf(message, "[%s]: %s\n", client->name, buffer);
                    send_message(message, client->room, client->socket);
                }
            }
        } else if (receive == 0) {
            sprintf(message, "%s has disconnected\n", client->name);
            printf("%s", message);
            send_message(message, client->room, client->socket);
            leave_flag = 1;
        } else {
            perror("Read error");
            leave_flag = 1;
        }
    }
    
    close(client->socket);
    
    // Only remove from array if client was added
    if (added_to_list) {
        remove_client(client->socket);
    } else {
        // Client failed validation, just free the struct
        free(client);
    }
    
    pthread_detach(pthread_self());
    
    return NULL;
}

void daemonize() {
    pid_t pid;
    
    // Fork pertama
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // Parent keluar
    }
    
    // Child menjadi session leader
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }
    
    // Fork kedua
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // Parent kedua keluar
    }
    
    // Set file permissions
    umask(0);
    
    // Change working directory
    chdir("/");
    
    // Close file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect ke /dev/null
    open("/dev/null", O_RDONLY); // stdin
    open("/dev/null", O_WRONLY); // stdout
    open("/dev/null", O_WRONLY); // stderr
}

void save_pid() {
    FILE *fp = fopen(PID_FILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
}

void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        close(server_socket);
        unlink(PID_FILE);
        exit(0);
    }
}

void start_server() {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    pthread_t tid;
    
    // Check if already running
    FILE *fp = fopen(PID_FILE, "r");
    if (fp) {
        int old_pid;
        fscanf(fp, "%d", &old_pid);
        fclose(fp);
        if (kill(old_pid, 0) == 0) {
            printf("Server already running with PID %d\n", old_pid);
            exit(1);
        }
    }
    
    // Daemonize
    daemonize();
    save_pid();
    
    // Setup signal handler
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_socket, 10) < 0) {
        exit(EXIT_FAILURE);
    }
    
    // Accept clients
    while (1) {
        client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_socket < 0) {
            continue;
        }
        
        client_t *client = (client_t *)malloc(sizeof(client_t));
        client->socket = client_socket;
        client->active = 1;
        
        pthread_create(&tid, NULL, &handle_client, (void *)client);
    }
}

void stop_server() {
    FILE *fp = fopen(PID_FILE, "r");
    if (!fp) {
        printf("Server is not running\n");
        exit(1);
    }
    
    int pid;
    fscanf(fp, "%d", &pid);
    fclose(fp);
    
    if (kill(pid, SIGTERM) == 0) {
        printf("Server stopped (PID: %d)\n", pid);
    } else {
        printf("Failed to stop server\n");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s --start | --stop\n", argv[0]);
        return 1;
    }
    
    if (strcmp(argv[1], "--start") == 0) {
        printf("Starting server on port %d...\n", PORT);
        start_server();
    } else if (strcmp(argv[1], "--stop") == 0) {
        stop_server();
    } else {
        printf("Invalid option. Use --start or --stop\n");
        return 1;
    }
    
    return 0;
}
