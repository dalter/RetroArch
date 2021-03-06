/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <time.h>

#include "../config.def.h"

#include "menu.h"
#include "menu_display.h"
#include "../gfx/video_context_driver.h"
#include "../gfx/video_thread_wrapper.h"

typedef struct menu_framebuf
{
   uint16_t *data;
   unsigned width;
   unsigned height;
   size_t pitch;
   bool dirty;
} menu_framebuf_t;

static menu_display_t  menu_display_state;

static menu_framebuf_t frame_buf_state;

menu_display_t *menu_display_get_ptr(void)
{
   return &menu_display_state;
}

static menu_framebuf_t *menu_display_fb_get_ptr(void)
{
   return &frame_buf_state;
}



static void menu_display_fb_free(menu_framebuf_t *frame_buf)
{
   if (!frame_buf)
      return;

   if (frame_buf->data)
      free(frame_buf->data);
   frame_buf->data = NULL;
}

void menu_display_free(void)
{
   menu_display_t *disp = menu_display_get_ptr();
   if (!disp)
      return;

   if (disp->msg_queue)
      msg_queue_free(disp->msg_queue);
   disp->msg_queue = NULL;

   menu_animation_free();

   menu_display_fb_free(&frame_buf_state);
   memset(&frame_buf_state,    0, sizeof(menu_framebuf_t));
   memset(&menu_display_state, 0, sizeof(menu_display_t));
}

bool menu_display_init(void)
{
   menu_display_t *disp = menu_display_get_ptr();
   if (!disp)
      return false;

   rarch_assert(disp->msg_queue = msg_queue_new(8));

   return true;
}

bool menu_display_font_init_first(const void **font_driver,
      void **font_handle, void *video_data, const char *font_path,
      float font_size)
{
   settings_t *settings = config_get_ptr();
   const struct retro_hw_render_callback *hw_render =
      (const struct retro_hw_render_callback*)video_driver_callback();

   if (settings->video.threaded && !hw_render->context_type)
   {
      thread_packet_t pkt;
      driver_t *driver    = driver_get_ptr();
      thread_video_t *thr = (thread_video_t*)driver->video_data;

      if (!thr)
         return false;

      pkt.type                       = CMD_FONT_INIT;
      pkt.data.font_init.method      = font_init_first;
      pkt.data.font_init.font_driver = (const void**)font_driver;
      pkt.data.font_init.font_handle = font_handle;
      pkt.data.font_init.video_data  = video_data;
      pkt.data.font_init.font_path   = font_path;
      pkt.data.font_init.font_size   = font_size;
      pkt.data.font_init.api         = FONT_DRIVER_RENDER_OPENGL_API;

      thr->send_and_wait(thr, &pkt);

      return pkt.data.font_init.return_value;
   }

   return font_init_first(font_driver, font_handle, video_data,
         font_path, font_size, FONT_DRIVER_RENDER_OPENGL_API);
}

bool menu_display_font_bind_block(void *data,
      const struct font_renderer *font_driver, void *userdata)
{
   menu_display_t *disp = menu_display_get_ptr();
   if (!disp || !font_driver || !font_driver->bind_block)
      return false;

   font_driver->bind_block(disp->font.buf, userdata);

   return true;
}

bool menu_display_font_flush_block(void *data,
      const struct font_renderer *font_driver)
{
   menu_handle_t *menu = (menu_handle_t*)data;
   menu_display_t *disp = menu_display_get_ptr();
   if (!font_driver || !font_driver->flush)
      return false;

   font_driver->flush(disp->font.buf);

   return menu_display_font_bind_block(menu,
         font_driver, NULL);
}

void menu_display_free_main_font(void)
{
   driver_t     *driver = driver_get_ptr();
   menu_display_t *disp = menu_display_get_ptr();
    
   if (disp && disp->font.buf)
   {
      driver->font_osd_driver->free(disp->font.buf);
      disp->font.buf = NULL;
   }
}

bool menu_display_init_main_font(void *data,
      const char *font_path, float font_size)
{
   bool      ret;
   driver_t    *driver  = driver_get_ptr();
   void        *video   = video_driver_get_ptr(NULL);
   menu_display_t *disp = menu_display_get_ptr();

   if (!disp)
      return false;

   if (disp->font.buf)
      menu_display_free_main_font();

   ret = menu_display_font_init_first(
         (const void**)&driver->font_osd_driver,
         &disp->font.buf, video,
         font_path, font_size);

   if (ret)
      disp->font.size = font_size;
   else
      disp->font.buf = NULL;

   return ret;
}

bool menu_display_ctl(enum menu_display_ctl_state state, void *data)
{
   unsigned width, height;
   menu_framebuf_t *frame_buf = menu_display_fb_get_ptr();
   menu_display_t  *disp      = menu_display_get_ptr();
   settings_t *settings       = config_get_ptr();

   switch (state)
   {
      case MENU_DISPLAY_CTL_LIBRETRO:
         video_driver_set_texture_enable(true, false);

         if (!settings->menu.pause_libretro)
         {
            driver_t *driver     = driver_get_ptr();
            global_t *global     = global_get_ptr();

            if (global->inited.main && (global->inited.core.type != CORE_TYPE_DUMMY))
            {
               bool block_libretro_input = driver->block_libretro_input;
               driver->block_libretro_input = true;
               pretro_run();
               driver->block_libretro_input = block_libretro_input;
               return true;
            }
         }

         video_driver_cached_frame();
         return true;
      case MENU_DISPLAY_CTL_SET_WIDTH:
         {
            unsigned *ptr = (unsigned*)data;
            if (!ptr)
               return false;
            frame_buf->width = *ptr;
         }
         return true;
      case MENU_DISPLAY_CTL_WIDTH:
         {
            unsigned *ptr = (unsigned*)data;
            if (!ptr)
               return false;
            *ptr = frame_buf->width;
         }
         return true;
      case MENU_DISPLAY_CTL_HEIGHT:
         {
            unsigned *ptr = (unsigned*)data;
            if (!ptr)
               return false;
            *ptr = frame_buf->height;
         }
         return true;
      case MENU_DISPLAY_CTL_HEADER_HEIGHT:
         {
            unsigned *ptr = (unsigned*)data;
            if (!ptr)
               return false;
            *ptr = disp->header_height;
         }
         return true;
      case MENU_DISPLAY_CTL_SET_HEIGHT:
         {
            unsigned *ptr = (unsigned*)data;
            if (!ptr)
               return false;
            frame_buf->height = *ptr;
         }
         return true;
      case MENU_DISPLAY_CTL_FB_DATA:
         {
            uint16_t **ptr = (uint16_t**)data;
            if (!ptr)
               return false;
            *ptr = frame_buf->data;
         }
         return true;
      case MENU_DISPLAY_CTL_SET_FB_DATA:
         {
            uint16_t *ptr = (uint16_t*)data;
            if (!ptr)
               return false;
            frame_buf->data = ptr;
         }
         return true;
      case MENU_DISPLAY_CTL_FB_PITCH:
         {
            size_t *ptr = (size_t*)data;
            if (!ptr)
               return false;
            *ptr = frame_buf->pitch;
         }
         return true;
      case MENU_DISPLAY_CTL_SET_FB_PITCH:
         {
            size_t *ptr = (size_t*)data;
            if (!ptr)
               return false;
            frame_buf->pitch = *ptr;
         }
         return true;
      case MENU_DISPLAY_CTL_UPDATE_PENDING:
         if (menu_animation_ctl(MENU_ANIMATION_CTL_IS_ACTIVE, NULL) || (frame_buf && frame_buf->dirty))
            return true;
         return false;
      case MENU_DISPLAY_CTL_SET_VIEWPORT:
         video_driver_get_size(&width, &height);
         video_driver_set_viewport(width,
               height, true, false);
         return true;
      case MENU_DISPLAY_CTL_UNSET_VIEWPORT:
         video_driver_get_size(&width, &height);
         video_driver_set_viewport(width,
               height, false, true);
         return true;
      case MENU_DISPLAY_CTL_GET_FRAMEBUFFER_DIRTY_FLAG:
         {
            bool *ptr = (bool*)data;
            if (!ptr)
               return false;
            *ptr = frame_buf->dirty;
         }
         return true;
      case MENU_DISPLAY_CTL_SET_FRAMEBUFFER_DIRTY_FLAG:
         if (frame_buf && frame_buf->data)
            frame_buf->dirty = true;
         return true;
      case MENU_DISPLAY_CTL_UNSET_FRAMEBUFFER_DIRTY_FLAG:
         if (frame_buf && frame_buf->data)
            frame_buf->dirty = false;
         return true;
      case MENU_DISPLAY_CTL_GET_DPI:
         {
            float           *dpi = (float*)data;
            *dpi                 = menu_dpi_override_value;

            if (!settings)
               return true;

            if (settings->menu.dpi.override_enable)
               *dpi = settings->menu.dpi.override_value;
            else if (!gfx_ctx_get_metrics(DISPLAY_METRIC_DPI, dpi))
               *dpi = menu_dpi_override_value;
         }
         return true;
   }

   return false;
}

void menu_display_timedate(char *s, size_t len, unsigned time_mode)
{
   time_t time_;
   time(&time_);

   switch (time_mode)
   {
      case 0: /* Date and time */
         strftime(s, len, "%Y-%m-%d %H:%M:%S", localtime(&time_));
         break;
      case 1: /* Date */
         strftime(s, len, "%Y-%m-%d", localtime(&time_));
         break;
      case 2: /* Time */
         strftime(s, len, "%H:%M:%S", localtime(&time_));
         break;
      case 3: /* Time (hours-minutes) */
         strftime(s, len, "%H:%M", localtime(&time_));
         break;
   }
}

void menu_display_msg_queue_push(const char *msg, unsigned prio, unsigned duration,
      bool flush)
{
   rarch_main_msg_queue_push(msg, prio, duration, flush);
}
