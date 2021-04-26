/*
 * Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "gstnvdsmeta.h"
#ifndef PLATFORM_TEGRA
#include "gst-nvmessage.h"

#include "nvdsmeta_schema.h"
#endif

#define MEMORY_FEATURES "memory:NVMM"

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH  960
#define MUXER_OUTPUT_HEIGHT 752

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 33000

#define TILED_OUTPUT_WIDTH 1280
#define TILED_OUTPUT_HEIGHT 720

/* NVIDIA Decoder source pad memory feature. This feature signifies that source
 * pads having this capability will push GstBuffers containing cuda buffers. */
#define GST_CAPS_FEATURES_NVMM "memory:NVMM"

/*  Define Tracker group features */

#define CONFIG_GROUP_TRACKER "tracker"
#define CONFIG_GROUP_TRACKER_WIDTH "tracker-width"
#define CONFIG_GROUP_TRACKER_HEIGHT "tracker-height"
#define CONFIG_GROUP_TRACKER_LL_CONFIG_FILE "ll-config-file"
#define CONFIG_GROUP_TRACKER_LL_LIB_FILE "ll-lib-file"
#define CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS "enable-batch-process"
#define CONFIG_GROUP_TRACKER_TRACKING_SURFACE_TYPE "tracking-surface-type"
#define CONFIG_GPU_ID "gpu-id"


#define MAX_DISPLAY_LEN 64
#define MAX_TIME_STAMP_LEN 32


#define PGIE_CLASS_ID_PERSON 0
#define PGIE_CLASS_ID_BAG 1
#define PGIE_CLASS_ID_FACE 2


#define PERF_DEWARP
#define PERF_DEWARP_INFER

gint frame_number = 0;


static gchar *
get_absolute_file_path (gchar *cfg_file_path, gchar *file_path)
{
  gchar abs_cfg_path[PATH_MAX + 1];
  gchar *abs_file_path;
  gchar *delim;

  if (file_path && file_path[0] == '/') {
    return file_path;
  }

  if (!realpath (cfg_file_path, abs_cfg_path)) {
    g_free (file_path);
    return NULL;
  }

  /* Return absolute path of config file if file_path is NULL. */
  if (!file_path) {
    abs_file_path = g_strdup (abs_cfg_path);
    return abs_file_path;
  }

  delim = g_strrstr (abs_cfg_path, "/");
  *(delim + 1) = '\0';

  abs_file_path = g_strconcat (abs_cfg_path, file_path, NULL);
  g_free (file_path);

  return abs_file_path;
}

static gboolean
bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_WARNING:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_warning (msg, &error, &debug);
      g_printerr ("WARNING from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), error->message);
      g_free (debug);
      g_printerr ("Warning: %s\n", error->message);
      g_error_free (error);
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_error (msg, &error, &debug);
      g_printerr ("ERROR from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), error->message);
      if (debug)
        g_printerr ("Error details: %s\n", debug);
      g_free (debug);
      g_error_free (error);
      g_main_loop_quit (loop);
      break;
    }
#ifndef PLATFORM_TEGRA
    case GST_MESSAGE_ELEMENT:
    {
      if (gst_nvmessage_is_stream_eos (msg)) {
        guint stream_id;
        if (gst_nvmessage_parse_stream_eos (msg, &stream_id)) {
          g_print ("Got EOS from stream %d\n", stream_id);
        }
      }
      break;
    }
#endif
    default:
      break;
  }
  return TRUE;
}

typedef struct _perf_measure{
	 GstClockTime pre_time;
	 GstClockTime total_time;
	 guint count;
}perf_measure;

static void
cb_newpad (GstElement * decodebin, GstPad * decoder_src_pad, gpointer data)
{
  g_print ("In cb_newpad\n");
  GstCaps *caps = gst_pad_get_current_caps (decoder_src_pad);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);
  GstElement *source_bin = (GstElement *) data;
  GstCapsFeatures *features = gst_caps_get_features (caps, 0);

  /* Need to check if the pad created by the decodebin is for video and not
   * audio. */
  if (!strncmp (name, "video", 5)) {
    /* Link the decodebin pad only if decodebin has picked nvidia
     * decoder plugin nvdec_*. We do this by checking if the pad caps contain
     * NVMM memory features. */
    if (gst_caps_features_contains (features, GST_CAPS_FEATURES_NVMM)) {
      /* Get the source bin ghost pad */
      GstPad *bin_ghost_pad = gst_element_get_static_pad (source_bin, "src");
      if (!gst_ghost_pad_set_target (GST_GHOST_PAD (bin_ghost_pad),
              decoder_src_pad)) { g_print("Hi debug 4");
        g_printerr ("Failed to link decoder src pad to source bin ghost pad\n");
      }
      gst_object_unref (bin_ghost_pad);
    } else {
      g_printerr ("Error: Decodebin did not pick nvidia decoder plugin.\n");
    }
  }
}

static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
  g_print ("Decodebin child added: %s\n", name);
  if (g_strrstr (name, "decodebin") == name) {
    g_signal_connect (G_OBJECT (object), "child-added",
        G_CALLBACK (decodebin_child_added), user_data);
  }
}

#define CHECK_ERROR(error) \
  if (error) { \
    g_printerr ("Error while parsing config file: %s\n", error->message); \
    goto done; \
  }

static gboolean
set_tracker_properties (GstElement *nvtracker, char * config_file_name)
{
  gboolean ret = FALSE;
  GError *error = NULL;
  gchar **keys = NULL;
  gchar **key = NULL;
  GKeyFile *key_file = g_key_file_new ();

  if (!g_key_file_load_from_file (key_file, config_file_name, G_KEY_FILE_NONE,
                                  &error)) {
    g_printerr ("Failed to load config file: %s\n", error->message);
    return FALSE;
  }

  keys = g_key_file_get_keys (key_file, CONFIG_GROUP_TRACKER, NULL, &error);
  CHECK_ERROR (error);

  for (key = keys; *key; key++) {
    if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_WIDTH)) {
      gint width =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_WIDTH, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracker-width", width, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_HEIGHT)) {
      gint height =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_HEIGHT, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracker-height", height, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GPU_ID)) {
      guint gpu_id =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GPU_ID, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "gpu_id", gpu_id, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_LL_CONFIG_FILE)) {
      char* ll_config_file = get_absolute_file_path (config_file_name,
                    g_key_file_get_string (key_file,
                    CONFIG_GROUP_TRACKER,
                    CONFIG_GROUP_TRACKER_LL_CONFIG_FILE, &error));
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "ll-config-file",
          ll_config_file, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_LL_LIB_FILE)) {
      char* ll_lib_file = get_absolute_file_path (config_file_name,
                g_key_file_get_string (key_file,
                    CONFIG_GROUP_TRACKER,
                    CONFIG_GROUP_TRACKER_LL_LIB_FILE, &error));
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "ll-lib-file", ll_lib_file, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS)) {
      gboolean enable_batch_process =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "enable_batch_process",
                    enable_batch_process, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_TRACKING_SURFACE_TYPE)) {
      guint tracking_surface_type =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_TRACKING_SURFACE_TYPE, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracking_surface_type",
                    tracking_surface_type, NULL);
    } else {
      g_printerr ("Unknown key '%s' for group [%s]", *key,
          CONFIG_GROUP_TRACKER);
    }
  }

  ret = TRUE;
done:
  if (error) {
    g_error_free (error);
  }
  if (keys) {
    g_strfreev (keys);
  }
  if (!ret) {
    g_printerr ("%s failed", __func__);
  }
  return ret;
}


static GstElement *
create_source_bin (guint index, gchar * uri)
{
  GstElement *bin = NULL, *uri_decode_bin = NULL;
  gchar bin_name[16] = { };

  g_snprintf (bin_name, 15, "source-bin-%02d", index);
  /* Create a source GstBin to abstract this bin's content from the rest of the
   * pipeline */
  bin = gst_bin_new (bin_name);

  /* Source element for reading from the uri.
   * We will use decodebin and let it figure out the container format of the
   * stream and the codec and plug the appropriate demux and decode plugins. */
  uri_decode_bin = gst_element_factory_make ("uridecodebin", "uri-decode-bin");

  if (!bin || !uri_decode_bin) {
    g_printerr ("One element in source bin could not be created.\n");
    return NULL;
  }

  /* We set the input uri to the source element */
  g_object_set (G_OBJECT (uri_decode_bin), "uri", uri, NULL);

  /* Connect to the "pad-added" signal of the decodebin which generates a
   * callback once a new pad for raw data has beed created by the decodebin */
  g_signal_connect (G_OBJECT (uri_decode_bin), "pad-added",
      G_CALLBACK (cb_newpad), bin);
  g_signal_connect (G_OBJECT (uri_decode_bin), "child-added",
      G_CALLBACK (decodebin_child_added), bin);

  gst_bin_add (GST_BIN (bin), uri_decode_bin);

  /* We need to create a ghost pad for the source bin which will act as a proxy
   * for the video decoder src pad. The ghost pad will not have a target right
   * now. Once the decode bin creates the video decoder and generates the
   * cb_newpad callback, we will set the ghost pad target to the video decoder
   * src pad. */
  if (!gst_element_add_pad (bin, gst_ghost_pad_new_no_target ("src",
              GST_PAD_SRC))) {
    g_printerr ("Failed to add ghost pad in source bin\n");
    return NULL;
  }

  return bin;
}



/* osd_sink_pad_buffer_probe  will extract metadata received on OSD sink pad
 * and dump bounding box data to a file. */

static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
  GstBuffer *buf = (GstBuffer *) info->data;
  NvDsFrameMeta *frame_meta = NULL;
  guint person_count = 0;
  guint bag_count = 0;
  guint face_count = 0;
  
  NvDsMetaList *l_frame, *l_obj;

  gchar bbox_file[1024] = { 0 };
  FILE *bbox_params_dump_file = NULL;
  GstClockTime now;
  perf_measure * perf = (perf_measure *)(u_data);
  
  
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);
  
  
  now = g_get_monotonic_time();

  if (perf->pre_time == GST_CLOCK_TIME_NONE) {
    perf->pre_time = now;
    perf->total_time = GST_CLOCK_TIME_NONE;
  } else {
    if (perf->total_time == GST_CLOCK_TIME_NONE) {
      perf->total_time = (now - perf->pre_time);
    } else {
      perf->total_time += (now - perf->pre_time);
    }
    perf->pre_time = now;
    perf->count++;
  }


  if (!batch_meta) {
    // No batch meta attached.
    return GST_PAD_PROBE_OK;
  }
  
  gchar kitti_track_dir_path[50] = "/home/ds_root/peoplenet/saved_metadata/";
  //NvDsFrameMeta *frame_meta = l_frame->data;
  //guint stream_id = frame_meta->pad_index;
  g_snprintf (bbox_file, sizeof (bbox_file) - 1,
      "%stracker_working_data1.txt", kitti_track_dir_path);
  bbox_params_dump_file = fopen (bbox_file, "a");
  //if (!bbox_params_dump_file)
    //continue;
   

  for (l_frame = batch_meta->frame_meta_list; l_frame; l_frame = l_frame->next) {
    frame_meta = (NvDsFrameMeta *) l_frame->data;

    if (frame_meta == NULL) {
      // Ignore Null frame meta.
      continue;
    }
    

    for (l_obj = frame_meta->obj_meta_list; l_obj; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) l_obj->data;

      if (obj_meta == NULL) {
        // Ignore Null object.
        continue;
      }

     
      if (obj_meta->class_id == PGIE_CLASS_ID_PERSON)
        person_count++;
      if (obj_meta->class_id == PGIE_CLASS_ID_BAG)
        bag_count++;
      if (obj_meta->class_id == PGIE_CLASS_ID_FACE)
        face_count++;

     
      
      float top = obj_meta->rect_params.top;
      float left = obj_meta->rect_params.left;
      float right = left + obj_meta->rect_params.width;
      float bottom = top + obj_meta->rect_params.height;
      float confidence = obj_meta->confidence;
      guint64 id = obj_meta->object_id;
      fprintf (bbox_params_dump_file,
          "%s %lu 0.0 0 0.0 %f %f %f %f 0.0 0.0 0.0 0.0 0.0 0.0 0.0 %f\n",
          obj_meta->obj_label, id, left, top, right, bottom, confidence);
      
    }
        
    
  }
  
  fprintf (bbox_params_dump_file,"Frame Number = %d People Count = %d Bag Count = %d Face Count = %d \n", frame_number, person_count, bag_count, face_count);
  g_print ("Frame Number = %d People Count = %d Bag Count = %d Face Count = %d \n",
      frame_number, person_count, bag_count, face_count);
  frame_number++;
  fclose (bbox_params_dump_file);
  

  return GST_PAD_PROBE_OK;
}

/* osd_sink_pad_buffer_probe  will extract metadata received on OSD sink pad
 * and dump bounding box data with tracking id added to a file*/

static GstPadProbeReturn
osd_sink_pad_buffer_probe_tracking (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
  GstBuffer *buf = (GstBuffer *) info->data;
  NvDsFrameMeta *frame_meta = NULL;
  guint person_count = 0;
  guint bag_count = 0;
  guint face_count = 0;
  
  NvDsMetaList *l_frame, *l_obj;

  gchar bbox_file[1024] = { 0 };
  FILE *bbox_params_dump_file = NULL;
  GstClockTime now;
  
  
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);
  
  perf_measure * perf = (perf_measure *)(u_data);
  
  now = g_get_monotonic_time();

  if (perf->pre_time == GST_CLOCK_TIME_NONE) {
    perf->pre_time = now;
    perf->total_time = GST_CLOCK_TIME_NONE;
  } else {
    if (perf->total_time == GST_CLOCK_TIME_NONE) {
      perf->total_time = (now - perf->pre_time);
    } else {
      perf->total_time += (now - perf->pre_time);
    }
    perf->pre_time = now;
    perf->count++;
  }
  
  if (!batch_meta) {
    // No batch meta attached.
    return GST_PAD_PROBE_OK;
  }
  
  //gchar kitti_track_dir_path[50] = "/home/ds_root/peoplenet/saved_metadata/";
  //NvDsFrameMeta *frame_meta = l_frame->data;
  //guint stream_id = frame_meta->pad_index;
  //g_snprintf (bbox_file, sizeof (bbox_file) - 1,
  //    "%stracker_working_data.txt", kitti_track_dir_path);
  g_snprintf (bbox_file, sizeof (bbox_file) - 1,
      "metadata_dwarper.txt");
  bbox_params_dump_file = fopen (bbox_file, "a");
  //if (!bbox_params_dump_file)
    //continue;
   

  for (l_frame = batch_meta->frame_meta_list; l_frame; l_frame = l_frame->next) {
    frame_meta = (NvDsFrameMeta *) l_frame->data;

    if (frame_meta == NULL) {
      // Ignore Null frame meta.
      continue;
    }
    

    for (l_obj = frame_meta->obj_meta_list; l_obj; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) l_obj->data;

      if (obj_meta == NULL) {
        // Ignore Null object.
        continue;
      }

     
      if (obj_meta->class_id == PGIE_CLASS_ID_PERSON)
        person_count++;
      if (obj_meta->class_id == PGIE_CLASS_ID_BAG)
        bag_count++;
      if (obj_meta->class_id == PGIE_CLASS_ID_FACE)
        face_count++;

     
      
      float top = obj_meta->tracker_bbox_info.org_bbox_coords.top;//rect_params.top;
      float left = obj_meta->tracker_bbox_info.org_bbox_coords.left;//rect_params.left;
      float right = left + obj_meta->tracker_bbox_info.org_bbox_coords.width;//rect_params.width;
      float bottom = top + obj_meta->tracker_bbox_info.org_bbox_coords.height;//rect_params.height;
      float confidence = obj_meta->tracker_confidence; //confidence;
      guint64 id = obj_meta->object_id;
      fprintf (bbox_params_dump_file,
          "%s %lu 0.0 0 0.0 %f %f %f %f 0.0 0.0 0.0 0.0 0.0 0.0 0.0 %f\n",
          obj_meta->obj_label, id, left, top, right, bottom, confidence);
      
    }
        
    
  }
  
  fprintf (bbox_params_dump_file,"Frame Number = %d People Count = %d Bag Count = %d Face Count = %d \n", frame_number, person_count, bag_count, face_count);
  //g_print ("Frame Number = %d People Count = %d Bag Count = %d Face Count = %d \n",
  //    frame_number, person_count, bag_count, face_count);
  frame_number++;
  fclose (bbox_params_dump_file);
  

  return GST_PAD_PROBE_OK;
}


int
main (int argc, char *argv[])
{
  GMainLoop *loop = NULL;
  GstElement *pipeline = NULL, *streammux = NULL, *caps_filter = NULL, 
             *tiler = NULL, *nvdewarper = NULL, *nvvideoconvert = NULL, *nvinfer = NULL, *nvosd = NULL, *sink = NULL;
 
 GstElement  *nvh264enc = NULL, *capfilt = NULL, *nvvidconv1 = NULL , *queue1 = NULL, *queue2 = NULL;

 GstElement *tracker = NULL;

#ifdef PLATFORM_TEGRA
  GstElement *transform = NULL;
#endif
  GstBus *bus = NULL;
  guint bus_watch_id;
  guint i, num_sources;
  guint tiler_rows, tiler_columns;
  guint arg_index = 0;
  guint max_surface_per_frame;
 
  GstCaps *caps = NULL;
  GstCapsFeatures *feature = NULL;
  GstPad *osd_sink_pad = NULL;
  perf_measure perf_measure;
  
  //static guint i = 0;
 
  //perf_measure perf_measure;
  /* Check input arguments */
  if (argc < 6) {
    g_printerr ("Usage: %s [1:file sink|2: fakesink|3:display sink] [1:no tracking| 2:tracking] <uri1> <source id1> [<uri2> <source id2>] ... [<uriN> <source idN>]\n", argv[0]); //TODO @mj: changed the Usage
    return -1;
  }
  num_sources = (argc - 3) / 3; 
  
  gst_init (&argc, &argv);
  loop = g_main_loop_new (NULL, FALSE);

  perf_measure.pre_time = GST_CLOCK_TIME_NONE;
  perf_measure.total_time = GST_CLOCK_TIME_NONE;
  perf_measure.count = 0;

  /* Create gstreamer elements */
  /* Create Pipeline element that will form a connection of other elements */
  pipeline = gst_pipeline_new ("dewarper-app-pipeline");

  /* Create nvstreammux instance to form batches from one or more sources. */
  streammux = gst_element_factory_make ("nvstreammux", "stream-muxer");

  if (!pipeline || !streammux) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }
  gst_bin_add (GST_BIN (pipeline), streammux);

  arg_index = 3;
  
  for (i = 0; i < num_sources; i++) {
    guint source_id = 0;

    GstPad *mux_sinkpad, *srcbin_srcpad, *dewarper_srcpad, *nvvideoconvert_sinkpad;
    gchar pad_name[16] = { };
    
    g_printerr ("args1: %s",argv[arg_index] );
    
    GstElement *source_bin = create_source_bin (i, argv[arg_index++]);
    g_printerr ("args2: %s",argv[arg_index] );
    if (!source_bin) {
      g_printerr ("Failed to create source bin. Exiting.\n");
      return -1;
    }

    source_id = atoi(argv[arg_index++]);

    /* create nv dewarper element */
    nvvideoconvert = gst_element_factory_make ("nvvideoconvert", NULL);
    if (!nvvideoconvert) {
      g_printerr ("Failed to create nvvideoconvert element. Exiting.\n");
      return -1;
    }

    caps_filter = gst_element_factory_make ("capsfilter", NULL);
    if (!caps_filter) {
      g_printerr ("Failed to create capsfilter element. Exiting.\n");
      return -1;
    }

    GstCaps *caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL);
    GstCapsFeatures *feature = gst_caps_features_new (MEMORY_FEATURES, NULL);
    gst_caps_set_features (caps, 0, feature);

    g_object_set (G_OBJECT (caps_filter), "caps", caps, NULL);

    /* create nv dewarper element */
    nvdewarper = gst_element_factory_make ("nvdewarper", NULL);
    if (!nvdewarper) {
      g_printerr ("Failed to create nvdewarper element. Exiting.\n");
      return -1;
    }

    g_object_set (G_OBJECT (nvdewarper),
      "config-file", argv[arg_index++],
      "source-id", source_id,
      NULL);

    gst_bin_add_many (GST_BIN (pipeline), source_bin, nvvideoconvert, caps_filter, nvdewarper, NULL);

    if (!gst_element_link_many (nvvideoconvert, caps_filter, nvdewarper, NULL)) {
      g_printerr ("Elements could not be linked. Exiting.\n");
      return -1;
    }

    g_snprintf (pad_name, 15, "sink_%u", i);
    mux_sinkpad = gst_element_get_request_pad (streammux, pad_name);
    if (!mux_sinkpad) {
      g_printerr ("Streammux request sink pad failed. Exiting.\n");
      return -1;
    }

    srcbin_srcpad = gst_element_get_static_pad (source_bin, "src");
    if (!srcbin_srcpad) {
      g_printerr ("Failed to get src pad of source bin. Exiting.\n");
      return -1;
    }

    nvvideoconvert_sinkpad = gst_element_get_static_pad (nvvideoconvert, "sink");
    if (!nvvideoconvert_sinkpad) {
      g_printerr ("Failed to get sink pad of nvvideoconvert. Exiting.\n");
      return -1;
    }

    dewarper_srcpad = gst_element_get_static_pad (nvdewarper, "src");
    if (!dewarper_srcpad) {
      g_printerr ("Failed to get src pad of nvdewarper. Exiting.\n");
      return -1;
    }

    if (gst_pad_link (srcbin_srcpad, nvvideoconvert_sinkpad) != GST_PAD_LINK_OK) {
      g_printerr ("Failed to link source bin to stream muxer. Exiting.\n");
      return -1;
    }

    if (gst_pad_link (dewarper_srcpad, mux_sinkpad) != GST_PAD_LINK_OK) {
      g_printerr ("Failed to link source bin to stream muxer. Exiting.\n");
      return -1;
    }
                      
    gst_object_unref (srcbin_srcpad);
    gst_object_unref (mux_sinkpad);
    gst_object_unref (dewarper_srcpad);
    gst_object_unref (nvvideoconvert_sinkpad);
    gst_caps_unref (caps);
  }

  nvinfer = gst_element_factory_make ("nvinfer", NULL);
  if (!nvinfer) {
    g_printerr ("Failed to create nvdewarper element. Exiting.\n");
    return -1;
  }
  g_object_set (G_OBJECT (nvinfer), 
    "config-file-path", "inference_files/config_infer_primary_peoplenet.txt", NULL);

  /* Use nvtiler to composite the batched frames into a 2D tiled array based
   * on the source of the frames. */
  tiler = gst_element_factory_make ("nvmultistreamtiler", "nvtiler");

  
    
    
  nvosd = gst_element_factory_make ("nvdsosd", "nv-onscreendisplay");
  nvvidconv1 = gst_element_factory_make ("nvvideoconvert", "nvvid-converter1");
  nvh264enc = gst_element_factory_make ("nvv4l2h264enc" ,"nvvideo-h264enc"); 
  capfilt = gst_element_factory_make ("capsfilter", "nvvideo-caps");
 

  
  tracker = gst_element_factory_make ("nvtracker", "nvtracker"); 
  queue1 = gst_element_factory_make ("queue", "queue1"); 
  queue2 = gst_element_factory_make ("queue", "queue2");  

  
  /* check for sink methods */
  //g_print("argument for sink %d\n", atoi(argv[1]));
  if (atoi(argv[1]) == 1)
	   sink = gst_element_factory_make ("filesink", "nvvideo-renderer");
  else if (atoi(argv[1]) == 2)
	  sink = gst_element_factory_make ("fakesink", "fake-renderer");
  else if (atoi(argv[1]) == 3) {
#ifdef PLATFORM_TEGRA
	   transform = gst_element_factory_make ("nvegltransform", "nvegltransform");
	   if(!transform) {
		   g_printerr ("nvegltransform element could not be created. Exiting.\n");
		    return -1;   
	   }
#endif  
	   sink = gst_element_factory_make ("nveglglessink", "nvvideo-renderer");
  }



  if (!nvdewarper || !nvinfer || !tiler || !nvosd || !sink) {
   //g_print("plugins %s, %s\n", nvosd, sink); 
    g_printerr ("One element could not be created. cExiting.\n");
    return -1;
  }
//  g_object_set (G_OBJECT (sink), location, out.mp4, NULL);
  g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
      MUXER_OUTPUT_HEIGHT, "nvbuf-memory-type", 0,
      "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);
  
  g_object_get (G_OBJECT (nvdewarper), "num-batch-buffers", &max_surface_per_frame, NULL);
  //max_surface_per_frame =1

  g_object_set (G_OBJECT (streammux),
      "batch-size", num_sources*max_surface_per_frame,
      "num-surfaces-per-frame", max_surface_per_frame, NULL);  
  //GstPad *demux_sinkpad;
  gchar pad_name_d[16] = { };
  g_snprintf (pad_name_d, 15, "sink");
  //demux_sinkpad = gst_element_get_request_pad (demux, pad_name_d);
  /*if (!demux_sinkpad) {
    g_printerr ("Streamdemux request sink pad failed. Exiting.\n");
    return -1;
  }*/
  tiler_rows = (guint) sqrt (num_sources);
  tiler_columns = (guint) ceil (1.0 * num_sources / tiler_rows);
  /* we set the tiler properties here */
  g_object_set (G_OBJECT (tiler), "rows", tiler_rows, "columns", tiler_columns,
      "width", TILED_OUTPUT_WIDTH, "height", TILED_OUTPUT_HEIGHT, NULL);

  char name[300];
  snprintf(name, 300, "tracker_files/dstest_tracker_config.txt");
  
  
  if(!set_tracker_properties(tracker, name)){
	  g_printerr("Failed to set tracker properties. Exiting.\n");
	  return -1; 
  }


  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  /* Set up the pipeline */
  /* we add all elements into the pipeline */

  /* Set the pipeline the basic pipeline */
  nvvideoconvert = gst_element_factory_make("nvvideoconvert", NULL);
  gst_bin_add_many (GST_BIN (pipeline), nvinfer,
		  tracker,nvvideoconvert, nvosd, tiler, sink, NULL);

  /* add the elements for sink type */
  

  caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "I420", NULL);
  feature = gst_caps_features_new ("memory:NVMM", NULL);
  gst_caps_set_features (caps, 0, feature);
  g_object_set (G_OBJECT (capfilt), "caps", caps, NULL);

  
  if(atoi(argv[1]) == 1){
    g_object_set (G_OBJECT (sink), "location", "out.h264", NULL);

	  gst_bin_add_many (GST_BIN (pipeline), nvvidconv1, nvh264enc, capfilt, queue1, queue2, NULL);
    if (atoi(argv[2]) == 1){ 
	    if (!gst_element_link_many (streammux, nvinfer,  tiler, nvosd,  queue1, nvvidconv1, capfilt, queue2, nvh264enc, sink, NULL)){
		    g_printerr ("OSD and sink elements link failure.\n");
		    return -1;
      }
    } else if (atoi(argv[2]) == 2) {
      if (!gst_element_link_many (streammux, queue1, nvinfer, tiler,  nvvidconv1, capfilt, queue2, nvh264enc, sink, NULL)){//nvinfer, tracker , tiler, nvosd, 
		    g_printerr ("OSD and sink elements link failure.\n");
		    return -1;
      }
	  } else {
      g_printerr ("Tracking option can only be 1 or 2\n");
      g_printerr ("Usage: %s [1:file sink|2: fakesink|3:display sink] [1:without tracking| 2: with tracking] <uri1> <source id1> [<uri2> <source id2>] ... [<uriN> <source idN>]\n", argv[0]);
      return -1;
   }
   

  } else if(atoi(argv[1]) == 2){
	  
	  g_object_set (G_OBJECT (sink), "sync", 0, "async", false, NULL);
    gst_bin_add_many (GST_BIN (pipeline), nvinfer,  tracker, tiler, nvosd, sink, NULL);
	  if (!gst_element_link_many (streammux, nvinfer,  tracker, tiler, nvosd, sink, NULL)){
		  g_printerr ("OSD and sink elements link failure.\n");
		  return -1;
	  }
     
	 
  }else if (atoi(argv[1]) == 3){
#ifdef PLATFORM_TEGRA
	  gst_bin_add_many (GST_BIN (pipeline), transform,  NULL);
     
    if (atoi(argv[2]) == 1){ 
	    if (!gst_element_link_many (streammux, nvinfer,  tiler, nvosd, transform, sink, NULL)){ 
		    g_printerr ("OSD and sink elements link failure.\n");
		    return -1;
      }
    } else if (atoi(argv[2]) == 2) {
      if (!gst_element_link_many (streammux, nvinfer,  tracker, tiler,  nvosd,  transform, sink, NULL)){
		    g_printerr ("OSD and sink elements link failure.\n");
		    return -1;
      }
	  } else {
      g_printerr ("Tracking option can only be 1 or 2\n");
      g_printerr ("Usage: %s [1:file sink|2: fakesink|3:display sink] [1:without tracking| 2:with tracking] <uri1> <source id1> [<uri2> <source id2>] ... [<uriN> <source idN>]\n", argv[0]);
      return -1;
   } 
#else
	  gst_bin_add (GST_BIN (pipeline), queue1);
	  
    if (atoi(argv[2]) == 1){ 
	    if (!gst_element_link_many (streammux, nvinfer,  tiler, nvosd, sink, NULL)){
		    g_printerr ("OSD and sink elements link failure.\n");
		    return -1;
      }
    } else if (atoi(argv[2]) == 2) {
      if (!gst_element_link_many (streammux, nvinfer,  tracker, nvosd, tiler, sink, NULL)){
		    g_printerr ("OSD and sink elements link failure.\n");
		    return -1;
      }
	  } else {
      g_printerr ("Tracking option can only be 1 or 2\n");
      g_printerr ("Usage: %s [1:file sink|2: fakesink|3:display sink] [1:without tracking| 2:with tracking] <uri1> <source id1> [<uri2> <source id2>] ... [<uriN> <source idN>]\n", argv[0]);
      return -1;
   }
#endif
  }

 

  /* Lets add probe to get informed of the meta data generated, we add probe to
   * the sink pad of the osd element, since by that time, the buffer would have
   * had got all the metadata. */
  osd_sink_pad = gst_element_get_static_pad (nvosd, "sink");
  if (!osd_sink_pad)
    g_print ("Unable to get sink pad\n");
  else if (atoi(argv[2]) == 1)
    gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        osd_sink_pad_buffer_probe, NULL, NULL);
  else if (atoi(argv[2]) == 2)
    gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        osd_sink_pad_buffer_probe_tracking, &perf_measure, NULL);
  gst_object_unref (osd_sink_pad);
  
  

  g_print ("Now playing:");
   

  /*for (i = 0; i < num_sources; i++) {
    g_print (" %s,", argv[i + 2]);
  }*/
  g_print ("\n");

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
                  GST_DEBUG_GRAPH_SHOW_ALL, "dewarper_test_playing");

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Wait till pipeline encounters an error or EOS */
  g_print ("Running...\n");
  g_main_loop_run (loop);

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  
  g_print ("Average fps %f\n",
      ((perf_measure.count-1)*1000000.0)/perf_measure.total_time);
  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
  return 0;
}
