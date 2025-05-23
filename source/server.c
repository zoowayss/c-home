#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "../libs/document.h"
#include "../libs/markdown.h"

#define MAX_USERNAME_LEN 64
#define MAX_COMMAND_LEN 256
#define MAX_CLIENTS 10
#define FIFO_PERM 0666

// 客户端角色
typedef enum {
    ROLE_NONE,
    ROLE_READ,
    ROLE_WRITE
} client_role;

// 客户端信息
typedef struct {
    pid_t pid;
    char username[MAX_USERNAME_LEN];
    client_role role;
    int c2s_fd; // 客户端到服务器的管道
    int s2c_fd; // 服务器到客户端的管道
    pthread_t thread;
    int connected;
} client_info;

// 命令队列节点
typedef struct command_node {
    char username[MAX_USERNAME_LEN];
    char command[MAX_COMMAND_LEN];
    time_t timestamp;
    struct command_node *next;
} command_node;

// 命令日志条目
typedef struct {
    uint64_t version;
    char **entries;
    size_t count;
    size_t capacity;
} version_log;

// 命令日志
typedef struct {
    version_log *versions;
    size_t count;
    size_t capacity;
} command_log;

// 全局变量
static document doc;
static client_info clients[MAX_CLIENTS];
static int client_count = 0;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;
static command_node *command_queue = NULL;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static int update_interval_ms;
static int server_running = 1;
static command_log log = {NULL, 0, 0};
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// 函数声明
void handle_signal(int sig, siginfo_t *info, void *ucontext);
void *client_handler(void *arg);
void *update_thread(void *arg);
client_role get_user_role(const char *username);
void process_command(const char *username, const char *command);
void broadcast_update(int version_changed);
void save_document();
void cleanup_resources();
void handle_client_disconnect(int client_index);
int parse_command(const char *command, char *cmd_type, size_t *pos1, size_t *pos2, char *content, int *level, uint64_t *version);
void add_log_entry(uint64_t version, const char *entry);
void print_command_log();

/**
 * 信号处理函数
 */
void handle_signal(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext; // 未使用的参数
    if (sig == SIGRTMIN) {
        pid_t client_pid = info->si_pid;

        // 查找可用的客户端槽位
        pthread_mutex_lock(&client_mutex);
        int client_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].connected) {
                client_index = i;
                break;
            }
        }

        if (client_index == -1) {
            // 没有可用槽位，拒绝连接
            pthread_mutex_unlock(&client_mutex);
            return;
        }

        // 初始化客户端信息
        clients[client_index].pid = client_pid;
        clients[client_index].connected = 1;
        clients[client_index].role = ROLE_NONE;
        clients[client_index].c2s_fd = -1;
        clients[client_index].s2c_fd = -1;
        client_count++;

        // 创建客户端处理线程
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        int *arg = malloc(sizeof(int));
        if (!arg) {
            clients[client_index].connected = 0;
            client_count--;
            pthread_mutex_unlock(&client_mutex);
            return;
        }

        *arg = client_index;

        if (pthread_create(&clients[client_index].thread, &attr, client_handler, arg) != 0) {
            free(arg);
            clients[client_index].connected = 0;
            client_count--;
            pthread_mutex_unlock(&client_mutex);
            return;
        }

        pthread_attr_destroy(&attr);
        pthread_mutex_unlock(&client_mutex);
    }
}

/**
 * 主函数
 */
int main(int argc, char *argv[]) {
    // 检查命令行参数
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <update_interval_ms>\n", argv[0]);
        return 1;
    }

    // 解析更新间隔
    update_interval_ms = atoi(argv[1]);
    if (update_interval_ms <= 0) {
        fprintf(stderr, "Error: update interval must be a positive integer\n");
        return 1;
    }

    // 初始化文档
    markdown_init(&doc);

    // 初始化客户端数组
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].connected = 0;
    }

    // 设置信号处理
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handle_signal;

    if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        return 1;
    }

    // 打印服务器PID
    printf("Server PID: %d\n", getpid());

    // 创建更新线程
    pthread_t update_tid;
    if (pthread_create(&update_tid, NULL, update_thread, NULL) != 0) {
        return 1;
    }

    // 主循环，处理服务器命令
    char command[MAX_COMMAND_LEN];
    while (server_running) {
        if (fgets(command, MAX_COMMAND_LEN, stdin)) {
            // 移除换行符
            size_t len = strlen(command);
            if (len > 0 && command[len - 1] == '\n') {
                command[len - 1] = '\0';
            }

            // 处理服务器命令
            if (strcmp(command, "QUIT") == 0) {
                // 检查是否有客户端连接
                pthread_mutex_lock(&client_mutex);
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
                pthread_mutex_unlock(&client_mutex);
            }
        }
    }

    // 等待更新线程结束
    pthread_cancel(update_tid);
    pthread_join(update_tid, NULL);

    // 保存文档并清理资源
    save_document();
    cleanup_resources();

    return 0;
}

/**
 * 客户端处理线程
 */
void *client_handler(void *arg) {
    int client_index = *((int *)arg);
    free(arg);

    pid_t client_pid = clients[client_index].pid;

    // 创建命名管道
    char c2s_path[64], s2c_path[64];
    snprintf(c2s_path, sizeof(c2s_path), "FIFO_C2S_%d", client_pid);
    snprintf(s2c_path, sizeof(s2c_path), "FIFO_S2C_%d", client_pid);

    // 删除已存在的管道
    unlink(c2s_path);
    unlink(s2c_path);

    // 创建新管道
    if (mkfifo(c2s_path, FIFO_PERM) == -1 || mkfifo(s2c_path, FIFO_PERM) == -1) {
        handle_client_disconnect(client_index);
        return NULL;
    }

    // 向客户端发送信号，通知管道已创建
    if (kill(client_pid, SIGRTMIN + 1) == -1) {
        unlink(c2s_path);
        unlink(s2c_path);
        handle_client_disconnect(client_index);
        return NULL;
    }

    // 打开管道
    int c2s_fd = open(c2s_path, O_RDONLY | O_NONBLOCK);
    int s2c_fd = open(s2c_path, O_WRONLY);

    if (c2s_fd == -1 || s2c_fd == -1) {
        if (c2s_fd != -1) close(c2s_fd);
        if (s2c_fd != -1) close(s2c_fd);
        unlink(c2s_path);
        unlink(s2c_path);
        handle_client_disconnect(client_index);
        return NULL;
    }

    // 更新客户端信息
    clients[client_index].c2s_fd = c2s_fd;
    clients[client_index].s2c_fd = s2c_fd;

    // 读取用户名
    char username[MAX_USERNAME_LEN];
    ssize_t bytes_read = 0;
    size_t total_read = 0;

    // 非阻塞读取可能需要多次尝试
    fd_set read_fds;
    struct timeval tv;
    int ready;

    while (total_read < MAX_USERNAME_LEN - 1) {
        FD_ZERO(&read_fds);
        FD_SET(c2s_fd, &read_fds);

        tv.tv_sec = 1;  // 1秒超时
        tv.tv_usec = 0;

        ready = select(c2s_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready == -1) {
            perror("select");
            break;
        } else if (ready == 0) {
            // 超时，继续尝试
            continue;
        }

        bytes_read = read(c2s_fd, username + total_read, MAX_USERNAME_LEN - 1 - total_read);

        if (bytes_read <= 0) {
            if (bytes_read == 0 || errno != EAGAIN) {
                // 连接关闭或错误
                break;
            }
            // EAGAIN，继续尝试
            continue;
        }

        total_read += bytes_read;

        // 检查是否读取到换行符
        if (username[total_read - 1] == '\n') {
            username[total_read - 1] = '\0';  // 替换换行符为终止符
            break;
        }
    }

    if (total_read == 0) {
        // 未能读取用户名
        close(c2s_fd);
        close(s2c_fd);
        unlink(c2s_path);
        unlink(s2c_path);
        handle_client_disconnect(client_index);
        return NULL;
    }

    username[total_read] = '\0';  // 确保字符串正确终止

    // 检查用户权限
    client_role role = get_user_role(username);

    // 保存用户信息
    strncpy(clients[client_index].username, username, MAX_USERNAME_LEN - 1);
    clients[client_index].username[MAX_USERNAME_LEN - 1] = '\0';
    clients[client_index].role = role;

    // 发送角色信息和文档内容
    if (role != ROLE_NONE) {
        // 发送角色
        const char *role_str = (role == ROLE_READ) ? "read\n" : "write\n";
        write(s2c_fd, role_str, strlen(role_str));

        // 发送文档版本号
        char version_str[32];
        snprintf(version_str, sizeof(version_str), "%lu\n", doc.version);
        write(s2c_fd, version_str, strlen(version_str));

        // 发送文档内容
        pthread_mutex_lock(&doc_mutex);
        char *content = markdown_flatten(&doc);
        pthread_mutex_unlock(&doc_mutex);

        if (content) {
            // 发送文档长度
            char length_str[32];
            snprintf(length_str, sizeof(length_str), "%zu\n", strlen(content));
            write(s2c_fd, length_str, strlen(length_str));

            // 发送文档内容
            write(s2c_fd, content, strlen(content));
            free(content);
        } else {
            // 空文档
            write(s2c_fd, "0\n", 2);
        }
    } else {
        // 未授权用户
        write(s2c_fd, "Reject UNAUTHORISED.\n", 21);

        // 等待1秒
        sleep(1);

        // 关闭连接
        close(c2s_fd);
        close(s2c_fd);
        unlink(c2s_path);
        unlink(s2c_path);
        handle_client_disconnect(client_index);
        return NULL;
    }

    // 主循环，处理客户端命令
    char command[MAX_COMMAND_LEN];
    while (clients[client_index].connected) {
        FD_ZERO(&read_fds);
        FD_SET(c2s_fd, &read_fds);

        tv.tv_sec = 1;  // 1秒超时
        tv.tv_usec = 0;

        ready = select(c2s_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready == -1) {
            if (errno == EINTR) {
                // 被信号中断，继续
                continue;
            }
            break;
        } else if (ready == 0) {
            // 超时，继续
            continue;
        }

        // 读取命令
        bytes_read = read(c2s_fd, command, MAX_COMMAND_LEN - 1);

        if (bytes_read <= 0) {
            if (bytes_read == 0 || errno != EAGAIN) {
                // 连接关闭或错误
                break;
            }
            // EAGAIN，继续
            continue;
        }

        command[bytes_read] = '\0';

        printf("Received command: %s\n", command);
        // 处理命令
        if (strncmp(command, "DISCONNECT", 10) == 0) {
            break;
        } else if (strncmp(command, "DOC?", 4) == 0) {
            // 发送文档内容和版本号
            pthread_mutex_lock(&doc_mutex);
            char *content = markdown_flatten(&doc);
            uint64_t current_version = doc.version;
            pthread_mutex_unlock(&doc_mutex);

            // 先发送版本号
            char version_str[32];
            snprintf(version_str, sizeof(version_str), "%lu\n", current_version);
            write(s2c_fd, version_str, strlen(version_str));

            if (content) {
                write(s2c_fd, content, strlen(content));
                printf("send content: %s\n", content);
                write(s2c_fd, "\n", 1);
                free(content);
            } else {
                write(s2c_fd, "\n", 1);
            }
        } else if (strncmp(command, "PERM?", 5) == 0) {
            // 发送权限信息
            const char *role_str = (role == ROLE_READ) ? "read\n" : "write\n";
            write(s2c_fd, role_str, strlen(role_str));
        } else {
            // 添加命令到队列
            command_node *new_node = (command_node *)malloc(sizeof(command_node));
            if (new_node) {
                strncpy(new_node->username, username, MAX_USERNAME_LEN - 1);
                new_node->username[MAX_USERNAME_LEN - 1] = '\0';
                strncpy(new_node->command, command, MAX_COMMAND_LEN - 1);
                new_node->command[MAX_COMMAND_LEN - 1] = '\0';
                new_node->timestamp = time(NULL);
                new_node->next = NULL;

                // 添加到队列末尾
                if (!command_queue) {
                    command_queue = new_node;
                } else {
                    command_node *current = command_queue;
                    while (current->next) {
                        current = current->next;
                    }
                    current->next = new_node;
                }
            }
        }
    }

    // 关闭连接
    close(c2s_fd);
    close(s2c_fd);
    unlink(c2s_path);
    unlink(s2c_path);
    handle_client_disconnect(client_index);

    return NULL;
}

/**
 * 更新线程，定期处理命令队列并广播更新
 */
void *update_thread(void *arg) {
    (void)arg; // 未使用的参数

    while (server_running) {
        // 等待指定的更新间隔
        usleep(update_interval_ms * 1000);

        // 处理命令队列
        int version_changed = 0;
        command_node *command_list = NULL; // 用于临时存储命令队列

        pthread_mutex_lock(&queue_mutex);
        // 按时间戳排序命令
        if (command_queue) {
            // 简单的冒泡排序
            int swapped;
            command_node *ptr1;
            command_node *lptr = NULL;

            do {
                swapped = 0;
                ptr1 = command_queue;

                while (ptr1->next != lptr) {
                    if (ptr1->timestamp > ptr1->next->timestamp) {
                        // 交换节点数据
                        char temp_username[MAX_USERNAME_LEN];
                        char temp_command[MAX_COMMAND_LEN];
                        time_t temp_timestamp;

                        strncpy(temp_username, ptr1->username, MAX_USERNAME_LEN - 1);
                        temp_username[MAX_USERNAME_LEN - 1] = '\0';
                        strncpy(temp_command, ptr1->command, MAX_COMMAND_LEN - 1);
                        temp_command[MAX_COMMAND_LEN - 1] = '\0';
                        temp_timestamp = ptr1->timestamp;

                        strncpy(ptr1->username, ptr1->next->username, MAX_USERNAME_LEN - 1);
                        ptr1->username[MAX_USERNAME_LEN - 1] = '\0';
                        strncpy(ptr1->command, ptr1->next->command, MAX_COMMAND_LEN - 1);
                        ptr1->command[MAX_COMMAND_LEN - 1] = '\0';
                        ptr1->timestamp = ptr1->next->timestamp;

                        strncpy(ptr1->next->username, temp_username, MAX_USERNAME_LEN - 1);
                        ptr1->next->username[MAX_USERNAME_LEN - 1] = '\0';
                        strncpy(ptr1->next->command, temp_command, MAX_COMMAND_LEN - 1);
                        ptr1->next->command[MAX_COMMAND_LEN - 1] = '\0';
                        ptr1->next->timestamp = temp_timestamp;

                        swapped = 1;
                    }
                    ptr1 = ptr1->next;
                }
                lptr = ptr1;
            } while (swapped);

            // 保存排序后的命令队列，并清空全局队列
            command_list = command_queue;
            command_queue = NULL;
        }
        // 处理命令
        if (command_list) {
            command_node *current = command_list;
            command_node *next;

            while (current) {
                // 处理命令
                process_command(current->username, current->command);
                version_changed = 1;

                next = current->next;
                free(current);
                current = next;
            }

            // 如果有命令被处理，先广播更新，再增加文档版本号
            if (version_changed) {
                pthread_mutex_unlock(&queue_mutex);

                // 广播更新（函数内部会自己获取锁）
                broadcast_update(version_changed);

                // 增加文档版本号
                markdown_increment_version(&doc);

                // 继续下一次循环
                continue;
            }
        }
        pthread_mutex_unlock(&queue_mutex);
    }

    return NULL;
}

/**
 * 获取用户角色
 */
client_role get_user_role(const char *username) {
    FILE *roles_file = fopen("roles.txt", "r");
    if (!roles_file) {
        return ROLE_NONE;
    }

    char line[256];
    char file_username[MAX_USERNAME_LEN];
    char role_str[10];

    while (fgets(line, sizeof(line), roles_file)) {
        // 跳过空行
        if (line[0] == '\n' || line[0] == '\0') {
            continue;
        }

        // 解析用户名和角色
        if (sscanf(line, "%s %s", file_username, role_str) == 2) {
            if (strcmp(file_username, username) == 0) {
                fclose(roles_file);

                if (strcmp(role_str, "read") == 0) {
                    return ROLE_READ;
                } else if (strcmp(role_str, "write") == 0) {
                    return ROLE_WRITE;
                } else {
                    return ROLE_NONE;
                }
            }
        }
    }

    fclose(roles_file);
    return ROLE_NONE;
}

/**
 * 处理客户端命令
 */
void process_command(const char *username, const char *command) {
    // 查找用户
    int client_index = -1;
    client_role role = ROLE_NONE;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected && strcmp(clients[i].username, username) == 0) {
            client_index = i;
            role = clients[i].role;
            break;
        }
    }

    if (client_index == -1 || role == ROLE_NONE) {
        return; // 用户不存在或未授权
    }

    // 解析命令
    char cmd_type[32];
    size_t pos1 = 0, pos2 = 0;
    char content[MAX_COMMAND_LEN];
    int level = 0;
    uint64_t cmd_version = 0;

    if (!parse_command(command, cmd_type, &pos1, &pos2, content, &level, &cmd_version)) {
        return; // 命令格式错误
    }

    // 获取当前文档版本号用于执行命令
    pthread_mutex_lock(&doc_mutex);
    uint64_t current_version = doc.version;
    pthread_mutex_unlock(&doc_mutex);

    // 执行命令
    // 只有写权限的用户才能修改文档
    if (role != ROLE_WRITE &&
        (strcmp(cmd_type, "INSERT") == 0 ||
         strcmp(cmd_type, "DEL") == 0 ||
         strcmp(cmd_type, "HEADING") == 0 ||
         strcmp(cmd_type, "BOLD") == 0 ||
         strcmp(cmd_type, "ITALIC") == 0 ||
         strcmp(cmd_type, "BLOCKQUOTE") == 0 ||
         strcmp(cmd_type, "ORDERED_LIST") == 0 ||
         strcmp(cmd_type, "UNORDERED_LIST") == 0 ||
         strcmp(cmd_type, "CODE") == 0 ||
         strcmp(cmd_type, "HORIZONTAL_RULE") == 0 ||
         strcmp(cmd_type, "LINK") == 0 ||
         strcmp(cmd_type, "NEWLINE") == 0)) {
        // 权限不足，创建一个状态为 UNAUTHORIZED 的命令
        edit_command *cmd = create_command(CMD_INSERT, current_version, 0, 0, NULL, 0, username, command);
        if (cmd) {
            cmd->status = UNAUTHORIZED;
            add_pending_edit(&doc, cmd);
        }
        return;
    }

    if (strcmp(cmd_type, "INSERT") == 0) {
        markdown_insert(&doc, current_version, pos1, content, username, command);
    } else if (strcmp(cmd_type, "DEL") == 0) {
        markdown_delete(&doc, current_version, pos1, pos2, username, command);
    } else if (strcmp(cmd_type, "HEADING") == 0) {
        markdown_heading(&doc, current_version, level, pos1, username, command);
    } else if (strcmp(cmd_type, "BOLD") == 0) {
        markdown_bold(&doc, current_version, pos1, pos2, username, command);
    } else if (strcmp(cmd_type, "ITALIC") == 0) {
        markdown_italic(&doc, current_version, pos1, pos2, username, command);
    } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0) {
        markdown_blockquote(&doc, current_version, pos1, username, command);
    } else if (strcmp(cmd_type, "ORDERED_LIST") == 0) {
        markdown_ordered_list(&doc, current_version, pos1, username, command);
    } else if (strcmp(cmd_type, "UNORDERED_LIST") == 0) {
        markdown_unordered_list(&doc, current_version, pos1, username, command);
    } else if (strcmp(cmd_type, "CODE") == 0) {
        markdown_code(&doc, current_version, pos1, pos2, username, command);
    } else if (strcmp(cmd_type, "HORIZONTAL_RULE") == 0) {
        markdown_horizontal_rule(&doc, current_version, pos1, username, command);
    } else if (strcmp(cmd_type, "LINK") == 0) {
        markdown_link(&doc, current_version, pos1, pos2, content, username, command);
    } else if (strcmp(cmd_type, "NEWLINE") == 0) {
        markdown_newline(&doc, current_version, pos1, username, command);
    }

    // 不再需要单独记录日志，因为我们现在使用 pending_edits
}

/**
 * 广播更新到所有客户端
 */
void broadcast_update(int version_changed) {
    (void)version_changed; // 标记参数为未使用
    // 构造广播消息
    char *message = NULL;
    size_t message_len = 0;
    FILE *message_stream = open_memstream(&message, &message_len);

    if (!message_stream) {
        return;
    }

    // 版本号
    fprintf(message_stream, "VERSION %lu\n", doc.version);

    // 使用 pending_edits 构造广播消息
    pthread_mutex_lock(&doc_mutex);
    edit_command *cmd = doc.pending_edits;
    // original_cmd 去除换行
    char original_cmd[MAX_COMMAND_LEN];
    strncpy(original_cmd, cmd->original_cmd, MAX_COMMAND_LEN - 1);
    original_cmd[MAX_COMMAND_LEN - 1] = '\0';
    if (original_cmd[strlen(original_cmd) - 1] == '\n') {
        original_cmd[strlen(original_cmd) - 1] = '\0';
    }
    while (cmd) {
        // 构造EDIT行：EDIT <username> <command>
        fprintf(message_stream, "EDIT %s %s", cmd->username, original_cmd);

        // 根据命令状态构造状态行
        if (cmd->status == SUCCESS) {
            fprintf(message_stream, " SUCCESS\n");
        } else {
            // 根据错误码构造拒绝消息
            const char *reason = "UNKNOWN";
            switch (cmd->status) {
                case INVALID_CURSOR_POS:
                    reason = "INVALID_POSITION";
                    break;
                case DELETED_POSITION:
                    reason = "DELETED_POSITION";
                    break;
                case OUTDATED_VERSION:
                    reason = "OUTDATED_VERSION";
                    break;
                case UNAUTHORIZED:
                    reason = "UNAUTHORISED";
                    break;
            }
            fprintf(message_stream, " Reject %s\n", reason);
        }
        cmd = cmd->next;
    }
    pthread_mutex_unlock(&doc_mutex);

    // 结束标记
    fprintf(message_stream, "END\n");

    fclose(message_stream);

    // 广播给所有客户端
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected && clients[i].s2c_fd != -1) {
            printf("Broadcasting message:%s\n", message);
            ssize_t bytes_written = write(clients[i].s2c_fd, message, message_len);
            if (bytes_written <= 0) {
                // 写入失败，可能客户端已断开
                close(clients[i].s2c_fd);
                clients[i].s2c_fd = -1;
                clients[i].connected = 0;
            }
        }
    }

    free(message);
}

/**
 * 保存文档到文件
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
void cleanup_resources() {
    // 关闭所有客户端连接
    pthread_mutex_lock(&client_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected) {
            if (clients[i].c2s_fd != -1) {
                close(clients[i].c2s_fd);
            }
            if (clients[i].s2c_fd != -1) {
                close(clients[i].s2c_fd);
            }

            char c2s_path[64], s2c_path[64];
            snprintf(c2s_path, sizeof(c2s_path), "FIFO_C2S_%d", clients[i].pid);
            snprintf(s2c_path, sizeof(s2c_path), "FIFO_S2C_%d", clients[i].pid);

            unlink(c2s_path);
            unlink(s2c_path);
        }
    }

    pthread_mutex_unlock(&client_mutex);

    // 释放命令队列
    pthread_mutex_lock(&queue_mutex);

    command_node *current = command_queue;
    command_node *next;

    while (current) {
        next = current->next;
        free(current);
        current = next;
    }

    command_queue = NULL;

    pthread_mutex_unlock(&queue_mutex);

    // 释放日志资源
    pthread_mutex_lock(&log_mutex);
    if (log.versions) {
        for (size_t i = 0; i < log.count; i++) {
            if (log.versions[i].entries) {
                for (size_t j = 0; j < log.versions[i].count; j++) {
                    free(log.versions[i].entries[j]);
                }
                free(log.versions[i].entries);
            }
        }
        free(log.versions);
        log.versions = NULL;
        log.count = 0;
        log.capacity = 0;
    }
    pthread_mutex_unlock(&log_mutex);

    // 释放文档资源
    pthread_mutex_lock(&doc_mutex);
    markdown_free(&doc);
    pthread_mutex_unlock(&doc_mutex);

    // 销毁互斥锁
    pthread_mutex_destroy(&client_mutex);
    pthread_mutex_destroy(&doc_mutex);
    pthread_mutex_destroy(&queue_mutex);
    pthread_mutex_destroy(&log_mutex);
}

/**
 * 处理客户端断开连接
 */
void handle_client_disconnect(int client_index) {
    pthread_mutex_lock(&client_mutex);

    if (client_index >= 0 && client_index < MAX_CLIENTS && clients[client_index].connected) {
        clients[client_index].connected = 0;

        if (clients[client_index].c2s_fd != -1) {
            close(clients[client_index].c2s_fd);
            clients[client_index].c2s_fd = -1;
        }

        if (clients[client_index].s2c_fd != -1) {
            close(clients[client_index].s2c_fd); // 修复：正确关闭文件描述符
            clients[client_index].s2c_fd = -1;
        }

        client_count--;
        printf("客户端 %s 已断开连接\n", clients[client_index].username);
    }

    pthread_mutex_unlock(&client_mutex);
}

/**
 * 解析命令 - 不包含版本号
 * 格式：INSERT <pos> <content>
 * 修复：手动解析以保留内容中的空格
 */
int parse_command(const char *command, char *cmd_type, size_t *pos1, size_t *pos2, char *content, int *level, uint64_t *version) {
    if (!command || !cmd_type || !pos1 || !pos2 || !content || !level || !version) {
        return 0;
    }

    // 版本号设为0，表示不使用版本检查
    *version = 0;

    // 移除末尾的换行符
    char cmd_copy[MAX_COMMAND_LEN];
    strncpy(cmd_copy, command, MAX_COMMAND_LEN - 1);
    cmd_copy[MAX_COMMAND_LEN - 1] = '\0';

    size_t len = strlen(cmd_copy);
    if (len > 0 && cmd_copy[len - 1] == '\n') {
        cmd_copy[len - 1] = '\0';
        len--;
    }

    // 手动解析命令，避免使用strtok丢失空格
    char *ptr = cmd_copy;

    // 跳过开头的空格
    while (*ptr == ' ') ptr++;

    // 解析命令类型
    char *cmd_start = ptr;
    while (*ptr && *ptr != ' ') ptr++;

    size_t cmd_len = ptr - cmd_start;
    if (cmd_len > 31) cmd_len = 31;
    strncpy(cmd_type, cmd_start, cmd_len);
    cmd_type[cmd_len] = '\0';

    // 如果命令后没有参数，直接返回（如某些单参数命令）
    if (*ptr == '\0') {
        return 1;
    }

    // 跳过空格
    while (*ptr == ' ') ptr++;

    // 参数部分（不再分离版本号）
    char *args = ptr;

    // 根据命令类型解析参数
    if (strcmp(cmd_type, "INSERT") == 0) {
        // INSERT <pos> <content>
        char *pos_end = strchr(args, ' ');
        if (!pos_end) return 0; // 缺少内容部分

        *pos_end = '\0';
        *pos1 = atol(args);

        // 内容部分（保留所有空格）
        char *content_start = pos_end + 1;
        strncpy(content, content_start, MAX_COMMAND_LEN - 1);
        content[MAX_COMMAND_LEN - 1] = '\0';

    } else if (strcmp(cmd_type, "DEL") == 0) {
        // DEL <pos> <no_char>
        if (sscanf(args, "%zu %zu", pos1, pos2) != 2) return 0;

    } else if (strcmp(cmd_type, "HEADING") == 0) {
        // HEADING <level> <pos>
        if (sscanf(args, "%d %zu", level, pos1) != 2) return 0;

    } else if (strcmp(cmd_type, "BOLD") == 0 ||
               strcmp(cmd_type, "ITALIC") == 0 ||
               strcmp(cmd_type, "CODE") == 0) {
        // <cmd> <pos_start> <pos_end>
        if (sscanf(args, "%zu %zu", pos1, pos2) != 2) return 0;

    } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0 ||
               strcmp(cmd_type, "ORDERED_LIST") == 0 ||
               strcmp(cmd_type, "UNORDERED_LIST") == 0 ||
               strcmp(cmd_type, "HORIZONTAL_RULE") == 0 ||
               strcmp(cmd_type, "NEWLINE") == 0) {
        // <cmd> <pos>
        if (sscanf(args, "%zu", pos1) != 1) return 0;

    } else if (strcmp(cmd_type, "LINK") == 0) {
        // LINK <pos_start> <pos_end> <link>
        char *first_space = strchr(args, ' ');
        if (!first_space) return 0;

        *first_space = '\0';
        *pos1 = atol(args);

        char *second_space = strchr(first_space + 1, ' ');
        if (!second_space) return 0;

        *second_space = '\0';
        *pos2 = atol(first_space + 1);

        // 链接内容（保留所有空格）
        char *link_start = second_space + 1;
        strncpy(content, link_start, MAX_COMMAND_LEN - 1);
        content[MAX_COMMAND_LEN - 1] = '\0';

    } else {
        // 未知命令
        return 0;
    }

    return 1;
}

/**
 * 添加日志条目
 */
void add_log_entry(uint64_t version, const char *entry) {
    if (!entry) return;

    // 获取日志互斥锁
    pthread_mutex_lock(&log_mutex);

    // 初始化日志数组
    if (!log.versions) {
        log.capacity = 10;
        log.versions = (version_log *)malloc(log.capacity * sizeof(version_log));
        if (!log.versions) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }
        memset(log.versions, 0, log.capacity * sizeof(version_log));
    }

    // 查找版本对应的日志
    int version_index = -1;
    for (size_t i = 0; i < log.count; i++) {
        if (log.versions[i].version == version) {
            version_index = i;
            break;
        }
    }

    // 如果没有找到，创建新的版本日志
    if (version_index == -1) {
        // 扩展数组容量
        if (log.count >= log.capacity) {
            log.capacity *= 2;
            version_log *new_versions = (version_log *)realloc(log.versions, log.capacity * sizeof(version_log));
            if (!new_versions) {
                pthread_mutex_unlock(&log_mutex);
                return;
            }
            log.versions = new_versions;
        }

        version_index = log.count++;
        log.versions[version_index].version = version;
        log.versions[version_index].entries = NULL;
        log.versions[version_index].count = 0;
        log.versions[version_index].capacity = 0;
    }

    // 初始化条目数组
    if (!log.versions[version_index].entries) {
        log.versions[version_index].capacity = 10;
        log.versions[version_index].entries = (char **)malloc(log.versions[version_index].capacity * sizeof(char *));
        if (!log.versions[version_index].entries) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }
    }

    // 扩展条目数组容量
    if (log.versions[version_index].count >= log.versions[version_index].capacity) {
        log.versions[version_index].capacity *= 2;
        char **new_entries = (char **)realloc(log.versions[version_index].entries,
                                             log.versions[version_index].capacity * sizeof(char *));
        if (!new_entries) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }
        log.versions[version_index].entries = new_entries;
    }

    // 添加新条目
    log.versions[version_index].entries[log.versions[version_index].count] = strdup(entry);
    if (log.versions[version_index].entries[log.versions[version_index].count]) {
        log.versions[version_index].count++;
    }

    // 释放日志互斥锁
    pthread_mutex_unlock(&log_mutex);
}

/**
 * 打印命令日志
 */
void print_command_log() {
    // 获取日志互斥锁
    pthread_mutex_lock(&log_mutex);

    for (size_t i = 0; i < log.count; i++) {
        printf("VERSION %lu\n", log.versions[i].version);
        for (size_t j = 0; j < log.versions[i].count; j++) {
            printf("%s\n", log.versions[i].entries[j]);
        }
        printf("END\n");
    }

    // 释放日志互斥锁
    pthread_mutex_unlock(&log_mutex);
}
