#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../libs/markdown.h"
#include "../libs/document.h"

/**
 * 初始化文档
 * @param doc 文档指针
 */
void markdown_init(document *doc) {
    if (!doc) {
        return;
    }

    doc->head = NULL;
    doc->total_length = 0;
    doc->version = 0;
    doc->pending_edits = NULL;
    doc->edit_history = NULL;
}

/**
 * 释放文档及其所有资源
 * @param doc 文档指针
 */
void markdown_free(document *doc) {
    if (!doc) {
        return;
    }

    // 释放文档块链表
    chunk *current = doc->head;
    chunk *next;

    while (current) {
        next = current->next;
        free_chunk(current);
        current = next;
    }

    // 释放待处理的编辑命令
    edit_command *cmd = doc->pending_edits;
    edit_command *next_cmd;

    while (cmd) {
        next_cmd = cmd->next;
        free_command(cmd);
        cmd = next_cmd;
    }

    // 释放编辑历史
    cmd = doc->edit_history;

    while (cmd) {
        next_cmd = cmd->next;
        free_command(cmd);
        cmd = next_cmd;
    }

    // 重置文档状态
    doc->head = NULL;
    doc->total_length = 0;
    doc->pending_edits = NULL;
    doc->edit_history = NULL;
}

/**
 * 检查位置是否有效
 * @param doc 文档指针
 * @param pos 位置
 * @return 1 如果有效，0 如果无效
 */
static int is_valid_position(const document *doc, size_t pos) {
    return pos <= doc->total_length;
}

/**
 * 检查位置范围是否有效
 * @param doc 文档指针
 * @param start 起始位置
 * @param end 结束位置
 * @return 1 如果有效，0 如果无效
 */
static int is_valid_range(const document *doc, size_t start, size_t end) {
    // 允许 start 等于 doc->total_length（空选择在文档末尾）
    // 但通常 start 应该小于 end，且 end 不能超过文档长度
    return start < end && start <= doc->total_length && end <= doc->total_length;
}

/**
 * 检查版本是否有效
 * @param doc 文档指针
 * @param version 版本号
 * @return 1 如果有效，0 如果无效
 */
static int is_valid_version(const document *doc, uint64_t version) {
    return version == doc->version;
}

/**
 * 在文档中查找指定位置
 * @param doc 文档指针
 * @param pos 位置
 * @param chunk_pos 返回找到的块
 * @param offset 返回在块中的偏移量
 * @return 1 如果找到，0 如果未找到
 */
static int find_position(const document *doc, size_t pos, chunk **chunk_pos, size_t *offset) {
    if (!doc || !chunk_pos || !offset || pos > doc->total_length) {
        return 0;
    }

    chunk *current = doc->head;
    size_t current_pos = 0;

    while (current) {
        if (current_pos + current->length > pos) {
            *chunk_pos = current;
            *offset = pos - current_pos;
            return 1;
        }

        current_pos += current->length;
        current = current->next;
    }

    // 如果位置正好是文档末尾
    if (pos == doc->total_length && doc->head) {
        chunk *last = doc->head;
        while (last->next) {
            last = last->next;
        }
        *chunk_pos = last;
        *offset = last->length;
        return 1;
    }

    return 0;
}

/**
 * 直接在指定位置插入内容，不创建编辑命令
 * @param doc 文档指针
 * @param pos 插入位置
 * @param content 要插入的内容
 * @param content_len 内容长度
 * @return 成功返回 SUCCESS，否则返回错误码
 */
static int direct_insert(document *doc, size_t pos, const char *content, size_t content_len) {
    if (!doc || !content || content_len == 0) {
        return INVALID_CURSOR_POS;
    }

    // 如果文档为空，直接创建一个新块
    if (!doc->head) {
        doc->head = create_chunk(content, content_len);
        if (!doc->head) {
            return INVALID_CURSOR_POS;
        }
        doc->total_length = content_len;
        return SUCCESS;
    }

    // 查找插入位置
    chunk *target_chunk;
    size_t offset;

    if (!find_position(doc, pos, &target_chunk, &offset)) {
        return INVALID_CURSOR_POS;
    }

    // 在块中间插入
    if (offset > 0 && offset < target_chunk->length) {
        // 分割当前块
        chunk *new_chunk1 = create_chunk(target_chunk->content, offset);
        chunk *new_chunk2 = create_chunk(content, content_len);
        chunk *new_chunk3 = create_chunk(target_chunk->content + offset, target_chunk->length - offset);

        if (!new_chunk1 || !new_chunk2 || !new_chunk3) {
            free_chunk(new_chunk1);
            free_chunk(new_chunk2);
            free_chunk(new_chunk3);
            return INVALID_CURSOR_POS;
        }

        new_chunk2->next = new_chunk3;
        new_chunk1->next = new_chunk2;

        // 替换原块
        if (doc->head == target_chunk) {
            doc->head = new_chunk1;
        } else {
            chunk *prev = doc->head;
            while (prev->next != target_chunk) {
                prev = prev->next;
            }
            prev->next = new_chunk1;
        }

        free_chunk(target_chunk);
    }
    // 在块开头插入
    else if (offset == 0) {
        chunk *new_chunk = create_chunk(content, content_len);
        if (!new_chunk) {
            return INVALID_CURSOR_POS;
        }

        new_chunk->next = target_chunk;

        if (doc->head == target_chunk) {
            doc->head = new_chunk;
        } else {
            chunk *prev = doc->head;
            while (prev->next != target_chunk) {
                prev = prev->next;
            }
            prev->next = new_chunk;
        }
    }
    // 在块末尾插入 (offset == target_chunk->length)
    else {
        chunk *new_chunk = create_chunk(content, content_len);
        if (!new_chunk) {
            return INVALID_CURSOR_POS;
        }

        new_chunk->next = target_chunk->next;
        target_chunk->next = new_chunk;
    }

    doc->total_length += content_len;
    return SUCCESS;
}

/**
 * 在文档中插入内容
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 插入位置
 * @param content 要插入的内容
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content, const char *username, const char *original_cmd) {
    if (!doc || !content) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    size_t content_len = strlen(content);
    if (content_len == 0) {
        return SUCCESS; // 空内容，无需插入
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_INSERT, version, pos, 0, content, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 如果文档为空，直接创建一个新块
    if (!doc->head) {
        doc->head = create_chunk(content, content_len);
        if (!doc->head) {
            return INVALID_CURSOR_POS; // 内存分配失败
        }
        doc->total_length = content_len;
        return SUCCESS;
    }

    // 查找插入位置
    chunk *target_chunk;
    size_t offset;

    if (!find_position(doc, pos, &target_chunk, &offset)) {
        return INVALID_CURSOR_POS;
    }

    // 在块中间插入
    if (offset > 0 && offset < target_chunk->length) {
        // 分割当前块
        chunk *new_chunk1 = create_chunk(target_chunk->content, offset);
        chunk *new_chunk2 = create_chunk(content, content_len);
        chunk *new_chunk3 = create_chunk(target_chunk->content + offset, target_chunk->length - offset);

        if (!new_chunk1 || !new_chunk2 || !new_chunk3) {
            free_chunk(new_chunk1);
            free_chunk(new_chunk2);
            free_chunk(new_chunk3);
            return INVALID_CURSOR_POS; // 内存分配失败
        }

        // 更新链表
        new_chunk2->next = new_chunk3;
        new_chunk1->next = new_chunk2;

        // 替换原块
        if (doc->head == target_chunk) {
            doc->head = new_chunk1;
        } else {
            chunk *prev = doc->head;
            while (prev->next != target_chunk) {
                prev = prev->next;
            }
            prev->next = new_chunk1;
        }

        free_chunk(target_chunk);
    }
    // 在块开头插入
    else if (offset == 0) {
        chunk *new_chunk = create_chunk(content, content_len);
        if (!new_chunk) {
            return INVALID_CURSOR_POS; // 内存分配失败
        }

        new_chunk->next = target_chunk;

        if (doc->head == target_chunk) {
            doc->head = new_chunk;
        } else {
            chunk *prev = doc->head;
            while (prev->next != target_chunk) {
                prev = prev->next;
            }
            prev->next = new_chunk;
        }
    }
    // 在块末尾插入
    else {
        chunk *new_chunk = create_chunk(content, content_len);
        if (!new_chunk) {
            return INVALID_CURSOR_POS; // 内存分配失败
        }

        new_chunk->next = target_chunk->next;
        target_chunk->next = new_chunk;
    }

    doc->total_length += content_len;
    return SUCCESS;
}

/**
 * 在文档中删除内容
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 删除起始位置
 * @param len 删除长度
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos) || pos + len > doc->total_length) {
        return INVALID_CURSOR_POS;
    }

    if (len == 0) {
        return SUCCESS; // 无需删除
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_DELETE, version, pos, pos + len, NULL, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 查找删除起始位置
    chunk *start_chunk;
    size_t start_offset;

    if (!find_position(doc, pos, &start_chunk, &start_offset)) {
        return INVALID_CURSOR_POS;
    }

    // 查找删除结束位置
    chunk *end_chunk;
    size_t end_offset;

    if (!find_position(doc, pos + len, &end_chunk, &end_offset)) {
        return INVALID_CURSOR_POS;
    }

    // 如果删除范围在同一个块内
    if (start_chunk == end_chunk) {
        // 创建新块，排除要删除的部分
        char *new_content = (char *)malloc(start_chunk->length - len + 1);
        if (!new_content) {
            return INVALID_CURSOR_POS; // 内存分配失败
        }

        // 复制前半部分
        memcpy(new_content, start_chunk->content, start_offset);

        // 复制后半部分
        memcpy(new_content + start_offset,
               start_chunk->content + start_offset + len,
               start_chunk->length - start_offset - len);

        new_content[start_chunk->length - len] = '\0';

        // 更新块内容
        free(start_chunk->content);
        start_chunk->content = new_content;
        start_chunk->length -= len;

        // 如果块变为空，则删除它
        if (start_chunk->length == 0) {
            if (doc->head == start_chunk) {
                doc->head = start_chunk->next;
            } else {
                chunk *prev = doc->head;
                while (prev->next != start_chunk) {
                    prev = prev->next;
                }
                prev->next = start_chunk->next;
            }
            free_chunk(start_chunk);
        }
    } else {
        // 删除跨越多个块

        // 处理起始块
        if (start_offset == 0) {
            // 整个起始块都要删除
            if (doc->head == start_chunk) {
                doc->head = start_chunk->next;
            } else {
                chunk *prev = doc->head;
                while (prev->next != start_chunk) {
                    prev = prev->next;
                }
                prev->next = start_chunk->next;
            }
            free_chunk(start_chunk);
        } else {
            // 只删除起始块的一部分
            char *new_content = (char *)malloc(start_offset + 1);
            if (!new_content) {
                return INVALID_CURSOR_POS; // 内存分配失败
            }

            memcpy(new_content, start_chunk->content, start_offset);
            new_content[start_offset] = '\0';

            free(start_chunk->content);
            start_chunk->content = new_content;
            start_chunk->length = start_offset;
        }

        // 处理中间块（全部删除）
        chunk *current = start_chunk->next;
        while (current != end_chunk) {
            chunk *next = current->next;

            if (doc->head == current) {
                doc->head = next;
            } else {
                chunk *prev = doc->head;
                while (prev->next != current) {
                    prev = prev->next;
                }
                prev->next = next;
            }

            free_chunk(current);
            current = next;
        }

        // 处理结束块
        if (end_offset == 0) {
            // 不需要删除结束块
        } else if (end_offset == end_chunk->length) {
            // 整个结束块都要删除
            if (doc->head == end_chunk) {
                doc->head = end_chunk->next;
            } else {
                chunk *prev = doc->head;
                while (prev->next != end_chunk) {
                    prev = prev->next;
                }
                prev->next = end_chunk->next;
            }
            free_chunk(end_chunk);
        } else {
            // 只删除结束块的一部分
            char *new_content = (char *)malloc(end_chunk->length - end_offset + 1);
            if (!new_content) {
                return INVALID_CURSOR_POS; // 内存分配失败
            }

            memcpy(new_content, end_chunk->content + end_offset, end_chunk->length - end_offset);
            new_content[end_chunk->length - end_offset] = '\0';

            free(end_chunk->content);
            end_chunk->content = new_content;
            end_chunk->length = end_chunk->length - end_offset;
        }
    }

    doc->total_length -= len;
    return SUCCESS;
}

/**
 * 在文档中插入换行符
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 插入位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_newline(document *doc, uint64_t version, size_t pos, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_NEWLINE, version, pos, 0, NULL, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 执行实际的换行符插入
    return direct_insert(doc, pos, "\n", 1);
}

/**
 * 在文档中添加标题格式
 * @param doc 文档指针
 * @param version 版本号
 * @param level 标题级别（1-3）
 * @param pos 插入位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_heading(document *doc, uint64_t version, int level, size_t pos, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    // 检查标题级别是否有效
    if (level < 1 || level > 3) {
        return INVALID_CURSOR_POS;
    }

    // 确保在行首插入标题
    char *prefix = NULL;

    // 如果不在文档开头，检查前一个字符是否为换行符
    if (pos > 0) {
        chunk *prev_chunk;
        size_t prev_offset;

        if (find_position(doc, pos - 1, &prev_chunk, &prev_offset)) {
            if (prev_chunk->content[prev_offset] != '\n') {
                // 需要先插入换行符
                int result = markdown_insert(doc, version, pos, "\n", username, original_cmd);
                if (result != SUCCESS) {
                    return result;
                }
                pos++; // 调整插入位置
            }
        }
    }

    // 根据级别生成标题前缀
    switch (level) {
        case 1:
            prefix = "# ";
            break;
        case 2:
            prefix = "## ";
            break;
        case 3:
            prefix = "### ";
            break;
    }

    // 插入标题前缀
    return markdown_insert(doc, version, pos, prefix, username, original_cmd);
}

/**
 * 在文档中添加粗体格式
 * @param doc 文档指针
 * @param version 版本号
 * @param start 起始位置
 * @param end 结束位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_range(doc, start, end)) {
        return INVALID_CURSOR_POS;
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_BOLD, version, start, end, NULL, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 先在结束位置插入 "**"
    int result = direct_insert(doc, end, "**", 2);
    if (result != SUCCESS) {
        return result;
    }

    // 在起始位置插入 "**"
    return direct_insert(doc, start, "**", 2);
}

/**
 * 在文档中添加斜体格式
 * @param doc 文档指针
 * @param version 版本号
 * @param start 起始位置
 * @param end 结束位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_range(doc, start, end)) {
        return INVALID_CURSOR_POS;
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_ITALIC, version, start, end, NULL, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 先在结束位置插入 "*"
    int result = direct_insert(doc, end, "*", 1);
    if (result != SUCCESS) {
        return result;
    }

    // 在起始位置插入 "*"
    return direct_insert(doc, start, "*", 1);
}

/**
 * 在文档中添加引用块格式
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 插入位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_blockquote(document *doc, uint64_t version, size_t pos, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    // 确保在行首插入引用块
    // 如果不在文档开头，检查前一个字符是否为换行符
    if (pos > 0) {
        chunk *prev_chunk;
        size_t prev_offset;

        if (find_position(doc, pos - 1, &prev_chunk, &prev_offset)) {
            if (prev_chunk->content[prev_offset] != '\n') {
                // 需要先插入换行符
                int result = markdown_insert(doc, version, pos, "\n", username, original_cmd);
                if (result != SUCCESS) {
                    return result;
                }
                pos++; // 调整插入位置
            }
        }
    }

    // 插入引用块前缀
    return markdown_insert(doc, version, pos, "> ", username, original_cmd);
}

/**
 * 在文档中添加有序列表格式
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 插入位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_ordered_list(document *doc, uint64_t version, size_t pos, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    // 确保在行首插入列表
    // 如果不在文档开头，检查前一个字符是否为换行符
    if (pos > 0) {
        chunk *prev_chunk;
        size_t prev_offset;

        if (find_position(doc, pos - 1, &prev_chunk, &prev_offset)) {
            if (prev_chunk->content[prev_offset] != '\n') {
                // 需要先插入换行符
                int result = markdown_insert(doc, version, pos, "\n", username, original_cmd);
                if (result != SUCCESS) {
                    return result;
                }
                pos++; // 调整插入位置
            }
        }
    }

    // 查找前一个列表项，确定编号
    int number = 1;

    // 从插入位置向前查找最近的换行符
    size_t search_pos = pos;
    while (search_pos > 0) {
        search_pos--;

        chunk *chunk_pos;
        size_t offset;

        if (find_position(doc, search_pos, &chunk_pos, &offset)) {
            if (chunk_pos->content[offset] == '\n') {
                // 找到换行符，检查下一行是否为有序列表
                if (search_pos + 1 < doc->total_length) {
                    chunk *next_chunk;
                    size_t next_offset;

                    if (find_position(doc, search_pos + 1, &next_chunk, &next_offset)) {
                        // 检查是否为数字后跟点和空格
                        if (next_offset + 2 < next_chunk->length &&
                            next_chunk->content[next_offset] >= '1' &&
                            next_chunk->content[next_offset] <= '9' &&
                            next_chunk->content[next_offset + 1] == '.' &&
                            next_chunk->content[next_offset + 2] == ' ') {

                            number = next_chunk->content[next_offset] - '0' + 1;
                            if (number > 9) number = 1; // 限制为1-9
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // 生成列表项前缀
    char prefix[5];
    snprintf(prefix, sizeof(prefix), "%d. ", number);

    // 插入列表项前缀
    return markdown_insert(doc, version, pos, prefix, username, original_cmd);
}

/**
 * 在文档中添加无序列表格式
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 插入位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_unordered_list(document *doc, uint64_t version, size_t pos, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    // 确保在行首插入列表
    // 如果不在文档开头，检查前一个字符是否为换行符
    if (pos > 0) {
        chunk *prev_chunk;
        size_t prev_offset;

        if (find_position(doc, pos - 1, &prev_chunk, &prev_offset)) {
            if (prev_chunk->content[prev_offset] != '\n') {
                // 需要先插入换行符
                int result = markdown_insert(doc, version, pos, "\n", username, original_cmd);
                if (result != SUCCESS) {
                    return result;
                }
                pos++; // 调整插入位置
            }
        }
    }

    // 插入无序列表前缀
    return markdown_insert(doc, version, pos, "- ", username, original_cmd);
}

/**
 * 在文档中添加代码格式
 * @param doc 文档指针
 * @param version 版本号
 * @param start 起始位置
 * @param end 结束位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_code(document *doc, uint64_t version, size_t start, size_t end, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_range(doc, start, end)) {
        return INVALID_CURSOR_POS;
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_CODE, version, start, end, NULL, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 先在结束位置插入 "`"
    int result = direct_insert(doc, end, "`", 1);
    if (result != SUCCESS) {
        return result;
    }

    // 在起始位置插入 "`"
    return direct_insert(doc, start, "`", 1);
}

/**
 * 在文档中添加水平线
 * @param doc 文档指针
 * @param version 版本号
 * @param pos 插入位置
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos, const char *username, const char *original_cmd) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_position(doc, pos)) {
        return INVALID_CURSOR_POS;
    }

    // 确保在行首插入水平线
    // 如果不在文档开头，检查前一个字符是否为换行符
    if (pos > 0) {
        chunk *prev_chunk;
        size_t prev_offset;

        if (find_position(doc, pos - 1, &prev_chunk, &prev_offset)) {
            if (prev_chunk->content[prev_offset] != '\n') {
                // 需要先插入换行符
                int result = markdown_insert(doc, version, pos, "\n", username, original_cmd);
                if (result != SUCCESS) {
                    return result;
                }
                pos++; // 调整插入位置
            }
        }
    }

    // 插入水平线并确保后面有换行符
    int result = markdown_insert(doc, version, pos, "---", username, original_cmd);
    if (result != SUCCESS) {
        return result;
    }

    // 检查水平线后是否需要添加换行符
    if (pos + 3 < doc->total_length) {
        chunk *next_chunk;
        size_t next_offset;

        if (find_position(doc, pos + 3, &next_chunk, &next_offset)) {
            if (next_chunk->content[next_offset] != '\n') {
                // 需要在水平线后插入换行符
                return markdown_insert(doc, version, pos + 3, "\n", username, original_cmd);
            }
        }
    } else {
        // 水平线在文档末尾，添加换行符
        return markdown_insert(doc, version, pos + 3, "\n", username, original_cmd);
    }

    return SUCCESS;
}

/**
 * 在文档中添加链接格式
 * @param doc 文档指针
 * @param version 版本号
 * @param start 起始位置
 * @param end 结束位置
 * @param url 链接URL
 * @param username 用户名
 * @param original_cmd 原始命令字符串
 * @return 成功返回 SUCCESS，否则返回错误码
 */
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url, const char *username, const char *original_cmd) {
    if (!doc || !url) {
        return INVALID_CURSOR_POS;
    }

    // 检查版本和位置是否有效
    if (!is_valid_version(doc, version)) {
        return OUTDATED_VERSION;
    }

    if (!is_valid_range(doc, start, end)) {
        return INVALID_CURSOR_POS;
    }

    // 创建编辑命令并添加到待处理列表
    edit_command *cmd = create_command(CMD_LINK, version, start, end, url, 0, username, original_cmd);
    if (!cmd) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    add_pending_edit(doc, cmd);

    // 构造链接格式：[文本](URL)
    // 先在结束位置插入 "](URL)"
    char *suffix = (char *)malloc(strlen(url) + 4); // "](" + url + ")"
    if (!suffix) {
        return INVALID_CURSOR_POS; // 内存分配失败
    }

    sprintf(suffix, "](%s)", url);
    int result = direct_insert(doc, end, suffix, strlen(suffix));
    free(suffix);

    if (result != SUCCESS) {
        return result;
    }

    // 在起始位置插入 "["
    return direct_insert(doc, start, "[", 1);
}

/**
 * 打印文档内容到指定流
 * @param doc 文档指针
 * @param stream 输出流
 */
void markdown_print(const document *doc, FILE *stream) {
    if (!doc || !stream) {
        return;
    }

    chunk *current = doc->head;

    while (current) {
        fprintf(stream, "%s", current->content);
        current = current->next;
    }
}

/**
 * 将文档内容扁平化为单个字符串
 * @param doc 文档指针
 * @return 扁平化后的字符串，调用者负责释放
 */
char *markdown_flatten(const document *doc) {
    if (!doc) {
        return NULL;
    }

    // 如果文档为空，返回空字符串
    if (doc->total_length == 0) {
        char *result = (char *)malloc(1);
        if (result) {
            result[0] = '\0';
        }
        return result;
    }

    // 分配足够的内存
    char *result = (char *)malloc(doc->total_length + 1);
    if (!result) {
        return NULL;
    }

    // 复制所有块的内容
    size_t offset = 0;
    chunk *current = doc->head;

    while (current) {
        memcpy(result + offset, current->content, current->length);
        offset += current->length;
        current = current->next;
    }

    // 添加终止符
    result[doc->total_length] = '\0';

    return result;
}

/**
 * 增加文档版本号
 * @param doc 文档指针
 */
void markdown_increment_version(document *doc) {
    if (!doc) {
        return;
    }

    doc->version++;

    // 将待处理的编辑命令移动到历史记录中
    if (doc->pending_edits) {
        edit_command *current = doc->pending_edits;

        // 找到待处理列表的最后一个节点
        while (current->next) {
            current = current->next;
        }

        // 将整个待处理列表添加到历史记录中
        if (!doc->edit_history) {
            doc->edit_history = doc->pending_edits;
        } else {
            edit_command *history_last = doc->edit_history;
            while (history_last->next) {
                history_last = history_last->next;
            }
            history_last->next = doc->pending_edits;
        }

        // 清空待处理列表
        doc->pending_edits = NULL;
    }
}