/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_TOOLS_H
#define _LVM_TOOLS_H

#define _GNU_SOURCE

#include "activate.h"
#include "archive.h"
#include "cache.h"
#include "config.h"
#include "dbg_malloc.h"
#include "dev-cache.h"
#include "device.h"
#include "display.h"
#include "errors.h"
#include "filter.h"
#include "filter-composite.h"
#include "filter-persistent.h"
#include "filter-regex.h"
#include "format-text.h"
#include "metadata.h"
#include "list.h"
#include "locking.h"
#include "log.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "pool.h"
#include "toolcontext.h"
#include "toollib.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define CMD_LEN 256
#define MAX_ARGS 64

/* command functions */
typedef int (*command_fn) (struct cmd_context * cmd, int argc, char **argv);

#define xx(a, b...) int a(struct cmd_context *cmd, int argc, char **argv);
#include "commands.h"
#undef xx

/* define the enums for the command line switches */
enum {
#define arg(a, b, c, d) a ,
#include "args.h"
#undef arg
};

typedef enum {
	SIGN_NONE = 0,
	SIGN_PLUS = 1,
	SIGN_MINUS = 2
} sign_t;

/* a global table of possible arguments */
struct arg {
	char short_arg;
	char *long_arg;
	int (*fn) (struct cmd_context * cmd, struct arg * a);

	int count;
	char *value;
	uint32_t i_value;
	uint64_t i64_value;
	sign_t sign;
	void *ptr;
};

/* a register of the lvm commands */
struct command {
	const char *name;
	const char *desc;
	const char *usage;
	command_fn fn;

	int num_args;
	int *valid_args;
};

void usage(const char *name);

/* the argument verify/normalise functions */
int yes_no_arg(struct cmd_context *cmd, struct arg *a);
int size_kb_arg(struct cmd_context *cmd, struct arg *a);
int size_mb_arg(struct cmd_context *cmd, struct arg *a);
int int_arg(struct cmd_context *cmd, struct arg *a);
int int_arg_with_sign(struct cmd_context *cmd, struct arg *a);
int minor_arg(struct cmd_context *cmd, struct arg *a);
int string_arg(struct cmd_context *cmd, struct arg *a);
int permission_arg(struct cmd_context *cmd, struct arg *a);
int metadatatype_arg(struct cmd_context *cmd, struct arg *a);

char yes_no_prompt(const char *prompt, ...);

/* we use the enums to access the switches */
static inline int arg_count(struct cmd_context *cmd, int a)
{
	return cmd->args[a].count;
}

static inline char *arg_value(struct cmd_context *cmd, int a)
{
	return cmd->args[a].value;
}

static inline char *arg_str_value(struct cmd_context *cmd, int a, char *def)
{
	return arg_count(cmd, a) ? cmd->args[a].value : def;
}

static inline uint32_t arg_int_value(struct cmd_context *cmd, int a,
				     uint32_t def)
{
	return arg_count(cmd, a) ? cmd->args[a].i_value : def;
}

static inline uint64_t arg_int64_value(struct cmd_context *cmd, int a,
				       uint64_t def)
{
	return arg_count(cmd, a) ? cmd->args[a].i64_value : def;
}

static inline void *arg_ptr_value(struct cmd_context *cmd, int a, void *def)
{
	return arg_count(cmd, a) ? cmd->args[a].ptr : def;
}

static inline sign_t arg_sign_value(struct cmd_context *cmd, int a, sign_t def)
{
	return arg_count(cmd, a) ? cmd->args[a].sign : def;
}

static inline int arg_count_increment(struct cmd_context *cmd, int a)
{
	return cmd->args[a].count++;
}

static inline const char *command_name(struct cmd_context *cmd)
{
	return cmd->command->name;
}

#endif
