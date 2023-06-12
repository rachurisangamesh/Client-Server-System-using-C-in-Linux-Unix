#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define SERVER_PORT "65001"
#define BUFFER_SIZE 1024
#define TAR_FILE "temp.tar.gz"

int connect_to_server(const char *server_address, const char *port);
void communicate_with_server(int server_fd);
int receive_tar(int serverfd);
void extract_tar();
void invalid_command();
int validate_dgetfiles(char *date1, char *date2);

int main() {
    int server_fd = connect_to_server("localhost", SERVER_PORT);
    if (server_fd == -1) {
        fprintf(stderr, "Failed to connect to the main server.\n");
        exit(EXIT_FAILURE);
    }

    communicate_with_server(server_fd);

    return 0;
}

// connect to primary server/mirror server
int connect_to_server(const char *server_address, const char *port) {
    int server_fd;
    struct addrinfo hints, *res, *p;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(server_address, port, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        // Create a socket
        server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_fd == -1) {
            perror("socket");
            continue;
        }

        // Connect to the server
        if (connect(server_fd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("connect");
            close(server_fd);
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "Failed to connect\n");
        return -1;
    }

    freeaddrinfo(res);
    return server_fd;
}

// send commands to server
void communicate_with_server(int server_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t num_bytes_received, num_bytes_sent;
    const char *quit_command = "quit";
    int input_cmd_size = 0;
    int is_quit = 0;
    int is_first_cmd = 1;

    while (1) {
        // check if the command will be processed by primary server or mirror server
        if (is_first_cmd == 1) {
            send(server_fd, "test", 4, 0);
            recv(server_fd, buffer, BUFFER_SIZE, 0);

            if (strncmp(buffer, "REDIRECT:", 9) == 0) {
                char mirror_address[256];
                int mirror_port;
                sscanf(buffer, "REDIRECT:%255[^:]:%d", mirror_address, &mirror_port);

                printf("Redirecting to mirror server at %s:%d...\n", mirror_address, mirror_port);
                close(server_fd);

                // Connect to the mirror server
                char mirror_port_str[16];
                snprintf(mirror_port_str, sizeof(mirror_port_str), "%d", mirror_port);
                server_fd = connect_to_server(mirror_address, mirror_port_str);
                if (server_fd == -1) {
                    fprintf(stderr, "Failed to connect to the mirror server.\n");
                    break;
                }
            }
            is_first_cmd = 0;
            continue;
        }
        fflush(stdout);
        printf("Enter command: ");
        char* command;
        fgets(buffer, BUFFER_SIZE, stdin);
        input_cmd_size = strlen(buffer);
        command = malloc(input_cmd_size + 1);
        strcpy(command, buffer);

        // splitting command entered by user based on delimiter(space)
        char *token = strtok(buffer, " ");
        int argc = 0;
        char *argv[10];
        while (token != NULL) {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }

        if (argc == 1) {
            if (strncmp(argv[0], "quit", 4) == 0) {
                is_quit = 1;
            } else {
                invalid_command();
                continue;
            }
        }

        // Validate command entered by user
        if (strncmp(argv[0], "findfile", 8) == 0) {
            if (argc != 2) {
                invalid_command();
                continue;
            }     
        } else if (strcmp(argv[0], "sgetfiles") == 0) {
            if (argc < 3 || argc > 4 || (argc == 4 && strncmp(argv[3], "-u", 2) != 0)) {
                invalid_command();
                continue;
            }

            int size1 = atoi(argv[1]);
            int size2 = atoi(argv[2]);
            if (!(size1 >= 0 && size2 >= 0 && size1 <= size2)) {
                invalid_command();
                printf("Usage2: sgets size1 size2 <-u>\n");
                printf("size >= 0, size2 >= 0 and size1 <= 2\n");
                continue;
            }
        } else if (strncmp(argv[0], "dgetfiles", 9) == 0) {
            if (argc < 3 || argc > 4 || ( argc == 4 && strncmp(argv[3], "-u", 2) != 0)) {
                invalid_command();
                continue;
            }
            
            int res = validate_dgetfiles(argv[1], argv[2]);
            if (res == 1) {
                continue;
            }
        } else if (strncmp(argv[0], "getfiles", 8) == 0 || strncmp(argv[0], "gettargz", 8) == 0) {
            if ((argc < 2 || argc > 8) || ( argc == 8 && strncmp(argv[7], "-u", 2) != 0)) {
                invalid_command();
                continue;
            }
        }
        else if (strncmp(argv[0], "quit", 4) == 0) {
            
        } else {
            invalid_command();
            continue;
        }

        // Send the command to the server
        num_bytes_sent = send(server_fd, command, strlen(command), 0);
        if (num_bytes_sent == -1) {
            perror("send");
            break;
        }
        
        // handle server response based on command entered by user
        if (is_quit == 0 && strncmp(argv[0], "findfile", 8) != 0) {
            int res = receive_tar(server_fd);
            if (res == 1){
                printf("No files found\n");
                continue;
            } else if (strncmp(argv[argc - 1], "-u", 2) == 0) {
                extract_tar();
                // after extraction, delete the tar file received   
                remove(TAR_FILE);
            }
            fflush(stdout);
            continue;
        }

        memset(buffer, 0, BUFFER_SIZE);

        // Receive the server's response
        // reachable only if findfile command or quit command is entered
        num_bytes_received = recv(server_fd, buffer, BUFFER_SIZE - 1, 0);
        if (num_bytes_received <= 0) {
            perror("recv");
            break;
        }

        buffer[num_bytes_received] = '\0';
        
        if (strncmp(buffer, quit_command, strlen(quit_command)) == 0) {
            printf("Quitting\n");
            close(server_fd);
            break;
        } else {
            printf("Server response: %s\n", buffer);
        }
    }
}

// receive tar sent by server
int receive_tar(int serverfd) {
    FILE *fp;
    long file_size = 0;
    char *file_buffer;
    int valread;
    char buffer[1024] = {0};

    // Get file size
    recv(serverfd, &file_size, sizeof(long), 0);

    // if file size is zero, it means there is no tar to be sent by server
    if (file_size <= 50) {
        return 1;
    }

    // create a tar file on client
    fp = fopen(TAR_FILE, "wb");
    if (fp == NULL){
        perror("file open failed");
        exit(EXIT_FAILURE);
    }

    // Allocate memory for file buffer
    file_buffer = (char *)malloc(file_size * sizeof(char));
    if (file_buffer == NULL){
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }

    // Receive file into server and store in buffer
    valread = recv(serverfd, file_buffer, file_size, 0);

    // Write buffer to file
    fwrite(file_buffer, sizeof(char), file_size, fp);

    free(file_buffer);
    fclose(fp);

    // completion message sent by server
    valread = recv(serverfd, buffer, 12, 0);
    printf("%s\n", buffer);
    return 0;
}

// extract the tar file sent by server
void extract_tar() {
    int pid = fork();
    int status;

    printf("Extracting tar...\n");

    if (pid < 0) {
        fprintf(stderr, "Error: Failed to create child process\n");
        return;
    }

    if (pid == 0) {
        // Child process to extract tar file
        char *args[4];
        args[0] = "tar";
        args[1] = "-xf";
        args[2] = "temp.tar.gz";
        args[3] = NULL;

        execvp(args[0], args);

        /* If execvp returns, there was an error */
        fprintf(stderr, "Error: Failed to execute tar command\n");
        exit(1);
    } else {
        // Parent process will wait until the tar is extracted
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            // Child process exited normally
            if (WEXITSTATUS(status) == 0) {
                printf("Tar command completed successfully\n");
            } else {
                fprintf(stderr, "Error: Tar command exited with status %d\n", WEXITSTATUS(status));
            }
        } else {
            // Child process exited abnormally
            fprintf(stderr, "Error: Tar command exited abnormally\n");
        }
    }
}

// print the error response in case of invalid command entered by user
void invalid_command() {
    printf("ERROR: Invalid command\n");
}

int validate_dgetfiles(char *date1, char *date2) {

    // Parse the input dates
    struct tm tm1 = {0}, tm2 = {0};
    char *strptime_format = "%Y-%m-%d";
    if (strptime(date1, strptime_format, &tm1) == NULL || strptime(date2, strptime_format, &tm2) == NULL) {
        invalid_command();
        printf("Usage: dgetfiles date1 date2 <-u>\n");
        return 1;
    }

    // Convert the input dates to time_t values
    time_t t1 = mktime(&tm1);
    time_t t2 = mktime(&tm2);

    // Ensure date1 <= date2
    if (t1 > t2) {
        invalid_command();
        printf("Usage: dgetfiles date1 date2 <-u>\n");
        return 1;
    }

    return 0;
}
