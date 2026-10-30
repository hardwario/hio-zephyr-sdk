/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "ram_store.h"

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <string.h>

struct entry {
	bool used;
	char name[64];
	uint8_t value[128];
	size_t len;
};

static struct entry m_entries[32];

static struct entry *find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(m_entries); i++) {
		if (m_entries[i].used && strcmp(m_entries[i].name, name) == 0) {
			return &m_entries[i];
		}
	}

	return NULL;
}

void ram_store_put(const char *name, const void *value, size_t len)
{
	struct entry *e = find(name);

	if (e == NULL) {
		for (size_t i = 0; i < ARRAY_SIZE(m_entries); i++) {
			if (!m_entries[i].used) {
				e = &m_entries[i];
				break;
			}
		}
	}

	zassert_not_null(e, "RAM store full");
	zassert_true(strlen(name) < sizeof(e->name));
	zassert_true(len <= sizeof(e->value));

	e->used = true;
	strcpy(e->name, name);
	memcpy(e->value, value, len);
	e->len = len;
}

void ram_store_clear(void)
{
	memset(m_entries, 0, sizeof(m_entries));
}

static ssize_t read_cb(void *cb_arg, void *data, size_t len)
{
	struct entry *e = cb_arg;
	size_t n = MIN(len, e->len);

	memcpy(data, e->value, n);

	return n;
}

static int ram_load(struct settings_store *cs, const struct settings_load_arg *arg)
{
	for (size_t i = 0; i < ARRAY_SIZE(m_entries); i++) {
		struct entry *e = &m_entries[i];

		if (!e->used) {
			continue;
		}

		int ret = settings_call_set_handler(e->name, e->len, read_cb, e, arg);
		if (ret) {
			return ret;
		}
	}

	return 0;
}

static int ram_save(struct settings_store *cs, const char *name, const char *value,
		    size_t val_len)
{
	/* A zero-length value deletes the key */
	if (value == NULL || val_len == 0) {
		struct entry *e = find(name);

		if (e != NULL) {
			e->used = false;
		}
		return 0;
	}

	ram_store_put(name, value, val_len);

	return 0;
}

static const struct settings_store_itf m_itf = {
	.csi_load = ram_load,
	.csi_save = ram_save,
};

static struct settings_store m_store = {
	.cs_itf = &m_itf,
};

void ram_store_register(void)
{
	static bool registered;

	if (!registered) {
		settings_src_register(&m_store);
		settings_dst_register(&m_store);
		registered = true;
	}
}
