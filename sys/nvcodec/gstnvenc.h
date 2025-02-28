/* GStreamer NVENC plugin
 * Copyright (C) 2015 Centricular Ltd
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

#ifndef __GST_NVENC_H_INCLUDED__
#define __GST_NVENC_H_INCLUDED__

#include <gst/gst.h>
#include <gst/video/video.h>

#include "gstcudaloader.h"
#include "nvEncodeAPI.h"

GST_DEBUG_CATEGORY_EXTERN (gst_nvenc_debug);

CUcontext               gst_nvenc_create_cuda_context (guint device_id);

gboolean                gst_nvenc_destroy_cuda_context (CUcontext ctx);

gboolean                gst_nvenc_cmp_guid (GUID g1, GUID g2);

NV_ENC_BUFFER_FORMAT    gst_nvenc_get_nv_buffer_format (GstVideoFormat fmt);

gboolean                gst_nv_enc_get_supported_input_formats (gpointer encoder,
                                                                GUID codec_id,
                                                                GValue ** formats);


GValue *                gst_nv_enc_get_interlace_modes (gpointer enc,
                                                        GUID codec_id);

GValue *                gst_nv_enc_get_supported_codec_profiles (gpointer enc,
                                                                 GUID codec_id);


void                    gst_nvenc_plugin_init (GstPlugin * plugin);


#endif /* __GST_NVENC_H_INCLUDED__ */
