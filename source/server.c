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
int parse_command(const char *command, char *cmd_type, size_t *pos1, size_t *pos2, char *content, int *level);

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
            perror("malloc");
            clients[client_index].connected = 0;
            client_count--;
            pthread_mutex_unlock(&client_mutex);
            return;
        }

        *arg = client_index;

        if (pthread_create(&clients[client_index].thread, &attr, client_handler, arg) != 0) {
            perror("pthread_create");
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
        perror("sigaction");
        return 1;
    }

    // 打印服务器PID
    printf("Server PID: %d\n", getpid());

    // 创建更新线程
    pthread_t update_tid;
    if (pthread_create(&update_tid, NULL, update_thread, NULL) != 0) {
        perror("pthread_create");
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
            if (strcmp(command, "DOC?") == 0) {
                pthread_mutex_lock(&doc_mutex);
                printf("Document content:\n");
                markdown_print(&doc, stdout);
                printf("\n");
                pthread_mutex_unlock(&doc_mutex);
            } else if (strcmp(command, "LOG?") == 0) {
                // 输出命令日志
                pthread_mutex_lock(&doc_mutex);
                printf("Command log:\n");
                // TODO: 实现日志输出
                pthread_mutex_unlock(&doc_mutex);
            } else if (strcmp(command, "QUIT") == 0) {
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
        perror("mkfifo");
        handle_client_disconnect(client_index);
        return NULL;
    }

    // 向客户端发送信号，通知管道已创建
    if (kill(client_pid, SIGRTMIN + 1) == -1) {
        perror("kill");
        unlink(c2s_path);
        unlink(s2c_path);
        handle_client_disconnect(client_index);
        return NULL;
    }

    // 打开管道
    int c2s_fd = open(c2s_path, O_RDONLY | O_NONBLOCK);
    int s2c_fd = open(s2c_path, O_WRONLY);

    if (c2s_fd == -1 || s2c_fd == -1) {
        perror("open");
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
            perror("select");
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

        // 处理命令
        if (strncmp(command, "DISCONNECT", 10) == 0) {
            break;
        } else if (strncmp(command, "DOC?", 4) == 0) {
            // 发送文档内容
            pthread_mutex_lock(&doc_mutex);
            char *content = markdown_flatten(&doc);
            pthread_mutex_unlock(&doc_mutex);

            if (content) {
                write(s2c_fd, content, strlen(content));
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
            pthread_mutex_lock(&queue_mutex);

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

            pthread_mutex_unlock(&queue_mutex);
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

            // 处理排序后的命令
            pthread_mutex_lock(&doc_mutex);

            command_node *current = command_queue;
            command_node *next;

            while (current) {
                // 处理命令
                process_command(current->username, current->command);
                version_changed = 1;

                next = current->next;
                free(current);
                current = next;
            }

            // 如果有命令被处理，增加文档版本号
            if (version_changed) {
                markdown_increment_version(&doc);
            }

            pthread_mutex_unlock(&doc_mutex);

            // 清空命令队列
            command_queue = NULL;
        }

        pthread_mutex_unlock(&queue_mutex);

        // 广播更新
        broadcast_update(version_changed);
    }

    return NULL;
}

/**
 * 获取用户角色
 */
client_role get_user_role(const char *username) {
    FILE *roles_file = fopen("roles.txt", "r");
    if (!roles_file) {
        perror("fopen");
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

    pthread_mutex_lock(&client_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected && strcmp(clients[i].username, username) == 0) {
            client_index = i;
            role = clients[i].role;
            break;
        }
    }

    pthread_mutex_unlock(&client_mutex);

    if (client_index == -1 || role == ROLE_NONE) {
        return; // 用户不存在或未授权
    }

    // 解析命令
    char cmd_type[32];
    size_t pos1 = 0, pos2 = 0;
    char content[MAX_COMMAND_LEN];
    int level = 0;

    if (!parse_command(command, cmd_type, &pos1, &pos2, content, &level)) {
        return; // 命令格式错误
    }

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
        return; // 权限不足
    }

    if (strcmp(cmd_type, "INSERT") == 0) {
        markdown_insert(&doc, doc.version, pos1, content);
    } else if (strcmp(cmd_type, "DEL") == 0) {
        markdown_delete(&doc, doc.version, pos1, pos2);
    } else if (strcmp(cmd_type, "HEADING") == 0) {
        markdown_heading(&doc, doc.version, level, pos1);
    } else if (strcmp(cmd_type, "BOLD") == 0) {
        markdown_bold(&doc, doc.version, pos1, pos2);
    } else if (strcmp(cmd_type, "ITALIC") == 0) {
        markdown_italic(&doc, doc.version, pos1, pos2);
    } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0) {
        markdown_blockquote(&doc, doc.version, pos1);
    } else if (strcmp(cmd_type, "ORDERED_LIST") == 0) {
        markdown_ordered_list(&doc, doc.version, pos1);
    } else if (strcmp(cmd_type, "UNORDERED_LIST") == 0) {
        markdown_unordered_list(&doc, doc.version, pos1);
    } else if (strcmp(cmd_type, "CODE") == 0) {
        markdown_code(&doc, doc.version, pos1, pos2);
    } else if (strcmp(cmd_type, "HORIZONTAL_RULE") == 0) {
        markdown_horizontal_rule(&doc, doc.version, pos1);
    } else if (strcmp(cmd_type, "LINK") == 0) {
        markdown_link(&doc, doc.version, pos1, pos2, content);
    } else if (strcmp(cmd_type, "NEWLINE") == 0) {
        markdown_newline(&doc, doc.version, pos1);
    }

    // 记录命令结果
    // TODO: 实现命令日志
}

/**
 * 广播更新到所有客户端
 */
void broadcast_update(int version_changed) {
    if (!version_changed) {
        return;
    }

    pthread_mutex_lock(&client_mutex);
    pthread_mutex_lock(&doc_mutex);

    // 构造广播消息
    char *message = NULL;
    size_t message_len = 0;
    FILE *message_stream = open_memstream(&message, &message_len);

    if (!message_stream) {
        pthread_mutex_unlock(&doc_mutex);
        pthread_mutex_unlock(&client_mutex);
        return;
    }

    // 版本号
    fprintf(message_stream, "VERSION %lu\n", doc.version);

    // 添加编辑历史（示例）
    fprintf(message_stream, "EDIT server UPDATE SUCCESS\n");

    // 结束标记
    fprintf(message_stream, "END\n");

    fclose(message_stream);

    printf("[日志] 广播更新消息: %s\n", message);

    // 广播给所有客户端
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected && clients[i].s2c_fd != -1) {
            printf("[日志] 向客户端 %s (PID: %d) 发送更新\n",
                   clients[i].username, clients[i].pid);

            ssize_t bytes_written = write(clients[i].s2c_fd, message, message_len);

            if (bytes_written < 0) {
                printf("[错误] 向客户端 %s 发送更新失败: %s\n",
                       clients[i].username, strerror(errno));
            } else {
                printf("[日志] 成功向客户端 %s 发送 %zd 字节的更新\n",
                       clients[i].username, bytes_written);
            }
        }
    }

    free(message);

    pthread_mutex_unlock(&doc_mutex);
    pthread_mutex_unlock(&client_mutex);
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

    // 释放文档资源
    pthread_mutex_lock(&doc_mutex);
    markdown_free(&doc);
    pthread_mutex_unlock(&doc_mutex);

    // 销毁互斥锁
    pthread_mutex_destroy(&client_mutex);
    pthread_mutex_destroy(&doc_mutex);
    pthread_mutex_destroy(&queue_mutex);
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
            close(clients[client_index].s2c_fd = -1);
            clients[client_index].s2c_fd = -1;
        }

        client_count--;
    }

    pthread_mutex_unlock(&client_mutex);
}

/**
 * 解析命令
 */
int parse_command(const char *command, char *cmd_type, size_t *pos1, size_t *pos2, char *content, int *level) {
    if (!command || !cmd_type || !pos1 || !pos2 || !content || !level) {
        return 0;
    }

    // 移除末尾的换行符
    char cmd_copy[MAX_COMMAND_LEN];
    strncpy(cmd_copy, command, MAX_COMMAND_LEN - 1);
    cmd_copy[MAX_COMMAND_LEN - 1] = '\0';

    size_t len = strlen(cmd_copy);
    if (len > 0 && cmd_copy[len - 1] == '\n') {
        cmd_copy[len - 1] = '\0';
    }

    // 解析命令类型
    char *token = strtok(cmd_copy, " ");
    if (!token) {
        return 0;
    }

    strncpy(cmd_type, token, 31);
    cmd_type[31] = '\0';

    // 根据命令类型解析参数
    if (strcmp(cmd_type, "INSERT") == 0) {
        // INSERT <pos> <content>
        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos1 = atol(token);

        token = strtok(NULL, "");
        if (!token) return 0;
        strncpy(content, token, MAX_COMMAND_LEN - 1);
        content[MAX_COMMAND_LEN - 1] = '\0';

    } else if (strcmp(cmd_type, "DEL") == 0) {
        // DEL <pos> <no_char>
        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos1 = atol(token);

        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos2 = atol(token);

    } else if (strcmp(cmd_type, "HEADING") == 0) {
        // HEADING <level> <pos>
        token = strtok(NULL, " ");
        if (!token) return 0;
        *level = atoi(token);

        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos1 = atol(token);

    } else if (strcmp(cmd_type, "BOLD") == 0 ||
               strcmp(cmd_type, "ITALIC") == 0 ||
               strcmp(cmd_type, "CODE") == 0) {
        // <cmd> <pos_start> <pos_end>
        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos1 = atol(token);

        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos2 = atol(token);

    } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0 ||
               strcmp(cmd_type, "ORDERED_LIST") == 0 ||
               strcmp(cmd_type, "UNORDERED_LIST") == 0 ||
               strcmp(cmd_type, "HORIZONTAL_RULE") == 0 ||
               strcmp(cmd_type, "NEWLINE") == 0) {
        // <cmd> <pos>
        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos1 = atol(token);

    } else if (strcmp(cmd_type, "LINK") == 0) {
        // LINK <pos_start> <pos_end> <link>
        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos1 = atol(token);

        token = strtok(NULL, " ");
        if (!token) return 0;
        *pos2 = atol(token);

        token = strtok(NULL, "");
        if (!token) return 0;
        strncpy(content, token, MAX_COMMAND_LEN - 1);
        content[MAX_COMMAND_LEN - 1] = '\0';

    } else {
        // 未知命令
        return 0;
    }

    return 1;
}
