#include <stdlib.h>
#include <string.h>
#include "../libs/document.h"

/**
 * 创建一个新的文档块
 * @param content 块内容
 * @param length 内容长度
 * @return 新创建的块指针
 */
chunk *create_chunk(const char *content, size_t length) {
    chunk *new_chunk = (chunk *)malloc(sizeof(chunk));
    if (!new_chunk) {
        return NULL;
    }
    
    new_chunk->content = (char *)malloc(length + 1);
    if (!new_chunk->content) {
        free(new_chunk);
        return NULL;
    }
    
    memcpy(new_chunk->content, content, length);
    new_chunk->content[length] = '\0';
    new_chunk->length = length;
    new_chunk->next = NULL;
    
    return new_chunk;
}

/**
 * 释放文档块及其内容
 * @param c 要释放的块
 */
void free_chunk(chunk *c) {
    if (c) {
        free(c->content);
        free(c);
    }
}

/**
 * 创建一个新的编辑命令
 * @param type 命令类型
 * @param version 文档版本
 * @param pos1 起始位置
 * @param pos2 结束位置
 * @param content 内容
 * @param level 级别（用于标题等）
 * @return 新创建的命令指针
 */
edit_command *create_command(command_type type, uint64_t version, size_t pos1, size_t pos2, const char *content, int level) {
    edit_command *cmd = (edit_command *)malloc(sizeof(edit_command));
    if (!cmd) {
        return NULL;
    }
    
    cmd->type = type;
    cmd->version = version;
    cmd->pos1 = pos1;
    cmd->pos2 = pos2;
    cmd->level = level;
    cmd->next = NULL;
    
    if (content) {
        cmd->content = strdup(content);
        if (!cmd->content) {
            free(cmd);
            return NULL;
        }
    } else {
        cmd->content = NULL;
    }
    
    return cmd;
}

/**
 * 释放编辑命令及其内容
 * @param cmd 要释放的命令
 */
void free_command(edit_command *cmd) {
    if (cmd) {
        free(cmd->content);
        free(cmd);
    }
}

/**
 * 添加待处理的编辑命令到文档
 * @param doc 文档
 * @param cmd 编辑命令
 */
void add_pending_edit(document *doc, edit_command *cmd) {
    if (!doc || !cmd) {
        return;
    }
    
    if (!doc->pending_edits) {
        doc->pending_edits = cmd;
    } else {
        edit_command *current = doc->pending_edits;
        while (current->next) {
            current = current->next;
        }
        current->next = cmd;
    }
}

/**
 * 添加编辑命令到文档历史
 * @param doc 文档
 * @param cmd 编辑命令
 */
void add_edit_history(document *doc, edit_command *cmd) {
    if (!doc || !cmd) {
        return;
    }
    
    if (!doc->edit_history) {
        doc->edit_history = cmd;
    } else {
        edit_command *current = doc->edit_history;
        while (current->next) {
            current = current->next;
        }
        current->next = cmd;
    }
}
