/*
 * People counting message library for nvmsgconv (DeepStream 7.1).
 *
 * nvmsgconv loads this instead of its built-in converter
 * (app.txt: msg-conv-msg2p-lib). For every camera it sends one JSON message:
 *   - right away when someone crosses an entry or exit line
 *   - every heartbeat-sec otherwise, even when nobody is in view
 *
 * Counts come from nvdsanalytics line crossing metadata.
 * Settings are read from config/msgconv.txt. Message format: README.md.
 */

#include <cmath>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <glib.h>
#include <json-glib/json-glib.h>

#include "nvds_analytics_meta.h"
#include "nvdsmeta.h"
#include "nvmsgconv.h"

#define MESSAGE_VERSION "1.0"

struct SensorConfig {
  std::string id;
  std::string description;
  std::string place;
  std::vector<std::string> entry_lines;
  std::vector<std::string> exit_lines;
};

struct SensorState {
  bool started = false;
  std::string since;            /* when the totals started counting */
  guint64 seen_entries = 0;     /* totals in the last frame */
  guint64 seen_exits = 0;
  guint64 sent_entries = 0;     /* totals in the last sent message */
  guint64 sent_exits = 0;
  gint64 last_sent_us = 0;      /* monotonic clock */
};

struct PeopleCountCtx {
  std::string store_id;
  std::string store_name;
  gint64 heartbeat_us = 1000000;
  gboolean include_objects = TRUE;
  gint person_class_id = 0;
  std::unordered_map<guint, SensorConfig> sensors;   /* key: source id */
  std::unordered_map<guint, SensorState> state;      /* key: source id */
  NvDsMetaType analytics_frame_meta = NVDS_INVALID_META;
  NvDsMetaType analytics_obj_meta = NVDS_INVALID_META;
};

/* ---------- helpers ---------- */

static std::string
iso_time (gint64 epoch_us)
{
  time_t sec = epoch_us / G_USEC_PER_SEC;
  int ms = (int) ((epoch_us % G_USEC_PER_SEC) / 1000);
  struct tm t;
  char buf[40], out[48];

  gmtime_r (&sec, &t);
  strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%S", &t);
  g_snprintf (out, sizeof (out), "%s.%03dZ", buf, ms);
  return out;
}

static std::string
get_string (GKeyFile * kf, const gchar * group, const gchar * key,
    const gchar * fallback)
{
  gchar *v = g_key_file_get_string (kf, group, key, NULL);
  std::string s = v ? g_strstrip (v) : fallback;
  g_free (v);
  return s;
}

static std::vector<std::string>
get_list (GKeyFile * kf, const gchar * group, const gchar * key,
    const gchar * fallback)
{
  std::vector<std::string> out;
  gchar **items = g_strsplit (get_string (kf, group, key, fallback).c_str (),
      ";", -1);
  for (gchar ** it = items; *it; it++) {
    g_strstrip (*it);
    if (**it)
      out.push_back (*it);
  }
  g_strfreev (items);
  return out;
}

static guint64
sum_counts (const std::unordered_map<std::string, uint64_t> &counts,
    const std::vector<std::string> &lines)
{
  guint64 total = 0;
  for (const auto &line : lines) {
    auto it = counts.find (line);
    if (it != counts.end ())
      total += it->second;
  }
  return total;
}

static gboolean
contains (const std::vector<std::string> &list, const std::string &s)
{
  for (const auto &item : list)
    if (item == s)
      return TRUE;
  return FALSE;
}

static const SensorConfig &
sensor_config (PeopleCountCtx * pc, guint source_id)
{
  auto it = pc->sensors.find (source_id);
  if (it != pc->sensors.end ())
    return it->second;

  /* No [sensorN] for this camera: warn once and use defaults. */
  g_printerr ("peoplecount_msgconv: no [sensor%u] in msgconv config, "
      "using defaults\n", source_id);
  SensorConfig cfg;
  cfg.id = "source" + std::to_string (source_id);
  cfg.entry_lines = { "Entry" };
  cfg.exit_lines = { "Exit" };
  return pc->sensors.emplace (source_id, cfg).first->second;
}

/* ---------- message ---------- */

static NvDsPayload *
build_payload (PeopleCountCtx * pc, NvDsFrameMeta * frame)
{
  const guint src = frame->source_id;
  const SensorConfig &cfg = sensor_config (pc, src);
  SensorState &st = pc->state[src];

  gint64 frame_us = frame->ntp_timestamp ?
      (gint64) (frame->ntp_timestamp / 1000) : g_get_real_time ();
  gint64 now_us = g_get_monotonic_time ();

  /* 1. Line crossing totals from nvdsanalytics. */
  gboolean has_analytics = FALSE;
  guint64 total_entries = 0, total_exits = 0;
  for (NvDsMetaList * l = frame->frame_user_meta_list; l; l = l->next) {
    NvDsUserMeta *um = (NvDsUserMeta *) l->data;
    if (!um || um->base_meta.meta_type != pc->analytics_frame_meta)
      continue;
    NvDsAnalyticsFrameMeta *am = (NvDsAnalyticsFrameMeta *) um->user_meta_data;
    total_entries = sum_counts (am->objLCCumCnt, cfg.entry_lines);
    total_exits = sum_counts (am->objLCCumCnt, cfg.exit_lines);
    has_analytics = TRUE;
    break;
  }
  if (!has_analytics) {
    /* Keep the last known totals. */
    total_entries = st.seen_entries;
    total_exits = st.seen_exits;
  }

  /* 2. Totals went down = counter was reset (app or stream restart). */
  if (!st.started || total_entries < st.seen_entries
      || total_exits < st.seen_exits) {
    st.started = true;
    st.since = iso_time (frame_us);
    st.sent_entries = 0;
    st.sent_exits = 0;
  }
  st.seen_entries = total_entries;
  st.seen_exits = total_exits;

  /* 3. Send on new crossings, or when the heartbeat is due. */
  guint64 new_entries = total_entries - st.sent_entries;
  guint64 new_exits = total_exits - st.sent_exits;
  gboolean crossing = new_entries || new_exits;
  gboolean heartbeat = pc->heartbeat_us > 0 &&
      (!st.last_sent_us || now_us - st.last_sent_us >= pc->heartbeat_us);
  if (!crossing && !heartbeat)
    return NULL;

  /* 4. People in view, and who crossed a line in this frame. */
  float sx = 1.0f, sy = 1.0f;
  if (frame->pipeline_width && frame->pipeline_height) {
    sx = (float) frame->source_frame_width / frame->pipeline_width;
    sy = (float) frame->source_frame_height / frame->pipeline_height;
  }

  guint in_view = 0;
  JsonBuilder *events = json_builder_new ();
  JsonBuilder *objects = json_builder_new ();
  json_builder_begin_array (events);
  json_builder_begin_array (objects);

  for (NvDsMetaList * l = frame->obj_meta_list; l; l = l->next) {
    NvDsObjectMeta *obj = (NvDsObjectMeta *) l->data;
    if (!obj || obj->class_id != pc->person_class_id)
      continue;
    in_view++;

    gint64 obj_id = obj->object_id == UNTRACKED_OBJECT_ID ?
        -1 : (gint64) obj->object_id;

    for (NvDsMetaList * u = obj->obj_user_meta_list; u; u = u->next) {
      NvDsUserMeta *um = (NvDsUserMeta *) u->data;
      if (!um || um->base_meta.meta_type != pc->analytics_obj_meta)
        continue;
      NvDsAnalyticsObjInfo *oi = (NvDsAnalyticsObjInfo *) um->user_meta_data;
      for (const auto &line : oi->lcStatus) {
        const gchar *type = contains (cfg.entry_lines, line) ? "entry" :
            contains (cfg.exit_lines, line) ? "exit" : NULL;
        if (!type)
          continue;
        json_builder_begin_object (events);
        json_builder_set_member_name (events, "type");
        json_builder_add_string_value (events, type);
        json_builder_set_member_name (events, "object_id");
        json_builder_add_int_value (events, obj_id);
        json_builder_set_member_name (events, "line");
        json_builder_add_string_value (events, line.c_str ());
        json_builder_end_object (events);
      }
    }

    if (!pc->include_objects)
      continue;
    const NvOSD_RectParams &r = obj->rect_params;
    json_builder_begin_object (objects);
    json_builder_set_member_name (objects, "id");
    json_builder_add_int_value (objects, obj_id);
    /* whole percent: json-glib prints doubles as 0.98999999999999999 */
    json_builder_set_member_name (objects, "confidence_pct");
    json_builder_add_int_value (objects, std::lround (obj->confidence * 100.0));
    json_builder_set_member_name (objects, "bbox");
    json_builder_begin_object (objects);
    json_builder_set_member_name (objects, "left");
    json_builder_add_int_value (objects, std::lround (r.left * sx));
    json_builder_set_member_name (objects, "top");
    json_builder_add_int_value (objects, std::lround (r.top * sy));
    json_builder_set_member_name (objects, "right");
    json_builder_add_int_value (objects, std::lround ((r.left + r.width) * sx));
    json_builder_set_member_name (objects, "bottom");
    json_builder_add_int_value (objects, std::lround ((r.top + r.height) * sy));
    json_builder_end_object (objects);
    json_builder_end_object (objects);
  }
  json_builder_end_array (events);
  json_builder_end_array (objects);

  /* 5. Whole message. */
  gchar *uuid = g_uuid_string_random ();
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "version");
  json_builder_add_string_value (b, MESSAGE_VERSION);
  json_builder_set_member_name (b, "messageid");
  json_builder_add_string_value (b, uuid);
  json_builder_set_member_name (b, "@timestamp");
  json_builder_add_string_value (b, iso_time (frame_us).c_str ());
  json_builder_set_member_name (b, "reason");
  json_builder_add_string_value (b, crossing ? "crossing" : "heartbeat");

  json_builder_set_member_name (b, "store");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, pc->store_id.c_str ());
  json_builder_set_member_name (b, "name");
  json_builder_add_string_value (b, pc->store_name.c_str ());
  json_builder_end_object (b);

  json_builder_set_member_name (b, "sensor");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, cfg.id.c_str ());
  json_builder_set_member_name (b, "description");
  json_builder_add_string_value (b, cfg.description.c_str ());
  json_builder_set_member_name (b, "place");
  json_builder_add_string_value (b, cfg.place.c_str ());
  json_builder_set_member_name (b, "source_id");
  json_builder_add_int_value (b, src);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "counts");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "in_view");
  json_builder_add_int_value (b, in_view);
  json_builder_set_member_name (b, "entries");
  json_builder_add_int_value (b, new_entries);
  json_builder_set_member_name (b, "exits");
  json_builder_add_int_value (b, new_exits);
  json_builder_set_member_name (b, "total_entries");
  json_builder_add_int_value (b, total_entries);
  json_builder_set_member_name (b, "total_exits");
  json_builder_add_int_value (b, total_exits);
  json_builder_set_member_name (b, "since");
  json_builder_add_string_value (b, st.since.c_str ());
  json_builder_end_object (b);

  json_builder_set_member_name (b, "events");
  json_builder_add_value (b, json_builder_get_root (events));
  if (pc->include_objects) {
    json_builder_set_member_name (b, "objects");
    json_builder_add_value (b, json_builder_get_root (objects));
  }

  json_builder_set_member_name (b, "frame");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "number");
  json_builder_add_int_value (b, frame->frame_num);
  json_builder_set_member_name (b, "width");
  json_builder_add_int_value (b, frame->source_frame_width);
  json_builder_set_member_name (b, "height");
  json_builder_add_int_value (b, frame->source_frame_height);
  json_builder_end_object (b);

  json_builder_end_object (b);

  JsonNode *root = json_builder_get_root (b);
  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, root);
  gchar *text = json_generator_to_data (gen, NULL);

  json_node_unref (root);
  g_object_unref (gen);
  g_object_unref (b);
  g_object_unref (events);
  g_object_unref (objects);
  g_free (uuid);

  st.sent_entries = total_entries;
  st.sent_exits = total_exits;
  st.last_sent_us = now_us;

  NvDsPayload *payload = (NvDsPayload *) g_malloc0 (sizeof (NvDsPayload));
  payload->payload = text;
  payload->payloadSize = strlen (text);
  return payload;
}

static NvDsPayload *
payload_from_info (NvDsMsg2pCtx * ctx, void *metadataInfo)
{
  NvDsMsg2pMetaInfo *info = (NvDsMsg2pMetaInfo *) metadataInfo;
  if (!ctx || !ctx->privData || !info || !info->frameMeta
      || g_strcmp0 (info->mediaType, "video"))
    return NULL;
  return build_payload ((PeopleCountCtx *) ctx->privData,
      (NvDsFrameMeta *) info->frameMeta);
}

/* ---------- nvmsgconv library API ---------- */

extern "C" {

NvDsMsg2pCtx *
nvds_msg2p_ctx_create (const gchar * file, NvDsPayloadType type)
{
  GKeyFile *kf = g_key_file_new ();
  GError *err = NULL;

  if (!file || !g_key_file_load_from_file (kf, file, G_KEY_FILE_NONE, &err)) {
    g_printerr ("peoplecount_msgconv: cannot read config '%s': %s\n",
        file ? file : "(none)", err ? err->message : "no file given");
    g_clear_error (&err);
    g_key_file_free (kf);
    return NULL;
  }

  PeopleCountCtx *pc = new PeopleCountCtx ();
  pc->store_id = get_string (kf, "store", "id", "");
  pc->store_name = get_string (kf, "store", "name", "");

  if (g_key_file_has_key (kf, "message", "heartbeat-sec", NULL)) {
    gdouble sec = g_key_file_get_double (kf, "message", "heartbeat-sec", NULL);
    pc->heartbeat_us = sec > 0 ? (gint64) (sec * G_USEC_PER_SEC) : 0;
  }
  if (g_key_file_has_key (kf, "message", "include-objects", NULL))
    pc->include_objects =
        g_key_file_get_boolean (kf, "message", "include-objects", NULL);
  if (g_key_file_has_key (kf, "message", "person-class-id", NULL))
    pc->person_class_id =
        g_key_file_get_integer (kf, "message", "person-class-id", NULL);

  gchar **groups = g_key_file_get_groups (kf, NULL);
  for (gchar ** g = groups; *g; g++) {
    guint n;
    char end;
    if (sscanf (*g, "sensor%u%c", &n, &end) != 1)
      continue;

    SensorConfig cfg;
    cfg.id = get_string (kf, *g, "id", ("source" + std::to_string (n)).c_str ());
    cfg.description = get_string (kf, *g, "description", "");
    cfg.place = get_string (kf, *g, "place", "");
    cfg.entry_lines = get_list (kf, *g, "entry-lines", "Entry");
    cfg.exit_lines = get_list (kf, *g, "exit-lines", "Exit");
    pc->sensors[n] = cfg;
  }
  g_strfreev (groups);
  g_key_file_free (kf);

  if (pc->store_id.empty ()) {
    g_printerr ("peoplecount_msgconv: [store] id is missing in '%s'\n", file);
    delete pc;
    return NULL;
  }

  pc->analytics_frame_meta = NVDS_USER_FRAME_META_NVDSANALYTICS;
  pc->analytics_obj_meta = NVDS_USER_OBJ_META_NVDSANALYTICS;

  g_print ("peoplecount_msgconv: store=%s heartbeat=%.1fs sensors=%zu\n",
      pc->store_id.c_str (), pc->heartbeat_us / (double) G_USEC_PER_SEC,
      pc->sensors.size ());

  NvDsMsg2pCtx *ctx = (NvDsMsg2pCtx *) g_malloc0 (sizeof (NvDsMsg2pCtx));
  ctx->payloadType = type;
  ctx->privData = pc;
  return ctx;
}

void
nvds_msg2p_ctx_destroy (NvDsMsg2pCtx * ctx)
{
  if (!ctx)
    return;
  delete (PeopleCountCtx *) ctx->privData;
  g_free (ctx);
}

NvDsPayload *
nvds_msg2p_generate_new (NvDsMsg2pCtx * ctx, void *metadataInfo)
{
  return payload_from_info (ctx, metadataInfo);
}

NvDsPayload **
nvds_msg2p_generate_multiple_new (NvDsMsg2pCtx * ctx, void *metadataInfo,
    guint * payloadCount)
{
  *payloadCount = 0;
  NvDsPayload *payload = payload_from_info (ctx, metadataInfo);
  if (!payload)
    return NULL;
  NvDsPayload **list = (NvDsPayload **) g_malloc0 (sizeof (NvDsPayload *));
  list[0] = payload;
  *payloadCount = 1;
  return list;
}

/* Old event-meta API: not used (needs msg-conv-msg2p-new-api=1). */
NvDsPayload *
nvds_msg2p_generate (NvDsMsg2pCtx * ctx, NvDsEvent * events, guint size)
{
  return NULL;
}

NvDsPayload **
nvds_msg2p_generate_multiple (NvDsMsg2pCtx * ctx, NvDsEvent * events,
    guint size, guint * payloadCount)
{
  *payloadCount = 0;
  return NULL;
}

void
nvds_msg2p_release (NvDsMsg2pCtx * ctx, NvDsPayload * payload)
{
  if (!payload)
    return;
  g_free (payload->payload);
  g_free (payload);
}

}
