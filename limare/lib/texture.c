/*
 * Copyright (c) 2012-2013 Luc Verhaegen <libv@skynet.be>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>

#include "limare.h"
#include "texture.h"
#include "formats.h"

/*
 * Below is a space filler algorithm that is spatially optimized which makes
 * life a lot easier for the memory subsytem, and also makes for easier
 * mipmapping.
 *
 * At first glance, it resembles the hilbert curve, but this is not true. It
 * is a simplified calculation of the hilbert curve which does not rotate
 * subsequent levels. It has similar spatial properties though.
 *
 * These indices are generated by the following code:
 *
 *  index = 0;
 *  index |= (i & 0x8) << 3;
 *  index |= (i & 0x4) << 2;
 *  index |= (i & 0x2) << 1;
 *  index |= (i & 0x1) << 0;
 *
 * Basically spacing out individual bits.
 */
static int
space_filler_indices[16] = {
	0x00, /* 0 */
	0x01, /* 1 */
	0x04, /* 2 */
	0x05, /* 3 */
	0x10, /* 4 */
	0x11, /* 5 */
	0x14, /* 6 */
	0x15, /* 7 */
	0x40, /* 8 */
	0x41, /* 9 */
	0x44, /* A */
	0x45, /* B */
	0x50, /* C */
	0x51, /* D */
	0x54, /* E */
	0x55, /* F */
};

static int
space_filler_index(int x, int y)
{
	return space_filler_indices[y ^ x] | (space_filler_indices[y] << 1);
}

static void
texture_24_swizzle(struct texture *texture, const unsigned char *pixels)
{
	int block_x, block_y, block_pitch;
	int x, y, rem_x, rem_y, index, source_pitch;;
	const unsigned char *source;
	unsigned char *dest;

	block_pitch = ALIGN(texture->width, 16) >> 4;
	source_pitch = ALIGN(texture->width * 3, 4);

	for (y = 0; y < texture->height; y++) {
		block_y = y >> 4;
		rem_y = y & 0x0F;

		for (x = 0; x < texture->width; x++) {
			block_x = x >> 4;
			rem_x = x & 0x0F;

			index = space_filler_index(rem_x, rem_y);

			source = &pixels[y * source_pitch + 3 * x];
			dest = texture->level[0].dest;
			dest += (3 * 256) * (block_y * block_pitch + block_x);
			dest += 3 * index;

			dest[0] = source[0];
			dest[1] = source[1];
			dest[2] = source[2];
		}
	}
}

/*
 * This code does not fully produce the same results as the ARM binary driver.
 * The ARM binary driver has a different algorithm, and for some reason, when
 * there is an uneven width and/or height at the previous level, and when width
 * and/or height is below 8, only half the pixels at the edge, in each uneven
 * dimension, are averaged. This seems quite incorrect behaviour, as all
 * required upper level pixels are still available, and only the next average
 * would have half the data available, but this next average is never taken.
 *
 * It seems like the phalanx or ARM engineers tried to average 3x2, 2x3 or 3x3
 * pixels respectively, but were slightly off and now only average 1x2, 2x1 or
 * 1x1 pixels.
 *
 * Our code averages the 2x2 upper level pixels at edges.
 */
static void
texture_24_mipmap(struct texture_level *dst, struct texture_level *src)
{
	int x, y, dx, dy, offset;
	int block_x, block_y, block_pitch;
	int source_x, source_y, source_pitch; /* in blocks */
	unsigned char *source;
	unsigned char *dest;

	block_pitch = ALIGN(dst->width, 16) >> 4;
	source_pitch = ALIGN(src->width, 16) >> 4;

	if (src->width == 1) {
		for (y = 0; y < dst->height; y++) {
			block_y = y >> 4;
			dy = y & 0x0F;
			source_y = y >> 3;

			offset = space_filler_index(0, dy);

			dest = dst->dest;
			dest += (3 * 256) * (block_pitch * block_y);
			dest += 3 * offset;

			source = src->dest;
			source += (3 * 256) * (source_pitch * source_y);
			source += 3 * ((offset << 2) & 0xFF);

			dest[0] = (source[0] + source[9]) / 2;
			dest[1] = (source[1] + source[10]) / 2;
			dest[2] = (source[2] + source[11]) / 2;
		}
	} else if (src->height == 1) {
		for (x = 0; x < dst->width; x++) {
			block_x = x >> 4;
			dx = x & 0x0F;
			source_x = x >> 3;

			offset = space_filler_index(dx, 0);

			dest = dst->dest;
			dest += (3 * 256) * block_x;
			dest += 3 * offset;

			source = src->dest;
			source += (3 * 256) * source_x;
			source += 3 * ((offset << 2) & 0xFF);

			dest[0] = (source[0] + source[3]) / 2;
			dest[1] = (source[1] + source[4]) / 2;
			dest[2] = (source[2] + source[5]) / 2;
		}
	} else {
		for (y = 0; y < dst->height; y++) {
			block_y = y >> 4;
			dy = y & 0x0F;
			source_y = y >> 3;

			for (x = 0; x < dst->width; x++) {
				block_x = x >> 4;
				dx = x & 0x0F;
				source_x = x >> 3;

				offset = space_filler_index(dx, dy);

				dest = dst->dest;
				dest += (3 * 256) *
					(block_pitch * block_y + block_x);
				dest += 3 * offset;

				source = src->dest;
				source += (3 * 256) *
					(source_pitch * source_y + source_x);
				source += 3 * ((offset << 2) & 0xFF);

				dest[0] = (source[0] + source[3] +
					   source[6] + source[9]) / 4;
				dest[1] = (source[1] + source[4] +
					   source[7] + source[10]) / 4;
				dest[2] = (source[2] + source[5] +
					   source[8] + source[11]) / 4;
			}
		}
	}
}

static int
texture_24_create(struct limare_state *state, struct texture *texture,
		  const void *src)
{
	struct texture_level *level;
	int i, size = 0;

	for (i = 0; i < texture->levels; i++) {
		int width, height, pitch;
		level = &texture->level[i];

		level->level = i;

		level->width = texture->width >> i;
		level->height = texture->height >> i;
		if (!level->width)
			level->width = 1;
		if (!level->height)
			level->height = 1;

		width = ALIGN(level->width, 16);
		height = ALIGN(level->height, 16);
		pitch = ALIGN(width * 3, 4);
		level->size = ALIGN(pitch * height, 0x400);
		size += level->size;
	}

	if ((state->aux_mem_size - state->aux_mem_used) < size) {
		printf("%s: size (0x%X) exceeds available size (0x%X)\n",
		       __func__, size,
		       state->aux_mem_size - state->aux_mem_used);
		return -1;
	}

	for (i = 0; i < texture->levels; i++) {
		level = &texture->level[i];

		level->dest = state->aux_mem_address + state->aux_mem_used;
		level->mem_physical =
			state->aux_mem_physical + state->aux_mem_used;
		state->aux_mem_used += level->size;
	}

	texture_24_swizzle(texture, src);

	for (i = 1; i < texture->levels; i++)
		texture_24_mipmap(&texture->level[i], &texture->level[i - 1]);

	return 0;
}

static void
texture_descriptor_level_attach(struct texture *texture, int i)
{
	struct texture_level *level = &texture->level[i];

	switch (i) {
	case 0:
		texture->descriptor[6] &= ~0xC0000000;
		texture->descriptor[6] |= level->mem_physical << 24;
		texture->descriptor[7] &= ~0x00FFFFFF;
		texture->descriptor[7] |= level->mem_physical >> 8;
		return;
	case 1:
		texture->descriptor[7] &= ~0xFF000000;
		texture->descriptor[7] |= level->mem_physical << 18;
		texture->descriptor[8] &= ~0x0003FFFF;
		texture->descriptor[8] |= level->mem_physical >> 14;
		return;
	case 2:
		texture->descriptor[8] &= ~0xFFFC0000;
		texture->descriptor[8] |= level->mem_physical << 12;
		texture->descriptor[9] &= ~0x00000FFF;
		texture->descriptor[9] |= level->mem_physical >> 20;
		return;
	case 3:
		texture->descriptor[9] &= ~0xFFFFF000;
		texture->descriptor[9] |= level->mem_physical << 6;
		texture->descriptor[10] &= ~0x0000003F;
		texture->descriptor[10] |= level->mem_physical >> 26;
		return;
	case 4:
		texture->descriptor[10] &= ~0xFFFFFFC0;
		texture->descriptor[10] |= level->mem_physical;
		return;
	case 5:
		texture->descriptor[11] &= ~0x03FFFFFF;
		texture->descriptor[11] |= level->mem_physical >> 6;
		return;
	case 6:
		texture->descriptor[11] &= ~0xFC000000;
		texture->descriptor[11] |= level->mem_physical << 20;
		texture->descriptor[12] &= ~0x000FFFFF;
		texture->descriptor[12] |= level->mem_physical >> 12;
		return;
	case 7:
		texture->descriptor[12] &= ~0xFFF00000;
		texture->descriptor[12] |= level->mem_physical << 14;
		texture->descriptor[13] &= ~0x00003FFF;
		texture->descriptor[13] |= level->mem_physical >> 18;
		return;
	case 8:
		texture->descriptor[13] &= ~0xFFFFC000;
		texture->descriptor[13] |= level->mem_physical << 8;
		texture->descriptor[14] &= ~0x000000FF;
		texture->descriptor[14] |= level->mem_physical >> 24;
		return;
	case 9:
		texture->descriptor[14] &= ~0xFFFFFF00;
		texture->descriptor[14] |= level->mem_physical << 2;
		texture->descriptor[15] &= ~0x03;
		texture->descriptor[15] |= level->mem_physical >> 30;
		return;
	case 10:
		texture->descriptor[15] &= ~0x0FFFFFFC;
		texture->descriptor[15] |= level->mem_physical >> 4;
		return;
	case 11:
	case 12:
		/*
		 * implied: these 2x2 and 1x1 mipmaps should be following
		 * 4x4 directly at 0x400 offsets.
		 */
		return;
	default:
		printf("%s: level %d not implemented yet.\n", __func__, i);
		return;
	}
}

static void
texture_descriptor_levels_attach(struct texture *texture)
{
	int i;

	for (i = 0; i < texture->levels; i++)
		texture_descriptor_level_attach(texture, i);
}

struct texture *
texture_create(struct limare_state *state, const void *src,
	       int width, int height, int format, int mipmap)
{
	struct texture *texture = calloc(1, sizeof(struct texture));
	int flag0 = 0, flag1 = 1, layout = 0;

	if ((width > 4096) || (height > 4096)) {
		free(texture);
		return NULL;
	}

	texture->width = width;
	texture->height = height;
	texture->format = format;

	if (mipmap) {
		int max, i;

		if (width > height)
			max = width;
		else
			max = height;

		for (i = 0; max >> i; i++)
			;

		texture->levels = i;
	} else
		texture->levels = 1;

	switch (texture->format) {
	// case LIMA_TEXEL_FORMAT_RGB_555:
	// case LIMA_TEXEL_FORMAT_RGBA_5551:
	// case LIMA_TEXEL_FORMAT_RGBA_4444:
	// case LIMA_TEXEL_FORMAT_LA_88:
	case LIMA_TEXEL_FORMAT_RGB_888:
		if (texture_24_create(state, texture, src)) {
			free(texture);
			return NULL;
		}

		flag0 = 1;
		flag1 = 0;
		layout = 3;
		break;
	// case LIMA_TEXEL_FORMAT_RGBA_8888:
	// case LIMA_TEXEL_FORMAT_BGRA_8888:
	// case LIMA_TEXEL_FORMAT_RGBA64:
	// case LIMA_TEXEL_FORMAT_DEPTH_STENCIL_32:
	default:
		free(texture);
		printf("%s: unsupported format %x\n", __func__, format);
		return NULL;
	}

	texture->descriptor[0] = (flag0 << 7) | (flag1 << 6) | format;
	/* assume both min and mag filter are linear */
	/* assume that we are not a cubemap */
	texture->descriptor[1] = 0x00000400;
	texture->descriptor[2] = width << 22;
	texture->descriptor[3] = 0x10000 | (height << 3) | (width >> 10);
	texture->descriptor[6] = layout << 13;

	texture_descriptor_levels_attach(texture);

	return texture;
}
