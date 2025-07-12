/* gui_image.c - GUI component to display an image.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008,2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/gui.h>
#include <grub/time.h>
#include <grub/gui_string_util.h>
#include <grub/bitmap.h>
#include <grub/bitmap_scale.h>

struct grub_gui_image
{
  struct grub_gui_component component;

  grub_gui_container_t parent;
  grub_video_rect_t bounds;
  char *id;
  char *theme_dir;
  struct grub_video_bitmap *raw_bitmap;
  struct grub_video_bitmap *bitmap;
};

typedef struct grub_gui_image *grub_gui_image_t;

struct scaled_image
{
  struct grub_video_bitmap *bitmap;
  struct grub_video_bitmap *raw_bitmap;
};

struct grub_gui_animated_image // extends image
{
  struct grub_gui_image image;

  // frame information
  int frame_count;
  int frame_duration_ms;
  struct scaled_image* frame_bitmaps;

  // draw status
  grub_uint64_t first_draw_ms;
};

typedef struct grub_gui_animated_image* grub_gui_animated_image_t;

static void
image_destroy (void *vself)
{
  grub_gui_image_t self = vself;

  /* Free the scaled bitmap, unless it's a reference to the raw bitmap.  */
  if (self->bitmap && (self->bitmap != self->raw_bitmap))
    grub_video_bitmap_destroy (self->bitmap);
  if (self->raw_bitmap)
    grub_video_bitmap_destroy (self->raw_bitmap);

  grub_free (self);
}

static const char *
image_get_id (void *vself)
{
  grub_gui_image_t self = vself;
  return self->id;
}

static int
image_is_instance (void *vself __attribute__((unused)), const char *type)
{
  return grub_strcmp (type, "component") == 0;
}

static void
image_paint (void *vself, const grub_video_rect_t *region)
{
  grub_gui_image_t self = vself;
  grub_video_rect_t vpsave;

  if (! self->bitmap)
    return;
  if (!grub_video_have_common_points (region, &self->bounds))
    return;

  grub_gui_set_viewport (&self->bounds, &vpsave);
  grub_video_blit_bitmap (self->bitmap, GRUB_VIDEO_BLIT_BLEND,
                          0, 0, 0, 0,
                          grub_video_bitmap_get_width (self->bitmap),
                          grub_video_bitmap_get_height (self->bitmap));
  grub_gui_restore_viewport (&vpsave);
}

static void
image_set_parent (void *vself, grub_gui_container_t parent)
{
  grub_gui_image_t self = vself;
  self->parent = parent;
}

static grub_gui_container_t
image_get_parent (void *vself)
{
  grub_gui_image_t self = vself;
  return self->parent;
}

static grub_err_t
rescale_image (grub_gui_image_t self)
{
  signed width;
  signed height;

  if (! self->raw_bitmap)
    {
      if (self->bitmap)
        {
          grub_video_bitmap_destroy (self->bitmap);
          self->bitmap = 0;
        }
      return grub_errno;
    }

  width = self->bounds.width;
  height = self->bounds.height;

  if (self->bitmap
      && ((signed) grub_video_bitmap_get_width (self->bitmap) == width)
      && ((signed) grub_video_bitmap_get_height (self->bitmap) == height))
    {
      /* Nothing to do; already the right size.  */
      return grub_errno;
    }

  /* Free any old scaled bitmap,
     *unless* it's a reference to the raw bitmap.  */
  if (self->bitmap && (self->bitmap != self->raw_bitmap))
    grub_video_bitmap_destroy (self->bitmap);

  self->bitmap = 0;

  /* Create a scaled bitmap, unless the requested size is the same
     as the raw size -- in that case a reference is made.  */
  if ((signed) grub_video_bitmap_get_width (self->raw_bitmap) == width
      && (signed) grub_video_bitmap_get_height (self->raw_bitmap) == height)
    {
      self->bitmap = self->raw_bitmap;
      return grub_errno;
    }

  /* Don't scale to an invalid size.  */
  if (width <= 0 || height <= 0)
    return grub_errno;

  /* Create the scaled bitmap.  */
  grub_video_bitmap_create_scaled (&self->bitmap,
                                   width,
                                   height,
                                   self->raw_bitmap,
                                   GRUB_VIDEO_BITMAP_SCALE_METHOD_BEST);
  return grub_errno;
}

static void
image_set_bounds (void *vself, const grub_video_rect_t *bounds)
{
  grub_gui_image_t self = vself;
  self->bounds = *bounds;
  rescale_image (self);
}

static void
image_get_bounds (void *vself, grub_video_rect_t *bounds)
{
  grub_gui_image_t self = vself;
  *bounds = self->bounds;
}

/* FIXME: inform rendering system it's not forced minimum.  */
static void
image_get_minimal_size (void *vself, unsigned *width, unsigned *height)
{
  grub_gui_image_t self = vself;

  if (self->raw_bitmap)
    {
      *width = grub_video_bitmap_get_width (self->raw_bitmap);
      *height = grub_video_bitmap_get_height (self->raw_bitmap);
    }
  else
    {
      *width = 0;
      *height = 0;
    }
}

static grub_err_t
load_image (grub_gui_image_t self, const char *path)
{
  struct grub_video_bitmap *bitmap;
  if (grub_video_bitmap_load (&bitmap, path) != GRUB_ERR_NONE)
    return grub_errno;

  if (self->bitmap && (self->bitmap != self->raw_bitmap))
    grub_video_bitmap_destroy (self->bitmap);
  if (self->raw_bitmap)
    grub_video_bitmap_destroy (self->raw_bitmap);

  /*
   * Either self->bitmap is being freed or it shares memory with
   * self->raw_bitmap which is being freed. To ensure self->bitmap doesn't
   * point to memory that has been freed, we can set it to NULL.
   */
  self->bitmap = NULL;
  self->raw_bitmap = bitmap;
  return rescale_image (self);
}

static grub_err_t
image_set_property (void *vself, const char *name, const char *value)
{
  grub_gui_image_t self = vself;
  if (grub_strcmp (name, "theme_dir") == 0)
    {
      grub_free (self->theme_dir);
      self->theme_dir = grub_strdup (value);
    }
  else if (grub_strcmp (name, "file") == 0)
    {
      char *absvalue;
      grub_err_t err;

      /* Resolve to an absolute path.  */
      if (! self->theme_dir)
	return grub_error (GRUB_ERR_BUG, "unspecified theme_dir");
      absvalue = grub_resolve_relative_path (self->theme_dir, value);
      if (! absvalue)
	return grub_errno;

      err = load_image (self, absvalue);
      grub_free (absvalue);

      return err;
    }
  else if (grub_strcmp (name, "id") == 0)
    {
      grub_free (self->id);
      if (value)
        self->id = grub_strdup (value);
      else
        self->id = 0;
    }
  return grub_errno;
}

static struct grub_gui_component_ops image_ops =
{
  .destroy = image_destroy,
  .get_id = image_get_id,
  .is_instance = image_is_instance,
  .paint = image_paint,
  .set_parent = image_set_parent,
  .get_parent = image_get_parent,
  .set_bounds = image_set_bounds,
  .get_bounds = image_get_bounds,
  .get_minimal_size = image_get_minimal_size,
  .set_property = image_set_property
};

grub_gui_component_t
grub_gui_image_new (void)
{
  grub_gui_image_t image;
  image = grub_zalloc (sizeof (*image));
  if (! image)
    return 0;
  image->component.ops = &image_ops;
  return (grub_gui_component_t) image;
}

static void
animated_image_destroy_frames(grub_gui_animated_image_t vself)
{
  grub_gui_animated_image_t self = vself;
  struct grub_video_bitmap *bitmap;
  struct grub_video_bitmap *raw_bitmap;
  int index;

  if (self->frame_bitmaps)
  {
    for (index = 0; index < self->frame_count; index++)
    {
      bitmap = self->frame_bitmaps[index].bitmap;
      raw_bitmap = self->frame_bitmaps[index].raw_bitmap;
      if (bitmap && bitmap != raw_bitmap)
      {
        grub_video_bitmap_destroy (bitmap);
      }
      if (raw_bitmap)
      {
        grub_video_bitmap_destroy (raw_bitmap);
      }
    }
    grub_free(self->frame_bitmaps);
  }

  self->frame_count = 0;
  self->frame_bitmaps = NULL;
}

static void
animated_image_destroy(void* vself)
{
  grub_gui_animated_image_t self = vself;
  animated_image_destroy_frames(self);
  grub_free(self);
}

static void
animated_image_paint (void *vself, const grub_video_rect_t *region)
{
  grub_gui_animated_image_t self = vself;
  grub_uint64_t time = grub_get_time_ms();
  grub_uint64_t bitmap_index = 0;
  // switch to current image
  if (self->frame_bitmaps)
  {
    if (self->first_draw_ms == 0)
    {
      self->first_draw_ms = time;
    }
    bitmap_index = grub_divmod64(time - self->first_draw_ms, self->frame_duration_ms, NULL);
    grub_divmod64(bitmap_index, self->frame_count, &bitmap_index);

    self->image.bitmap = self->frame_bitmaps[bitmap_index].bitmap;
    self->image.raw_bitmap = self->frame_bitmaps[bitmap_index].raw_bitmap;
    image_paint(vself, region);

    if (true)
    {
      char buffer[32];
      grub_snprintf(buffer, sizeof(buffer), "frame_%ld", bitmap_index);
      grub_font_draw_string(buffer,
                            grub_font_get("Unknown Regular 16"),
                            grub_video_map_rgb(0, 255, 255),
                            (int)(self->image.bounds.x),
                            (int)(self->image.bounds.y + self->image.bounds.height - 16));
    }

    // restore
    self->image.bitmap = self->image.raw_bitmap = NULL;
  }
}

static grub_err_t animated_image_load_frames(grub_gui_animated_image_t self, const char* value)
{
  // set file property, eg: animation_frames*.png
  char* abspattern = NULL;
  char* abspath = NULL;
  int abspathlen;
  char* prefix = NULL;
  char* suffix;
  char* star;
  int index;
  grub_err_t err = GRUB_ERR_NONE;

  /* Resolve to an absolute path.  */
  if (!self->image.theme_dir)
  {
    return grub_error(GRUB_ERR_BUG, "unspecified theme_dir");
  }
  if (self->frame_count <= 0)
  {
    return grub_error(GRUB_ERR_BUG, "unspecified frame_count");
  }
  if (self->frame_duration_ms <= 0)
  {
    return grub_error(GRUB_ERR_BUG, "unspecified frame_duration_ms or invalid data");
  }
  abspattern = grub_resolve_relative_path(self->image.theme_dir, value);
  if (!abspattern)
  {
    return grub_error(GRUB_ERR_BUG, "invalid theme_dir for animated_image");
  }

  star = grub_strchr(abspattern, '*');
  if (!star)
  {
    err = grub_error(GRUB_ERR_BAD_ARGUMENT,
                     "missing `*' in frame_animation file pattern `%s'", abspattern);
    goto fail;
  }

  /* Prefix: Get the part before the '*'.  */
  prefix = grub_malloc(star - abspattern + 1);
  if (!prefix)
  {
    err = grub_errno;
    goto fail;
  }

  grub_memcpy(prefix, abspattern, star - abspattern);
  prefix[star - abspattern] = '\0';

  /* Suffix:  Everything after the '*' is the suffix.  */
  suffix = star + 1;

  abspathlen = (int)grub_strlen(abspattern) + 15; // enough for 4G number
  abspath = grub_calloc(abspathlen, sizeof(char));
  if (!abspath)
  {
    err = grub_errno;
    goto fail;
  }

  self->frame_bitmaps = grub_calloc(self->frame_count, sizeof (*self->frame_bitmaps));
  if (!self->frame_bitmaps)
  {
    err = grub_errno;
    goto fail;
  }
  for (index = 0; index < self->frame_count; index++)
  {
    grub_snprintf(abspath, abspathlen, "%s%d%s", prefix, index, suffix);
    self->image.bitmap = self->image.raw_bitmap = NULL;
    err = load_image(&self->image, abspath);

    if (err != GRUB_ERR_NONE)
    {
      err = grub_error(err, "failed to load image at path %s", abspath);
      goto fail;
    }
    self->frame_bitmaps[index].bitmap = self->image.bitmap;
    self->frame_bitmaps[index].raw_bitmap = self->image.raw_bitmap;
  }
  goto success;

fail:
  animated_image_destroy_frames(self);

success:
  if (abspattern)
    grub_free(abspattern);
  if (abspath)
    grub_free(abspath);
  if (prefix)
    grub_free(prefix);

  // restore
  self->image.bitmap = self->image.raw_bitmap = NULL;
  return err;
}

static grub_err_t
animated_image_set_property(void* vself, const char* name, const char* value)
{
  grub_gui_animated_image_t self = vself;

  if (grub_strcmp(name, "frame_count") == 0)
  {
    self->frame_count = grub_strtoul(value, NULL, 10);
  }
  else if (grub_strcmp(name, "frame_duration_ms") == 0)
  {
    self->frame_duration_ms = grub_strtoul(value, NULL, 10);
  }
  else if (grub_strcmp(name, "file") == 0)
  {
    return animated_image_load_frames(self, value);
  }
  return image_set_property(vself, name, value);
}

static void
animated_image_set_bounds (void *vself, const grub_video_rect_t *bounds)
{
  grub_gui_animated_image_t self = vself;
  int index = 0;
  self->image.bounds = *bounds;
  if (self->frame_bitmaps)
  {
    for (index = 0; index < self->frame_count; index++)
    {
      self->image.bitmap = self->frame_bitmaps[index].bitmap;
      self->image.raw_bitmap = self->frame_bitmaps[index].raw_bitmap;

      rescale_image(&self->image);

      self->frame_bitmaps[index].bitmap = self->image.bitmap;
      self->frame_bitmaps[index].raw_bitmap = self->image.raw_bitmap;
    }
    self->image.bitmap = self->image.raw_bitmap = NULL;
  }
}
static void
animated_image_get_minimal_size (void *vself, unsigned *width, unsigned *height)
{
  grub_gui_animated_image_t self = vself;
  if (self->frame_bitmaps)
  {
    self->image.bitmap = self->frame_bitmaps[0].bitmap;
    self->image.raw_bitmap = self->frame_bitmaps[0].raw_bitmap;
    image_get_minimal_size(&self->image, width, height);
    self->image.bitmap = self->image.raw_bitmap = NULL;
  }
  else
  {
    *width = *height = 0;
  }
}

static int
animated_image_is_instance (void *vself __attribute__((unused)), const char *type)
{
  return grub_strcmp (type, "animated_image") == 0;
}

static struct grub_gui_component_ops animated_image_ops =
{
  .destroy = animated_image_destroy,
  .get_id = image_get_id,
  .is_instance = animated_image_is_instance,
  .paint = animated_image_paint,
  .set_parent = image_set_parent,
  .get_parent = image_get_parent,
  .set_bounds = animated_image_set_bounds,
  .get_bounds = image_get_bounds,
  .get_minimal_size = animated_image_get_minimal_size,
  .set_property = animated_image_set_property,
};

grub_gui_component_t
grub_gui_animated_image_new(void)
{
  grub_gui_animated_image_t image;
  image = grub_zalloc(sizeof(*image));
  if (!image)
    return 0;
  image->image.component.ops = &animated_image_ops;
  return (grub_gui_component_t)image;
}
