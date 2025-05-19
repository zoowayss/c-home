#ifndef DOCUMENT_H
#define DOCUMENT_H
/**
 * This file is the header file for all the document functions. You will be tested on the functions inside markdown.h
 * You are allowed to and encouraged multiple helper functions and data structures, and make your code as modular as possible.
 * Ensure you DO NOT change the name of document struct.
 */
#include <stddef.h>
#include <stdint.h>

// 定义返回码
#define SUCCESS 0
#define INVALID_CURSOR_POS -1
#define DELETED_POSITION -2
#define OUTDATED_VERSION -3

// 定义命令类型
typedef enum {
    CMD_INSERT,
    CMD_DELETE,
    CMD_HEADING,
    CMD_BOLD,
    CMD_ITALIC,
    CMD_BLOCKQUOTE,
    CMD_ORDERED_LIST,
    CMD_UNORDERED_LIST,
    CMD_CODE,
    CMD_HORIZONTAL_RULE,
    CMD_LINK,
    CMD_NEWLINE
} command_type;

// 定义编辑命令结构
typedef struct edit_command {
    command_type type;
    uint64_t version;
    size_t pos1;
    size_t pos2;
    char *content;
    int level;
    struct edit_command *next;
} edit_command;

// 定义文档块结构（链表节点）
typedef struct chunk {
    char *content;
    size_t length;
    struct chunk *next;
} chunk;

// 定义文档结构
typedef struct {
    chunk *head;           // 文档内容链表头
    size_t total_length;   // 文档总长度
    uint64_t version;      // 当前版本号
    edit_command *pending_edits; // 待处理的编辑命令
    edit_command *edit_history;  // 编辑历史
} document;

// 辅助函数声明
chunk *create_chunk(const char *content, size_t length);
void free_chunk(chunk *c);
edit_command *create_command(command_type type, uint64_t version, size_t pos1, size_t pos2, const char *content, int level);
void free_command(edit_command *cmd);
void add_pending_edit(document *doc, edit_command *cmd);
void add_edit_history(document *doc, edit_command *cmd);

#endif // DOCUMENT_H
