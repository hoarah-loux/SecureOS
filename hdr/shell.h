#pragma once

#include <stddef.h>
#include <fat32_fs.h>
#include <keyboard.h>
#include <serial.h>
#include <string.h>

#define SHELL_MAX_LINE 256
#define SHELL_MAX_ARGS 8

typedef struct {
    fat32_mount_t *mount;
    char line[SHELL_MAX_LINE];
    char **argv;
    size_t argc;
} shell_ctx_t;

void shell_init(shell_ctx_t *ctx, fat32_mount_t *mount);
size_t shell_split_args(char *line, char **argv, size_t max_args);
int shell_eval(shell_ctx_t *ctx);
