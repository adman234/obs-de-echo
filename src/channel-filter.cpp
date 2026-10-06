/*
OBS De-Echo
Copyright (C) 2026 adman234

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>

#include <atomic>
#include <cstring>

#define SETTING_CHANNEL "channel"

namespace {

struct ChannelFilter {
	std::atomic<long long> channel{0};
};

const char *channel_filter_get_name(void *)
{
	return obs_module_text("ChannelSelect.Filter");
}

void channel_filter_update(void *data, obs_data_t *settings)
{
	static_cast<ChannelFilter *>(data)->channel = obs_data_get_int(settings, SETTING_CHANNEL);
}

void *channel_filter_create(obs_data_t *settings, obs_source_t *)
{
	ChannelFilter *filter = new ChannelFilter();
	channel_filter_update(filter, settings);
	return filter;
}

void channel_filter_destroy(void *data)
{
	delete static_cast<ChannelFilter *>(data);
}

void channel_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_CHANNEL, 1);
}

obs_properties_t *channel_filter_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, SETTING_CHANNEL, obs_module_text("ChannelSelect.Channel"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(list, obs_module_text("ChannelSelect.Channel.Tooltip"));
	obs_property_list_add_int(list, obs_module_text("ChannelSelect.Channel.1"), 0);
	obs_property_list_add_int(list, obs_module_text("ChannelSelect.Channel.2"), 1);

	return props;
}

/* Copies the chosen channel over every other channel, so one input of a
 * multi-input device is heard on its own and centered. */
struct obs_audio_data *channel_filter_audio(void *data, struct obs_audio_data *audio)
{
	const long long channel = static_cast<ChannelFilter *>(data)->channel;
	const size_t channels = audio_output_get_channels(obs_get_audio());

	if (channel < 0 || static_cast<size_t>(channel) >= channels || !audio->data[channel])
		return audio;

	const size_t size = static_cast<size_t>(audio->frames) * sizeof(float);
	for (size_t i = 0; i < channels; i++) {
		if (i != static_cast<size_t>(channel) && audio->data[i])
			memcpy(audio->data[i], audio->data[channel], size);
	}
	return audio;
}

} // namespace

void channel_filter_register()
{
	struct obs_source_info info = {};
	info.id = "deecho_channel_select";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = channel_filter_get_name;
	info.create = channel_filter_create;
	info.destroy = channel_filter_destroy;
	info.update = channel_filter_update;
	info.get_defaults = channel_filter_get_defaults;
	info.get_properties = channel_filter_get_properties;
	info.filter_audio = channel_filter_audio;

	obs_register_source(&info);
}
