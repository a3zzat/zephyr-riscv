/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <misc/printk.h>
#include <net/nbuf.h>
#include <net/net_context.h>
#include <net/net_core.h>
#include <net/net_if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr.h>
#include <json.h>

#include "nats.h"

struct nats_info {
	const char *server_id;
	const char *version;
	const char *go;
	const char *host;
	size_t max_payload;
	uint16_t port;
	bool ssl_required;
	bool auth_required;
};

struct io_vec {
	const void *base;
	size_t len;
};

static bool is_subject_valid(const char *subject, size_t len)
{
	size_t pos;
	char last = '\0';

	if (!subject) {
		return false;
	}

	for (pos = 0; pos < len; last = subject[pos++]) {
		switch (subject[pos]) {
		case '>':
			if (subject[pos + 1] != '\0') {
				return false;
			}

			break;
		case '.':
		case '*':
			if (last == subject[pos]) {
				return false;
			}

			break;
		default:
			if (isalnum(subject[pos])) {
				continue;
			}

			return false;
		}
	}

	return true;
}

static bool is_sid_valid(const char *sid, size_t len)
{
	size_t pos;

	if (!sid) {
		return false;
	}

	for (pos = 0; pos < len; pos++) {
		if (!isalnum(sid[pos])) {
			return false;
		}
	}

	return true;
}

#define TRANSMITV_LITERAL(lit_) { .base = lit_, .len = sizeof(lit_) - 1 }

static int transmitv(struct net_context *conn, int iovcnt,
		     struct io_vec *iov)
{
	struct net_buf *buf;
	int i;

	buf = net_nbuf_get_tx(conn, K_FOREVER);
	if (!buf) {
		return -ENOMEM;
	}

	for (i = 0; i < iovcnt; i++) {
		if (!net_nbuf_append(buf, iov[i].len, iov[i].base, K_FOREVER)) {
			net_nbuf_unref(buf);

			return -ENOMEM;
		}
	}

	return net_context_send(buf, NULL, K_NO_WAIT, NULL, NULL);
}

static inline int transmit(struct net_context *conn, const char buffer[],
			   size_t len)
{
	return transmitv(conn, 1, (struct io_vec[]) {
		{ .base = buffer, .len = len },
	});
}

#define FIELD(struct_, member_, type_) { \
	.field_name = #member_, \
	.field_name_len = sizeof(#member_) - 1, \
	.offset = offsetof(struct_, member_), \
	.type = type_ \
}
static int handle_server_info(struct nats *nats, char *payload, size_t len)
{
	static const struct json_obj_descr descr[] = {
		FIELD(struct nats_info, server_id, JSON_TOK_STRING),
		FIELD(struct nats_info, version, JSON_TOK_STRING),
		FIELD(struct nats_info, go, JSON_TOK_STRING),
		FIELD(struct nats_info, host, JSON_TOK_STRING),
		FIELD(struct nats_info, port, JSON_TOK_NUMBER),
		FIELD(struct nats_info, auth_required, JSON_TOK_TRUE),
		FIELD(struct nats_info, ssl_required, JSON_TOK_TRUE),
		FIELD(struct nats_info, max_payload, JSON_TOK_NUMBER),
	};
	struct nats_info info = {};
	char user[32], pass[64];
	size_t user_len = sizeof(user), pass_len = sizeof(pass);
	int ret;

	ret = json_obj_parse(payload, len, descr, ARRAY_SIZE(descr), &info);
	if (ret < 0) {
		return -EINVAL;
	}

	if (info.ssl_required) {
		return -ENOTSUP;
	}

	if (!info.auth_required) {
		return 0;
	}

	if (!nats->on_auth_required) {
		return -EPERM;
	}

	ret = nats->on_auth_required(nats, user, &user_len, pass, &pass_len);
	if (ret < 0) {
		return ret;
	}

	ret = json_escape(user, &user_len, sizeof(user));
	if (ret < 0) {
		return ret;
	}

	ret = json_escape(pass, &pass_len, sizeof(pass));
	if (ret < 0) {
		return ret;
	}

	return transmitv(nats->conn, 5, (struct io_vec[]) {
		TRANSMITV_LITERAL("CONNECT {\"user\":\""),
		{ .base = user, .len = user_len },
		TRANSMITV_LITERAL("\",\"pass\":\""),
		{ .base = pass, .len = pass_len },
		TRANSMITV_LITERAL("\"}\r\n"),
	});
}
#undef FIELD

static bool char_in_set(char chr, const char *set)
{
	const char *ptr;

	for (ptr = set; *ptr; ptr++) {
		if (*ptr == chr) {
			return true;
		}
	}

	return false;
}

static char *strsep(char *strp, const char *delims)
{
	const char *delim;
	char *ptr;

	if (!strp) {
		return NULL;
	}

	for (delim = delims; *delim; delim++) {
		ptr = strchr(strp, *delim);
		if (ptr) {
			*ptr = '\0';

			for (ptr++; *ptr; ptr++) {
				if (!char_in_set(*ptr, delims)) {
					break;
				}
			}

			return ptr;
		}
	}

	return NULL;
}

static int handle_server_msg(struct nats *nats, char *payload, size_t len)
{
	char *subject, *sid, *reply_to, *bytes;
	char *end_ptr;
	char prev_end = payload[len];
	size_t payload_size;

	/* strsep() uses strchr(), ensure payload is NUL-terminated */
	payload[len] = '\0';

	/* Slice the tokens */
	subject = payload;
	sid = strsep(subject, " \t");
	reply_to = strsep(sid, " \t");
	if (!reply_to) {
		bytes = strsep(sid, "\r");
	} else {
		bytes = strsep(reply_to, "\r");
	}

	payload[len] = prev_end;

	if (!bytes) {
		return -EINVAL;
	}

	/* Parse the payload size */
	errno = 0;
	payload_size = strtoul(bytes, &end_ptr, 10);
	if (errno != 0) {
		return -errno;
	}

	if (!end_ptr) {
		return -EINVAL;
	}

	if (payload_size >= payload + len - end_ptr) {
		return -EINVAL;
	}

	payload = end_ptr + 2;

	return nats->on_message(nats, &(struct nats_msg) {
		.subject = subject,
		.sid = sid,
		.reply_to = reply_to,
		.payload = payload,
		.payload_len = payload_size,
	});
}

static int handle_server_ping(struct nats *nats, char *payload, size_t len)
{
	static const char pong[] = "PONG\r\n";

	return transmit(nats->conn, pong, sizeof(pong) - 1);
}

static int ignore(struct nats *nats, char *payload, size_t len)
{
	/* FIXME: Notify user of success/errors.  This would require
	 * maintaining information of what was the last sent command in
	 * order to provide the best error information for the user.
	 * Without VERBOSE set, these won't be sent -- but be cautious and
	 * ignore them just in case.
	 */
	return 0;
}

#define CMD(cmd_, handler_) { \
	.op = cmd_, \
	.len = sizeof(cmd_) - 1, \
	.handle = handler_ \
}
static int handle_server_cmd(struct nats *nats, char *cmd, size_t len)
{
	static const struct {
		const char *op;
		size_t len;
		int (*handle)(struct nats *nats, char *payload, size_t len);
	} cmds[] = {
		CMD("INFO", handle_server_info),
		CMD("MSG", handle_server_msg),
		CMD("PING", handle_server_ping),
		CMD("+OK", ignore),
		CMD("-ERR", ignore),
	};
	size_t i;
	char *payload;
	size_t payload_len;

	payload = strsep(cmd, " \t");
	if (!payload) {
		payload = strsep(cmd, "\r");
		if (!payload) {
			return -EINVAL;
		}
	}
	payload_len = len - (size_t)(payload - cmd);
	len = (size_t)(payload - cmd);

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		if (len != cmds[i].len) {
			continue;
		}

		if (!strncmp(cmds[i].op, cmd, len)) {
			return cmds[i].handle(nats, payload, payload_len);
		}
	}

	return -ENOENT;
}
#undef CMD

int nats_subscribe(const struct nats *nats,
		   const char *subject, size_t subject_len,
		   const char *queue_group, size_t queue_group_len,
		   const char *sid, size_t sid_len)
{
	if (!is_subject_valid(subject, subject_len)) {
		return -EINVAL;
	}

	if (!is_sid_valid(sid, sid_len)) {
		return -EINVAL;
	}

	if (queue_group) {
		return transmitv(nats->conn, 7, (struct io_vec[]) {
			TRANSMITV_LITERAL("SUB "),
			{
				.base = subject,
				.len = subject_len ?
					subject_len : strlen(subject)
			},
			TRANSMITV_LITERAL(" "),
			{
				.base = queue_group,
				.len = queue_group_len ?
					queue_group_len : strlen(queue_group)
			},
			TRANSMITV_LITERAL(" "),
			{
				.base = sid,
				.len = sid_len ? sid_len : strlen(sid)
			},
			TRANSMITV_LITERAL("\r\n")
		});
	}

	return transmitv(nats->conn, 5, (struct io_vec[]) {
		TRANSMITV_LITERAL("SUB "),
		{
			.base = subject,
			.len = subject_len ? subject_len : strlen(subject)
		},
		TRANSMITV_LITERAL(" "),
		{
			.base = sid,
			.len = sid_len ? sid_len : strlen(sid)
		},
		TRANSMITV_LITERAL("\r\n")
	});
}

int nats_unsubscribe(const struct nats *nats,
		     const char *sid, size_t sid_len, size_t max_msgs)
{
	if (!is_sid_valid(sid, sid_len)) {
		return -EINVAL;
	}

	if (max_msgs) {
		char max_msgs_str[3 * sizeof(size_t)];
		int ret;

		ret = snprintk(max_msgs_str, sizeof(max_msgs_str),
			       "%zu", max_msgs);
		if (ret < 0 || ret >= (int)sizeof(max_msgs_str)) {
			return -ENOMEM;
		}

		return transmitv(nats->conn, 5, (struct io_vec[]) {
			TRANSMITV_LITERAL("UNSUB "),
			{
				.base = sid,
				.len = sid_len ? sid_len : strlen(sid)
			},
			TRANSMITV_LITERAL(" "),
			{ .base = max_msgs_str, .len = ret },
			TRANSMITV_LITERAL("\r\n"),
		});
	}

	return transmitv(nats->conn, 3, (struct io_vec[]) {
		TRANSMITV_LITERAL("UNSUB "),
		{
			.base = sid,
			.len = sid_len ? sid_len : strlen(sid)
		},
		TRANSMITV_LITERAL("\r\n")
	});
}

int nats_publish(const struct nats *nats,
		 const char *subject, size_t subject_len,
		 const char *reply_to, size_t reply_to_len,
		 const char *payload, size_t payload_len)
{
	char payload_len_str[3 * sizeof(size_t)];
	int ret;

	if (!is_subject_valid(subject, subject_len)) {
		return -EINVAL;
	}

	ret = snprintk(payload_len_str, sizeof(payload_len_str), "%zu",
		       payload_len);
	if (ret < 0 || ret >= (int)sizeof(payload_len_str)) {
		return -ENOMEM;
	}

	if (reply_to) {
		return transmitv(nats->conn, 7, (struct io_vec[]) {
			TRANSMITV_LITERAL("PUB "),
			{
				.base = subject,
				.len = subject_len ?
					subject_len : strlen(subject)
			},
			TRANSMITV_LITERAL(" "),
			{
				.base = reply_to,
				.len = reply_to_len ?
					reply_to_len : strlen(reply_to)
			},
			TRANSMITV_LITERAL(" "),
			{ .base = payload_len_str, .len = ret },
			TRANSMITV_LITERAL("\r\n"),
		});
	}

	return transmitv(nats->conn, 5, (struct io_vec[]) {
		TRANSMITV_LITERAL("PUB "),
		{
			.base = subject,
			.len = subject_len ? subject_len : strlen(subject)
		},
		TRANSMITV_LITERAL(" "),
		{
			.base = reply_to,
			.len = reply_to_len ? reply_to_len : strlen(reply_to)
		},
		TRANSMITV_LITERAL("\r\n"),
	});
}

static void receive_cb(struct net_context *ctx, struct net_buf *buf, int status,
		       void *user_data)
{
	struct nats *nats = user_data;
	char cmd_buf[256];
	struct net_buf *tmp;
	uint16_t pos = 0, cmd_len = 0;
	size_t len;
	uint8_t *end_of_line;

	if (!buf) {
		/* FIXME: How to handle disconnection? */
		return;
	}

	if (status) {
		/* FIXME: How to handle connectio error? */
		net_buf_unref(buf);
		return;
	}

	tmp = buf->frags;
	pos = net_nbuf_appdata(buf) - tmp->data;

	while (tmp) {
		len = tmp->len - pos;

		end_of_line = memchr((uint8_t *)tmp->data + pos, '\r', len);
		if (end_of_line) {
			len = end_of_line - ((uint8_t *)tmp->data + pos);
		}

		if (cmd_len + len > sizeof(cmd_buf)) {
			break;
		}

		tmp = net_nbuf_read(tmp, pos, &pos, len, cmd_buf + cmd_len);
		cmd_len += len;

		if (end_of_line) {
			if (tmp) {
				tmp = net_nbuf_read(tmp, pos, &pos, 1, NULL);
			}

			cmd_buf[cmd_len] = '\0';
			if (handle_server_cmd(nats, cmd_buf, cmd_len) < 0) {
				/* FIXME: What to do with unhandled messages? */
				break;
			}
			cmd_len = 0;
		}
	}

	net_nbuf_unref(buf);
}

int nats_connect(struct nats *nats, struct sockaddr *addr, socklen_t addrlen)
{
	int ret;

	ret = net_context_connect(nats->conn, addr, addrlen,
				  NULL, K_FOREVER, NULL);
	if (ret < 0) {
		return ret;
	}

	return net_context_recv(nats->conn, receive_cb, K_NO_WAIT, nats);
}

int nats_disconnect(struct nats *nats)
{
	int ret;

	ret = net_context_put(nats->conn);
	if (ret < 0) {
		return ret;
	}

	nats->conn = NULL;

	return 0;
}
