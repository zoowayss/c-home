#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libs/markdown.h"
#include "../libs/document.h"

/**
 * 初始化文档
 */
void markdown_init(document *doc) {
    if (!doc) {
        return;
    }
    
    doc->head = NULL;
    doc->tail = NULL;
    doc->current_version = 0;
    doc->total_length = 0;
}

/**
 * 释放文档
 */
void markdown_free(document *doc) {
    if (!doc) {
        return;
    }
    
    chunk *current = doc->head;
    while (current) {
        chunk *next = current->next;
        free_chunk(current);
        current = next;
    }
    
    doc->head = NULL;
    doc->tail = NULL;
    doc->total_length = 0;
}

/**
 * 在文档中插入内容
 */
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content) {
    if (!doc || !content) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    size_t content_len = strlen(content);
    if (content_len == 0) {
        return SUCCESS;  // 没有内容需要插入
    }
    
    // 如果文档为空，直接创建一个新块
    if (!doc->head) {
        chunk *new_chunk = create_chunk(NORMAL, content, content_len, version);
        if (!new_chunk) {
            return INVALID_CURSOR_POS;
        }
        
        doc->head = new_chunk;
        doc->tail = new_chunk;
        doc->total_length = content_len;
        return SUCCESS;
    }
    
    // 在指定位置分割块
    split_chunk_at_position(doc, pos);
    
    // 创建新的插入块
    chunk *new_chunk = create_chunk(INSERTED, content, content_len, version);
    if (!new_chunk) {
        return INVALID_CURSOR_POS;
    }
    
    // 找到插入位置
    size_t offset;
    chunk *target = find_chunk_at_position(doc, pos, &offset);
    
    if (!target) {
        // 插入到文档末尾
        if (doc->tail) {
            new_chunk->prev = doc->tail;
            doc->tail->next = new_chunk;
            doc->tail = new_chunk;
        } else {
            doc->head = new_chunk;
            doc->tail = new_chunk;
        }
    } else if (offset == 0) {
        // 插入到块的开头
        new_chunk->next = target;
        new_chunk->prev = target->prev;
        
        if (target->prev) {
            target->prev->next = new_chunk;
        } else {
            doc->head = new_chunk;
        }
        
        target->prev = new_chunk;
    } else if (offset == target->length) {
        // 插入到块的末尾
        new_chunk->next = target->next;
        new_chunk->prev = target;
        
        if (target->next) {
            target->next->prev = new_chunk;
        } else {
            doc->tail = new_chunk;
        }
        
        target->next = new_chunk;
    }
    
    // 更新文档长度
    doc->total_length += content_len;
    
    return SUCCESS;
}

/**
 * 在文档中删除内容
 */
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    if (len == 0 || doc->total_length == 0) {
        return SUCCESS;  // 没有内容需要删除
    }
    
    // 确保不会删除超出文档范围的内容
    if (pos + len > doc->total_length) {
        len = doc->total_length - pos;
    }
    
    // 在删除范围的起点和终点分割块
    split_chunk_at_position(doc, pos);
    split_chunk_at_position(doc, pos + len);
    
    // 标记删除范围内的块
    size_t remaining = len;
    size_t current_pos = 0;
    chunk *current = doc->head;
    
    while (current && remaining > 0) {
        if (current->type != DELETED) {
            if (current_pos + current->length > pos && current_pos < pos + len) {
                // 计算重叠部分
                size_t start = (current_pos < pos) ? pos - current_pos : 0;
                size_t end = (current_pos + current->length > pos + len) ? 
                            pos + len - current_pos : current->length;
                size_t overlap = end - start;
                
                if (start == 0 && end == current->length) {
                    // 整个块都在删除范围内
                    current->type = DELETED;
                    remaining -= current->length;
                } else {
                    // 只有部分块在删除范围内，需要分割
                    if (start > 0) {
                        split_chunk_at_position(doc, current_pos + start);
                        current = current->next;
                    }
                    
                    if (end < current->length) {
                        split_chunk_at_position(doc, current_pos + end);
                    }
                    
                    current->type = DELETED;
                    remaining -= overlap;
                }
            }
            current_pos += current->length;
        }
        current = current->next;
    }
    
    // 更新文档长度
    doc->total_length -= len;
    
    return SUCCESS;
}

/**
 * 添加标题格式
 */
int markdown_heading(document *doc, uint64_t version, int level, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 验证标题级别
    if (level < 1 || level > 3) {
        return INVALID_CURSOR_POS;
    }
    
    // 确保在行首插入标题
    char prefix[5] = {0};
    int i;
    for (i = 0; i < level; i++) {
        prefix[i] = '#';
    }
    prefix[i] = ' ';  // 标题后必须有空格
    
    // 如果不在行首，先插入换行
    if (pos > 0) {
        size_t offset;
        chunk *target = find_chunk_at_position(doc, pos - 1, &offset);
        if (target && target->content && offset < target->length && target->content[offset] != '\n') {
            markdown_insert(doc, version, pos, "\n");
            pos += 1;  // 调整插入位置
        }
    }
    
    // 插入标题标记
    return markdown_insert(doc, version, pos, prefix);
}

/**
 * 添加粗体格式
 */
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标范围
    int validation = validate_cursor_range(doc, version, start, end);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 在文本前后添加 **
    markdown_insert(doc, version, end, "**");
    markdown_insert(doc, version, start, "**");
    
    return SUCCESS;
}

/**
 * 添加斜体格式
 */
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标范围
    int validation = validate_cursor_range(doc, version, start, end);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 在文本前后添加 *
    markdown_insert(doc, version, end, "*");
    markdown_insert(doc, version, start, "*");
    
    return SUCCESS;
}

/**
 * 添加引用格式
 */
int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 确保在行首插入引用
    // 如果不在行首，先插入换行
    if (pos > 0) {
        size_t offset;
        chunk *target = find_chunk_at_position(doc, pos - 1, &offset);
        if (target && target->content && offset < target->length && target->content[offset] != '\n') {
            markdown_insert(doc, version, pos, "\n");
            pos += 1;  // 调整插入位置
        }
    }
    
    // 插入引用标记
    return markdown_insert(doc, version, pos, "> ");
}

/**
 * 添加有序列表格式
 */
int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 确保在行首插入列表
    // 如果不在行首，先插入换行
    if (pos > 0) {
        size_t offset;
        chunk *target = find_chunk_at_position(doc, pos - 1, &offset);
        if (target && target->content && offset < target->length && target->content[offset] != '\n') {
            markdown_insert(doc, version, pos, "\n");
            pos += 1;  // 调整插入位置
        }
    }
    
    // 查找前一个列表项，确定序号
    int number = 1;
    char *content = get_document_content(doc);
    if (content) {
        // 从插入位置向前查找最近的有序列表项
        size_t i = (pos > 0) ? pos - 1 : 0;
        while (i > 0) {
            if (content[i] == '\n') {
                // 检查下一行是否是有序列表
                if (i + 1 < pos && content[i + 1] >= '1' && content[i + 1] <= '9' && 
                    i + 2 < pos && content[i + 2] == '.') {
                    number = content[i + 1] - '0' + 1;
                    if (number > 9) number = 1;  // 限制在1-9范围内
                    break;
                }
            }
            i--;
        }
        free(content);
    }
    
    // 插入有序列表标记
    char list_mark[5];
    snprintf(list_mark, sizeof(list_mark), "%d. ", number);
    return markdown_insert(doc, version, pos, list_mark);
}

/**
 * 添加无序列表格式
 */
int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 确保在行首插入列表
    // 如果不在行首，先插入换行
    if (pos > 0) {
        size_t offset;
        chunk *target = find_chunk_at_position(doc, pos - 1, &offset);
        if (target && target->content && offset < target->length && target->content[offset] != '\n') {
            markdown_insert(doc, version, pos, "\n");
            pos += 1;  // 调整插入位置
        }
    }
    
    // 插入无序列表标记
    return markdown_insert(doc, version, pos, "- ");
}

/**
 * 添加代码格式
 */
int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标范围
    int validation = validate_cursor_range(doc, version, start, end);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 在文本前后添加 `
    markdown_insert(doc, version, end, "`");
    markdown_insert(doc, version, start, "`");
    
    return SUCCESS;
}

/**
 * 添加水平线
 */
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标位置
    int validation = validate_cursor_position(doc, version, pos);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 确保在行首插入水平线
    // 如果不在行首，先插入换行
    if (pos > 0) {
        size_t offset;
        chunk *target = find_chunk_at_position(doc, pos - 1, &offset);
        if (target && target->content && offset < target->length && target->content[offset] != '\n') {
            markdown_insert(doc, version, pos, "\n");
            pos += 1;  // 调整插入位置
        }
    }
    
    // 插入水平线标记，确保后面有换行
    int result = markdown_insert(doc, version, pos, "---");
    if (result == SUCCESS) {
        // 检查水平线后是否有换行
        size_t new_pos = pos + 3;  // "---" 的长度
        if (new_pos < doc->total_length) {
            size_t offset;
            chunk *target = find_chunk_at_position(doc, new_pos, &offset);
            if (target && target->content && offset < target->length && target->content[offset] != '\n') {
                markdown_insert(doc, version, new_pos, "\n");
            }
        } else {
            // 如果水平线在文档末尾，添加换行
            markdown_insert(doc, version, new_pos, "\n");
        }
    }
    
    return result;
}

/**
 * 添加链接格式
 */
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url) {
    if (!doc || !url) {
        return INVALID_CURSOR_POS;
    }
    
    // 验证光标范围
    int validation = validate_cursor_range(doc, version, start, end);
    if (validation != SUCCESS) {
        return validation;
    }
    
    // 构建链接格式：[文本](URL)
    char *link_suffix = (char *)malloc(strlen(url) + 4);  // +4 for "]()" and null terminator
    if (!link_suffix) {
        return INVALID_CURSOR_POS;
    }
    
    sprintf(link_suffix, "](%s)", url);
    
    // 在文本前后添加链接标记
    markdown_insert(doc, version, end, link_suffix);
    markdown_insert(doc, version, start, "[");
    
    free(link_suffix);
    return SUCCESS;
}

/**
 * 打印文档内容
 */
void markdown_print(const document *doc, FILE *stream) {
    if (!doc || !stream) {
        return;
    }
    
    char *content = markdown_flatten(doc);
    if (content) {
        fprintf(stream, "%s", content);
        free(content);
    }
}

/**
 * 扁平化文档内容
 */
char *markdown_flatten(const document *doc) {
    return get_document_content(doc);
}

/**
 * 增加文档版本
 */
void markdown_increment_version(document *doc) {
    if (!doc) {
        return;
    }
    
    doc->current_version++;
    
    // 将所有 INSERTED 类型的块转换为 NORMAL 类型
    chunk *current = doc->head;
    while (current) {
        if (current->type == INSERTED) {
            current->type = NORMAL;
        }
        current = current->next;
    }
    
    // 移除所有 DELETED 类型的块
    current = doc->head;
    while (current) {
        chunk *next = current->next;
        
        if (current->type == DELETED) {
            if (current->prev) {
                current->prev->next = current->next;
            } else {
                doc->head = current->next;
            }
            
            if (current->next) {
                current->next->prev = current->prev;
            } else {
                doc->tail = current->prev;
            }
            
            free_chunk(current);
        }
        
        current = next;
    }
    
    // 合并相邻的相同类型的块
    merge_adjacent_chunks(doc);
}
