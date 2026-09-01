// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Copyright (C) 2016-2020 by Marc Schink <dev@zapb.de>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <helper/log.h>
#include <helper/list.h>
#include <target/target.h>
#include <target/rtt.h>

#include "rtt.h"

static struct {
	struct rtt_source source;
	/** Control block. */
	struct rtt_control ctrl;
	struct target *target;
	/** Start address to search for the control block. */
	target_addr_t addr;
	/** Size of the control block search area. */
	size_t size;
	/** Control block identifier. */
	char id[RTT_CB_MAX_ID_LENGTH];
	/** Whether RTT is configured. */
	bool configured;
	/** Whether RTT is started. */
	bool started;
	/** Whether configuration changed. */
	bool changed;
	/** Whether the control block was found. */
	bool found_cb;

	struct rtt_sink_list **sink_list;
	size_t sink_list_length;

	unsigned int polling_interval;
	bool halt_polling;
} rtt;

int rtt_init(void)
{
	rtt.sink_list_length = 1;
	rtt.sink_list = calloc(rtt.sink_list_length,
		sizeof(struct rtt_sink_list *));

	if (!rtt.sink_list)
		return ERROR_FAIL;

	rtt.sink_list[0] = NULL;
	rtt.started = false;

	rtt.polling_interval = 100;
	rtt.halt_polling = false;

	return ERROR_OK;
}

int rtt_exit(void)
{
	free(rtt.sink_list);

	return ERROR_OK;
}

static int rtt_target_access_begin(bool *resume)
{
	int ret;

	*resume = false;

	if (!rtt.halt_polling || rtt.target->state != TARGET_RUNNING)
		return ERROR_OK;

	ret = target_halt(rtt.target);
	if (ret != ERROR_OK)
		return ret;

	ret = target_wait_state(rtt.target, TARGET_HALTED, 1000);
	if (ret != ERROR_OK) {
		if (rtt.target->state == TARGET_HALTED)
			target_resume(rtt.target, true, 0, false, false);
		return ret;
	}

	*resume = true;
	return ERROR_OK;
}

static int rtt_target_access_end(bool resume, int access_ret)
{
	if (!resume)
		return access_ret;

	int resume_ret = target_resume(rtt.target, true, 0, false, false);
	return access_ret == ERROR_OK ? resume_ret : access_ret;
}

static int read_channel_callback(void *user_data)
{
	int ret;
	bool resume = false;

	if (rtt.halt_polling) {
		bool has_sink = false;

		for (size_t i = 0; i < rtt.sink_list_length; i++) {
			if (rtt.sink_list[i]) {
				has_sink = true;
				break;
			}
		}

		if (!has_sink)
			return ERROR_OK;
	}

	ret = rtt_target_access_begin(&resume);
	if (ret != ERROR_OK)
		return ret;

	ret = rtt.source.read(rtt.target, &rtt.ctrl, rtt.sink_list,
		rtt.sink_list_length, NULL);
	ret = rtt_target_access_end(resume, ret);

	if (ret != ERROR_OK) {
		target_unregister_timer_callback(&read_channel_callback, NULL);
		rtt.source.stop(rtt.target, NULL);
		return ret;
	}

	return ERROR_OK;
}

int rtt_setup(target_addr_t address, size_t size, const char *id)
{
	size_t id_length = strlen(id);

	if (!id_length || id_length >= RTT_CB_MAX_ID_LENGTH) {
		LOG_ERROR("rtt: Invalid control block ID");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	rtt.addr = address;
	rtt.size = size;
	strncpy(rtt.id, id, id_length + 1);
	rtt.changed = true;
	rtt.configured = true;

	return ERROR_OK;
}

int rtt_register_source(const struct rtt_source source,
		struct target *target)
{
	if (!source.find_cb || !source.read_cb || !source.read_channel_info)
		return ERROR_FAIL;

	if (!source.start || !source.stop)
		return ERROR_FAIL;

	if (!source.read || !source.write)
		return ERROR_FAIL;

	rtt.source = source;
	rtt.target = target;

	return ERROR_OK;
}

int rtt_start(void)
{
	int ret;
	target_addr_t addr = rtt.addr;
	bool resume = false;

	if (rtt.started)
		return ERROR_OK;

	ret = rtt_target_access_begin(&resume);
	if (ret != ERROR_OK)
		return ret;

	if (!rtt.found_cb || rtt.changed) {
		rtt.source.find_cb(rtt.target, &addr, rtt.size, rtt.id,
			&rtt.found_cb, NULL);

		rtt.changed = false;

		if (rtt.found_cb) {
			LOG_INFO("rtt: Control block found at 0x%" TARGET_PRIxADDR,
				addr);
			rtt.ctrl.address = addr;
		} else {
			LOG_ERROR("rtt: No control block found");
			ret = ERROR_FAIL;
			goto out;
		}
	}

	ret = rtt.source.read_cb(rtt.target, rtt.ctrl.address, &rtt.ctrl, NULL);

	if (ret != ERROR_OK)
		goto out;

	ret = rtt.source.start(rtt.target, &rtt.ctrl, NULL);

	if (ret != ERROR_OK)
		goto out;

	target_register_timer_callback(&read_channel_callback,
		rtt.polling_interval, 1, NULL);
	rtt.started = true;

out:
	return rtt_target_access_end(resume, ret);
}

int rtt_stop(void)
{
	int ret;

	if (!rtt.configured) {
		LOG_ERROR("rtt: Not configured");
		return ERROR_FAIL;
	}

	target_unregister_timer_callback(&read_channel_callback, NULL);
	rtt.started = false;

	ret = rtt.source.stop(rtt.target, NULL);

	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

static int adjust_sink_list(size_t length)
{
	struct rtt_sink_list **tmp;

	if (length <= rtt.sink_list_length)
		return ERROR_OK;

	tmp = realloc(rtt.sink_list, sizeof(struct rtt_sink_list *) * length);

	if (!tmp)
		return ERROR_FAIL;

	for (size_t i = rtt.sink_list_length; i < length; i++)
		tmp[i] = NULL;

	rtt.sink_list = tmp;
	rtt.sink_list_length = length;

	return ERROR_OK;
}

int rtt_register_sink(unsigned int channel_index, rtt_sink_read read,
		void *user_data)
{
	struct rtt_sink_list *tmp;

	if (channel_index >= rtt.sink_list_length) {
		if (adjust_sink_list(channel_index + 1) != ERROR_OK)
			return ERROR_FAIL;
	}

	LOG_DEBUG("rtt: Registering sink for channel %u", channel_index);

	tmp = malloc(sizeof(struct rtt_sink_list));

	if (!tmp)
		return ERROR_FAIL;

	tmp->read = read;
	tmp->user_data = user_data;
	tmp->next = rtt.sink_list[channel_index];

	rtt.sink_list[channel_index] = tmp;

	return ERROR_OK;
}

int rtt_unregister_sink(unsigned int channel_index, rtt_sink_read read,
		void *user_data)
{
	struct rtt_sink_list *prev_sink;

	LOG_DEBUG("rtt: Unregistering sink for channel %u", channel_index);

	if (channel_index >= rtt.sink_list_length)
		return ERROR_FAIL;

	prev_sink = rtt.sink_list[channel_index];

	for (struct rtt_sink_list *sink = rtt.sink_list[channel_index]; sink;
			prev_sink = sink, sink = sink->next) {
		if (sink->read == read && sink->user_data == user_data) {

			if (sink == rtt.sink_list[channel_index])
				rtt.sink_list[channel_index] = sink->next;
			else
				prev_sink->next = sink->next;

			free(sink);

			return ERROR_OK;
		}
	}

	return ERROR_OK;
}

int rtt_get_polling_interval(unsigned int *interval)
{
	if (!interval)
		return ERROR_FAIL;

	*interval = rtt.polling_interval;

	return ERROR_OK;
}

int rtt_set_polling_interval(unsigned int interval)
{
	if (!interval)
		return ERROR_FAIL;

	if (rtt.polling_interval != interval) {
		target_unregister_timer_callback(&read_channel_callback, NULL);
		target_register_timer_callback(&read_channel_callback, interval, 1,
			NULL);
	}

	rtt.polling_interval = interval;

	return ERROR_OK;
}

void rtt_set_halt_polling(bool enabled)
{
	rtt.halt_polling = enabled;
}

bool rtt_get_halt_polling(void)
{
	return rtt.halt_polling;
}

int rtt_write_channel(unsigned int channel_index, const uint8_t *buffer,
		size_t *length)
{
	int ret;
	bool resume = false;

	if (channel_index >= rtt.ctrl.num_up_channels) {
		LOG_WARNING("rtt: Down-channel %u is not available", channel_index);
		return ERROR_OK;
	}

	ret = rtt_target_access_begin(&resume);
	if (ret != ERROR_OK)
		return ret;

	ret = rtt.source.write(rtt.target, &rtt.ctrl, channel_index, buffer,
		length, NULL);
	return rtt_target_access_end(resume, ret);
}

bool rtt_configured(void)
{
	return rtt.configured;
}

bool rtt_found_cb(void)
{
	return rtt.found_cb;
}

const struct rtt_control *rtt_get_control(void)
{
	return &rtt.ctrl;
}

int rtt_read_channel_info(unsigned int channel_index,
	enum rtt_channel_type type, struct rtt_channel_info *info)
{
	int ret;
	bool resume = false;

	ret = rtt_target_access_begin(&resume);
	if (ret != ERROR_OK)
		return ret;

	ret = rtt.source.read_channel_info(rtt.target, &rtt.ctrl,
		channel_index, type, info, NULL);
	return rtt_target_access_end(resume, ret);
}
