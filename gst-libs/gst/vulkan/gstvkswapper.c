/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstvkswapper.h"

#define GST_CAT_DEFAULT gst_vulkan_swapper_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define RENDER_GET_LOCK(o) &(GST_VULKAN_SWAPPER (o)->priv->render_lock)
#define RENDER_LOCK(o) g_mutex_lock (RENDER_GET_LOCK(o));
#define RENDER_UNLOCK(o) g_mutex_unlock (RENDER_GET_LOCK(o));

struct _GstVulkanSwapperPrivate
{
  VkSurfaceKHR surface;

  VkSurfaceCapabilitiesKHR surf_props;
  VkSurfaceFormatKHR *surf_formats;
  guint32 n_surf_formats;
  VkPresentModeKHR *surf_present_modes;
  guint32 n_surf_present_modes;

  VkSwapchainKHR swap_chain;
  GstVulkanImageMemory **swap_chain_images;
  guint32 n_swap_chain_images;

  GstCaps *caps;
  GstVideoInfo v_info;

  PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
      GetPhysicalDeviceSurfaceCapabilitiesKHR;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
      GetPhysicalDeviceSurfacePresentModesKHR;
  PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
  PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
  PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
  PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
  PFN_vkQueuePresentKHR QueuePresentKHR;

  /* <private> */
  /* runtime variables */
  gint to_quit;
  GstBuffer *current_buffer;

  /* signal handlers */
  gulong close_id;
  gulong draw_id;

  /* properties */
  gboolean force_aspect_ratio;
  gint par_n;
  gint par_d;

  GMutex render_lock;

  GstVulkanTrashList *trash_list;

  /* source sizes accounting for all aspect ratios */
  guint dar_width;
  guint dar_height;
};

enum
{
  PROP_0,
  PROP_FORCE_ASPECT_RATIO,
  PROP_PIXEL_ASPECT_RATIO,
};

#define DEFAULT_FORCE_ASPECT_RATIO TRUE
#define DEFAULT_PIXEL_ASPECT_RATIO_N 0
#define DEFAULT_PIXEL_ASPECT_RATIO_D 1

#define gst_vulkan_swapper_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanSwapper, gst_vulkan_swapper,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstVulkanSwapper)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT,
        "vulkanswapper", 0, "Vulkan Swapper"));

static void _on_window_draw (GstVulkanWindow * window,
    GstVulkanSwapper * swapper);

static gboolean
_get_function_table (GstVulkanSwapper * swapper)
{
  GstVulkanDevice *device = swapper->device;
  GstVulkanInstance *instance = gst_vulkan_device_get_instance (device);

  if (!instance) {
    GST_ERROR_OBJECT (swapper, "Failed to get instance from the device");
    return FALSE;
  }
#define GET_PROC_ADDRESS_REQUIRED(obj, type, name) \
  G_STMT_START { \
    obj->priv->G_PASTE (, name) = G_PASTE(G_PASTE(gst_vulkan_, type), _get_proc_address) (type, "vk" G_STRINGIFY(name)); \
    if (!obj->priv->G_PASTE(, name)) { \
      GST_ERROR_OBJECT (obj, "Failed to find required function vk" G_STRINGIFY(name)); \
      gst_object_unref (instance); \
      return FALSE; \
    } \
  } G_STMT_END

  GET_PROC_ADDRESS_REQUIRED (swapper, instance,
      GetPhysicalDeviceSurfaceSupportKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, instance,
      GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, instance,
      GetPhysicalDeviceSurfaceFormatsKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, instance,
      GetPhysicalDeviceSurfacePresentModesKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, CreateSwapchainKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, DestroySwapchainKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, GetSwapchainImagesKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, AcquireNextImageKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, QueuePresentKHR);

  gst_object_unref (instance);

  return TRUE;

#undef GET_PROC_ADDRESS_REQUIRED
}

static GstVideoFormat
_vk_format_to_video_format (VkFormat format)
{
  switch (format) {
      /* double check endianess */
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
      return GST_VIDEO_FORMAT_RGBA;
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R8G8B8_SRGB:
      return GST_VIDEO_FORMAT_RGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
      return GST_VIDEO_FORMAT_BGRA;
    case VK_FORMAT_B8G8R8_UNORM:
    case VK_FORMAT_B8G8R8_SRGB:
      return GST_VIDEO_FORMAT_BGR;
    default:
      return GST_VIDEO_FORMAT_UNKNOWN;
  }
}

static VkFormat
_vk_format_from_video_info (GstVideoInfo * v_info)
{
  switch (GST_VIDEO_INFO_FORMAT (v_info)) {
    case GST_VIDEO_FORMAT_RGBA:
      if (GST_VIDEO_INFO_COLORIMETRY (v_info).transfer ==
          GST_VIDEO_TRANSFER_SRGB)
        return VK_FORMAT_R8G8B8A8_SRGB;
      else
        return VK_FORMAT_R8G8B8A8_UNORM;
    case GST_VIDEO_FORMAT_RGB:
      if (GST_VIDEO_INFO_COLORIMETRY (v_info).transfer ==
          GST_VIDEO_TRANSFER_SRGB)
        return VK_FORMAT_R8G8B8_SRGB;
      else
        return VK_FORMAT_R8G8B8_UNORM;
    case GST_VIDEO_FORMAT_BGRA:
      if (GST_VIDEO_INFO_COLORIMETRY (v_info).transfer ==
          GST_VIDEO_TRANSFER_SRGB)
        return VK_FORMAT_B8G8R8A8_SRGB;
      else
        return VK_FORMAT_B8G8R8A8_UNORM;
    case GST_VIDEO_FORMAT_BGR:
      if (GST_VIDEO_INFO_COLORIMETRY (v_info).transfer ==
          GST_VIDEO_TRANSFER_SRGB)
        return VK_FORMAT_B8G8R8_SRGB;
      else
        return VK_FORMAT_B8G8R8_UNORM;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

static VkColorSpaceKHR
_vk_color_space_from_video_info (GstVideoInfo * v_info)
{
  return VK_COLORSPACE_SRGB_NONLINEAR_KHR;
}

static void
_add_vk_format_to_list (GValue * list, VkFormat format)
{
  GstVideoFormat v_format;

  v_format = _vk_format_to_video_format (format);
  if (v_format) {
    const gchar *format_str = gst_video_format_to_string (v_format);
    GValue item = G_VALUE_INIT;
    GValue new_list = G_VALUE_INIT;

    g_value_init (&item, G_TYPE_STRING);
    g_value_set_string (&item, format_str);
    gst_value_list_merge (&new_list, list, &item);
    g_value_unset (&item);

    g_value_unset (list);
    *list = new_list;
  }
}

static gboolean
_vulkan_swapper_ensure_surface (GstVulkanSwapper * swapper, GError ** error)
{
  if (!swapper->priv->surface) {
    if (!(swapper->priv->surface =
            gst_vulkan_window_get_surface (swapper->window, error))) {
      return FALSE;
    }
  }

  return TRUE;
}

struct choose_data
{
  GstVulkanSwapper *swapper;
  GstVulkanQueue *graphics_queue;
  GstVulkanQueue *present_queue;
};

static gboolean
_choose_queue (GstVulkanDevice * device, GstVulkanQueue * queue,
    struct choose_data *data)
{
  guint flags = device->queue_family_props[queue->family].queueFlags;
  VkPhysicalDevice gpu;
  gboolean supports_present;

  gpu = gst_vulkan_device_get_physical_device (data->swapper->device);

  {
    VkResult err;
    GError *error = NULL;
    VkBool32 physical_device_supported;

    err =
        data->swapper->priv->GetPhysicalDeviceSurfaceSupportKHR (gpu,
        queue->index, data->swapper->priv->surface, &physical_device_supported);
    if (gst_vulkan_error_to_g_error (err, &error,
            "GetPhysicalDeviceSurfaceSupport") < 0) {
      GST_DEBUG_OBJECT (data->swapper,
          "surface not supported by the physical device: %s", error->message);
      return TRUE;
    }
  }

  supports_present =
      gst_vulkan_window_get_presentation_support (data->swapper->window,
      device, queue->index);

  if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0) {
    if (supports_present) {
      /* found one that supports both */
      if (data->graphics_queue)
        gst_object_unref (data->graphics_queue);
      data->graphics_queue = gst_object_ref (queue);
      if (data->present_queue)
        gst_object_unref (data->present_queue);
      data->present_queue = gst_object_ref (queue);
      return FALSE;
    }
    if (!data->graphics_queue)
      data->present_queue = gst_object_ref (queue);
  } else if (supports_present) {
    if (!data->present_queue)
      data->present_queue = gst_object_ref (queue);
  }

  return TRUE;
}

/*
 * gst_vulkan_swapper_choose_queue:
 * @swapper: a #GstVulkanSwapper
 * @available_queue: (transfer none): a #GstVulkanQueue chosen elsewhere
 * @error: a #GError
 */
gboolean
gst_vulkan_swapper_choose_queue (GstVulkanSwapper * swapper,
    GstVulkanQueue * available_queue, GError ** error)
{
  if (!_vulkan_swapper_ensure_surface (swapper, error))
    return FALSE;

  if (swapper->queue)
    return TRUE;

  if (available_queue) {
    guint flags =
        swapper->device->queue_family_props[available_queue->family].queueFlags;
    gboolean supports_present;

    supports_present =
        gst_vulkan_window_get_presentation_support (swapper->window,
        swapper->device, available_queue->index);
    if (supports_present && flags & VK_QUEUE_GRAPHICS_BIT)
      swapper->queue = gst_object_ref (available_queue);
  }

  if (!swapper->queue) {
    struct choose_data data;

    data.swapper = swapper;
    data.present_queue = NULL;
    data.graphics_queue = NULL;

    gst_vulkan_device_foreach_queue (swapper->device,
        (GstVulkanDeviceForEachQueueFunc) _choose_queue, &data);

    if (data.graphics_queue != data.present_queue) {
      /* FIXME: add support for separate graphics/present queues */
      g_set_error (error, GST_VULKAN_ERROR,
          VK_ERROR_INITIALIZATION_FAILED,
          "Failed to find a compatible present/graphics queue");
      if (data.present_queue)
        gst_object_unref (data.present_queue);
      if (data.graphics_queue)
        gst_object_unref (data.graphics_queue);
      return FALSE;
    }

    swapper->queue = gst_object_ref (data.present_queue);
    if (data.present_queue)
      gst_object_unref (data.present_queue);
    if (data.graphics_queue)
      gst_object_unref (data.graphics_queue);
  }

  return TRUE;
}

static gboolean
_vulkan_swapper_retrieve_surface_properties (GstVulkanSwapper * swapper,
    GError ** error)
{
  VkPhysicalDevice gpu;
  VkResult err;

  if (swapper->priv->surf_formats)
    return TRUE;

  gpu = gst_vulkan_device_get_physical_device (swapper->device);

  if (!gst_vulkan_swapper_choose_queue (swapper, NULL, error))
    return FALSE;

  if (!(swapper->cmd_pool =
          gst_vulkan_queue_create_command_pool (swapper->queue, error)))
    return FALSE;

  err =
      swapper->priv->GetPhysicalDeviceSurfaceCapabilitiesKHR (gpu,
      swapper->priv->surface, &swapper->priv->surf_props);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceCapabilitiesKHR") < 0)
    return FALSE;

  err =
      swapper->priv->GetPhysicalDeviceSurfaceFormatsKHR (gpu,
      swapper->priv->surface, &swapper->priv->n_surf_formats, NULL);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceFormatsKHR") < 0)
    return FALSE;

  swapper->priv->surf_formats =
      g_new0 (VkSurfaceFormatKHR, swapper->priv->n_surf_formats);
  err =
      swapper->priv->GetPhysicalDeviceSurfaceFormatsKHR (gpu,
      swapper->priv->surface, &swapper->priv->n_surf_formats,
      swapper->priv->surf_formats);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceFormatsKHR") < 0)
    return FALSE;

  err =
      swapper->priv->GetPhysicalDeviceSurfacePresentModesKHR (gpu,
      swapper->priv->surface, &swapper->priv->n_surf_present_modes, NULL);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfacePresentModesKHR") < 0)
    return FALSE;

  swapper->priv->surf_present_modes =
      g_new0 (VkPresentModeKHR, swapper->priv->n_surf_present_modes);
  err =
      swapper->priv->GetPhysicalDeviceSurfacePresentModesKHR (gpu,
      swapper->priv->surface, &swapper->priv->n_surf_present_modes,
      swapper->priv->surf_present_modes);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfacePresentModesKHR") < 0)
    return FALSE;

  return TRUE;
}

static gboolean
_on_window_close (GstVulkanWindow * window, GstVulkanSwapper * swapper)
{
  g_atomic_int_set (&swapper->priv->to_quit, 1);

  return TRUE;
}

static void
gst_vulkan_swapper_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVulkanSwapper *swapper = GST_VULKAN_SWAPPER (object);

  switch (prop_id) {
    case PROP_FORCE_ASPECT_RATIO:
      swapper->priv->force_aspect_ratio = g_value_get_boolean (value);
      break;
    case PROP_PIXEL_ASPECT_RATIO:
      swapper->priv->par_n = gst_value_get_fraction_numerator (value);
      swapper->priv->par_d = gst_value_get_fraction_denominator (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_swapper_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVulkanSwapper *swapper = GST_VULKAN_SWAPPER (object);

  switch (prop_id) {
    case PROP_FORCE_ASPECT_RATIO:
      g_value_set_boolean (value, swapper->priv->force_aspect_ratio);
      break;
    case PROP_PIXEL_ASPECT_RATIO:
      gst_value_set_fraction (value, swapper->priv->par_n,
          swapper->priv->par_d);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_swapper_finalize (GObject * object)
{
  GstVulkanSwapper *swapper = GST_VULKAN_SWAPPER (object);
  int i;

  if (!gst_vulkan_trash_list_wait (swapper->priv->trash_list, -1))
    GST_WARNING_OBJECT (swapper, "Failed to wait for all fences to complete "
        "before shutting down");
  gst_object_unref (swapper->priv->trash_list);
  swapper->priv->trash_list = NULL;

  if (swapper->priv->swap_chain_images) {
    for (i = 0; i < swapper->priv->n_swap_chain_images; i++) {
      gst_memory_unref ((GstMemory *) swapper->priv->swap_chain_images[i]);
      swapper->priv->swap_chain_images[i] = NULL;
    }
    g_free (swapper->priv->swap_chain_images);
  }
  swapper->priv->swap_chain_images = NULL;

  if (swapper->priv->swap_chain)
    swapper->priv->DestroySwapchainKHR (swapper->device->device,
        swapper->priv->swap_chain, NULL);
  swapper->priv->swap_chain = VK_NULL_HANDLE;

  g_free (swapper->priv->surf_present_modes);
  swapper->priv->surf_present_modes = NULL;

  g_free (swapper->priv->surf_formats);
  swapper->priv->surf_formats = NULL;

  gst_buffer_replace (&swapper->priv->current_buffer, NULL);
  gst_caps_replace (&swapper->priv->caps, NULL);

  g_mutex_clear (&swapper->priv->render_lock);

  if (swapper->cmd_pool)
    gst_object_unref (swapper->cmd_pool);
  swapper->cmd_pool = NULL;

  if (swapper->queue)
    gst_object_unref (swapper->queue);
  swapper->queue = NULL;

  if (swapper->device)
    gst_object_unref (swapper->device);
  swapper->device = NULL;

  g_signal_handler_disconnect (swapper->window, swapper->priv->draw_id);
  swapper->priv->draw_id = 0;

  g_signal_handler_disconnect (swapper->window, swapper->priv->close_id);
  swapper->priv->close_id = 0;

  if (swapper->window)
    gst_object_unref (swapper->window);
  swapper->window = NULL;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vulkan_swapper_init (GstVulkanSwapper * swapper)
{
  swapper->priv = gst_vulkan_swapper_get_instance_private (swapper);

  g_mutex_init (&swapper->priv->render_lock);

  swapper->priv->force_aspect_ratio = DEFAULT_FORCE_ASPECT_RATIO;
  swapper->priv->par_n = DEFAULT_PIXEL_ASPECT_RATIO_N;
  swapper->priv->par_d = DEFAULT_PIXEL_ASPECT_RATIO_D;

  swapper->priv->trash_list = gst_vulkan_trash_fence_list_new ();
}

static void
gst_vulkan_swapper_class_init (GstVulkanSwapperClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = gst_vulkan_swapper_set_property;
  gobject_class->get_property = gst_vulkan_swapper_get_property;
  gobject_class->finalize = gst_vulkan_swapper_finalize;

  g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio", "Force aspect ratio",
          "When enabled, scaling will respect original aspect ratio",
          DEFAULT_FORCE_ASPECT_RATIO,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PIXEL_ASPECT_RATIO,
      gst_param_spec_fraction ("pixel-aspect-ratio", "Pixel Aspect Ratio",
          "The pixel aspect ratio of the device", 0, 1, G_MAXINT, 1, 1, 1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

GstVulkanSwapper *
gst_vulkan_swapper_new (GstVulkanDevice * device, GstVulkanWindow * window)
{
  GstVulkanSwapper *swapper;

  swapper = g_object_new (GST_TYPE_VULKAN_SWAPPER, NULL);
  gst_object_ref_sink (swapper);
  swapper->device = gst_object_ref (device);
  swapper->window = gst_object_ref (window);

  if (!_get_function_table (swapper)) {
    gst_object_unref (swapper);
    return NULL;
  }

  swapper->priv->close_id = g_signal_connect (swapper->window, "close",
      (GCallback) _on_window_close, swapper);
  swapper->priv->draw_id = g_signal_connect (swapper->window, "draw",
      (GCallback) _on_window_draw, swapper);

  return swapper;
}

GstCaps *
gst_vulkan_swapper_get_supported_caps (GstVulkanSwapper * swapper,
    GError ** error)
{
  GstStructure *s;
  GstCaps *caps;

  g_return_val_if_fail (GST_IS_VULKAN_SWAPPER (swapper), NULL);

  if (!_vulkan_swapper_retrieve_surface_properties (swapper, error))
    return NULL;

  caps = gst_caps_new_empty_simple ("video/x-raw");
  gst_caps_set_features (caps, 0,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_VULKAN_IMAGE));
  s = gst_caps_get_structure (caps, 0);

  {
    int i;
    GValue list = G_VALUE_INIT;

    g_value_init (&list, GST_TYPE_LIST);

    if (swapper->priv->n_surf_formats
        && swapper->priv->surf_formats[0].format == VK_FORMAT_UNDEFINED) {
      _add_vk_format_to_list (&list, VK_FORMAT_B8G8R8A8_UNORM);
    } else {
      for (i = 0; i < swapper->priv->n_surf_formats; i++) {
        _add_vk_format_to_list (&list, swapper->priv->surf_formats[i].format);
      }
    }

    gst_structure_set_value (s, "format", &list);
    g_value_unset (&list);
  }

  {
    guint32 max_dim = swapper->device->gpu_props.limits.maxImageDimension2D;

    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, (gint) max_dim,
        "height", GST_TYPE_INT_RANGE, 1, (gint) max_dim, "pixel-aspect-ratio",
        GST_TYPE_FRACTION, 1, 1, "framerate", GST_TYPE_FRACTION_RANGE, 0, 1,
        G_MAXINT, 1, NULL);
  }

  GST_INFO_OBJECT (swapper, "Probed the following caps %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
_allocate_swapchain (GstVulkanSwapper * swapper, GstCaps * caps,
    GError ** error)
{
  VkSurfaceTransformFlagsKHR preTransform;
  VkCompositeAlphaFlagsKHR alpha_flags;
  VkPresentModeKHR present_mode;
  VkImageUsageFlags usage = 0;
  VkColorSpaceKHR color_space;
  VkImage *swap_chain_images;
  VkExtent2D swapchain_dims;
  guint32 n_images_wanted;
  VkPhysicalDevice gpu;
  VkFormat format;
  VkResult err;
  guint32 i;

  if (!_vulkan_swapper_ensure_surface (swapper, error))
    return FALSE;

  gpu = gst_vulkan_device_get_physical_device (swapper->device);
  err =
      swapper->priv->GetPhysicalDeviceSurfaceCapabilitiesKHR (gpu,
      swapper->priv->surface, &swapper->priv->surf_props);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceCapabilitiesKHR") < 0)
    return FALSE;

  /* width and height are either both -1, or both not -1. */
  if (swapper->priv->surf_props.currentExtent.width == -1) {
    /* If the surface size is undefined, the size is set to
     * the size of the images requested. */
    swapchain_dims.width = 320;
    swapchain_dims.height = 240;
  } else {
    /* If the surface size is defined, the swap chain size must match */
    swapchain_dims = swapper->priv->surf_props.currentExtent;
  }

  /* If mailbox mode is available, use it, as is the lowest-latency non-
   * tearing mode.  If not, try IMMEDIATE which will usually be available,
   * and is fastest (though it tears).  If not, fall back to FIFO which is
   * always available. */
  present_mode = VK_PRESENT_MODE_FIFO_KHR;
  for (i = 0; i < swapper->priv->n_surf_present_modes; i++) {
    if (swapper->priv->surf_present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
      present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
      break;
    }
    if ((present_mode != VK_PRESENT_MODE_MAILBOX_KHR) &&
        (swapper->priv->surf_present_modes[i] ==
            VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }

  /* Determine the number of VkImage's to use in the swap chain (we desire to
   * own only 1 image at a time, besides the images being displayed and
   * queued for display): */
  n_images_wanted = swapper->priv->surf_props.minImageCount + 1;
  if ((swapper->priv->surf_props.maxImageCount > 0) &&
      (n_images_wanted > swapper->priv->surf_props.maxImageCount)) {
    /* Application must settle for fewer images than desired: */
    n_images_wanted = swapper->priv->surf_props.maxImageCount;
  }

  if (swapper->priv->surf_props.
      supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
    preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  } else {
    preTransform = swapper->priv->surf_props.currentTransform;
  }

  format = _vk_format_from_video_info (&swapper->priv->v_info);
  color_space = _vk_color_space_from_video_info (&swapper->priv->v_info);

  if ((swapper->priv->surf_props.supportedCompositeAlpha &
          VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
    alpha_flags = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  } else if ((swapper->priv->surf_props.supportedCompositeAlpha &
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0) {
    alpha_flags = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  } else {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Incorrect alpha flags available for the swap images");
    return FALSE;
  }

  if ((swapper->priv->surf_props.supportedUsageFlags &
          VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
    usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  } else {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Incorrect usage flags available for the swap images");
    return FALSE;
  }
  if ((swapper->priv->
          surf_props.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      != 0) {
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  } else {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Incorrect usage flags available for the swap images");
    return FALSE;
  }

  {
    VkSwapchainCreateInfoKHR swap_chain_info = { 0, };
    VkSwapchainKHR old_swap_chain;

    old_swap_chain = swapper->priv->swap_chain;

    /* *INDENT-OFF* */
    swap_chain_info = (VkSwapchainCreateInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = NULL,
        .surface = swapper->priv->surface,
        .minImageCount = n_images_wanted,
        .imageFormat = format,
        .imageColorSpace = color_space,
        .imageExtent = swapchain_dims,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
        .preTransform = preTransform,
        .presentMode = present_mode,
        .compositeAlpha = alpha_flags,
        .clipped = TRUE,
        .oldSwapchain = old_swap_chain
    };
    /* *INDENT-ON* */

    err =
        swapper->priv->CreateSwapchainKHR (swapper->device->device,
        &swap_chain_info, NULL, &swapper->priv->swap_chain);
    if (gst_vulkan_error_to_g_error (err, error, "vkCreateSwapchainKHR") < 0)
      return FALSE;

    if (old_swap_chain != VK_NULL_HANDLE) {
      swapper->priv->DestroySwapchainKHR (swapper->device->device,
          old_swap_chain, NULL);
    }
  }

  err =
      swapper->priv->GetSwapchainImagesKHR (swapper->device->device,
      swapper->priv->swap_chain, &swapper->priv->n_swap_chain_images, NULL);
  if (gst_vulkan_error_to_g_error (err, error, "vkGetSwapchainImagesKHR") < 0)
    return FALSE;

  swap_chain_images = g_new0 (VkImage, swapper->priv->n_swap_chain_images);
  err =
      swapper->priv->GetSwapchainImagesKHR (swapper->device->device,
      swapper->priv->swap_chain, &swapper->priv->n_swap_chain_images,
      swap_chain_images);
  if (gst_vulkan_error_to_g_error (err, error, "vkGetSwapchainImagesKHR") < 0) {
    g_free (swap_chain_images);
    return FALSE;
  }

  swapper->priv->swap_chain_images =
      g_new0 (GstVulkanImageMemory *, swapper->priv->n_swap_chain_images);
  for (i = 0; i < swapper->priv->n_swap_chain_images; i++) {
    swapper->priv->swap_chain_images[i] = (GstVulkanImageMemory *)
        gst_vulkan_image_memory_wrapped (swapper->device, swap_chain_images[i],
        format, swapchain_dims.width, swapchain_dims.height,
        VK_IMAGE_TILING_OPTIMAL, usage, NULL, NULL);

    swapper->priv->swap_chain_images[i]->barrier.parent.pipeline_stages =
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    swapper->priv->swap_chain_images[i]->barrier.parent.access_flags =
        VK_ACCESS_MEMORY_READ_BIT;
    swapper->priv->swap_chain_images[i]->barrier.image_layout =
        VK_IMAGE_LAYOUT_UNDEFINED;
  }

  g_free (swap_chain_images);
  return TRUE;
}

static gboolean
_swapchain_resize (GstVulkanSwapper * swapper, GError ** error)
{
  int i;

  if (!swapper->queue) {
    if (!_vulkan_swapper_retrieve_surface_properties (swapper, error)) {
      return FALSE;
    }
  }

  if (swapper->priv->swap_chain_images) {
    for (i = 0; i < swapper->priv->n_swap_chain_images; i++) {
      if (swapper->priv->swap_chain_images[i])
        gst_memory_unref ((GstMemory *) swapper->priv->swap_chain_images[i]);
    }
    g_free (swapper->priv->swap_chain_images);
  }

  return _allocate_swapchain (swapper, swapper->priv->caps, error);
}


static gboolean
configure_display_from_info (GstVulkanSwapper * swapper, GstVideoInfo * vinfo)
{
  gint width;
  gint height;
  gboolean ok;
  gint par_n, par_d;
  gint display_par_n, display_par_d;
  guint display_ratio_num, display_ratio_den;

  width = GST_VIDEO_INFO_WIDTH (vinfo);
  height = GST_VIDEO_INFO_HEIGHT (vinfo);

  par_n = GST_VIDEO_INFO_PAR_N (vinfo);
  par_d = GST_VIDEO_INFO_PAR_D (vinfo);

  if (!par_n)
    par_n = 1;

  /* get display's PAR */
  if (swapper->priv->par_n != 0 && swapper->priv->par_d != 0) {
    display_par_n = swapper->priv->par_n;
    display_par_d = swapper->priv->par_d;
  } else {
    display_par_n = 1;
    display_par_d = 1;
  }

  ok = gst_video_calculate_display_ratio (&display_ratio_num,
      &display_ratio_den, width, height, par_n, par_d, display_par_n,
      display_par_d);

  if (!ok)
    return FALSE;

  GST_TRACE_OBJECT (swapper, "PAR: %u/%u DAR:%u/%u", par_n, par_d,
      display_par_n, display_par_d);

  if (height % display_ratio_den == 0) {
    GST_DEBUG_OBJECT (swapper, "keeping video height");
    swapper->priv->dar_width = (guint)
        gst_util_uint64_scale_int (height, display_ratio_num,
        display_ratio_den);
    swapper->priv->dar_height = height;
  } else if (width % display_ratio_num == 0) {
    GST_DEBUG_OBJECT (swapper, "keeping video width");
    swapper->priv->dar_width = width;
    swapper->priv->dar_height = (guint)
        gst_util_uint64_scale_int (width, display_ratio_den, display_ratio_num);
  } else {
    GST_DEBUG_OBJECT (swapper, "approximating while keeping video height");
    swapper->priv->dar_width = (guint)
        gst_util_uint64_scale_int (height, display_ratio_num,
        display_ratio_den);
    swapper->priv->dar_height = height;
  }
  GST_DEBUG_OBJECT (swapper, "scaling to %dx%d", swapper->priv->dar_width,
      swapper->priv->dar_height);

  return TRUE;
}

gboolean
gst_vulkan_swapper_set_caps (GstVulkanSwapper * swapper, GstCaps * caps,
    GError ** error)
{
  if (!gst_video_info_from_caps (&swapper->priv->v_info, caps)) {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED, "Failed to get GstVideoInfo from caps");
    return FALSE;
  }

  if (!configure_display_from_info (swapper, &swapper->priv->v_info)) {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED, "Failed to configure display sizes");
    return FALSE;
  }

  gst_caps_replace (&swapper->priv->caps, caps);

  return _swapchain_resize (swapper, error);
}

static gboolean
_build_render_buffer_cmd (GstVulkanSwapper * swapper, guint32 swap_idx,
    GstBuffer * buffer, VkCommandBuffer * cmd_ret, GError ** error)
{
  GstMemory *in_mem;
  GstVulkanImageMemory *swap_img;
  VkCommandBuffer cmd;
  GstVideoRectangle src, dst, rslt;
  VkResult err;

  g_return_val_if_fail (swap_idx < swapper->priv->n_swap_chain_images, FALSE);
  swap_img = swapper->priv->swap_chain_images[swap_idx];

  if (!(cmd = gst_vulkan_command_pool_create (swapper->cmd_pool, error)))
    return FALSE;

  {
    /* *INDENT-OFF* */
    VkCommandBufferBeginInfo cmd_buf_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = NULL
    };
    /* *INDENT-ON* */

    err = vkBeginCommandBuffer (cmd, &cmd_buf_info);
    if (gst_vulkan_error_to_g_error (err, error, "vkBeginCommandBuffer") < 0)
      return FALSE;
  }

  {
    /* *INDENT-OFF* */
    VkImageMemoryBarrier image_memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = swap_img->barrier.parent.access_flags,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = swap_img->barrier.image_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        /* FIXME: implement exclusive transfers */
        .srcQueueFamilyIndex = 0,
        .dstQueueFamilyIndex = 0,
        .image = swap_img->image,
        .subresourceRange = swap_img->barrier.subresource_range
    };
    /* *INDENT-ON* */

    vkCmdPipelineBarrier (cmd, swap_img->barrier.parent.pipeline_stages,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
        &image_memory_barrier);

    swap_img->barrier.parent.pipeline_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    swap_img->barrier.parent.access_flags = image_memory_barrier.dstAccessMask;
    swap_img->barrier.image_layout = image_memory_barrier.newLayout;
  }

  src.x = src.y = 0;
  src.w = swapper->priv->dar_width;
  src.h = swapper->priv->dar_height;

  dst.x = dst.y = 0;
  dst.w = gst_vulkan_image_memory_get_width (swap_img);
  dst.h = gst_vulkan_image_memory_get_height (swap_img);

  gst_video_sink_center_rect (src, dst, &rslt,
      swapper->priv->force_aspect_ratio);

  GST_TRACE_OBJECT (swapper, "rendering into result rectangle %ux%u+%u,%u "
      "src %ux%u dst %ux%u", rslt.w, rslt.h, rslt.x, rslt.y, src.w, src.h,
      dst.w, dst.h);

  in_mem = gst_buffer_peek_memory (buffer, 0);
  {
    GstVulkanImageMemory *img_mem = (GstVulkanImageMemory *) in_mem;
    /* *INDENT-OFF* */
    VkImageBlit blit = {
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffsets = {
            {0, 0, 0},
            {GST_VIDEO_INFO_WIDTH (&swapper->priv->v_info), GST_VIDEO_INFO_HEIGHT (&swapper->priv->v_info), 1},
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffsets = {
            {rslt.x, rslt.y, 0},
            {rslt.x + rslt.w, rslt.y + rslt.h, 1},
        },
    };
    VkImageMemoryBarrier image_memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = img_mem->barrier.parent.access_flags,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = img_mem->barrier.image_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        /* FIXME: implement exclusive transfers */
        .srcQueueFamilyIndex = 0,
        .dstQueueFamilyIndex = 0,
        .image = img_mem->image,
        .subresourceRange = img_mem->barrier.subresource_range
    };
    VkClearColorValue clear = {{0.0, 0.0, 0.0, 1.0}};
    VkImageSubresourceRange clear_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    /* *INDENT-ON* */

    vkCmdPipelineBarrier (cmd, img_mem->barrier.parent.pipeline_stages,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
        &image_memory_barrier);

    img_mem->barrier.parent.pipeline_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    img_mem->barrier.parent.access_flags = image_memory_barrier.dstAccessMask;
    img_mem->barrier.image_layout = image_memory_barrier.newLayout;

    vkCmdClearColorImage (cmd, swap_img->image, swap_img->barrier.image_layout,
        &clear, 1, &clear_range);
    vkCmdBlitImage (cmd, img_mem->image, img_mem->barrier.image_layout,
        swap_img->image, swap_img->barrier.image_layout, 1, &blit,
        VK_FILTER_LINEAR);
  }
  {
    /* *INDENT-OFF* */
    VkImageMemoryBarrier image_memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = swap_img->barrier.parent.access_flags,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = swap_img->barrier.image_layout,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        /* FIXME: implement exclusive transfers */
        .srcQueueFamilyIndex = 0,
        .dstQueueFamilyIndex = 0,
        .image = swap_img->image,
        .subresourceRange = swap_img->barrier.subresource_range
    };
    /* *INDENT-ON* */

    vkCmdPipelineBarrier (cmd, swap_img->barrier.parent.pipeline_stages,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
        &image_memory_barrier);

    swap_img->barrier.parent.pipeline_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    swap_img->barrier.parent.access_flags = image_memory_barrier.dstAccessMask;
    swap_img->barrier.image_layout = image_memory_barrier.newLayout;
  }

  err = vkEndCommandBuffer (cmd);
  if (gst_vulkan_error_to_g_error (err, error, "vkEndCommandBuffer") < 0)
    return FALSE;

  *cmd_ret = cmd;

  return TRUE;
}

static gboolean
_render_buffer_unlocked (GstVulkanSwapper * swapper,
    GstBuffer * buffer, GError ** error)
{
  VkSemaphore acquire_semaphore = { 0, };
  VkSemaphore present_semaphore = { 0, };
  VkSemaphoreCreateInfo semaphore_info = { 0, };
  GstVulkanFence *fence = NULL;
  VkPresentInfoKHR present;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  guint32 swap_idx;
  VkResult err, present_err = VK_SUCCESS;

  gst_vulkan_trash_list_gc (swapper->priv->trash_list);

  if (!buffer) {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED, "Invalid buffer");
    goto error;
  }

  if (g_atomic_int_get (&swapper->priv->to_quit)) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_SURFACE_LOST_KHR,
        "Output window was closed");
    goto error;
  }

  gst_buffer_replace (&swapper->priv->current_buffer, buffer);

  /* *INDENT-OFF* */
  semaphore_info = (VkSemaphoreCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
  };
  /* *INDENT-ON* */

reacquire:
  err = vkCreateSemaphore (swapper->device->device, &semaphore_info,
      NULL, &acquire_semaphore);
  if (gst_vulkan_error_to_g_error (err, error, "vkCreateSemaphore") < 0)
    goto error;

  err =
      swapper->priv->AcquireNextImageKHR (swapper->device->device,
      swapper->priv->swap_chain, -1, acquire_semaphore, VK_NULL_HANDLE,
      &swap_idx);
  /* TODO: Deal with the VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR */
  if (err == VK_ERROR_OUT_OF_DATE_KHR) {
    GST_DEBUG_OBJECT (swapper, "out of date frame acquired");

    vkDestroySemaphore (swapper->device->device, acquire_semaphore, NULL);
    if (!_swapchain_resize (swapper, error))
      goto error;
    goto reacquire;
  } else if (gst_vulkan_error_to_g_error (err, error,
          "vkAcquireNextImageKHR") < 0) {
    goto error;
  }

  if (!_build_render_buffer_cmd (swapper, swap_idx, buffer, &cmd, error))
    goto error;

  err = vkCreateSemaphore (swapper->device->device, &semaphore_info,
      NULL, &present_semaphore);
  if (gst_vulkan_error_to_g_error (err, error, "vkCreateSemaphore") < 0)
    goto error;

  {
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo submit_info = { 0, };

    /* *INDENT-OFF* */
    submit_info = (VkSubmitInfo) {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &acquire_semaphore,
        .pWaitDstStageMask = &stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &present_semaphore,
    };
    /* *INDENT-ON* */

    fence = gst_vulkan_fence_new (swapper->device, 0, error);
    if (!fence)
      goto error;

    err =
        vkQueueSubmit (swapper->queue->queue, 1, &submit_info,
        GST_VULKAN_FENCE_FENCE (fence));
    if (gst_vulkan_error_to_g_error (err, error, "vkQueueSubmit") < 0)
      goto error;

    gst_vulkan_trash_list_add (swapper->priv->trash_list,
        gst_vulkan_trash_new_free_command_buffer (gst_vulkan_fence_ref (fence),
            swapper->cmd_pool, cmd));
    gst_vulkan_trash_list_add (swapper->priv->trash_list,
        gst_vulkan_trash_new_free_semaphore (fence, acquire_semaphore));

    cmd = VK_NULL_HANDLE;
    fence = NULL;
  }

  /* *INDENT-OFF* */
  present = (VkPresentInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pNext = NULL,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &present_semaphore,
      .swapchainCount = 1,
      .pSwapchains = &swapper->priv->swap_chain,
      .pImageIndices = &swap_idx,
      .pResults = &present_err,
  };
  /* *INDENT-ON* */

  err = swapper->priv->QueuePresentKHR (swapper->queue->queue, &present);
  if (gst_vulkan_error_to_g_error (err, error, "vkQueuePresentKHR") < 0)
    goto error;

  if (present_err == VK_ERROR_OUT_OF_DATE_KHR) {
    GST_DEBUG_OBJECT (swapper, "out of date frame submitted");

    if (!_swapchain_resize (swapper, error))
      goto error;
  } else if (gst_vulkan_error_to_g_error (err, error, "vkQueuePresentKHR") < 0)
    goto error;

  {
    VkSubmitInfo submit_info = { 0, };
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    /* *INDENT-OFF* */
    submit_info = (VkSubmitInfo) {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pWaitDstStageMask = &stages,
        0,
    };
    /* *INDENT-ON* */

    fence = gst_vulkan_fence_new (swapper->device, 0, error);
    if (!fence)
      goto error;

    err =
        vkQueueSubmit (swapper->queue->queue, 1, &submit_info,
        GST_VULKAN_FENCE_FENCE (fence));
    if (gst_vulkan_error_to_g_error (err, error, "vkQueueSubmit") < 0)
      goto error;

    gst_vulkan_trash_list_add (swapper->priv->trash_list,
        gst_vulkan_trash_new_free_semaphore (fence, present_semaphore));
    fence = NULL;
  }

  return TRUE;

error:
  {
    if (acquire_semaphore)
      vkDestroySemaphore (swapper->device->device, acquire_semaphore, NULL);
    if (present_semaphore)
      vkDestroySemaphore (swapper->device->device, present_semaphore, NULL);
    if (cmd)
      vkFreeCommandBuffers (swapper->device->device, swapper->cmd_pool->pool,
          1, &cmd);
    return FALSE;
  }
}

gboolean
gst_vulkan_swapper_render_buffer (GstVulkanSwapper * swapper,
    GstBuffer * buffer, GError ** error)
{
  GstMemory *mem;
  gboolean ret;

  mem = gst_buffer_peek_memory (buffer, 0);
  if (!mem) {
    g_set_error_literal (error, GST_VULKAN_ERROR, VK_ERROR_FORMAT_NOT_SUPPORTED,
        "Buffer has no memory");
    return FALSE;
  }
  if (!gst_is_vulkan_image_memory (mem)) {
    g_set_error_literal (error, GST_VULKAN_ERROR, VK_ERROR_FORMAT_NOT_SUPPORTED,
        "Incorrect memory type");
    return FALSE;
  }

  RENDER_LOCK (swapper);
  ret = _render_buffer_unlocked (swapper, buffer, error);
  RENDER_UNLOCK (swapper);

  return ret;
}

static void
_on_window_draw (GstVulkanWindow * window, GstVulkanSwapper * swapper)
{
  GError *error = NULL;

  RENDER_LOCK (swapper);
  if (!swapper->priv->current_buffer) {
    GST_DEBUG_OBJECT (swapper, "No buffer to render");
    RENDER_UNLOCK (swapper);
    return;
  }

  /* TODO: perform some rate limiting of the number of redraw events */
  if (!_render_buffer_unlocked (swapper, swapper->priv->current_buffer, &error))
    GST_ERROR_OBJECT (swapper, "Failed to redraw buffer %p %s",
        swapper->priv->current_buffer, error->message);
  g_clear_error (&error);
  RENDER_UNLOCK (swapper);
}
