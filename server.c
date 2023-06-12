#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <ftw.h>
#include <time.h>
#include <stdbool.h>
#include <dirent.h>


#define PORT "65001"
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define MIRROR_PORT 65002
#define TAR_FILE "temp.tar.gz"
#define MAX_FILE_TYPES 6

void processclient(int client_fd);
void redirect_to_mirror(int client_fd);
void executeCommand(char *command);
int iterate_over_files(const char *fpath, const struct stat *sb, int typeflag);
void sendResponse(char* response);
void remove_trailing_spaces(char *str);
void create_tar(char *command);
void send_tar();
int get_file_types(char *arg[], int argc, char *file_types[]);
char* generate_cmd();
void search_files(const char *dir_name, char *file_names[], int num_files, bool *found_files);

char *target_filename;
int found = 0;
int argc = 0;
char *argv[10];
int clientfd;
char *response;
char *home_dir;

int main() {
    int server_fd, client_fd;
    struct addrinfo hints, *res, *p;
    struct sockaddr_in *server_addr, client_addr;
    socklen_t client_addr_size;
    pid_t child_pid;

    // Configure server address
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        perror("getaddrinfo");
        exit(EXIT_FAILURE);
    }

    // Create a socket
    for (p = res; p != NULL; p = p->ai_next) {
        server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_fd == -1) {
            perror("socket");
            continue;
        }

        // Bind the socket
        if (bind(server_fd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("bind");
            close(server_fd);
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);

    // Listen for clients
    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    server_addr = (struct sockaddr_in *)p->ai_addr;
    printf("Server is listening on port %d...\n", ntohs(server_addr->sin_port));

    int clients = 0;

    while (1) {
        client_addr_size = sizeof(client_addr);

        // Accept a client connection
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_size);
        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        // Redirect client to mirror server or process the request
        if (clients < 4 || (clients > 7 && clients % 2 == 0)) {
            // Fork a child process to handle the client request
            child_pid = fork();
            if (child_pid < 0) {
                perror("fork");
                exit(EXIT_FAILURE);
            }

            if (child_pid == 0) {
                // Closing server socket in child
                close(server_fd);
                processclient(client_fd);
                exit(EXIT_SUCCESS);
            } else {
                // Closing client socket in parent process
                close(client_fd);
            }
        } else {
            // redirecting to mirror server
            redirect_to_mirror(client_fd);
        }
        clients++;
    }

    return 0;
}

// Process client connection request
void processclient(int client_fd) {
    clientfd = client_fd;
    char buffer[BUFFER_SIZE];
    ssize_t num_bytes_received;
    const char *quit_command = "quit";

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);

        // Receive client command
        num_bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (num_bytes_received <= 0) {
            perror("recv");
            close(client_fd);
            return;
        }

        // Check for quit command
        if (strncmp(buffer, quit_command, strlen(quit_command)) == 0) {
            printf("Client has issued quit command. Closing connection.\n");
	        sendResponse("quit");
            close(client_fd);
            return;
        }

        if (strncmp(buffer, "test", 4) == 0) {
            sendResponse("Successfull connection");
            continue;
        }

        // Process client command and send response
        executeCommand(buffer);
    }
}

// Method to process command sent by client
void executeCommand(char *command) {
    remove_trailing_spaces(command);
    printf("Received command: %s\n", command);

    home_dir = getenv("HOME");
    if (!home_dir) {
        printf("Error: Home directory not found\n");
        return;
    }
    
    // Splitting command by delimiter(space)
    char *token = strtok(command, " ");
    argc = 0;
    while (token != NULL) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    // filtering commands
    if (strncmp(argv[0], "findfile", 8) == 0) {
        target_filename = argv[1];
        int result = ftw(home_dir, &iterate_over_files, 20);
        if (result == 0) {
            sendResponse("File not found");
        }
    } else if (strncmp(argv[0], "sgetfiles", 9) == 0 || strncmp(argv[0], "dgetfiles", 9) == 0 || strcmp(argv[0], "gettargz") == 0) {
        char *cmd = generate_cmd();
        printf("command: %s\n", cmd);
        system(cmd);
        send_tar();
    } else if (strncmp(argv[0], "getfiles", 8) == 0) {
        bool unzip = strcmp(argv[1], "-u") == 0;
        int file_end_index = unzip ? 1 : 0;
        int num_files = argc - file_end_index;
        char *file_names[num_files];
        bool found_files[num_files];

        for (int i = 0; i < num_files; i++) {
            file_names[i] = argv[i+file_end_index];
            found_files[i] = false;
        }
        search_files(home_dir, file_names, num_files, found_files);

        char cmd[2048] = "tar czf temp.tar.gz";
        for (int i = 0; i < num_files; i++) {
            if (found_files[i]) {
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " %s", file_names[i]);
            }
        }
        system(cmd);
        send_tar();
    } else {
        sendResponse("Invalid command\n");
    }

    return;
}

int get_file_types(char *arg[], int argc, char *file_types[]) {
    int i, num_types = 0;
    for (i = 1; i < argc && num_types < MAX_FILE_TYPES; i++) {
        if (strlen(arg[i]) <= 5 && strncmp(arg[i], ".", 1) != 0) {
            file_types[num_types++] = arg[i];
        }
    }
    return num_types;
}

// generate command for creating tar file
char* generate_cmd() {
    char *cmd;

    if (strncmp("dgetfiles", argv[0], 9) == 0) {
        cmd = malloc(BUFFER_SIZE);
        snprintf(cmd, BUFFER_SIZE, "find ~ -type f -newermt %s ! -newermt %s | tar -czf %s -T -", argv[1], argv[2], TAR_FILE);
    } else if (strncmp("sgetfiles", argv[0], 9) == 0) {
        cmd = malloc(BUFFER_SIZE);
        snprintf(cmd, BUFFER_SIZE, "find %s -type f -size +%ldc -size -%ldc -print0 | tar -czf %s --null -T -", home_dir, atol(argv[1]), atol(argv[2]), TAR_FILE);
    } else {
        char buf[BUFFER_SIZE], *filename;
        char *file_types[MAX_FILE_TYPES];
        int num_types = get_file_types(argv, argc, file_types);
        cmd = malloc(BUFFER_SIZE);
        filename = malloc(BUFFER_SIZE);
        snprintf(cmd, BUFFER_SIZE, "find ~ -type f \\( -name '*.%s'", file_types[0]);
        int i;
        for (i = 1; i < num_types; i++) {
            snprintf(buf, BUFFER_SIZE, " -o -name '*.%s'", file_types[i]);
            strncat(cmd, buf, BUFFER_SIZE - strlen(cmd));
        }
        snprintf(buf, BUFFER_SIZE, " \\) -print0 | tar -czf %s -T -", TAR_FILE);
        strncat(cmd, buf, BUFFER_SIZE - strlen(cmd));
        free(filename);
    }
    return cmd;
}

// for findfiles command, method will iterate over files in home directory
int iterate_over_files(const char *fpath, const struct stat *sb, int typeflag) {
    if (typeflag == FTW_F) {
        char *file_name = strrchr(fpath, '/') + 1;

        // if file is found, then will send details to client
        if (strcmp(target_filename, file_name) == 0) {

            int size = snprintf(NULL, 0, "File found: %s\nSize: %ld bytes\n", fpath, sb->st_size);
            char date_created[20];
            strftime(date_created, sizeof(date_created), "%Y-%m-%d %H:%M:%S", localtime(&(sb->st_ctime)));
            size += snprintf(NULL, 0, "Date created: %s\n", date_created);

            // allocate memory for the output buffer
            response = malloc(size + 1);

            // write the output to the buffer
            sprintf(response, "File found: %s\nSize: %ld bytes\n", fpath, sb->st_size);
            strftime(date_created, sizeof(date_created), "%Y-%m-%d %H:%M:%S", localtime(&(sb->st_ctime)));
            sprintf(response + strlen(response), "Date created: %s\n", date_created);

            // sending data to client
            sendResponse(response);

            found = 1;
            return 1;
        }
    }
    return 0;
}

// redirect to mirror
void redirect_to_mirror(int client_fd) {
    char redirect_msg[BUFFER_SIZE];
    snprintf(redirect_msg, BUFFER_SIZE, "REDIRECT:localhost:%d", MIRROR_PORT);
    send(client_fd, redirect_msg, strlen(redirect_msg), 0);
    close(client_fd);
}

// remove trailing spaces from command send by client
void remove_trailing_spaces(char *str) {
    int i = strlen(str) - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\n' || str[i] == '\r')) {
        str[i] = '\0';
        i--;
    }
}

// send the response back to client
void sendResponse(char* response) {
    if (send(clientfd, response, strlen(response), 0) == -1) {
        perror("send");
        close(clientfd);
        return;
    }
}

// send the generate tar to client
void send_tar() {

    // opening the tar file
    FILE *fp = fopen(TAR_FILE, "rb");
    long file_size = 0;
    char *file_buffer;

    // if tar file does not exist
    if (fp == NULL){
        perror("file open failed");
        send(clientfd, &file_size, sizeof(long), 0);
        return;
    }

    // Get file size
    fseek(fp, 0L, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0L, SEEK_SET);

    // Allocate memory for file buffer
    file_buffer = (char *)malloc(file_size * sizeof(char));
    if (file_buffer == NULL){
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }

    // send file size to client
    send(clientfd, &file_size, sizeof(long), 0);

    if (file_size < 50) {
	    free(file_buffer);
	    return;
    }

    // Read file into buffer
    fread(file_buffer, sizeof(char), file_size, fp);

    // Send file to client
    send(clientfd, file_buffer, file_size, 0);

    // Send completion message
    send(clientfd, "Tar received\n", 12, 0);
    fclose(fp);

    // after file transfer, deleting the tar file
    remove(TAR_FILE);
    free(file_buffer);
}

// search for list of files
void search_files(const char *dir_name, char *file_names[], int num_files, bool *found_files) {
    DIR *dir;
    struct dirent *entry;
    struct stat path_stat;

    if (!(dir = opendir(dir_name))) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_name, entry->d_name);
        stat(path, &path_stat);

        if (S_ISDIR(path_stat.st_mode)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            search_files(path, file_names, num_files, found_files);
        } else {
            for (int i = 0; i < num_files; i++) {
                if (strcmp(file_names[i], entry->d_name) == 0) {
                    file_names[i] = malloc(strlen(path) * sizeof(char));
                    strcpy(file_names[i], path);
                    printf("%s\n", path);
                    found_files[i] = true;
                }
            }
        }
    }

    closedir(dir);
}
