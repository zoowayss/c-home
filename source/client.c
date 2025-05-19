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

#define MAX_COMMAND_LEN 256
#define MAX_DOCUMENT_SIZE 1048576 // 1MB

// 全局变量
static pid_t server_pid;
static pid_t client_pid;
static char username[64];
static int c2s_fd = -1; // 客户端到服务器的管道
static int s2c_fd = -1; // 服务器到客户端的管道
static char *document = NULL; // 本地文档副本
static size_t document_size = 0;
static uint64_t document_version = 0;
static int is_write_permission = 0;
static int client_running = 1;
static pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;

// 函数声明
void handle_signal(int sig);
void *update_thread(void *arg);
void process_server_update(const char *update);
void cleanup_resources();
void print_document();

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
        perror("kill");
        return 1;
    }

    // 等待服务器响应
    int sig;
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGRTMIN + 1);

    if (sigwait(&wait_mask, &sig) != 0) {
        perror("sigwait");
        return 1;
    }

    // 打开命名管道
    char c2s_path[64], s2c_path[64];
    snprintf(c2s_path, sizeof(c2s_path), "FIFO_C2S_%d", client_pid);
    snprintf(s2c_path, sizeof(s2c_path), "FIFO_S2C_%d", client_pid);

    c2s_fd = open(c2s_path, O_WRONLY);
    s2c_fd = open(s2c_path, O_RDONLY);

    if (c2s_fd == -1 || s2c_fd == -1) {
        perror("open");
        cleanup_resources();
        return 1;
    }

    // 发送用户名
    write(c2s_fd, username, strlen(username));
    write(c2s_fd, "\n", 1);

    // 读取服务器响应（角色和文档内容）
    char response[1024];
    ssize_t bytes_read = read(s2c_fd, response, sizeof(response) - 1);

    if (bytes_read <= 0) {
        perror("read");
        cleanup_resources();
        return 1;
    }

    response[bytes_read] = '\0';

    // 检查是否被拒绝
    if (strncmp(response, "Reject", 6) == 0) {
        printf("%s\n", response);
        cleanup_resources();
        return 1;
    }

    // 解析角色
    if (strncmp(response, "write", 5) == 0) {
        is_write_permission = 1;
        printf("Connected with write permission.\n");
    } else {
        printf("Connected with read-only permission.\n");
    }

    // 读取文档版本号
    char version_str[32];
    bytes_read = read(s2c_fd, version_str, sizeof(version_str) - 1);
    if (bytes_read <= 0) {
        perror("read version");
        cleanup_resources();
        return 1;
    }
    version_str[bytes_read] = '\0';
    document_version = strtoull(version_str, NULL, 10);

    // 读取文档长度
    char length_str[32];
    bytes_read = read(s2c_fd, length_str, sizeof(length_str) - 1);
    if (bytes_read <= 0) {
        perror("read length");
        cleanup_resources();
        return 1;
    }
    length_str[bytes_read] = '\0';
    size_t doc_length = strtoull(length_str, NULL, 10);

    // 分配内存并读取文档内容
    document = (char *)malloc(doc_length + 1);
    if (!document) {
        perror("malloc");
        cleanup_resources();
        return 1;
    }

    document_size = doc_length;

    if (doc_length > 0) {
        bytes_read = read(s2c_fd, document, doc_length);
        if (bytes_read != (ssize_t)doc_length) {
            perror("read document");
            cleanup_resources();
            return 1;
        }
    }

    document[doc_length] = '\0';

    // 创建更新线程
    pthread_t update_tid;
    if (pthread_create(&update_tid, NULL, update_thread, NULL) != 0) {
        perror("pthread_create");
        cleanup_resources();
        return 1;
    }

    // 主循环，处理用户输入
    char command[MAX_COMMAND_LEN];
    printf("Enter commands (type 'DISCONNECT' to exit):\n");

    while (client_running && fgets(command, MAX_COMMAND_LEN, stdin)) {
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

        // 处理本地命令
        if (strcmp(command, "DOC?") == 0) {
            pthread_mutex_lock(&doc_mutex);
            print_document();
            pthread_mutex_unlock(&doc_mutex);
            continue;
        } else if (strcmp(command, "PERM?") == 0) {
            printf("%s\n", is_write_permission ? "write" : "read");
            continue;
        } else if (strcmp(command, "DISCONNECT") == 0) {
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

        // 发送命令给服务器
        write(c2s_fd, command, strlen(command));
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
    if (sig == SIGRTMIN + 1) {
        // 服务器已创建管道，可以继续连接过程
    }
}

/**
 * 更新线程，接收服务器更新
 */
void *update_thread(void *arg) {
    (void)arg; // 未使用的参数

    char buffer[MAX_DOCUMENT_SIZE];
    ssize_t bytes_read;

    while (client_running) {
        // 读取服务器更新
        bytes_read = read(s2c_fd, buffer, sizeof(buffer) - 1);

        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                // 连接关闭
                client_running = 0;
                break;
            } else if (errno == EINTR) {
                // 被信号中断，继续
                continue;
            } else {
                // 其他错误
                perror("read");
                client_running = 0;
                break;
            }
        }

        buffer[bytes_read] = '\0';

        // 处理更新
        pthread_mutex_lock(&doc_mutex);
        process_server_update(buffer);
        pthread_mutex_unlock(&doc_mutex);
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

        if (new_version > document_version) {
            document_version = new_version;
            // TODO: 处理文档更新
        }
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

    if (document) {
        free(document);
        document = NULL;
    }

    pthread_mutex_destroy(&doc_mutex);
}

/**
 * 打印文档内容
 */
void print_document() {
    if (document) {
        printf("%s\n", document);
    } else {
        printf("(empty document)\n");
    }
}
