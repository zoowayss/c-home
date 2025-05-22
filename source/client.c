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
#include <stdint.h>
#include <poll.h>
#include "../libs/document.h"
#include "../libs/markdown.h"

// 定义实时信号
#ifndef SIGRTMIN
#define SIGRTMIN 34
#endif

#define MAX_COMMAND_LEN 256
#define MAX_DOCUMENT_SIZE 1048576 // 1MB

// 全局变量
static pid_t server_pid;
static pid_t client_pid;
static char username[64];
static int c2s_fd = -1; // 客户端到服务器的管道
static int s2c_fd = -1; // 服务器到客户端的管道
static document doc; // 本地文档副本
static uint64_t document_version; // 文档版本号
static int is_write_permission = 0;
static int client_running = 1;
static pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;

// 存储客户端命令日志
typedef struct {
    char **log_entries;  // 日志条目数组
    size_t count;        // 当前日志条目数量
    size_t capacity;     // 日志数组容量
} command_log;

// 客户端命令日志
static command_log log = {NULL, 0, 0};

// 函数声明
void handle_signal(int sig);
void *update_thread(void *arg);
void process_server_update(const char *update);
void cleanup_resources();
void print_document();
void add_log_entry(const char *entry);
void print_command_log();

/**
 * 主函数
 */
int main(int argc, char *argv[]) {
    // 检查命令行参数
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_pid> <username>\n", argv[0]);
        return 1;
    }

    // 解析参数
    server_pid = atoi(argv[1]);
    strncpy(username, argv[2], sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';

    // 获取客户端PID
    client_pid = getpid();

    // 设置信号处理
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN + 1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGRTMIN + 1, &sa, NULL);

    // 向服务器发送连接请求
    if (kill(server_pid, SIGRTMIN) == -1) {
        return 1;
    }

    // 等待服务器响应
    int sig;
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGRTMIN + 1);

    if (sigwait(&wait_mask, &sig) != 0) {
        return 1;
    }

    // 打开命名管道
    char c2s_path[64], s2c_path[64];
    snprintf(c2s_path, sizeof(c2s_path), "FIFO_C2S_%d", client_pid);
    snprintf(s2c_path, sizeof(s2c_path), "FIFO_S2C_%d", client_pid);

    c2s_fd = open(c2s_path, O_WRONLY);
    if (c2s_fd == -1) {
        cleanup_resources();
        return 1;
    }

    s2c_fd = open(s2c_path, O_RDONLY);
    if (s2c_fd == -1) {
        cleanup_resources();
        return 1;
    }

    // 发送用户名
    ssize_t bytes_written = write(c2s_fd, username, strlen(username));
    if (bytes_written < 0) {
        cleanup_resources();
        return 1;
    }
    write(c2s_fd, "\n", 1);

    // 逐字符读取角色信息直到遇到换行符
    char role_str[32];
    memset(role_str, 0, sizeof(role_str));
    char c;
    int idx = 0;
    ssize_t bytes_read;

    while ((size_t)idx < sizeof(role_str) - 1) {
        bytes_read = read(s2c_fd, &c, 1);
        if (bytes_read <= 0) {
            cleanup_resources();
            return 1;
        }

        if (c == '\n') {
            break;
        }

        role_str[idx++] = c;
    }

    role_str[idx] = '\0';

    // 检查是否被拒绝
    if (strncmp(role_str, "Reject", 6) == 0) {
        printf("%s\n", role_str);
        cleanup_resources();
        return 1;
    }

    // 解析角色
    if (strncmp(role_str, "write", 5) == 0) {
        is_write_permission = 1;
        printf("Connected with write permission.\n");
    } else {
        printf("Connected with read-only permission.\n");
    }

    // 读取文档版本号
    char version_str[32];
    memset(version_str, 0, sizeof(version_str));

    // 重置索引，继续使用之前的变量
    idx = 0;

    while ((size_t)idx < sizeof(version_str) - 1) {
        bytes_read = read(s2c_fd, &c, 1);
        if (bytes_read <= 0) {
            cleanup_resources();
            return 1;
        }

        if (c == '\n') {
            break;
        }

        version_str[idx++] = c;
    }

    version_str[idx] = '\0';
    document_version = strtoull(version_str, NULL, 10);

    // 读取文档长度
    char length_str[32];
    memset(length_str, 0, sizeof(length_str));

    // 重置索引
    idx = 0;
    while ((size_t)idx < sizeof(length_str) - 1) {
        bytes_read = read(s2c_fd, &c, 1);
        if (bytes_read <= 0) {
            cleanup_resources();
            return 1;
        }

        if (c == '\n') {
            break;
        }

        length_str[idx++] = c;
    }

    length_str[idx] = '\0';
    size_t doc_length = strtoull(length_str, NULL, 10);

    // 初始化文档
    markdown_init(&doc);

    // 读取文档内容
    if (doc_length > 0) {
        char *content = (char *)malloc(doc_length + 1);
        if (!content) {
            cleanup_resources();
            return 1;
        }

        bytes_read = read(s2c_fd, content, doc_length);
        if (bytes_read != (ssize_t)doc_length) {
            free(content);
            cleanup_resources();
            return 1;
        }
        content[doc_length] = '\0';

        // 将内容插入到文档中
        markdown_insert(&doc, doc.version, 0, content, "client", "INSERT 0 content");
        free(content);
    }

    // 创建更新线程
    pthread_t update_tid;
    if (pthread_create(&update_tid, NULL, update_thread, NULL) != 0) {
        cleanup_resources();
        return 1;
    }

    // 主循环，处理用户输入
    char command[MAX_COMMAND_LEN];
    printf("Enter commands (type 'DISCONNECT' to exit):\n");

    while (client_running) {
        // 使用 fgets 读取用户输入（阻塞模式）
        printf("> ");
        fflush(stdout);

        if (fgets(command, MAX_COMMAND_LEN, stdin) == NULL) {
            // 读取错误
            break;
        }

        // 移除换行符
        size_t len = strlen(command);
        if (len > 0 && command[len - 1] == '\n') {
            command[len - 1] = '\0';
            len--;
        }

        // 检查命令是否为空
        if (len == 0) {
            continue;
        }
        // 记录客户端发送的命令
        add_log_entry(command);

        // 处理本地命令
        if (strcmp(command, "DOC?") == 0) {
            pthread_mutex_lock(&doc_mutex);
            print_document();
            pthread_mutex_unlock(&doc_mutex);
            continue;
        }

        if (strcmp(command, "PERM?") == 0) {
            printf("%s\n", is_write_permission ? "write" : "read");
            continue;
        }

        if (strcmp(command, "LOG?") == 0) {
            print_command_log();
            continue;
        }

        if (strcmp(command, "DISCONNECT") == 0) {
            // 发送断开连接命令给服务器
            write(c2s_fd, command, strlen(command));
            write(c2s_fd, "\n", 1);
            break;
        }

        // 检查是否有写权限
        if (!is_write_permission &&
            (strncmp(command, "INSERT", 6) == 0 ||
             strncmp(command, "DEL", 3) == 0 ||
             strncmp(command, "HEADING", 7) == 0 ||
             strncmp(command, "BOLD", 4) == 0 ||
             strncmp(command, "ITALIC", 6) == 0 ||
             strncmp(command, "BLOCKQUOTE", 10) == 0 ||
             strncmp(command, "ORDERED_LIST", 12) == 0 ||
             strncmp(command, "UNORDERED_LIST", 14) == 0 ||
             strncmp(command, "CODE", 4) == 0 ||
             strncmp(command, "HORIZONTAL_RULE", 15) == 0 ||
             strncmp(command, "LINK", 4) == 0 ||
             strncmp(command, "NEWLINE", 7) == 0)) {
            printf("Error: You do not have write permission.\n");
            continue;
        }
        // 构造带版本号的命令
        // 为了避免截断，我们需要确保命令长度不会超过MAX_COMMAND_LEN
        size_t cmd_len = strlen(command);

        // 首先发送命令
        write(c2s_fd, command, cmd_len);
        // 然后添加空格和版本号
        char version_str[32];
        int version_len = snprintf(version_str, sizeof(version_str), " %lu", doc.version);
        // 发送版本号和换行符
        write(c2s_fd, version_str, version_len);
        write(c2s_fd, "\n", 1);
    }

    // 等待更新线程结束
    client_running = 0;
    pthread_cancel(update_tid);
    pthread_join(update_tid, NULL);

    // 清理资源
    cleanup_resources();

    return 0;
}

/**
 * 信号处理函数
 */
void handle_signal(int sig) {
    // 服务器已创建管道，可以继续连接过程
    (void)sig; // 未使用的参数
}

/**
 * 更新线程，接收服务器更新
 */
void *update_thread(void *arg) {
    (void)arg; // 未使用的参数

    // 设置管道为非阻塞模式
    int flags = fcntl(s2c_fd, F_GETFL, 0);
    fcntl(s2c_fd, F_SETFL, flags | O_NONBLOCK);

    char line[MAX_COMMAND_LEN];
    ssize_t bytes_read;
    int line_idx = 0;
    char c;

    // 设置poll结构
    struct pollfd fds[1];
    fds[0].fd = s2c_fd;
    fds[0].events = POLLIN;

    while (client_running) {
        // 使用poll等待数据，超时时间为100毫秒
        int poll_result = poll(fds, 1, 100);

        if (poll_result < 0) {
            if (errno == EINTR) {
                // 被信号中断，继续
                continue;
            } else {
                // 其他错误
                client_running = 0;
                break;
            }
        } else if (poll_result == 0) {
            // 超时，没有数据可读
            continue;
        }

        // 有数据可读
        if (fds[0].revents & POLLIN) {
            // 逐字符读取
            bytes_read = read(s2c_fd, &c, 1);

            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    // 连接关闭
                    client_running = 0;
                    break;
                } else if (errno == EINTR) {
                    // 被信号中断，继续
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 非阻塞模式下没有数据可读
                    continue;
                } else {
                    // 其他错误
                    client_running = 0;
                    break;
                }
            }

            // 检查是否读取到换行符
            if (c == '\n') {
                line[line_idx] = '\0';

                // 处理这一行
                if (line_idx > 0) {
                    pthread_mutex_lock(&doc_mutex);
                    process_server_update(line);
                    pthread_mutex_unlock(&doc_mutex);
                }

                // 重置行缓冲区
                line_idx = 0;
                memset(line, 0, sizeof(line));
            } else {
                // 添加字符到行缓冲区
                if (line_idx < MAX_COMMAND_LEN - 1) {
                    line[line_idx++] = c;
                } else {
                    // 行缓冲区已满，处理这一行并重置
                    line[MAX_COMMAND_LEN - 1] = '\0';
                    pthread_mutex_lock(&doc_mutex);
                    process_server_update(line);
                    pthread_mutex_unlock(&doc_mutex);
                    line_idx = 0;
                    memset(line, 0, sizeof(line));
                    // 将当前字符放入新的行缓冲区
                    line[line_idx++] = c;
                }
            }
        }
    }
    return NULL;
}

/**
 * 处理服务器更新
 */
void process_server_update(const char *update) {
    if (!update || !*update) {
        return;
    }

    // 解析更新消息
    if (strncmp(update, "VERSION", 7) == 0) {
        // 版本更新
        uint64_t new_version;
        sscanf(update, "VERSION %lu", &new_version);

        // 更新本地文档版本号
        if (new_version > doc.version) {
            // 保存旧版本号
            uint64_t old_version = doc.version;
            doc.version = new_version;

            // 如果版本号不连续（可能错过了一些更新），请求完整文档
            if (new_version > old_version + 1 && old_version > 0) {
                // 发送DOC?命令请求完整文档
                write(c2s_fd, "DOC?\n", 5);
            }
        }
    } else if (strncmp(update, "EDIT", 4) == 0) {
        // 编辑命令
        char edit_username[64];
        char command[MAX_COMMAND_LEN];
        char status[32];

        // 格式: EDIT <username> <command> <status>
        if (sscanf(update, "EDIT %s %s %s", edit_username, command, status) >= 3) {
            // 如果命令成功执行，更新本地文档
            if (strcmp(status, "SUCCESS") == 0) {
                // 无论是谁的命令，都应用到本地文档
                // 这样可以确保所有客户端的文档内容一致

                // 从原始更新消息中提取完整命令
                char full_command[MAX_COMMAND_LEN];
                if (sscanf(update, "EDIT %s %[^\n]", edit_username, full_command) >= 2) {
                    // 解析命令类型
                    char cmd_type[32];
                    size_t pos1 = 0, pos2 = 0;
                    char content[MAX_COMMAND_LEN];
                    int level = 0;

                    // 跳过用户名和EDIT部分
                    char *cmd_part = strchr(full_command, ' ');
                    if (cmd_part) {
                        cmd_part++; // 跳过空格

                        // 提取命令类型
                        char *space = strchr(cmd_part, ' ');
                        if (space) {
                            size_t cmd_len = space - cmd_part;
                            strncpy(cmd_type, cmd_part, cmd_len);
                            cmd_type[cmd_len] = '\0';
                            cmd_part = space + 1; // 跳过空格

                            // 根据命令类型解析参数
                            if (strcmp(cmd_type, "INSERT") == 0) {
                                // INSERT <pos> <content>
                                if (sscanf(cmd_part, "%zu %[^\n]", &pos1, content) >= 2) {
                                    markdown_insert(&doc, doc.version, pos1, content, username, command);
                                }
                            } else if (strcmp(cmd_type, "DEL") == 0) {
                                // DEL <pos> <no_char>
                                if (sscanf(cmd_part, "%zu %zu", &pos1, &pos2) >= 2) {
                                    markdown_delete(&doc, doc.version, pos1, pos2, username, command);
                                }
                            } else if (strcmp(cmd_type, "HEADING") == 0) {
                                // HEADING <level> <pos>
                                if (sscanf(cmd_part, "%d %zu", &level, &pos1) >= 2) {
                                    markdown_heading(&doc, doc.version, level, pos1, username, command);
                                }
                            } else if (strcmp(cmd_type, "BOLD") == 0) {
                                // BOLD <pos_start> <pos_end>
                                if (sscanf(cmd_part, "%zu %zu", &pos1, &pos2) >= 2) {
                                    markdown_bold(&doc, doc.version, pos1, pos2, username, command);
                                }
                            } else if (strcmp(cmd_type, "ITALIC") == 0) {
                                // ITALIC <pos_start> <pos_end>
                                if (sscanf(cmd_part, "%zu %zu", &pos1, &pos2) >= 2) {
                                    markdown_italic(&doc, doc.version, pos1, pos2, username, command);
                                }
                            } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0) {
                                // BLOCKQUOTE <pos>
                                if (sscanf(cmd_part, "%zu", &pos1) >= 1) {
                                    markdown_blockquote(&doc, doc.version, pos1, username, command);
                                }
                            } else if (strcmp(cmd_type, "ORDERED_LIST") == 0) {
                                // ORDERED_LIST <pos>
                                if (sscanf(cmd_part, "%zu", &pos1) >= 1) {
                                    markdown_ordered_list(&doc, doc.version, pos1, username, command);
                                }
                            } else if (strcmp(cmd_type, "UNORDERED_LIST") == 0) {
                                // UNORDERED_LIST <pos>
                                if (sscanf(cmd_part, "%zu", &pos1) >= 1) {
                                    markdown_unordered_list(&doc, doc.version, pos1, username, command);
                                }
                            } else if (strcmp(cmd_type, "CODE") == 0) {
                                // CODE <pos_start> <pos_end>
                                if (sscanf(cmd_part, "%zu %zu", &pos1, &pos2) >= 2) {
                                    markdown_code(&doc, doc.version, pos1, pos2, username, command);
                                }
                            } else if (strcmp(cmd_type, "HORIZONTAL_RULE") == 0) {
                                // HORIZONTAL_RULE <pos>
                                if (sscanf(cmd_part, "%zu", &pos1) >= 1) {
                                    markdown_horizontal_rule(&doc, doc.version, pos1, username, command);
                                }
                            } else if (strcmp(cmd_type, "LINK") == 0) {
                                // LINK <pos_start> <pos_end> <link>
                                if (sscanf(cmd_part, "%zu %zu %[^\n]", &pos1, &pos2, content) >= 3) {
                                    markdown_link(&doc, doc.version, pos1, pos2, content, username, command);
                                }
                            } else if (strcmp(cmd_type, "NEWLINE") == 0) {
                                // NEWLINE <pos>
                                if (sscanf(cmd_part, "%zu", &pos1) >= 1) {
                                    markdown_newline(&doc, doc.version, pos1, username, command);
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (strncmp(update, "END", 3) == 0) {
        // 更新结束
    }
}

/**
 * 清理资源
 */
void cleanup_resources() {
    if (c2s_fd != -1) {
        close(c2s_fd);
        c2s_fd = -1;
    }

    if (s2c_fd != -1) {
        close(s2c_fd);
        s2c_fd = -1;
    }

    // 释放文档资源
    markdown_free(&doc);

    // 释放日志资源
    if (log.log_entries) {
        for (size_t i = 0; i < log.count; i++) {
            free(log.log_entries[i]);
        }
        free(log.log_entries);
        log.log_entries = NULL;
        log.count = 0;
        log.capacity = 0;
    }

    pthread_mutex_destroy(&doc_mutex);
}

/**
 * 打印文档内容
 */
void print_document() {
    markdown_print(&doc, stdout);
    printf("\n");
}

/**
 * 添加日志条目
 */
void add_log_entry(const char *entry) {
    if (!entry) return;

    // 初始化日志数组
    if (!log.log_entries) {
        log.capacity = 10;
        log.log_entries = (char **)malloc(log.capacity * sizeof(char *));
        if (!log.log_entries) return;
    }

    // 扩展数组容量
    if (log.count >= log.capacity) {
        log.capacity *= 2;
        char **new_entries = (char **)realloc(log.log_entries, log.capacity * sizeof(char *));
        if (!new_entries) return;
        log.log_entries = new_entries;
    }

    // 添加新条目
    log.log_entries[log.count] = strdup(entry);
    if (log.log_entries[log.count]) {
        log.count++;
    }
}

/**
 * 打印命令日志
 */
void print_command_log() {
    for (size_t i = 0; i < log.count; i++) {
        printf("%s\n", log.log_entries[i]);
    }
}
