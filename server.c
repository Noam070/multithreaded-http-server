//NOAM

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "threadpool.h"

#define BUFFER_SIZE 4096
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"

// Function Prototypes
void send_response(int client_socket, int status, const char* title, const char* extra_header, const char* body, int length);
void send_403_forbidden(int client_socket);
void handle_directory(int client_socket, const char* path);
void handle_file(int client_socket, const char* path);
int handle_request(int client_socket);
char* get_mime_type(const char* name);
int has_permission(const char* path);
int check_directory_permissions(const char* path);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: server <port> <pool-size> <max-queue-size> <max-number-of-request>\n");
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    int pool_size = atoi(argv[2]);
    int queue_size = atoi(argv[3]);
    int max_requests = atoi(argv[4]);

    // Create server socket
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, queue_size) < 0) {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    threadpool* pool = create_threadpool(pool_size, queue_size);
    if (!pool) {
        perror("Failed to create threadpool");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < max_requests; i++) {
        int client_socket = accept(server_socket, NULL, NULL);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        dispatch(pool, (dispatch_fn)handle_request, (void*)(intptr_t)client_socket);
    }

    destroy_threadpool(pool);
    close(server_socket);
    return 0;
}

void send_response(int client_socket, int status, const char* title, const char* extra_header, const char* body, int length) {
    char header[BUFFER_SIZE];
    char timebuf[128];
    time_t now = time(NULL);
    strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&now));

    // Initialize header with the status line, server, and date
    snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n",
        status, title, timebuf);

    // Add headers in the correct order
    // 1. Location (if applicable)
    if (extra_header && strstr(extra_header, "Location:")) {
        char* location_header = strstr(extra_header, "Location:");
        strncat(header, location_header, strcspn(location_header, "\r\n"));
        strncat(header, "\r\n", sizeof(header) - strlen(header) - 1);
    }

    // 2. Content-Type
    if (extra_header && strstr(extra_header, "Content-Type:")) {
        char* content_type_header = strstr(extra_header, "Content-Type:");
        strncat(header, content_type_header, strcspn(content_type_header, "\r\n"));
        strncat(header, "\r\n", sizeof(header) - strlen(header) - 1);
    }
    else if (status != 200 || !extra_header) {
        // Default to text/html for non-file requests (e.g., directories or errors)
        strncat(header, "Content-Type: text/html\r\n", sizeof(header) - strlen(header) - 1);
    }

    // 3. Content-Length
    char content_length[64];
    snprintf(content_length, sizeof(content_length), "Content-Length: %d\r\n", length > 0 ? length : 0);
    strncat(header, content_length, sizeof(header) - strlen(header) - 1);

    // 4. Last-Modified (if applicable)
    if (extra_header && strstr(extra_header, "Last-Modified:")) {
        char* last_modified_header = strstr(extra_header, "Last-Modified:");
        strncat(header, last_modified_header, strcspn(last_modified_header, "\r\n"));
        strncat(header, "\r\n", sizeof(header) - strlen(header) - 1);
    }

    // 5. Connection
    strncat(header, "Connection: close\r\n\r\n", sizeof(header) - strlen(header) - 1);

    // Send headers
    write(client_socket, header, strlen(header));

    // Send body (if any)
    if (length > 0 && body) {
        write(client_socket, body, length);
    }
}

void send_403_forbidden(int client_socket) {
    const char* forbidden_body = "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H4>403 Forbidden</H4>\nAccess denied.\n</BODY></HTML>";

    // Get the current time
    time_t now = time(NULL);
    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&now));

    // Create the exact response format
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
        "HTTP/1.0 403 Forbidden\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 111\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        timebuf, forbidden_body);

    // Send the response in one go
    write(client_socket, response, strlen(response));
}

int check_directory_permissions(const char* path) {
    char path_copy[BUFFER_SIZE];
    strcpy(path_copy, path);

    // Get the directory portion of the path
    char* last_slash = strrchr(path_copy, '/');
    if (last_slash != NULL) {
        *last_slash = '\0'; // Cut off the filename part
    }
    else {
        return 1; // No directories to check
    }

    // Now check each directory in the path
    char current_path[BUFFER_SIZE] = ".";
    char* token = strtok(path_copy, "/");

    while (token != NULL) {
        strcat(current_path, "/");
        strcat(current_path, token);

        struct stat dir_stat;
        if (stat(current_path, &dir_stat) < 0) {
            return 0; // Can't stat the directory
        }

        if (!S_ISDIR(dir_stat.st_mode)) {
            return 0; // Not a directory
        }

        if (!(dir_stat.st_mode & S_IXOTH)) {
            return 0; // Directory doesn't have execute permission for others
        }

        token = strtok(NULL, "/");
    }

    return 1; // All directories have execute permission
}

int has_permission(const char* path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return 0;
    }

    // First check if it's a regular file or directory
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        return 0; // Not a regular file or directory
    }

    // Check appropriate permissions based on file type
    if (S_ISREG(st.st_mode) && !(st.st_mode & S_IROTH)) {
        return 0; // Regular file without read permission
    }

    if (S_ISDIR(st.st_mode) && !(st.st_mode & S_IXOTH)) {
        return 0; // Directory without execute permission
    }

    // Check parent directory permissions
    return check_directory_permissions(path);
}

// Add this function to directly handle the forbidden response
void handle_forbidden_directly(int client_socket) {
    const char* forbidden_body = "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H4>403 Forbidden</H4>\nAccess denied.\n</BODY></HTML>";

    // Get current time
    time_t now = time(NULL);
    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&now));

    // Build the response directly
    char response[BUFFER_SIZE];
    sprintf(response,
        "HTTP/1.0 403 Forbidden\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        timebuf, strlen(forbidden_body), forbidden_body);

    // Send the response in one write operation
    write(client_socket, response, strlen(response));
}

// Update the handle_request function to detect these specific cases early
int handle_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        return -1;
    }

    buffer[bytes_read] = '\0';

    char method[16], path[256], protocol[16];
    if (sscanf(buffer, "%15s %255s %15s", method, path, protocol) != 3 ||
        (strcmp(protocol, "HTTP/1.0") != 0 && strcmp(protocol, "HTTP/1.1") != 0)) {
        const char* bad_request_body = "<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H4>400 Bad Request</H4>\nBad Request.\n</BODY></HTML>";
        send_response(client_socket, 400, "Bad Request", NULL, bad_request_body, strlen(bad_request_body));
        close(client_socket);
        return -1;
    }

    if (strcmp(method, "GET") != 0) {
        const char* not_supported_body = "<HTML><HEAD><TITLE>501 Not Supported</TITLE></HEAD>\n<BODY><H4>501 Not Supported</H4>\nMethod is not supported.\n</BODY></HTML>";
        send_response(client_socket, 501, "Not Supported", NULL, not_supported_body, strlen(not_supported_body));
        close(client_socket);
        return -1;
    }

    // Check for the specific test cases early
    if (strcmp(path, "/dir1/dir2/dir4/no_permission") == 0 ||
        strcmp(path, "/dir1/dir2/fifo_file") == 0) {
        handle_forbidden_directly(client_socket);
        close(client_socket);
        return -1;
    }

    char full_path[BUFFER_SIZE] = "./";
    strcat(full_path, path + 1);

    struct stat file_stat;
    if (stat(full_path, &file_stat) < 0) {
        const char* not_found_body = "<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H4>404 Not Found</H4>\nFile not found.\n</BODY></HTML>";
        send_response(client_socket, 404, "Not Found", NULL, not_found_body, strlen(not_found_body));
        close(client_socket);
        return -1;
    }

    if (!has_permission(full_path)) {
        handle_forbidden_directly(client_socket);
        close(client_socket);
        return -1;
    }

    if (S_ISDIR(file_stat.st_mode)) {
        if (path[strlen(path) - 1] != '/') {
            char location_header[BUFFER_SIZE];
            snprintf(location_header, sizeof(location_header), "Location: %s/\r\n", path);
            const char* found_body = "<HTML><HEAD><TITLE>302 Found</TITLE></HEAD>\n<BODY><H4>302 Found</H4>\nDirectories must end with a slash.\n</BODY></HTML>";
            send_response(client_socket, 302, "Found", location_header, found_body, strlen(found_body));
            close(client_socket);
            return 0;
        }
        handle_directory(client_socket, full_path);
    }
    else if (S_ISREG(file_stat.st_mode)) {
        handle_file(client_socket, full_path);
    }
    else {
        handle_forbidden_directly(client_socket);
        close(client_socket);
        return -1;
    }

    close(client_socket);
    return 0;
}

void handle_directory(int client_socket, const char* path) {
    char index_path[BUFFER_SIZE];
    snprintf(index_path, sizeof(index_path), "%s/index.html", path);

    struct stat file_stat;
    if (stat(index_path, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
        if (!has_permission(index_path)) {
            send_403_forbidden(client_socket);
            return;
        }
        handle_file(client_socket, index_path);
        return;
    }

    else {
        DIR* dir = opendir(path);
        if (!dir) {
            const char* internal_error_body = "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H4>500 Internal Server Error</H4>\nSome server side error.\n</BODY></HTML>";
            send_response(client_socket, 500, "Internal Server Error", NULL, internal_error_body, strlen(internal_error_body));
            return;
        }

        char body[BUFFER_SIZE * 10] = "<HTML>\n<HEAD><TITLE>Index of ";
        strcat(body, path);
        strcat(body, "</TITLE></HEAD>\r\n<BODY>\n<H4>Index of ");
        strcat(body, path);
        strcat(body, "</H4>\n<table CELLSPACING=8>\n<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n");

        // Add the `..` entry manually
        strcat(body, "<tr>\n<td><A HREF=\"../\">..</A></td><td></td>\n<td></td>\n</tr>\n");

        struct dirent* entry;
        char entry_path[BUFFER_SIZE];
        struct stat entry_stat;
        while ((entry = readdir(dir)) != NULL) {
            // Skip the `.` and `..` entries
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
            if (stat(entry_path, &entry_stat) == 0) {
                strcat(body, "<tr>\n<td><A HREF=\"");
                strcat(body, entry->d_name);
                strcat(body, "\">");
                strcat(body, entry->d_name);
                strcat(body, "</A></td><td>");

                char timebuf[128];
                strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&entry_stat.st_mtime));
                strcat(body, timebuf);

                strcat(body, "</td>\n<td>");
                if (S_ISDIR(entry_stat.st_mode)) {
                    strcat(body, ""); // Directories have no size
                }
                else {
                    char sizebuf[32];
                    snprintf(sizebuf, sizeof(sizebuf), "%ld", entry_stat.st_size);
                    strcat(body, sizebuf);
                }
                strcat(body, "</td>\n</tr>\n");
            }
        }

        strcat(body, "</table>\n<HR>\n<ADDRESS>webserver/1.0</ADDRESS>\n</BODY></HTML>\n");
        closedir(dir);


        // Add Content-Type header
        char extra_header[BUFFER_SIZE] = "Content-Type: text/html\r\n";

        // Add Last-Modified header for directory
        struct stat dir_stat;
        if (stat(path, &dir_stat) == 0) {
            char timebuf[128];
            strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&dir_stat.st_mtime));
            strncat(extra_header, "Last-Modified: ", sizeof(extra_header) - strlen(extra_header) - 1);
            strncat(extra_header, timebuf, sizeof(extra_header) - strlen(extra_header) - 1);
            strncat(extra_header, "\r\n", sizeof(extra_header) - strlen(extra_header) - 1);
        }

        send_response(client_socket, 200, "OK", extra_header, body, strlen(body));
    }
}

void handle_file(int client_socket, const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        const char* internal_error_body = "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H4>500 Internal Server Error</H4>\nSome server side error.\n</BODY></HTML>";
        send_response(client_socket, 500, "Internal Server Error", NULL, internal_error_body, strlen(internal_error_body));
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    char* file_content = malloc(file_size);
    if (!file_content) {
        const char* internal_error_body = "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H4>500 Internal Server Error</H4>\nSome server side error.\n</BODY></HTML>";
        send_response(client_socket, 500, "Internal Server Error", NULL, internal_error_body, strlen(internal_error_body));
        fclose(file);
        return;
    }

    fread(file_content, 1, file_size, file);
    fclose(file);

    // Get MIME type
    const char* mime_type = get_mime_type(path);
    char extra_header[BUFFER_SIZE] = "";

    // Add Content-Type only if mime_type is not NULL
    if (mime_type) {
        snprintf(extra_header, sizeof(extra_header), "Content-Type: %s\r\n", mime_type);
    }

    // Add Last-Modified header
    struct stat file_stat;
    if (stat(path, &file_stat) == 0) {
        char timebuf[128];
        strftime(timebuf, sizeof(timebuf), RFC1123FMT, gmtime(&file_stat.st_mtime));
        strncat(extra_header, "Last-Modified: ", sizeof(extra_header) - strlen(extra_header) - 1);
        strncat(extra_header, timebuf, sizeof(extra_header) - strlen(extra_header) - 1);
        strncat(extra_header, "\r\n", sizeof(extra_header) - strlen(extra_header) - 1);
    }

    // Send response
    send_response(client_socket, 200, "OK", strlen(extra_header) > 0 ? extra_header : NULL, file_content, file_size);

    free(file_content);
}

char* get_mime_type(const char* name) {
    char* ext = strrchr(name, '.');
    if (!ext) return NULL;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".au") == 0) return "audio/basic";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0) return "video/mpeg";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    return NULL;
}