#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libs/document.h"

/**
 * 创建一个新的文档块
 */
chunk *create_chunk(chunk_type type, const char *content, size_t length, uint64_t version) {
    chunk *c = (chunk *)malloc(sizeof(chunk));
    if (!c) {
        return NULL;
    }
    
    c->type = type;
    c->length = length;
    c->version = version;
    c->next = NULL;
    c->prev = NULL;
    
    if (content && length > 0) {
        c->content = (char *)malloc(length + 1);
        if (!c->content) {
            free(c);
            return NULL;
        }
        memcpy(c->content, content, length);
        c->content[length] = '\0';
    } else {
        c->content = NULL;
        c->length = 0;
    }
    
    return c;
}

/**
 * 释放一个文档块
 */
void free_chunk(chunk *c) {
    if (c) {
        if (c->content) {
            free(c->content);
        }
        free(c);
    }
}

/**
 * 获取文档的总长度
 */
size_t get_document_length(const document *doc) {
    if (!doc) {
        return 0;
    }
    return doc->total_length;
}

/**
 * 获取文档的内容（扁平化）
 */
char *get_document_content(const document *doc) {
    if (!doc || !doc->head || doc->total_length == 0) {
        char *empty = (char *)malloc(1);
        if (empty) {
            empty[0] = '\0';
        }
        return empty;
    }
    
    char *content = (char *)malloc(doc->total_length + 1);
    if (!content) {
        return NULL;
    }
    
    size_t offset = 0;
    chunk *current = doc->head;
    
    while (current) {
        if (current->type != DELETED && current->content) {
            memcpy(content + offset, current->content, current->length);
            offset += current->length;
        }
        current = current->next;
    }
    
    content[offset] = '\0';
    return content;
}

/**
 * 验证光标位置是否有效
 */
int validate_cursor_position(const document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 检查版本
    if (version < doc->current_version) {
        return OUTDATED_VERSION;
    }
    
    // 检查位置是否超出文档范围
    if (pos > doc->total_length) {
        return INVALID_CURSOR_POS;
    }
    
    return SUCCESS;
}

/**
 * 验证光标范围是否有效
 */
int validate_cursor_range(const document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 检查版本
    if (version < doc->current_version) {
        return OUTDATED_VERSION;
    }
    
    // 检查位置是否超出文档范围
    if (start > doc->total_length || end > doc->total_length) {
        return INVALID_CURSOR_POS;
    }
    
    // 检查范围是否有效
    if (end <= start) {
        return INVALID_CURSOR_POS;
    }
    
    return SUCCESS;
}

/**
 * 在指定位置查找文档块
 */
chunk *find_chunk_at_position(const document *doc, size_t pos, size_t *offset) {
    if (!doc || !doc->head) {
        return NULL;
    }
    
    size_t current_pos = 0;
    chunk *current = doc->head;
    
    while (current) {
        if (current->type != DELETED) {
            if (current_pos + current->length > pos) {
                // 找到了包含位置的块
                if (offset) {
                    *offset = pos - current_pos;
                }
                return current;
            }
            current_pos += current->length;
        }
        current = current->next;
    }
    
    // 如果位置在文档末尾
    if (pos == current_pos && doc->tail) {
        if (offset) {
            *offset = doc->tail->length;
        }
        return doc->tail;
    }
    
    return NULL;
}

/**
 * 在指定位置分割文档块
 */
void split_chunk_at_position(document *doc, size_t pos) {
    if (!doc) {
        return;
    }
    
    size_t offset;
    chunk *target = find_chunk_at_position(doc, pos, &offset);
    
    if (!target || target->type == DELETED || offset == 0 || offset == target->length) {
        // 不需要分割
        return;
    }
    
    // 创建新块
    chunk *new_chunk = create_chunk(target->type, 
                                    target->content + offset, 
                                    target->length - offset, 
                                    target->version);
    if (!new_chunk) {
        return;
    }
    
    // 调整原块大小
    char *new_content = (char *)realloc(target->content, offset + 1);
    if (!new_content) {
        free_chunk(new_chunk);
        return;
    }
    
    target->content = new_content;
    target->content[offset] = '\0';
    target->length = offset;
    
    // 插入新块
    new_chunk->next = target->next;
    new_chunk->prev = target;
    
    if (target->next) {
        target->next->prev = new_chunk;
    } else {
        doc->tail = new_chunk;
    }
    
    target->next = new_chunk;
}

/**
 * 合并相邻的相同类型的块
 */
void merge_adjacent_chunks(document *doc) {
    if (!doc || !doc->head) {
        return;
    }
    
    chunk *current = doc->head;
    
    while (current && current->next) {
        chunk *next = current->next;
        
        // 如果两个块类型相同且版本相同，则合并
        if (current->type == next->type && current->version == next->version) {
            size_t new_length = current->length + next->length;
            char *new_content = (char *)realloc(current->content, new_length + 1);
            
            if (!new_content) {
                current = next;
                continue;
            }
            
            current->content = new_content;
            memcpy(current->content + current->length, next->content, next->length);
            current->content[new_length] = '\0';
            current->length = new_length;
            
            // 移除下一个块
            current->next = next->next;
            if (next->next) {
                next->next->prev = current;
            } else {
                doc->tail = current;
            }
            
            free_chunk(next);
        } else {
            current = next;
        }
    }
}
