#include <stdlib.h>
#include <string.h>
#include "../libs/document.h"

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

/**
 * 添加服务器命令日志到文档
 * @param doc 文档
 * @param command 命令字符串
 * @param version 版本号
 */
void add_server_cmd_log(document *doc, const char *command, uint64_t version) {
    if (!doc || !command) {
        return;
    }

    server_cmd_log *new_log = create_server_cmd_log(command, version);
    if (!new_log) {
        return;
    }

    if (!doc->cmd_log_head) {
        doc->cmd_log_head = new_log;
    } else {
        server_cmd_log *current = doc->cmd_log_head;
        while (current->next) {
            current = current->next;
        }
        current->next = new_log;
    }
}
