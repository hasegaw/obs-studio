/******************************************************************************
    Copyright (C) 2013-2015 by Hugh Bailey <obs.jim@gmail.com>
                               Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "util/threading.h"
#include "graphics/math-defs.h"
#include "graphics/matrix4.h"
#include "obs-internal.h"

/* how obs scene! */

struct scene_item {
	obs_hotkey_pair_id             toggle_visibility;
};

struct obs_scene {
	struct obs_source              *source;

	pthread_mutex_t                mutex;
	DARRAY(obs_input_t*)           items;
};

static const char *obs_scene_signals[] = {
	"void item_add(ptr scene, ptr item)",
	"void item_remove(ptr scene, ptr item)",
	"void reorder(ptr scene)",
	"void item_visible(ptr scene, ptr item, bool visible)",
	"void item_select(ptr scene, ptr item)",
	"void item_deselect(ptr scene, ptr item)",
	"void item_transform(ptr scene, ptr item)",
	NULL
};

static inline void signal_item_remove(obs_input_t *item)
{
	obs_source_t *parent = obs_input_get_parent(item);
	struct calldata params = {0};
	calldata_set_ptr(&params, "scene", obs_scene_from_source(parent));
	calldata_set_ptr(&params, "item", item);

	signal_handler_signal(parent->context.signals, "item_remove", &params);
	calldata_free(&params);
}

static const char *scene_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Scene";
}

static void *scene_create(obs_data_t *settings, struct obs_source *source)
{
	pthread_mutexattr_t attr;
	struct obs_scene *scene = bzalloc(sizeof(struct obs_scene));
	scene->source = source;

	signal_handler_add_array(obs_source_get_signal_handler(source),
			obs_scene_signals);

	if (pthread_mutexattr_init(&attr) != 0)
		goto fail;
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0)
		goto fail;
	if (pthread_mutex_init(&scene->mutex, &attr) != 0) {
		blog(LOG_ERROR, "scene_create: Couldn't initialize mutex");
		goto fail;
	}

	UNUSED_PARAMETER(settings);
	return scene;

fail:
	pthread_mutexattr_destroy(&attr);
	bfree(scene);
	return NULL;
}

static inline obs_input_t *get_input(struct obs_scene *scene, size_t idx)
{
	return scene->items.array[idx];
}

static void remove_all_items(struct obs_scene *scene)
{
	pthread_mutex_lock(&scene->mutex);

	for (size_t i = 0; i < scene->items.num; i++)
		obs_input_detach(get_input(scene, i));

	da_free(scene->items);
	pthread_mutex_unlock(&scene->mutex);
}

static void scene_destroy(void *data)
{
	struct obs_scene *scene = data;

	remove_all_items(scene);
	pthread_mutex_destroy(&scene->mutex);
	bfree(scene);
}

static void scene_enum_sources(void *data,
		obs_source_enum_proc_t enum_callback,
		void *param)
{
	struct obs_scene *scene = data;

	pthread_mutex_lock(&scene->mutex);

	for (size_t i = 0; i < scene->items.num; i++) {
		obs_input_t *item = get_input(scene, i);
		obs_source_t *source = obs_input_get_source(item);

		if (source) {
			obs_input_addref(item);
			enum_callback(scene->source, source, param);
			obs_input_release(item);
		}
	}

	pthread_mutex_unlock(&scene->mutex);
}

static void scene_video_render(void *data, gs_effect_t *effect)
{
	struct obs_scene *scene = data;

	pthread_mutex_lock(&scene->mutex);

	gs_blend_state_push();
	gs_reset_blend_state();

	for (size_t i = 0; i < scene->items.num; i++) {
		obs_input_t *item = get_input(scene, i);

		if (obs_input_source_removed(item)) {
			obs_scene_remove(item);
			i--;
			continue;
		}

		obs_input_video_render(item);
	}

	gs_blend_state_pop();

	pthread_mutex_unlock(&scene->mutex);

	UNUSED_PARAMETER(effect);
}

static void init_hotkeys(obs_scene_t *scene, obs_input_t *input,
		const char *name);

static void scene_load(void *data, obs_data_t *settings)
{
	obs_scene_t *scene = data;
	obs_data_array_t *items = obs_data_get_array(settings, "items");
	size_t           count, i;

	remove_all_items(scene);

	if (!items) return;

	count = obs_data_array_count(items);

	for (i = 0; i < count; i++) {
		obs_data_t *item_data = obs_data_array_item(items, i);
		obs_input_t *input = obs_input_create(scene->source);
		obs_source_t *source;

		obs_input_load(input, item_data);

		source = obs_input_get_source(input);

		init_hotkeys(scene, input, obs_source_get_name(source));

		da_push_back(scene->items, &input);
		obs_data_release(item_data);
	}

	obs_data_array_release(items);
}

static void scene_save(void *data, obs_data_t *settings)
{
	struct obs_scene      *scene = data;
	obs_data_array_t      *array  = obs_data_array_create();

	pthread_mutex_lock(&scene->mutex);

	for (size_t i = 0; i < scene->items.num; i++) {
		obs_input_t *item = get_input(scene, i);
		obs_data_t *item_data = obs_data_create();
		obs_input_save(item, item_data);
		obs_data_array_push_back(array, item_data);
		obs_data_release(item_data);
	}

	pthread_mutex_unlock(&scene->mutex);

	obs_data_set_array(settings, "items", array);
	obs_data_array_release(array);
}

static uint32_t scene_getwidth(void *data)
{
	UNUSED_PARAMETER(data);
	return obs->video.base_width;
}

static uint32_t scene_getheight(void *data)
{
	UNUSED_PARAMETER(data);
	return obs->video.base_height;
}

const struct obs_source_info scene_info =
{
	.id            = "scene",
	.type          = OBS_SOURCE_TYPE_INPUT,
	.output_flags  = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name      = scene_getname,
	.create        = scene_create,
	.destroy       = scene_destroy,
	.video_render  = scene_video_render,
	.get_width     = scene_getwidth,
	.get_height    = scene_getheight,
	.load          = scene_load,
	.save          = scene_save,
	.enum_sources  = scene_enum_sources
};

obs_scene_t *obs_scene_create(const char *name)
{
	struct obs_source *source =
		obs_source_create(OBS_SOURCE_TYPE_INPUT, "scene", name, NULL,
				NULL);
	return source->context.data;
}

obs_scene_t *obs_scene_duplicate(obs_scene_t *scene, const char *name)
{
	struct obs_scene *new_scene = obs_scene_create(name);

	pthread_mutex_lock(&scene->mutex);

	for (size_t i = 0; i < scene->items.num; i++) {
		obs_input_t *item = get_input(scene, i);
		obs_input_t *new_item = obs_input_duplicate(item,
				new_scene->source);
		da_push_back(new_scene->items, new_item);
	}

	pthread_mutex_unlock(&scene->mutex);

	return new_scene;
}

void obs_scene_addref(obs_scene_t *scene)
{
	if (scene)
		obs_source_addref(scene->source);
}

void obs_scene_release(obs_scene_t *scene)
{
	if (scene)
		obs_source_release(scene->source);
}

obs_source_t *obs_scene_get_source(const obs_scene_t *scene)
{
	return scene ? scene->source : NULL;
}

obs_scene_t *obs_scene_from_source(const obs_source_t *source)
{
	if (!source || source->info.id != scene_info.id)
		return NULL;

	return source->context.data;
}

obs_scene_t *obs_scene_from_parent(const obs_input_t *input)
{
	obs_source_t *parent;

	if (!input)
		return NULL;

	parent = obs_input_get_parent(input);
	if (!parent || parent->info.id != scene_info.id)
		return NULL;

	return parent->context.data;
}

obs_input_t *obs_scene_find_source(obs_scene_t *scene, const char *name)
{
	obs_input_t *ret = NULL;

	if (!scene)
		return NULL;

	pthread_mutex_lock(&scene->mutex);

	for (size_t i = 0; i < scene->items.num; i++) {
		obs_input_t *item = get_input(scene, i);
		obs_source_t *source = obs_input_get_source(item);

		if (strcmp(source->context.name, name) == 0) {
			ret = item;
			break;
		}
	}

	pthread_mutex_unlock(&scene->mutex);

	return ret;
}

void obs_scene_enum_items(obs_scene_t *scene,
		bool (*callback)(obs_scene_t*, obs_input_t*, void*),
		void *param)
{
	if (!scene || !callback)
		return;

	pthread_mutex_lock(&scene->mutex);

	for (size_t i = 0; i < scene->items.num; i++) {
		obs_input_t *item = get_input(scene, i);
		size_t prev_count = scene->items.num;

		obs_input_addref(item);

		if (!callback(scene, item, param)) {
			obs_input_release(item);
			break;
		}

		if (scene->items.num < prev_count)
			i--;

		obs_input_release(item);
	}

	pthread_mutex_unlock(&scene->mutex);
}

static bool hotkey_show_sceneitem(void *data, obs_hotkey_pair_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	obs_input_t *si = obs_input_get_ref(data);
	if (pressed && si && !obs_input_visible(si)) {
		obs_input_set_visible(si, true);
		obs_input_release(si);
		return true;
	}

	obs_input_release(si);
	return false;
}

static bool hotkey_hide_sceneitem(void *data, obs_hotkey_pair_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	obs_input_t *si = obs_input_get_ref(data);
	if (pressed && si && obs_input_visible(si)) {
		obs_input_set_visible(si, false);
		obs_input_release(si);
		return true;
	}

	obs_input_release(si);
	return false;
}

static void sceneitem_destroy(void *data)
{
	struct scene_item *item = data;
	obs_hotkey_pair_unregister(item->toggle_visibility);
	bfree(item);
}

static void init_hotkeys(obs_scene_t *scene, obs_input_t *input,
		const char *name)
{
	struct dstr show = {0};
	struct dstr hide = {0};
	struct dstr show_desc = {0};
	struct dstr hide_desc = {0};
	struct scene_item *item = bmalloc(sizeof(*item));

	dstr_copy(&show, "libobs.show_scene_item.%1");
	dstr_replace(&show, "%1", name);
	dstr_copy(&hide, "libobs.hide_scene_item.%1");
	dstr_replace(&hide, "%1", name);

	dstr_copy(&show_desc, obs->hotkeys.sceneitem_show);
	dstr_replace(&show_desc, "%1", name);
	dstr_copy(&hide_desc, obs->hotkeys.sceneitem_hide);
	dstr_replace(&hide_desc, "%1", name);

	item->toggle_visibility = obs_hotkey_pair_register_source(scene->source,
			show.array, show_desc.array,
			hide.array, hide_desc.array,
			hotkey_show_sceneitem, hotkey_hide_sceneitem,
			input, input);

	obs_input_set_parent_data(input, item, sceneitem_destroy);

	dstr_free(&show);
	dstr_free(&hide);
	dstr_free(&show_desc);
	dstr_free(&hide_desc);
}

obs_input_t *obs_scene_add(obs_scene_t *scene, obs_source_t *source)
{
	obs_input_t *input;
	struct calldata params = {0};

	if (!scene)
		return NULL;

	if (!source) {
		blog(LOG_ERROR, "Tried to add a NULL source to a scene");
		return NULL;
	}

	input = obs_input_create(scene->source);
	obs_input_set_source(input, source);

	init_hotkeys(scene, input, obs_source_get_name(source));

	pthread_mutex_lock(&scene->mutex);
	da_push_back(scene->items, &input);
	pthread_mutex_unlock(&scene->mutex);

	calldata_set_ptr(&params, "scene", scene);
	calldata_set_ptr(&params, "item", input);
	signal_handler_signal(scene->source->context.signals, "item_add",
			&params);
	calldata_free(&params);

	return input;
}

void obs_scene_remove(obs_input_t *input)
{
	obs_scene_t *scene;

	if (!input)
		return;

	scene = obs_scene_from_source(obs_input_get_parent(input));
	if (!scene)
		return;

	pthread_mutex_lock(&scene->mutex);

	signal_item_remove(input);
	obs_input_detach(input);
	da_erase_item(scene->items, input);

	pthread_mutex_unlock(&scene->mutex);
}

static inline void signal_reorder(obs_input_t *item)
{
	const char *command = NULL;
	struct obs_scene *scene;
	struct calldata params = {0};
	obs_source_t *parent_source = obs_input_get_parent(item);

	command = "reorder";
	scene = obs_scene_from_source(parent_source);
	if (!scene)
		return;

	calldata_set_ptr(&params, "scene", scene);

	signal_handler_signal(parent_source->context.signals, command, &params);

	calldata_free(&params);
}

void obs_scene_set_order(obs_input_t *input, enum obs_order_movement movement)
{
	struct obs_scene *scene;
	size_t idx;

	if (!input)
		return;

	scene = obs_scene_from_source(obs_input_get_parent(input));
	if (!scene)
		return;

	obs_scene_addref(scene);
	pthread_mutex_lock(&scene->mutex);

	idx = da_find(scene->items, &input, 0);

	if (idx == DARRAY_INVALID) {
		pthread_mutex_unlock(&scene->mutex);
		obs_scene_release(scene);
		return;
	}

	if (movement == OBS_ORDER_MOVE_DOWN) {
		if (idx != 0)
			da_swap(scene->items, idx, idx - 1);

	} else if (movement == OBS_ORDER_MOVE_UP) {
		if (idx != scene->items.num - 1)
			da_swap(scene->items, idx, idx + 1);

	} else if (movement == OBS_ORDER_MOVE_TOP) {
		if (idx != scene->items.num - 1) {
			da_erase(scene->items, idx);
			da_push_back(scene->items, &input);
		}

	} else if (movement == OBS_ORDER_MOVE_BOTTOM) {
		if (idx != 0) {
			da_erase(scene->items, idx);
			da_insert(scene->items, 0, &input);
		}
	}

	signal_reorder(input);

	pthread_mutex_unlock(&scene->mutex);
	obs_scene_release(scene);
}

void obs_scene_set_order_position(obs_input_t *input, int position)
{
	struct obs_scene *scene;

	if (!input)
		return;

	scene = obs_scene_from_source(obs_input_get_parent(input));
	if (!scene)
		return;

	obs_scene_addref(scene);
	pthread_mutex_lock(&scene->mutex);

	da_erase_item(scene->items, input);
	da_insert(scene->items, position, &input);

	signal_reorder(input);

	pthread_mutex_unlock(&scene->mutex);
	obs_scene_release(scene);
}

static bool sceneitems_match(obs_scene_t *scene, obs_input_t *const *items,
		size_t size, bool *order_matches)
{
	for (size_t i = 0; i < scene->items.num && i < size; i++) {
		if (items[i] != scene->items.array[i]) {
			*order_matches = false;
			break;
		}
	}

	return scene->items.num == size;
}

bool obs_scene_reorder_items(obs_scene_t *scene,
		obs_input_t *const *item_order, size_t item_order_size)
{
	if (!scene || !item_order_size)
		return false;

	obs_scene_addref(scene);
	pthread_mutex_lock(&scene->mutex);

	bool order_matches = true;
	if (!sceneitems_match(scene, item_order, item_order_size,
				&order_matches) || order_matches) {
		pthread_mutex_unlock(&scene->mutex);
		obs_scene_release(scene);
		return false;
	}

	da_copy_array(scene->items, item_order, item_order_size);
	signal_reorder(item_order[0]);

	pthread_mutex_unlock(&scene->mutex);
	obs_scene_release(scene);
	return true;
}

void obs_scene_atomic_update(obs_scene_t *scene,
		obs_scene_atomic_update_func func, void *data)
{
	if (!scene)
		return;

	obs_scene_addref(scene);
	pthread_mutex_lock(&scene->mutex);
	func(data, scene);
	pthread_mutex_unlock(&scene->mutex);
	obs_scene_release(scene);
}
