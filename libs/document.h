#ifndef DOCUMENT_H
#define DOCUMENT_H
/**
 * This file is the header file for all the document functions. You will be tested on the functions inside markdown.h
 * You are allowed to and encouraged multiple helper functions and data structures, and make your code as modular as possible.
 * Ensure you DO NOT change the name of document struct.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

// 定义返回码
#define SUCCESS 0
#define INVALID_CURSOR_POS -1
#define DELETED_POSITION -2
#define OUTDATED_VERSION -3

// 定义文档块类型
typedef enum {
    NORMAL,     // 普通文本
    DELETED,    // 已删除的文本
    INSERTED    // 插入的文本
} chunk_type;

// 文档块结构
typedef struct chunk {
    chunk_type type;           // 块类型
    char *content;             // 块内容
    size_t length;             // 块长度
    uint64_t version;          // 块创建的版本
    struct chunk *next;        // 下一个块
    struct chunk *prev;        // 上一个块
} chunk;

// 文档结构
typedef struct {
    chunk *head;               // 文档头块
    chunk *tail;               // 文档尾块
    uint64_t current_version;  // 当前版本
    size_t total_length;       // 文档总长度
} document;

// 辅助函数声明
chunk *create_chunk(chunk_type type, const char *content, size_t length, uint64_t version);
void free_chunk(chunk *c);
size_t get_document_length(const document *doc);
char *get_document_content(const document *doc);
int validate_cursor_position(const document *doc, uint64_t version, size_t pos);
int validate_cursor_range(const document *doc, uint64_t version, size_t start, size_t end);
chunk *find_chunk_at_position(const document *doc, size_t pos, size_t *offset);
void split_chunk_at_position(document *doc, size_t pos);
void merge_adjacent_chunks(document *doc);

#endif // DOCUMENT_H
