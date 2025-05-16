#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include "../libs/markdown.h"
#include "../libs/document.h"

#define MAX_USERNAME_LEN 64
#define MAX_COMMAND_LEN 256
#define MAX_CLIENTS 10
#define FIFO_PERM 0666
#define ROLES_FILE "roles.txt"

// 客户端角色
typedef enum {
    ROLE_NONE,
    ROLE_READ,
    ROLE_WRITE
} client_role;

// 客户端命令
typedef struct {
    char *command;
    char *username;
    time_t timestamp;
} client_command;

// 客户端连接信息
typedef struct {
    pid_t pid;
    char username[MAX_USERNAME_LEN];
    client_role role;
    int c2s_fd;  // 客户端到服务器的管道
    int s2c_fd;  // 服务器到客户端的管道
    pthread_t thread;
    int connected;
} client_info;

// 全局变量
document doc;
client_info clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t update_cond = PTHREAD_COND_INITIALIZER;
int update_interval_ms;
int server_running = 1;

// 命令队列
client_command *command_queue = NULL;
int command_queue_size = 0;
int command_queue_capacity = 0;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// 函数声明
void handle_signal(int sig, siginfo_t *info, void *ucontext);
void *client_handler(void *arg);
void *update_handler(void *arg);
client_role get_client_role(const char *username);
void broadcast_update(uint64_t version, const char *updates);
void process_command(client_info *client, const char *command);
void add_command_to_queue(const char *command, const char *username);
void save_document();
void cleanup();

/**
 * 主函数
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <update_interval_ms>\n", argv[0]);
        return 1;
    }

    // 解析更新间隔
    update_interval_ms = atoi(argv[1]);
    if (update_interval_ms <= 0) {
        fprintf(stderr, "Update interval must be a positive integer\n");
        return 1;
    }

    // 初始化文档
    markdown_init(&doc);

    // 初始化客户端数组
    memset(clients, 0, sizeof(clients));

    // 设置信号处理
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handle_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN, &sa, NULL);

    // 打印服务器PID
    printf("Server PID: %d\n", getpid());

    // 创建更新线程
    pthread_t update_thread;
    pthread_create(&update_thread, NULL, update_handler, NULL);

    // 主循环，处理服务器命令
    char command[MAX_COMMAND_LEN];
    while (server_running) {
        if (fgets(command, MAX_COMMAND_LEN, stdin)) {
            command[strcspn(command, "\n")] = 0;  // 移除换行符

            if (strcmp(command, "DOC?") == 0) {
                // 打印文档内容
                pthread_mutex_lock(&doc_mutex);
                printf("\n");
                markdown_print(&doc, stdout);
                pthread_mutex_unlock(&doc_mutex);
            } else if (strcmp(command, "LOG?") == 0) {
                // 打印命令日志
                pthread_mutex_lock(&queue_mutex);
                printf("\n");
                for (int i = 0; i < command_queue_size; i++) {
                    printf("%s\n", command_queue[i].command);
                }
                pthread_mutex_unlock(&queue_mutex);
            } else if (strcmp(command, "QUIT") == 0) {
                // 检查是否有客户端连接
                pthread_mutex_lock(&clients_mutex);
                int connected_clients = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].connected) {
                        connected_clients++;
                    }
                }

                if (connected_clients > 0) {
                    printf("QUIT rejected, %d clients still connected.\n", connected_clients);
                } else {
                    server_running = 0;
                }
                pthread_mutex_unlock(&clients_mutex);
            }
        }
    }

    // 等待更新线程结束
    pthread_cancel(update_thread);
    pthread_join(update_thread, NULL);

    // 保存文档并清理资源
    save_document();
    cleanup();

    return 0;
}

/**
 * 信号处理函数
 */
void handle_signal(int sig, siginfo_t *info, void *ucontext) {
    if (sig == SIGRTMIN) {
        pid_t client_pid = info->si_pid;

        // 查找可用的客户端槽位
        pthread_mutex_lock(&clients_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].connected) {
                slot = i;
                break;
            }
        }

        if (slot == -1) {
            // 没有可用槽位
            pthread_mutex_unlock(&clients_mutex);
            return;
        }

        // 初始化客户端信息
        clients[slot].pid = client_pid;
        clients[slot].role = ROLE_NONE;
        clients[slot].connected = 1;
        clients[slot].c2s_fd = -1;
        clients[slot].s2c_fd = -1;

        // 创建客户端处理线程
        pthread_create(&clients[slot].thread, NULL, client_handler, &clients[slot]);
        pthread_mutex_unlock(&clients_mutex);
    }
}

/**
 * 客户端处理线程
 */
void *client_handler(void *arg) {
    client_info *client = (client_info *)arg;
    pid_t client_pid = client->pid;

    // 创建命名管道
    char c2s_path[64], s2c_path[64];
    snprintf(c2s_path, sizeof(c2s_path), "FIFO_C2S_%d", client_pid);
    snprintf(s2c_path, sizeof(s2c_path), "FIFO_S2C_%d", client_pid);

    // 删除已存在的管道
    unlink(c2s_path);
    unlink(s2c_path);

    // 创建新管道
    if (mkfifo(c2s_path, FIFO_PERM) == -1 || mkfifo(s2c_path, FIFO_PERM) == -1) {
        perror("mkfifo");
        client->connected = 0;
        return NULL;
    }

    // 向客户端发送信号
    kill(client_pid, SIGRTMIN + 1);

    // 打开管道
    client->c2s_fd = open(c2s_path, O_RDONLY | O_NONBLOCK);
    client->s2c_fd = open(s2c_path, O_WRONLY);

    if (client->c2s_fd == -1 || client->s2c_fd == -1) {
        perror("open");
        close(client->c2s_fd);
        close(client->s2c_fd);
        unlink(c2s_path);
        unlink(s2c_path);
        client->connected = 0;
        return NULL;
    }

    // 读取用户名
    char username[MAX_USERNAME_LEN];
    ssize_t bytes_read = 0;
    int retries = 0;

    // 非阻塞读取，尝试多次
    while (bytes_read <= 0 && retries < 10) {
        bytes_read = read(client->c2s_fd, username, MAX_USERNAME_LEN - 1);
        if (bytes_read == -1 && errno == EAGAIN) {
            // 没有数据可读，等待一下再试
            usleep(100000);  // 100ms
            retries++;
        } else if (bytes_read <= 0) {
            // 读取错误或连接关闭
            break;
        }
    }

    if (bytes_read <= 0) {
        // 读取用户名失败
        close(client->c2s_fd);
        close(client->s2c_fd);
        unlink(c2s_path);
        unlink(s2c_path);
        client->connected = 0;
        return NULL;
    }

    // 确保用户名以 null 结尾
    username[bytes_read] = '\0';
    if (username[bytes_read - 1] == '\n') {
        username[bytes_read - 1] = '\0';
    }

    strncpy(client->username, username, MAX_USERNAME_LEN - 1);
    client->username[MAX_USERNAME_LEN - 1] = '\0';

    // 检查用户权限
    client->role = get_client_role(username);

    if (client->role == ROLE_NONE) {
        // 未授权用户
        dprintf(client->s2c_fd, "Reject UNAUTHORISED\n");
        sleep(1);
        close(client->c2s_fd);
        close(client->s2c_fd);
        unlink(c2s_path);
        unlink(s2c_path);
        client->connected = 0;
        return NULL;
    }

    // 发送用户角色
    const char *role_str = (client->role == ROLE_WRITE) ? "write\n" : "read\n";
    write(client->s2c_fd, role_str, strlen(role_str));

    // 发送文档内容
    pthread_mutex_lock(&doc_mutex);
    char *content = markdown_flatten(&doc);
    size_t content_len = content ? strlen(content) : 0;

    // 发送版本号
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "%lu\n", (unsigned long)doc.current_version);
    write(client->s2c_fd, version_str, strlen(version_str));

    // 发送文档长度
    char length_str[32];
    snprintf(length_str, sizeof(length_str), "%zu\n", content_len);
    write(client->s2c_fd, length_str, strlen(length_str));

    // 发送文档内容
    if (content && content_len > 0) {
        write(client->s2c_fd, content, content_len);
    }

    if (content) {
        free(content);
    }
    pthread_mutex_unlock(&doc_mutex);

    // 设置管道为阻塞模式
    int flags = fcntl(client->c2s_fd, F_GETFL);
    fcntl(client->c2s_fd, F_SETFL, flags & ~O_NONBLOCK);

    // 处理客户端命令
    char command[MAX_COMMAND_LEN];
    while (client->connected && server_running) {
        bytes_read = read(client->c2s_fd, command, MAX_COMMAND_LEN - 1);
        if (bytes_read <= 0) {
            // 客户端断开连接
            break;
        }

        command[bytes_read] = '\0';
        if (command[bytes_read - 1] == '\n') {
            command[bytes_read - 1] = '\0';
        }

        // 处理命令
        process_command(client, command);
    }

    // 清理资源
    close(client->c2s_fd);
    close(client->s2c_fd);
    unlink(c2s_path);
    unlink(s2c_path);
    client->connected = 0;

    return NULL;
}

/**
 * 更新处理线程
 */
void *update_handler(void *arg) {
    struct timespec sleep_time;
    sleep_time.tv_sec = update_interval_ms / 1000;
    sleep_time.tv_nsec = (update_interval_ms % 1000) * 1000000;

    while (server_running) {
        // 等待指定的更新间隔
        nanosleep(&sleep_time, NULL);

        // 处理队列中的命令
        pthread_mutex_lock(&queue_mutex);
        if (command_queue_size > 0) {
            // 构建更新消息
            char *updates = NULL;
            size_t updates_len = 0;
            size_t updates_capacity = 0;

            // 添加版本号
            char version_str[64];
            snprintf(version_str, sizeof(version_str), "VERSION %lu\n", (unsigned long)doc.current_version);

            updates_len = strlen(version_str);
            updates_capacity = updates_len + 1;
            updates = (char *)malloc(updates_capacity);
            if (!updates) {
                pthread_mutex_unlock(&queue_mutex);
                continue;
            }

            strcpy(updates, version_str);

            // 处理所有命令
            pthread_mutex_lock(&doc_mutex);
            for (int i = 0; i < command_queue_size; i++) {
                client_command *cmd = &command_queue[i];

                // 解析命令
                char cmd_copy[MAX_COMMAND_LEN];
                strncpy(cmd_copy, cmd->command, MAX_COMMAND_LEN - 1);
                cmd_copy[MAX_COMMAND_LEN - 1] = '\0';

                char *token = strtok(cmd_copy, " ");
                if (!token) continue;

                int result = SUCCESS;
                char result_str[MAX_COMMAND_LEN];

                if (strcmp(token, "INSERT") == 0) {
                    // INSERT <pos> <content>
                    char *pos_str = strtok(NULL, " ");
                    char *content = pos_str ? pos_str + strlen(pos_str) + 1 : NULL;

                    if (pos_str && content) {
                        size_t pos = atol(pos_str);
                        result = markdown_insert(&doc, doc.current_version, pos, content);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "DEL") == 0) {
                    // DEL <pos> <no_char>
                    char *pos_str = strtok(NULL, " ");
                    char *len_str = strtok(NULL, " ");

                    if (pos_str && len_str) {
                        size_t pos = atol(pos_str);
                        size_t len = atol(len_str);
                        result = markdown_delete(&doc, doc.current_version, pos, len);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "NEWLINE") == 0) {
                    // NEWLINE <pos>
                    char *pos_str = strtok(NULL, " ");

                    if (pos_str) {
                        size_t pos = atol(pos_str);
                        result = markdown_insert(&doc, doc.current_version, pos, "\n");
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "HEADING") == 0) {
                    // HEADING <level> <pos>
                    char *level_str = strtok(NULL, " ");
                    char *pos_str = strtok(NULL, " ");

                    if (level_str && pos_str) {
                        int level = atoi(level_str);
                        size_t pos = atol(pos_str);
                        result = markdown_heading(&doc, doc.current_version, level, pos);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "BOLD") == 0) {
                    // BOLD <pos_start> <pos_end>
                    char *start_str = strtok(NULL, " ");
                    char *end_str = strtok(NULL, " ");

                    if (start_str && end_str) {
                        size_t start = atol(start_str);
                        size_t end = atol(end_str);
                        result = markdown_bold(&doc, doc.current_version, start, end);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "ITALIC") == 0) {
                    // ITALIC <pos_start> <pos_end>
                    char *start_str = strtok(NULL, " ");
                    char *end_str = strtok(NULL, " ");

                    if (start_str && end_str) {
                        size_t start = atol(start_str);
                        size_t end = atol(end_str);
                        result = markdown_italic(&doc, doc.current_version, start, end);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "BLOCKQUOTE") == 0) {
                    // BLOCKQUOTE <pos>
                    char *pos_str = strtok(NULL, " ");

                    if (pos_str) {
                        size_t pos = atol(pos_str);
                        result = markdown_blockquote(&doc, doc.current_version, pos);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "ORDERED_LIST") == 0) {
                    // ORDERED_LIST <pos>
                    char *pos_str = strtok(NULL, " ");

                    if (pos_str) {
                        size_t pos = atol(pos_str);
                        result = markdown_ordered_list(&doc, doc.current_version, pos);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "UNORDERED_LIST") == 0) {
                    // UNORDERED_LIST <pos>
                    char *pos_str = strtok(NULL, " ");

                    if (pos_str) {
                        size_t pos = atol(pos_str);
                        result = markdown_unordered_list(&doc, doc.current_version, pos);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "CODE") == 0) {
                    // CODE <pos_start> <pos_end>
                    char *start_str = strtok(NULL, " ");
                    char *end_str = strtok(NULL, " ");

                    if (start_str && end_str) {
                        size_t start = atol(start_str);
                        size_t end = atol(end_str);
                        result = markdown_code(&doc, doc.current_version, start, end);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "HORIZONTAL_RULE") == 0) {
                    // HORIZONTAL_RULE <pos>
                    char *pos_str = strtok(NULL, " ");

                    if (pos_str) {
                        size_t pos = atol(pos_str);
                        result = markdown_horizontal_rule(&doc, doc.current_version, pos);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                } else if (strcmp(token, "LINK") == 0) {
                    // LINK <pos_start> <pos_end> <link>
                    char *start_str = strtok(NULL, " ");
                    char *end_str = strtok(NULL, " ");
                    char *link = end_str ? end_str + strlen(end_str) + 1 : NULL;

                    if (start_str && end_str && link) {
                        size_t start = atol(start_str);
                        size_t end = atol(end_str);
                        result = markdown_link(&doc, doc.current_version, start, end, link);
                    } else {
                        result = INVALID_CURSOR_POS;
                    }
                }

                // 构建结果字符串
                if (result == SUCCESS) {
                    snprintf(result_str, sizeof(result_str), "EDIT %s %s SUCCESS\n",
                             cmd->username, cmd->command);
                } else if (result == INVALID_CURSOR_POS) {
                    snprintf(result_str, sizeof(result_str), "EDIT %s %s Reject INVALID_POSITION\n",
                             cmd->username, cmd->command);
                } else if (result == DELETED_POSITION) {
                    snprintf(result_str, sizeof(result_str), "EDIT %s %s Reject DELETED_POSITION\n",
                             cmd->username, cmd->command);
                } else if (result == OUTDATED_VERSION) {
                    snprintf(result_str, sizeof(result_str), "EDIT %s %s Reject OUTDATED_VERSION\n",
                             cmd->username, cmd->command);
                }

                // 添加结果到更新消息
                size_t result_len = strlen(result_str);

                // 检查是否超出合理大小限制
                if (updates_capacity > 1024*1024) { // 如果已经超过1MB
                    // 跳过此次添加，防止内存过度增长
                    fprintf(stderr, "Warning: Update message too large, skipping command result\n");
                } else if (updates_len + result_len + 1 > updates_capacity) {
                    // 计算新的容量，但设置上限
                    size_t new_capacity = updates_len + result_len + 1024;  // 增加缓冲区大小
                    if (new_capacity > 1024*1024) new_capacity = 1024*1024; // 最大1MB

                    char *new_updates = (char *)realloc(updates, new_capacity);
                    if (!new_updates) {
                        fprintf(stderr, "Failed to allocate memory for updates\n");
                        free(updates);
                        pthread_mutex_unlock(&doc_mutex);
                        pthread_mutex_unlock(&queue_mutex);
                        return NULL;
                    }
                    updates = new_updates;
                    updates_capacity = new_capacity;
                }

                strcpy(updates + updates_len, result_str);
                updates_len += result_len;
            }

            // 添加结束标记
            if (updates_len + 5 > updates_capacity) {
                // 确保不超过内存限制
                if (updates_capacity >= 1024*1024) {
                    // 如果已经达到最大限制，截断更新消息
                    updates[updates_capacity - 5] = '\0';
                    updates_len = updates_capacity - 5;
                    fprintf(stderr, "Warning: Update message truncated due to size limit\n");
                } else {
                    size_t new_capacity = updates_len + 5;
                    char *new_updates = (char *)realloc(updates, new_capacity);
                    if (!new_updates) {
                        fprintf(stderr, "Failed to allocate memory for END marker\n");
                        free(updates);
                        pthread_mutex_unlock(&doc_mutex);
                        pthread_mutex_unlock(&queue_mutex);
                        return NULL;
                    }
                    updates = new_updates;
                    updates_capacity = new_capacity;
                }
            }

            strcpy(updates + updates_len, "END\n");

            // 增加文档版本
            markdown_increment_version(&doc);

            // 广播更新
            broadcast_update(doc.current_version, updates);

            // 清空命令队列
            for (int i = 0; i < command_queue_size; i++) {
                free(command_queue[i].command);
                free(command_queue[i].username);
            }
            command_queue_size = 0;

            free(updates);
            pthread_mutex_unlock(&doc_mutex);
        } else {
            // 没有命令，只广播版本号
            pthread_mutex_lock(&doc_mutex);
            char version_msg[128];
            snprintf(version_msg, sizeof(version_msg), "VERSION %lu\nEND\n", (unsigned long)doc.current_version);
            broadcast_update(doc.current_version, version_msg);
            pthread_mutex_unlock(&doc_mutex);
        }

        pthread_mutex_unlock(&queue_mutex);
    }

    return NULL;
}

/**
 * 获取客户端角色
 */
client_role get_client_role(const char *username) {
    if (!username || strlen(username) == 0) {
        fprintf(stderr, "Error: Empty username provided to get_client_role\n");
        return ROLE_NONE;
    }

    printf("Checking role for user: '%s'\n", username);

    // 尝试打开角色文件
    FILE *roles_file = fopen(ROLES_FILE, "r");
    if (!roles_file) {
        perror("Error opening roles file");
        fprintf(stderr, "Failed to open roles file: %s\n", ROLES_FILE);

        // 尝试获取当前工作目录
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            fprintf(stderr, "Current working directory: %s\n", cwd);
        } else {
            perror("getcwd() error");
        }

        return ROLE_NONE;
    }

    char line[256];
    while (fgets(line, sizeof(line), roles_file)) {
        // 移除行尾的换行符
        line[strcspn(line, "\n")] = 0;

        printf("Processing line: '%s'\n", line);

        // 分割用户名和角色
        char line_copy[256];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        char *role_str = NULL;
        char *user = strtok(line_copy, " \t");
        if (user) {
            role_str = strtok(NULL, " \t");
        }

        printf("Parsed: user='%s', role='%s'\n",
               user ? user : "NULL",
               role_str ? role_str : "NULL");

        if (user && role_str && strcmp(user, username) == 0) {
            fclose(roles_file);

            if (strcmp(role_str, "write") == 0) {
                printf("User '%s' has WRITE permission\n", username);
                return ROLE_WRITE;
            } else if (strcmp(role_str, "read") == 0) {
                printf("User '%s' has READ permission\n", username);
                return ROLE_READ;
            } else {
                printf("User '%s' has UNKNOWN permission: '%s'\n", username, role_str);
                return ROLE_NONE;
            }
        }
    }

    printf("User '%s' not found in roles file\n", username);
    fclose(roles_file);
    return ROLE_NONE;
}

/**
 * 广播更新到所有客户端
 */
void broadcast_update(uint64_t version, const char *updates) {
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected && clients[i].s2c_fd != -1) {
            write(clients[i].s2c_fd, updates, strlen(updates));
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

/**
 * 处理客户端命令
 */
void process_command(client_info *client, const char *command) {
    if (!client || !command) {
        return;
    }

    // 检查命令是否有效
    size_t cmd_len = strlen(command);
    if (cmd_len == 0 || cmd_len >= MAX_COMMAND_LEN - 1) {
        return;
    }

    // 解析命令
    char cmd_copy[MAX_COMMAND_LEN];
    strncpy(cmd_copy, command, MAX_COMMAND_LEN - 1);
    cmd_copy[MAX_COMMAND_LEN - 1] = '\0';

    char *token = strtok(cmd_copy, " ");
    if (!token) {
        return;
    }

    // 处理不同类型的命令
    if (strcmp(token, "DISCONNECT") == 0) {
        // 断开连接
        client->connected = 0;
    } else if (strcmp(token, "DOC?") == 0) {
        // 打印文档内容
        pthread_mutex_lock(&doc_mutex);
        char *content = markdown_flatten(&doc);
        if (content) {
            dprintf(client->s2c_fd, "\n%s", content);
            free(content);
        }
        pthread_mutex_unlock(&doc_mutex);
    } else if (strcmp(token, "PERM?") == 0) {
        // 显示客户端权限
        const char *role_str = (client->role == ROLE_WRITE) ? "write" : "read";
        dprintf(client->s2c_fd, "\n%s", role_str);
    } else if (strcmp(token, "LOG?") == 0) {
        // 显示命令日志
        pthread_mutex_lock(&queue_mutex);
        for (int i = 0; i < command_queue_size; i++) {
            dprintf(client->s2c_fd, "\n%s", command_queue[i].command);
        }
        pthread_mutex_unlock(&queue_mutex);
    } else if (strcmp(token, "SAVE") == 0) {
        // 保存文档
        save_document();
        dprintf(client->s2c_fd, "\n文档已保存到 doc.md");
    } else {
        // 编辑命令
        if (client->role != ROLE_WRITE) {
            // 只有写权限的客户端才能执行编辑命令
            return;
        }

        // 添加命令到队列
        add_command_to_queue(command, client->username);
    }
}

/**
 * 添加命令到队列
 */
void add_command_to_queue(const char *command, const char *username) {
    pthread_mutex_lock(&queue_mutex);

    // 检查队列容量
    if (command_queue_size >= command_queue_capacity) {
        int new_capacity = command_queue_capacity == 0 ? 10 : command_queue_capacity * 2;
        client_command *new_queue = (client_command *)realloc(command_queue,
                                                             new_capacity * sizeof(client_command));
        if (!new_queue) {
            pthread_mutex_unlock(&queue_mutex);
            return;
        }

        command_queue = new_queue;
        command_queue_capacity = new_capacity;
    }

    // 添加命令
    command_queue[command_queue_size].command = strdup(command);
    command_queue[command_queue_size].username = strdup(username);
    command_queue[command_queue_size].timestamp = time(NULL);
    command_queue_size++;

    pthread_mutex_unlock(&queue_mutex);
}

/**
 * 保存文档
 */
void save_document() {
    pthread_mutex_lock(&doc_mutex);

    FILE *doc_file = fopen("doc.md", "w");
    if (doc_file) {
        markdown_print(&doc, doc_file);
        fclose(doc_file);
    }

    pthread_mutex_unlock(&doc_mutex);
}

/**
 * 清理资源
 */
void cleanup() {
    // 释放文档
    markdown_free(&doc);

    // 释放命令队列
    pthread_mutex_lock(&queue_mutex);
    for (int i = 0; i < command_queue_size; i++) {
        free(command_queue[i].command);
        free(command_queue[i].username);
    }
    free(command_queue);
    command_queue = NULL;
    command_queue_size = 0;
    command_queue_capacity = 0;
    pthread_mutex_unlock(&queue_mutex);

    // 销毁互斥锁和条件变量
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&doc_mutex);
    pthread_mutex_destroy(&queue_mutex);
    pthread_cond_destroy(&update_cond);
}
