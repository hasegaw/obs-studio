/******************************************************************************
    Copyright (C) 2015 by Hugh Bailey <obs.jim@gmail.com>

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

#include "graphics/vec2.h"
#include "graphics/matrix4.h"
#include "obs-internal.h"

struct obs_input {
	volatile long                 ref;
	volatile bool                 removed;

	obs_data_t                    *parent_settings;
	obs_data_t                    *private_settings;
	void                          *parent_data;
	void                          *private_data;
	obs_input_destroy_data_t      destroy_parent_data;
	obs_input_destroy_data_t      destroy_private_data;
  
  	struct obs_source             *parent;
  	struct obs_source             *source;
	bool                          visible;

	struct vec2                   pos;
	struct vec2                   scale;
	float                         rot;
	uint32_t                      align;

	/* last width/height of the source, this is used to check whether
	 * ths transform needs updating */
	uint32_t                      last_width;
	uint32_t                      last_height;

	struct matrix4                box_transform;
	struct matrix4                draw_transform;

	enum obs_bounds_type          bounds_type;
	uint32_t                      bounds_align;
	struct vec2                   bounds;
};

obs_input_t *obs_input_create(obs_source_t *parent)
{
	obs_input_t *input = bzalloc(sizeof(*input));
	input->parent = parent;
	input->visible = true;
	input->ref = 1;
	input->align = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	vec2_set(&input->scale, 1.0f, 1.0f);
	matrix4_identity(&input->draw_transform);
	matrix4_identity(&input->box_transform);

	return input;
}

obs_input_t *obs_input_duplicate(obs_input_t *input, obs_source_t *parent)
{
	struct obs_source *source = input->source;
	obs_input_t *new_input = obs_input_create(parent);

	obs_source_addref(source);
	new_input->source = source;
	new_input->visible = input->visible;
	new_input->pos = input->pos;
	new_input->scale = input->scale;
	new_input->align = input->align;
	new_input->last_width = input->last_width;
	new_input->last_height = input->last_height;
	new_input->box_transform = input->box_transform;
	new_input->draw_transform = input->draw_transform;
	new_input->bounds_type = input->bounds_type;
	new_input->bounds_align = input->bounds_align;
	new_input->bounds = input->bounds;

	return new_input;
}

static void obs_input_destroy(obs_input_t *input)
{
	if (input->destroy_parent_data)
		input->destroy_parent_data(input->parent_data);
	if (input->destroy_private_data)
		input->destroy_private_data(input->private_data);
	obs_source_release(input->source);
	bfree(input);
}

void obs_input_addref(obs_input_t *input)
{
	if (input)
		os_atomic_inc_long(&input->ref);
}

void obs_input_release(obs_input_t *input)
{
	if (!input)
		return;

	if (os_atomic_dec_long(&input->ref) == 0)
		obs_input_destroy(input);
}

obs_input_t *obs_input_get_ref(obs_input_t *input)
{
	long refs = input->ref;
	while (refs > 0) {
		if (os_atomic_compare_swap_long(&input->ref, refs, refs + 1))
			return input;

		refs = input->ref;
	}
	return NULL;
}

void obs_input_detach(obs_input_t *input)
{
	if (obs_ptr_valid(input, "obs_input_detach")) {
		if (input->destroy_parent_data) {
			input->destroy_parent_data(input->parent_data);
			input->destroy_parent_data = NULL;
			input->parent_data = NULL;
		}

		if (input->parent && input->source)
			obs_source_remove_child(input->parent, input->source);

		input->parent = NULL;
		obs_input_release(input);
	}
}

bool obs_input_source_removed(const obs_input_t *input)
{
	if (!obs_ptr_valid(input, "obs_input_source_removed"))
		return false;
	if (input->source)
		return input->source->removed;
	return false;
}

obs_source_t *obs_input_get_parent(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_parent") ?
		input->parent : NULL;
}

void obs_input_set_source(obs_input_t *input, obs_source_t *source)
{
	if (!obs_ptr_valid(input, "obs_input_set_source"))
		return;
	if (input->source == source)
		return;

	if (input->source) {
		obs_source_remove_child(input->parent, input->source);
		obs_source_release(input->source);
	}

	if (source) {
		obs_source_addref(source);
		obs_source_add_child(input->parent, source);
	}
	input->source = source;
}

obs_source_t *obs_input_get_source(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_source") ?
		input->source : NULL;
}

static void add_alignment(struct vec2 *v, uint32_t align, int cx, int cy)
{
	if (align & OBS_ALIGN_RIGHT)
		v->x += (float)cx;
	else if ((align & OBS_ALIGN_LEFT) == 0)
		v->x += (float)(cx / 2);

	if (align & OBS_ALIGN_BOTTOM)
		v->y += (float)cy;
	else if ((align & OBS_ALIGN_TOP) == 0)
		v->y += (float)(cy / 2);
}

static void calculate_bounds_data(obs_input_t *input,
		struct vec2 *origin, struct vec2 *scale,
		uint32_t *cx, uint32_t *cy)
{
	float    width         = (float)(*cx) * fabsf(scale->x);
	float    height        = (float)(*cy) * fabsf(scale->y);
	float    input_aspect  = width / height;
	float    bounds_aspect = input->bounds.x / input->bounds.y;
	uint32_t bounds_type   = input->bounds_type;
	float    width_diff, height_diff;

	if (input->bounds_type == OBS_BOUNDS_MAX_ONLY)
		if (width > input->bounds.x || height > input->bounds.y)
			bounds_type = OBS_BOUNDS_SCALE_INNER;

	if (bounds_type == OBS_BOUNDS_SCALE_INNER ||
	    bounds_type == OBS_BOUNDS_SCALE_OUTER) {
		bool  use_width = (bounds_aspect < input_aspect);
		float mul;

		if (input->bounds_type == OBS_BOUNDS_SCALE_OUTER)
			use_width = !use_width;

		mul = use_width ?
			input->bounds.x / width :
			input->bounds.y / height;

		vec2_mulf(scale, scale, mul);

	} else if (bounds_type == OBS_BOUNDS_SCALE_TO_WIDTH) {
		vec2_mulf(scale, scale, input->bounds.x / width);

	} else if (bounds_type == OBS_BOUNDS_SCALE_TO_HEIGHT) {
		vec2_mulf(scale, scale, input->bounds.y / height);

	} else if (bounds_type == OBS_BOUNDS_STRETCH) {
		scale->x = input->bounds.x / (float)(*cx);
		scale->y = input->bounds.y / (float)(*cy);
	}

	width       = (float)(*cx) * scale->x;
	height      = (float)(*cy) * scale->y;
	width_diff  = input->bounds.x - width;
	height_diff = input->bounds.y - height;
	*cx         = (uint32_t)input->bounds.x;
	*cy         = (uint32_t)input->bounds.y;

	add_alignment(origin, input->bounds_align,
			(int)-width_diff, (int)-height_diff);
}

static void update_input_transform(obs_input_t *input)
{
	uint32_t        width         = obs_source_get_width(input->source);
	uint32_t        height        = obs_source_get_height(input->source);
	uint32_t        cx            = width;
	uint32_t        cy            = height;
	struct vec2     base_origin;
	struct vec2     origin;
	struct vec2     scale         = input->scale;
	struct calldata params        = {0};

	vec2_zero(&base_origin);
	vec2_zero(&origin);

	/* ----------------------- */

	if (input->bounds_type != OBS_BOUNDS_NONE) {
		calculate_bounds_data(input, &origin, &scale, &cx, &cy);
	} else {
		cx = (uint32_t)((float)cx * scale.x);
		cy = (uint32_t)((float)cy * scale.y);
	}

	add_alignment(&origin, input->align, (int)cx, (int)cy);

	matrix4_identity(&input->draw_transform);
	matrix4_scale3f(&input->draw_transform, &input->draw_transform,
			scale.x, scale.y, 1.0f);
	matrix4_translate3f(&input->draw_transform, &input->draw_transform,
			-origin.x, -origin.y, 0.0f);
	matrix4_rotate_aa4f(&input->draw_transform, &input->draw_transform,
			0.0f, 0.0f, 1.0f, RAD(input->rot));
	matrix4_translate3f(&input->draw_transform, &input->draw_transform,
			input->pos.x, input->pos.y, 0.0f);

	/* ----------------------- */

	if (input->bounds_type != OBS_BOUNDS_NONE) {
		vec2_copy(&scale, &input->bounds);
	} else {
		scale.x = (float)width  * input->scale.x;
		scale.y = (float)height * input->scale.y;
	}

	add_alignment(&base_origin, input->align, (int)scale.x, (int)scale.y);

	matrix4_identity(&input->box_transform);
	matrix4_scale3f(&input->box_transform, &input->box_transform,
			scale.x, scale.y, 1.0f);
	matrix4_translate3f(&input->box_transform, &input->box_transform,
			-base_origin.x, -base_origin.y, 0.0f);
	matrix4_rotate_aa4f(&input->box_transform, &input->box_transform,
			0.0f, 0.0f, 1.0f, RAD(input->rot));
	matrix4_translate3f(&input->box_transform, &input->box_transform,
			input->pos.x, input->pos.y, 0.0f);

	/* ----------------------- */

	input->last_width  = width;
	input->last_height = height;

	calldata_set_ptr(&params, "scene", input->parent);
	calldata_set_ptr(&params, "item", input);
	signal_handler_signal(input->parent->context.signals,
			"item_transform", &params);
	calldata_free(&params);
}

void obs_input_set_pos(obs_input_t *input, const struct vec2 *pos)
{
	if (obs_ptr_valid(input, "obs_input_set_pos")) {
		vec2_copy(&input->pos, pos);
		update_input_transform(input);
	}
}

void obs_input_set_rot(obs_input_t *input, float rot)
{
	if (obs_ptr_valid(input, "obs_input_set_rot")) {
		input->rot = rot;
		update_input_transform(input);
	}
}

void obs_input_set_scale(obs_input_t *input, const struct vec2 *scale)
{
	if (obs_ptr_valid(input, "obs_input_set_scale")) {
		vec2_copy(&input->scale, scale);
		update_input_transform(input);
	}
}

void obs_input_set_alignment(obs_input_t *input, uint32_t alignment)
{
	if (obs_ptr_valid(input, "obs_input_set_alignment")) {
		input->align = alignment;
		update_input_transform(input);
	}
}

void obs_input_set_bounds_type(obs_input_t *input,
		enum obs_bounds_type type)
{
	if (obs_ptr_valid(input, "obs_input_set_bounds_type")) {
		input->bounds_type = type;
		update_input_transform(input);
	}
}

void obs_input_set_bounds_alignment(obs_input_t *input,
		uint32_t alignment)
{
	if (obs_ptr_valid(input, "obs_input_set_bounds_alignment")) {
		input->bounds_align = alignment;
		update_input_transform(input);
	}
}

void obs_input_set_bounds(obs_input_t *input, const struct vec2 *bounds)
{
	if (obs_ptr_valid(input, "obs_input_set_bounds")) {
		input->bounds = *bounds;
		update_input_transform(input);
	}
}

void obs_input_get_pos(const obs_input_t *input, struct vec2 *pos)
{
	if (obs_ptr_valid(input, "obs_input_get_pos"))
		vec2_copy(pos, &input->pos);
}

float obs_input_get_rot(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_rot") ?
		input->rot : 0.0f;
}

void obs_input_get_scale(const obs_input_t *input, struct vec2 *scale)
{
	if (obs_ptr_valid(input, "obs_input_get_scale"))
		vec2_copy(scale, &input->scale);
}

uint32_t obs_input_get_alignment(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_alignment") ?
		input->align : 0;
}

enum obs_bounds_type obs_input_get_bounds_type(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_bounds_type") ?
		input->bounds_type : OBS_BOUNDS_NONE;
}

uint32_t obs_input_get_bounds_alignment(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_bounds_alignment") ?
		input->bounds_align : 0;
}

void obs_input_get_bounds(const obs_input_t *input, struct vec2 *bounds)
{
	if (obs_ptr_valid(input, "obs_input_get_bounds"))
		*bounds = input->bounds;
}

void obs_input_get_transform_info(const obs_input_t *input,
		struct obs_transform_info *info)
{
	if (!obs_ptr_valid(input, "obs_input_get_transform_info"))
		return;
	if (!obs_ptr_valid(info, "obs_input_get_transform_info"))
		return;

	info->pos              = input->pos;
	info->rot              = input->rot;
	info->scale            = input->scale;
	info->alignment        = input->align;
	info->bounds_type      = input->bounds_type;
	info->bounds_alignment = input->bounds_align;
	info->bounds           = input->bounds;
}

void obs_input_set_transform_info(obs_input_t *input,
		const struct obs_transform_info *info)
{
	if (!obs_ptr_valid(input, "obs_input_set_transform_info"))
		return;
	if (!obs_ptr_valid(info, "obs_input_set_transform_info"))
		return;

	input->pos          = info->pos;
	input->rot          = info->rot;
	input->scale        = info->scale;
	input->align        = info->alignment;
	input->bounds_type  = info->bounds_type;
	input->bounds_align = info->bounds_alignment;
	input->bounds       = info->bounds;
	update_input_transform(input);
}

void obs_input_get_draw_transform(const obs_input_t *input,
		struct matrix4 *transform)
{
	if (obs_ptr_valid(input, "obs_input_get_draw_transform"))
		matrix4_copy(transform, &input->draw_transform);
}

void obs_input_get_box_transform(const obs_input_t *input,
		struct matrix4 *transform)
{
	if (obs_ptr_valid(input, "obs_input_get_box_transform"))
		matrix4_copy(transform, &input->box_transform);
}

void obs_input_set_parent_data(obs_input_t *input, void *data,
		obs_input_destroy_data_t destroy_func)
{
	if (obs_ptr_valid(input, "obs_input_set_parent_data")) {
		input->parent_data = data;
		input->destroy_parent_data = destroy_func;
	}
}

void *obs_input_get_parent_data(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_parent_data") ?
		input->private_data : NULL;
}

void obs_input_set_private_data(obs_input_t *input, void *data,
		obs_input_destroy_data_t destroy_func)
{
	if (obs_ptr_valid(input, "obs_input_set_private_data")) {
		input->private_data = data;
		input->destroy_private_data = destroy_func;
	}
}

void *obs_input_get_private_data(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_get_private_data") ?
		input->private_data : NULL;
}

bool obs_input_visible(const obs_input_t *input)
{
	return obs_ptr_valid(input, "obs_input_visible") ?
		input->visible : false;
}

void obs_input_set_visible(obs_input_t *input, bool visible)
{
	struct calldata cd = {0};

	if (!obs_ptr_valid(input, "obs_input_set_visible"))
		return;

	input->visible = visible;

	if (!input->parent)
		return;

	calldata_set_ptr(&cd, "scene", input->parent);
	calldata_set_ptr(&cd, "item", input);
	calldata_set_bool(&cd, "visible", visible);

	signal_handler_signal(input->parent->context.signals,
			"item_visible", &cd);

	calldata_free(&cd);
}

static inline bool source_size_changed(obs_input_t *item)
{
	uint32_t width  = obs_source_get_width(item->source);
	uint32_t height = obs_source_get_height(item->source);

	return item->last_width != width || item->last_height != height;
}

void obs_input_video_render(obs_input_t *input)
{
	if (!obs_ptr_valid(input, "obs_input_video_render"))
		return;

	if (source_size_changed(input))
		update_input_transform(input);

	if (input->visible) {
		gs_matrix_push();
		gs_matrix_mul(&input->draw_transform);
		obs_source_video_render(input->source);
		gs_matrix_pop();
	}
}

void obs_input_load(obs_input_t *input, obs_data_t *input_data)
{
	const char   *name = obs_data_get_string(input_data, "name");
	obs_source_t *source = obs_get_source_by_name(name);

	if (!source) {
		blog(LOG_WARNING, "[obs_input_load] Source %s not found!",
				name);
		return;
	}

	obs_data_set_default_int(input_data, "align",
			OBS_ALIGN_TOP | OBS_ALIGN_LEFT);

	input->rot     = (float)obs_data_get_double(input_data, "rot");
	input->align   = (uint32_t)obs_data_get_int(input_data, "align");
	input->visible = obs_data_get_bool(input_data, "visible");
	obs_data_get_vec2(input_data, "pos",    &input->pos);
	obs_data_get_vec2(input_data, "scale",  &input->scale);

	input->bounds_type =
		(enum obs_bounds_type)obs_data_get_int(input_data,
				"bounds_type");
	input->bounds_align =
		(uint32_t)obs_data_get_int(input_data, "bounds_align");
	obs_data_get_vec2(input_data, "bounds", &input->bounds);

	obs_input_set_source(input, source);
	obs_source_release(source);

	update_input_transform(input);
}

void obs_input_save(obs_input_t *input, obs_data_t *input_data)
{
	const char *name = obs_source_get_name(input->source);

	obs_data_set_string(input_data, "name", name);
	obs_data_set_vec2(input_data, "pos", &input->pos);
	obs_data_set_double(input_data, "rot", input->rot);
	obs_data_set_vec2(input_data, "scale", &input->scale);
	obs_data_set_bool(input_data, "visible", input->visible);
	obs_data_set_vec2(input_data, "bounds", &input->bounds);
	obs_data_set_int(input_data, "align", (int)input->align);
	obs_data_set_int(input_data, "bounds_type", (int)input->bounds_type);
	obs_data_set_int(input_data, "bounds_align", (int)input->bounds_align);
}
