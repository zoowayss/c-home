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
#include "../libs/markdown.h"
#include "../libs/document.h"

#define MAX_COMMAND_LEN 256
#define MAX_USERNAME_LEN 64

// 全局变量
document doc;
pid_t server_pid;
char username[MAX_USERNAME_LEN];
int c2s_fd = -1;  // 客户端到服务器的管道
int s2c_fd = -1;  // 服务器到客户端的管道
pthread_t update_thread;
int client_running = 1;
pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;

// 函数声明
void handle_signal(int sig, siginfo_t *info, void *ucontext);
void *update_handler(void *arg);
void cleanup();
void process_server_update(const char *update);

/**
 * 主函数
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_pid> <username>\n", argv[0]);
        return 1;
    }

    // 解析服务器PID和用户名
    server_pid = atoi(argv[1]);
    if (server_pid <= 0) {
        fprintf(stderr, "Invalid server PID\n");
        return 1;
    }

    strncpy(username, argv[2], MAX_USERNAME_LEN - 1);
    username[MAX_USERNAME_LEN - 1] = '\0';

    // 初始化文档
    markdown_init(&doc);

    // 设置信号处理
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handle_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN + 1, &sa, NULL);

    // 阻塞 SIGRTMIN+1 信号，直到准备好接收
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN + 1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    // 向服务器发送连接请求
    kill(server_pid, SIGRTMIN);

    // 等待服务器响应
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGRTMIN + 1);

    int sig;
    sigwait(&wait_mask, &sig);

    // 打开命名管道
    char c2s_path[64], s2c_path[64];
    snprintf(c2s_path, sizeof(c2s_path), "FIFO_C2S_%d", getpid());
    snprintf(s2c_path, sizeof(s2c_path), "FIFO_S2C_%d", getpid());

    c2s_fd = open(c2s_path, O_WRONLY);
    s2c_fd = open(s2c_path, O_RDONLY);

    if (c2s_fd == -1 || s2c_fd == -1) {
        perror("open");
        cleanup();
        return 1;
    }

    // 发送用户名
    write(c2s_fd, username, strlen(username));
    write(c2s_fd, "\n", 1);

    // 读取服务器响应
    char response[MAX_COMMAND_LEN];
    ssize_t bytes_read = read(s2c_fd, response, MAX_COMMAND_LEN - 1);
    if (bytes_read <= 0) {
        perror("read");
        cleanup();
        return 1;
    }

    response[bytes_read] = '\0';

    // 检查是否被拒绝
    if (strncmp(response, "Reject", 6) == 0) {
        printf("Connection rejected: %s\n", response);
        cleanup();
        return 1;
    }

    // 解析角色
    char role[10];
    sscanf(response, "%9s", role);
    printf("Connected as %s with role: %s\n", username, role);

    // 读取文档版本
    bytes_read = read(s2c_fd, response, MAX_COMMAND_LEN - 1);
    if (bytes_read <= 0) {
        perror("read version");
        cleanup();
        return 1;
    }
    response[bytes_read] = '\0';
    uint64_t version;
    sscanf(response, "%lu", &version);
    doc.current_version = version;

    // 读取文档长度
    bytes_read = read(s2c_fd, response, MAX_COMMAND_LEN - 1);
    if (bytes_read <= 0) {
        perror("read length");
        cleanup();
        return 1;
    }
    response[bytes_read] = '\0';
    size_t doc_length;
    sscanf(response, "%zu", &doc_length);

    // 读取文档内容
    if (doc_length > 0) {
        char *content = (char *)malloc(doc_length + 1);
        if (!content) {
            perror("malloc");
            cleanup();
            return 1;
        }

        size_t total_read = 0;
        while (total_read < doc_length) {
            bytes_read = read(s2c_fd, content + total_read, doc_length - total_read);
            if (bytes_read <= 0) {
                perror("read content");
                free(content);
                cleanup();
                return 1;
            }
            total_read += bytes_read;
        }

        content[doc_length] = '\0';

        // 将内容添加到文档
        pthread_mutex_lock(&doc_mutex);
        markdown_insert(&doc, doc.current_version, 0, content);
        pthread_mutex_unlock(&doc_mutex);

        free(content);
    }

    // 创建更新线程
    pthread_create(&update_thread, NULL, update_handler, NULL);

    // 主循环，处理用户命令
    char command[MAX_COMMAND_LEN];
    while (client_running) {
        if (fgets(command, MAX_COMMAND_LEN, stdin)) {
            // 移除换行符
            size_t len = strlen(command);
            if (len > 0 && command[len - 1] == '\n') {
                command[len - 1] = '\0';
                len--;
            }

            // 检查命令是否有效
            if (len == 0) {
                continue;
            }

            // 处理本地命令
            if (strcmp(command, "DOC?") == 0) {
                // 打印文档内容
                pthread_mutex_lock(&doc_mutex);
                printf("\n");
                markdown_print(&doc, stdout);
                pthread_mutex_unlock(&doc_mutex);
            } else {
                // 发送命令到服务器
                write(c2s_fd, command, strlen(command));
                write(c2s_fd, "\n", 1);

                // 如果是断开连接命令，退出循环
                if (strcmp(command, "DISCONNECT") == 0) {
                    client_running = 0;
                }
            }
        }
    }

    // 等待更新线程结束
    pthread_cancel(update_thread);
    pthread_join(update_thread, NULL);

    // 清理资源
    cleanup();

    return 0;
}

/**
 * 信号处理函数
 */
void handle_signal(int sig, siginfo_t *info, void *ucontext) {
    // 不需要在这里做任何事情，sigwait 会处理信号
}

/**
 * 更新处理线程
 */
void *update_handler(void *arg) {
    char buffer[4096];
    ssize_t bytes_read;

    while (client_running) {
        bytes_read = read(s2c_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞模式下没有数据可读
                continue;
            } else {
                // 读取错误或连接关闭
                client_running = 0;
                break;
            }
        }

        buffer[bytes_read] = '\0';

        // 处理服务器更新
        process_server_update(buffer);
    }

    return NULL;
}

/**
 * 处理服务器更新
 */
void process_server_update(const char *update) {
    if (!update) {
        return;
    }

    // 解析更新消息
    char *update_copy = strdup(update);
    if (!update_copy) {
        return;
    }

    char *line = strtok(update_copy, "\n");
    if (!line) {
        free(update_copy);
        return;
    }

    // 检查是否是版本更新
    if (strncmp(line, "VERSION", 7) == 0) {
        uint64_t version;
        sscanf(line, "VERSION %lu", &version);

        // 更新本地文档版本
        pthread_mutex_lock(&doc_mutex);
        if (version > doc.current_version) {
            markdown_increment_version(&doc);
        }
        pthread_mutex_unlock(&doc_mutex);

        // 处理编辑命令
        while ((line = strtok(NULL, "\n")) != NULL) {
            if (strcmp(line, "END") == 0) {
                break;
            }

            // 打印编辑结果
            printf("%s\n", line);

            // 如果需要，可以在这里解析和应用编辑命令
        }
    }

    free(update_copy);
}

/**
 * 清理资源
 */
void cleanup() {
    // 关闭管道
    if (c2s_fd != -1) {
        close(c2s_fd);
    }

    if (s2c_fd != -1) {
        close(s2c_fd);
    }

    // 释放文档
    markdown_free(&doc);

    // 销毁互斥锁
    pthread_mutex_destroy(&doc_mutex);
}
