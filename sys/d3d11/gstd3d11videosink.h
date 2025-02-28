/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_D3D11_VIDEO_SINK_H__
#define __GST_D3D11_VIDEO_SINK_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/videooverlay.h>
#include <gst/video/navigation.h>

#include "gstd3d11_fwd.h"
#include "gstd3d11window.h"

G_BEGIN_DECLS

#define GST_TYPE_D3D11_VIDEO_SINK                     (gst_d3d11_video_sink_get_type())
#define GST_D3D11_VIDEO_SINK(obj)                     (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_D3D11_VIDEO_SINK,GstD3D11VideoSink))
#define GST_D3D11_VIDEO_SINK_CLASS(klass)             (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_D3D11_VIDEO_SINK,GstD3D11VideoSinkClass))
#define GST_D3D11_VIDEO_SINK_GET_CLASS(obj)           (GST_D3D11_VIDEO_SINK_CLASS(G_OBJECT_GET_CLASS(obj)))
#define GST_IS_D3D11_VIDEO_SINK(obj)                  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_D3D11_VIDEO_SINK))
#define GST_IS_D3D11_VIDEO_SINK_CLASS(klass)          (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_D3D11_VIDEO_SINK))

typedef struct _GstD3D11VideoSink GstD3D11VideoSink;
typedef struct _GstD3D11VideoSinkClass GstD3D11VideoSinkClass;

struct _GstD3D11VideoSink
{
  GstVideoSink sink;
  GstD3D11Device *device;
  GstD3D11Window *window;

  GstVideoInfo info;
  DXGI_FORMAT dxgi_format;

  guintptr window_id;

  /* properties */
  gint adapter;
  gboolean force_aspect_ratio;
  gboolean enable_navigation_events;

  /* saved render rectangle until we have a window */
  GstVideoRectangle render_rect;
  gboolean pending_render_rect;

  ID3D11Texture2D *fallback_staging;
};

struct _GstD3D11VideoSinkClass
{
  GstVideoSinkClass parent_class;
};

GType    gst_d3d11_video_sink_get_type (void);

G_END_DECLS


#endif /* __GST_D3D11_VIDEO_SINK_H__ */
