/*! \file   janus_videocall.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \contributor Bang Tran <bangtv2@viettel.com.vn>
 * \copyright GNU General Public License v3
 * \brief  Janus VideoCall plugin
 * \details Check the \ref videocall for more details.
 */

#include "plugin.h"

#include <jansson.h>

#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../record.h"
#include "../rtp.h"
#include "../rtcp.h"
#include "../sdp-utils.h"
#include "../utils.h"
#include "../auth.h"
/* Plugin information */
#define JANUS_VIDEOCALL_VERSION 7
#define JANUS_VIDEOCALL_VERSION_STRING "0.0.7"
#define JANUS_VIDEOCALL_DESCRIPTION "This is a simple video call plugin for Janus, allowing two WebRTC peers to call each other through a server."
#define JANUS_VIDEOCALL_NAME "JANUS VideoCall plugin"
#define JANUS_VIDEOCALL_AUTHOR "Meetecho s.r.l."
#define JANUS_VIDEOCALL_PACKAGE "janus.plugin.videocall"

/* Plugin methods */
janus_plugin *create(void);
int janus_videocall_init(janus_callbacks *callback, const char *config_path);
void janus_videocall_destroy(void);
int janus_videocall_get_api_compatibility(void);
int janus_videocall_get_version(void);
const char *janus_videocall_get_version_string(void);
const char *janus_videocall_get_description(void);
const char *janus_videocall_get_name(void);
const char *janus_videocall_get_author(void);
const char *janus_videocall_get_package(void);
void janus_videocall_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_videocall_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void janus_videocall_setup_media(janus_plugin_session *handle);
void janus_videocall_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet);
void janus_videocall_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet);
void janus_videocall_incoming_data(janus_plugin_session *handle, janus_plugin_data *packet);
void janus_videocall_slow_link(janus_plugin_session *handle, int uplink, int video);
void janus_videocall_hangup_media(janus_plugin_session *handle);
void janus_videocall_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_videocall_query_session(janus_plugin_session *handle);

/* Plugin setup */
static janus_plugin janus_videocall_plugin =
	JANUS_PLUGIN_INIT(
			.init = janus_videocall_init,
			.destroy = janus_videocall_destroy,

			.get_api_compatibility = janus_videocall_get_api_compatibility,
			.get_version = janus_videocall_get_version,
			.get_version_string = janus_videocall_get_version_string,
			.get_description = janus_videocall_get_description,
			.get_name = janus_videocall_get_name,
			.get_author = janus_videocall_get_author,
			.get_package = janus_videocall_get_package,

			.create_session = janus_videocall_create_session,
			.handle_message = janus_videocall_handle_message,
			.setup_media = janus_videocall_setup_media,
			.incoming_rtp = janus_videocall_incoming_rtp,
			.incoming_rtcp = janus_videocall_incoming_rtcp,
			.incoming_data = janus_videocall_incoming_data,
			.slow_link = janus_videocall_slow_link,
			.hangup_media = janus_videocall_hangup_media,
			.destroy_session = janus_videocall_destroy_session,
			.query_session = janus_videocall_query_session, );

/* Plugin creator */
janus_plugin *create(void)
{
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_VIDEOCALL_NAME);
	return &janus_videocall_plugin;
}

/* Parameter validation */
static struct janus_json_parameter request_parameters[] = {
	{"request", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}};
static struct janus_json_parameter username_parameters[] = {
	{"username", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}};
static struct janus_json_parameter set_parameters[] = {
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"record", JANUS_JSON_BOOL, 0},
	{"filename", JSON_STRING, 0},
	{"restart", JANUS_JSON_BOOL, 0}};

/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static gboolean notify_events = TRUE;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static GThread *record_handler_thread;
static char *record_dir;
static void *janus_videocall_handler(void *data);
static void *janus_videocall_record_handler(void *data);

typedef struct janus_videocall_message
{
	janus_plugin_session *handle;
	char *transaction;
	json_t *message;
	json_t *jsep;
} janus_videocall_message;

typedef enum janus_call_state
{
	CALL_INIT,
	CALL_BUSY,
	CALL_RINGING,
	CALL_ACCEPTED,
	CALL_REJECT,
	CALL_MISSED,
	CALL_STARTED,
	CALL_TIMEOUT,
	CALL_ENDED
} janus_call_state;

typedef struct janus_videocall_record
{
	gboolean is_video;
	char *dir;
	char *video_1;
	char *audio_1;
	char *video_2;
	char *audio_2;
	gchar *output;
} janus_videocall_record;

static GAsyncQueue *messages = NULL;
static janus_videocall_message exit_message;

static GAsyncQueue *records = NULL;
static janus_videocall_record exit_record_event;

typedef struct janus_videocall_session
{
	//	guint64 videocall_id;
	uint32_t duration;
	janus_mutex mutex;
	janus_call_state state;
	volatile gint timeout;
	volatile gint has_started;
	volatile gint record;
	gboolean is_video;
	gint64 start_time;
	gint64 stop_time;
	gint64 start_ringtime;
	janus_refcount ref;
} janus_videocall_session;

typedef struct janus_user_session
{
	janus_videocall_session *call;
	janus_plugin_session *curr_handle;
	janus_plugin_session *handle;
	GList *handles;
	gchar *username;
	gboolean has_audio;
	gboolean has_video;
	gboolean has_data;
	gboolean audio_active;
	gboolean video_active;
	janus_audiocodec acodec; /* Codec used for audio, if available */
	janus_videocodec vcodec; /* Codec used for video, if available */
	uint32_t bitrate, peer_bitrate;
	guint16 slowlink_count;
	struct janus_user_session *peer;
	janus_rtp_switching_context context;
	uint32_t ssrc[3]; /* Only needed in case VP8 (or H.264) simulcasting is involved */
	char *rid[3];	  /* Only needed if simulcasting is rid-based */
	janus_rtp_simulcasting_context sim_context;
	janus_vp8_simulcast_context vp8_context;
	janus_recorder *arc;   /* The Janus recorder instance for this user's audio, if enabled */
	janus_recorder *vrc;   /* The Janus recorder instance for this user's video, if enabled */
	janus_recorder *drc;   /* The Janus recorder instance for this user's data, if enabled */
	janus_mutex rec_mutex; /* Mutex to protect the recorders from race conditions */
	janus_mutex mutex;
	volatile gint has_started;
	volatile gint incall;
	volatile gint hangingup;
	volatile gint destroyed;
	janus_refcount ref;
} janus_user_session;
static GHashTable *sessions;
static GHashTable *users = NULL;
static janus_mutex sessions_mutex = JANUS_MUTEX_INITIALIZER;

static void janus_user_session_destroy(janus_user_session *session)
{
	if (session && g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1))
		janus_refcount_decrease(&session->ref);
}

static void janus_user_session_free(const janus_refcount *session_ref)
{
	janus_user_session *session = janus_refcount_containerof(session_ref, janus_user_session, ref);
	/* Remove the reference to the core plugin session */
	if (session->handle)
		janus_refcount_decrease(&session->handle->ref);
	if (session->handles)
	{
		GList *handle_list = session->handles;
		while (handle_list)
		{
			janus_plugin_session *handle = (janus_plugin_session *)handle_list->data;
			janus_refcount_decrease(&handle->ref);
			handle_list = handle_list->next;
		}
		g_list_free(session->handles);
	}
	/* This session can be destroyed, free all the resources */
	if (session->call)
		janus_refcount_decrease(&session->call->ref);
	g_free(session->username);
	g_free(session);
}

static void janus_videocall_session_free(const janus_refcount *session_ref)
{
	janus_videocall_session *session = janus_refcount_containerof(session_ref, janus_videocall_session, ref);
	/* This session can be destroyed, free all the resources */
	g_free(session);
}

static void janus_videocall_message_free(janus_videocall_message *msg)
{
	if (!msg || msg == &exit_message)
		return;

	if (msg->handle && msg->handle->plugin_handle)
	{
		janus_user_session *session = (janus_user_session *)msg->handle->plugin_handle;
		janus_refcount_decrease(&session->ref);
	}
	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if (msg->message)
		json_decref(msg->message);
	msg->message = NULL;
	if (msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}

static void janus_auth_free_user(char *user)
{
	g_free(user);
}

static void janus_videocall_record_free(janus_videocall_record *record)
{
	if (!record || record == &exit_record_event)
		return;

	g_free(record->dir);
	record->dir = NULL;
	if (record->video_1)
	{
		g_free(record->video_1);
		record->video_1 = NULL;
	}
	if (record->audio_1)
	{
		g_free(record->audio_1);
		record->audio_1 = NULL;
	}
	if (record->video_2)
	{
		g_free(record->video_2);
		record->video_2 = NULL;
	}
	if (record->audio_2)
	{
		g_free(record->audio_2);
		record->audio_2 = NULL;
	}

	g_free(record);
}

/* Error codes */
#define JANUS_VIDEOCALL_ERROR_UNKNOWN_ERROR 499
#define JANUS_VIDEOCALL_ERROR_NO_MESSAGE 470
#define JANUS_VIDEOCALL_ERROR_INVALID_JSON 471
#define JANUS_VIDEOCALL_ERROR_INVALID_REQUEST 472
#define JANUS_VIDEOCALL_ERROR_REGISTER_FIRST 473
#define JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT 474
#define JANUS_VIDEOCALL_ERROR_MISSING_ELEMENT 475
#define JANUS_VIDEOCALL_ERROR_USERNAME_TAKEN 476
#define JANUS_VIDEOCALL_ERROR_ALREADY_REGISTERED 477
#define JANUS_VIDEOCALL_ERROR_NO_SUCH_USERNAME 478
#define JANUS_VIDEOCALL_ERROR_USE_ECHO_TEST 479
#define JANUS_VIDEOCALL_ERROR_ALREADY_IN_CALL 480
#define JANUS_VIDEOCALL_ERROR_NO_CALL 481
#define JANUS_VIDEOCALL_ERROR_MISSING_SDP 482
#define JANUS_VIDEOCALL_ERROR_INVALID_SDP 483

/* Plugin implementation */
int janus_videocall_init(janus_callbacks *callback, const char *config_path)
{
	if (g_atomic_int_get(&stopping))
	{
		/* Still stopping from before */
		return -1;
	}
	if (callback == NULL || config_path == NULL)
	{
		/* Invalid arguments */
		return -1;
	}
	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.jcfg", config_path, JANUS_VIDEOCALL_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if (config == NULL)
	{
		JANUS_LOG(LOG_WARN, "Couldn't find .jcfg configuration file (%s), trying .cfg\n", JANUS_VIDEOCALL_PACKAGE);
		g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_VIDEOCALL_PACKAGE);
		JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
		config = janus_config_parse(filename);
		janus_config_print(config);
	}
	if (config != NULL)
	{
		janus_config_print(config);
		janus_config_category *config_general = janus_config_get_create(config, NULL, janus_config_type_category, "general");
		janus_config_item *events = janus_config_get(config, config_general, janus_config_type_item, "events");
		if (events != NULL && events->value != NULL)
			notify_events = janus_is_true(events->value);

		if (!notify_events && callback->events_is_enabled())
		{
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_VIDEOCALL_NAME);
		}

		janus_config_item *record_dir_item = janus_config_get(config, config_general, janus_config_type_item, "record_dir");
		if (record_dir_item != NULL && record_dir_item->value != NULL)
			record_dir = record_dir_item->value;
		else
		{
			JANUS_LOG(LOG_ERR, "Misssing parameter (record_dir)\n");
			return -1;
		}
	}
	janus_config_destroy(config);
	config = NULL;

	sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)janus_user_session_destroy);
	users = g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)janus_auth_free_user, NULL);

	messages = g_async_queue_new_full((GDestroyNotify)janus_videocall_message_free);
	records = g_async_queue_new_full((GDestroyNotify)janus_videocall_record_free);
	/* This is the callback we'll need to invoke to contact the Janus core */
	gateway = callback;

	g_atomic_int_set(&initialized, 1);

	/* Launch the thread that will handle incoming messages */
	GError *error = NULL;
	handler_thread = g_thread_try_new("videocall handler", janus_videocall_handler, NULL, &error);
	if (error != NULL)
	{
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the VideoCall handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	record_handler_thread = g_thread_try_new("videocall record handler", janus_videocall_record_handler, NULL, &error);
	if (error != NULL)
	{
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the VideoCall record handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_VIDEOCALL_NAME);
	return 0;
}

void janus_videocall_destroy(void)
{
	if (!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if (handler_thread != NULL)
	{
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}
	g_async_queue_push(records, &exit_record_event);
	if (record_handler_thread != NULL)
	{
		g_thread_join(record_handler_thread);
		record_handler_thread = NULL;
	}
	/* FIXME We should destroy the sessions cleanly */
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	sessions = NULL;
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	g_free(record_dir);
	record_dir = NULL;
	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_VIDEOCALL_NAME);
}

int janus_videocall_get_api_compatibility(void)
{
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_videocall_get_version(void)
{
	return JANUS_VIDEOCALL_VERSION;
}

const char *janus_videocall_get_version_string(void)
{
	return JANUS_VIDEOCALL_VERSION_STRING;
}

const char *janus_videocall_get_description(void)
{
	return JANUS_VIDEOCALL_DESCRIPTION;
}

const char *janus_videocall_get_name(void)
{
	return JANUS_VIDEOCALL_NAME;
}

const char *janus_videocall_get_author(void)
{
	return JANUS_VIDEOCALL_AUTHOR;
}

const char *janus_videocall_get_package(void)
{
	return JANUS_VIDEOCALL_PACKAGE;
}

void janus_videocall_create_session(janus_plugin_session *handle, int *error)
{
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
	{
		*error = -1;
		return;
	}
	janus_user_session *session = g_malloc0(sizeof(janus_user_session));
	session->handles = NULL;
	session->handle = NULL;
	session->curr_handle = NULL;
	session->call = NULL;
	session->has_audio = FALSE;
	session->has_video = FALSE;
	session->has_data = FALSE;
	session->audio_active = TRUE;
	session->video_active = TRUE;
	session->bitrate = 0; /* No limit */
	session->peer_bitrate = 0;
	session->peer = NULL;
	session->username = NULL;
	janus_rtp_switching_context_reset(&session->context);
	janus_rtp_simulcasting_context_reset(&session->sim_context);
	janus_vp8_simulcast_context_reset(&session->vp8_context);
	janus_mutex_init(&session->rec_mutex);
	janus_mutex_init(&session->mutex);
	g_atomic_int_set(&session->has_started, 0);
	g_atomic_int_set(&session->incall, 0);
	g_atomic_int_set(&session->hangingup, 0);
	g_atomic_int_set(&session->destroyed, 0);
	handle->plugin_handle = session;
	janus_refcount_init(&session->ref, janus_user_session_free);

	return;
}

void janus_videocall_destroy_session(janus_plugin_session *handle, int *error)
{
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
	{
		*error = -1;
		return;
	}
	janus_user_session *session = (janus_user_session *)handle->plugin_handle;
	if (!session)
	{
		JANUS_LOG(LOG_ERR, "No VideoCall session associated with this handle...\n");
		*error = -2;
		return;
	}
	janus_mutex_lock(&sessions_mutex);
	janus_videocall_hangup_media(handle);

	JANUS_LOG(LOG_VERB, "Removing VideoCall user %s session...\n", session->username ? session->username : "'unknown'");
	if (session->username != NULL)
	{
		if (session->handles)
		{
			if (g_list_find(session->handles, handle))
			{
				JANUS_LOG(LOG_VERB, "g_list_remove: session->handles\n");
				session->handles = g_list_remove(session->handles, handle);
				janus_refcount_decrease(&handle->ref);
			}
		}

		if (session->handles == NULL)
		{
			int res = g_hash_table_remove(sessions, (gpointer)session->username);
			JANUS_LOG(LOG_VERB, "  -- Removed: %d\n", res);
		}
	}
	else
	{
		janus_user_session_destroy(session);
	}
	janus_mutex_unlock(&sessions_mutex);
	return;
}

json_t *janus_videocall_query_session(janus_plugin_session *handle)
{
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
	{
		return NULL;
	}
	janus_user_session *session = (janus_user_session *)handle->plugin_handle;
	if (!session)
	{
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	janus_refcount_increase(&session->ref);
	/* Provide some generic info, e.g., if we're in a call and with whom */
	janus_user_session *peer = session->peer;
	json_t *info = json_object();
	json_object_set_new(info, "state", json_string(session->peer ? "incall" : "idle"));
	json_object_set_new(info, "username", session->username ? json_string(session->username) : NULL);
	if (peer)
	{
		json_object_set_new(info, "peer", peer->username ? json_string(peer->username) : NULL);
		json_object_set_new(info, "audio_active", session->audio_active ? json_true() : json_false());
		json_object_set_new(info, "video_active", session->video_active ? json_true() : json_false());
		if (session->acodec != JANUS_AUDIOCODEC_NONE)
			json_object_set_new(info, "audio_codec", json_string(janus_audiocodec_name(session->acodec)));
		if (session->vcodec != JANUS_VIDEOCODEC_NONE)
			json_object_set_new(info, "video_codec", json_string(janus_videocodec_name(session->vcodec)));
		json_object_set_new(info, "video_active", session->video_active ? json_true() : json_false());
		json_object_set_new(info, "bitrate", json_integer(session->bitrate));
		json_object_set_new(info, "peer-bitrate", json_integer(session->peer_bitrate));
		json_object_set_new(info, "slowlink_count", json_integer(session->slowlink_count));
	}
	if (session->ssrc[0] != 0 || session->rid[0] != NULL)
	{
		json_object_set_new(info, "simulcast", json_true());
	}
	if (peer && (peer->ssrc[0] != 0 || peer->rid[0] != NULL))
	{
		json_object_set_new(info, "simulcast-peer", json_true());
		json_object_set_new(info, "substream", json_integer(session->sim_context.substream));
		json_object_set_new(info, "substream-target", json_integer(session->sim_context.substream_target));
		json_object_set_new(info, "temporal-layer", json_integer(session->sim_context.templayer));
		json_object_set_new(info, "temporal-layer-target", json_integer(session->sim_context.templayer_target));
	}
	if (session->arc || session->vrc || session->drc)
	{
		json_t *recording = json_object();
		if (session->arc && session->arc->filename)
			json_object_set_new(recording, "audio", json_string(session->arc->filename));
		if (session->vrc && session->vrc->filename)
			json_object_set_new(recording, "video", json_string(session->vrc->filename));
		if (session->drc && session->drc->filename)
			json_object_set_new(recording, "data", json_string(session->drc->filename));
		json_object_set_new(info, "recording", recording);
	}
	json_object_set_new(info, "incall", json_integer(g_atomic_int_get(&session->incall)));
	json_object_set_new(info, "hangingup", json_integer(g_atomic_int_get(&session->hangingup)));
	json_object_set_new(info, "destroyed", json_integer(g_atomic_int_get(&session->destroyed)));
	janus_refcount_decrease(&session->ref);
	return info;
}

struct janus_plugin_result *janus_videocall_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep)
{
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);
	janus_user_session *session = (janus_user_session *)handle->plugin_handle;
	if (!session)
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "No session associated with this handle", NULL);

	janus_videocall_message *msg = g_malloc(sizeof(janus_videocall_message));
	/* Increase the reference counter for this session: we'll decrease it after we handle the message */
	janus_refcount_increase(&session->ref);

	msg->handle = handle;
	msg->transaction = transaction;
	msg->message = message;
	msg->jsep = jsep;
	g_async_queue_push(messages, msg);

	/* All the requests to this plugin are handled asynchronously */
	return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
}

void janus_videocall_setup_media(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_INFO, "[%s-%p] WebRTC media is now available\n", JANUS_VIDEOCALL_PACKAGE, handle);
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_user_session *session = (janus_user_session *)handle->plugin_handle;
	if (!session)
	{
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}

	janus_videocall_session *call = session->call;
	janus_mutex_lock(&call->mutex);
	if (call->state == CALL_ACCEPTED && g_atomic_int_get(&session->peer->has_started))
	{
		call->start_time = janus_get_real_time();
		if (call->record) //  record config
		{
			char filename[255];
			memset(filename, 0, 255);
			JANUS_LOG(LOG_INFO, "Recording path: %s\n", record_dir);
			// peer 1
			janus_mutex_lock(&session->rec_mutex);
			if (session->has_audio)
			{
				/* FIXME We assume we're recording Opus, here */
				g_snprintf(filename, 255, "%s/%s-%ld_audio", record_dir, session->username, call->start_time);
				JANUS_LOG(LOG_VERB, "\nAudio file: %s\n", filename);
				session->arc = janus_recorder_create(NULL, janus_audiocodec_name(session->acodec), filename);
				if (session->arc == NULL)
				{
					/* FIXME We should notify the fact the recorder could not be created */
					JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this VideoCall user!\n");
				}
				JANUS_LOG(LOG_VERB, "Audio codec of peer 1: %s\n", janus_audiocodec_name(session->acodec));
			}
			if (session->has_video && session->call->is_video)
			{
				/* FIXME We assume we're recording VP8, here */
				memset(filename, 0, 255);
				g_snprintf(filename, 255, "%s/%s-%ld_video", record_dir, session->username, call->start_time);
				JANUS_LOG(LOG_VERB, "\nVideo file: %s\n", filename);
				session->vrc = janus_recorder_create(NULL, janus_videocodec_name(session->vcodec), filename);
				if (session->vrc == NULL)
				{
					/* FIXME We should notify the fact the recorder could not be created */
					JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this VideoCall user!\n");
				}
				/* Send a PLI */
				JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
				gateway->send_pli(session->handle);
				JANUS_LOG(LOG_VERB, "Video codec of peer 1: %s\n", janus_videocodec_name(session->vcodec));
			}
			janus_mutex_unlock(&session->rec_mutex);
			// peer 2
			janus_mutex_lock(&session->peer->rec_mutex);
			if (session->peer->has_audio)
			{
				/* FIXME We assume we're recording Opus, here */
				memset(filename, 0, 255);
				g_snprintf(filename, 255, "%s/%s-%ld_audio", record_dir, session->peer->username, call->start_time);
				JANUS_LOG(LOG_VERB, "\nAudio file: %s\n", filename);
				session->peer->arc = janus_recorder_create(NULL, janus_audiocodec_name(session->peer->acodec), filename);
				if (session->peer->arc == NULL)
				{
					/* FIXME We should notify the fact the recorder could not be created */
					JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this VideoCall user!\n");
				}
				JANUS_LOG(LOG_VERB, "Audio codec of peer 2: %s\n", janus_audiocodec_name(session->peer->acodec));
			}
			if (session->peer->has_video && call->is_video)
			{
				/* FIXME We assume we're recording VP8, here */
				memset(filename, 0, 255);
				g_snprintf(filename, 255, "%s/%s-%ld_video", record_dir, session->peer->username, call->start_time);
				JANUS_LOG(LOG_VERB, "\nVideo file: %s\n", filename);
				session->peer->vrc = janus_recorder_create(NULL, janus_videocodec_name(session->peer->vcodec), filename);
				if (session->peer->vrc == NULL)
				{
					/* FIXME We should notify the fact the recorder could not be created */
					JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this VideoCall user!\n");
				}
				/* Send a PLI */
				JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
				gateway->send_pli(session->peer->handle);
				JANUS_LOG(LOG_VERB, "Video codec of peer 2: %s\n", janus_videocodec_name(session->peer->vcodec));
			}
			janus_mutex_unlock(&session->peer->rec_mutex);
		}
		call->state = CALL_STARTED;
		JANUS_LOG(LOG_INFO, "A call has started...\n");
	}
	janus_mutex_unlock(&call->mutex);

	if (g_atomic_int_get(&session->destroyed))
		return;
	g_atomic_int_set(&session->has_started, 1);
	g_atomic_int_set(&session->hangingup, 0);
	/* We really don't care, as we only relay RTP/RTCP we get in the first place anyway */
}

void janus_videocall_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet)
{
	if (handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if (gateway)
	{
		/* Honour the audio/video active flags */
		janus_user_session *session = (janus_user_session *)handle->plugin_handle;
		janus_videocall_session *call = session->call;
		if (!session)
		{
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		janus_user_session *peer = session->peer;
		if (!peer)
		{
			JANUS_LOG(LOG_ERR, "Session has no peer...\n");
			return;
		}
		if (g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&peer->destroyed))
			return;

		// check call time exceed duration
		janus_mutex_lock(&call->mutex);
		if (call->duration && call->state == CALL_STARTED)
		{
			gint64 now = janus_get_real_time();
			if ((now - call->start_time) >= call->duration * G_USEC_PER_SEC) // timeout
			{
				call->state = CALL_TIMEOUT;
				gateway->close_pc(session->handle);
			}
		}
		janus_mutex_unlock(&call->mutex);

		gboolean video = packet->video;
		char *buf = packet->buffer;
		uint16_t len = packet->length;
		if (video && session->video_active && (session->ssrc[0] != 0 || session->rid[0] != NULL)) // handle simulcast
		{
			/* Handle simulcast: backup the header information first */
			janus_rtp_header *header = (janus_rtp_header *)buf;
			uint32_t seq_number = ntohs(header->seq_number);
			uint32_t timestamp = ntohl(header->timestamp);
			uint32_t ssrc = ntohl(header->ssrc);
			/* Process this packet: don't relay if it's not the SSRC/layer we wanted to handle
			 * The caveat is that the targets in OUR simulcast context are the PEER's targets */
			gboolean relay = janus_rtp_simulcasting_context_process_rtp(&peer->sim_context,
																		buf, len, session->ssrc, session->rid, session->vcodec, &peer->context);
			/* Do we need to drop this? */
			if (!relay)
				return;
			if (peer->sim_context.need_pli)
			{
				/* Send a PLI */
				JANUS_LOG(LOG_VERB, "We need a PLI for the simulcast context\n");
				gateway->send_pli(session->handle);
			}
			/* Any event we should notify? */
			if (peer->sim_context.changed_substream)
			{
				/* Notify the user about the substream change */
				json_t *event = json_object();
				json_object_set_new(event, "videocall", json_string("event"));
				json_t *result = json_object();
				json_object_set_new(result, "event", json_string("simulcast"));
				json_object_set_new(result, "videocodec", json_string(janus_videocodec_name(session->vcodec)));
				json_object_set_new(result, "substream", json_integer(session->sim_context.substream));
				json_object_set_new(event, "result", result);
				gateway->push_event(peer->handle, &janus_videocall_plugin, NULL, event, NULL);
				json_decref(event);
			}
			if (peer->sim_context.changed_temporal)
			{
				/* Notify the user about the temporal layer change */
				json_t *event = json_object();
				json_object_set_new(event, "videocall", json_string("event"));
				json_t *result = json_object();
				json_object_set_new(result, "event", json_string("simulcast"));
				json_object_set_new(result, "videocodec", json_string(janus_videocodec_name(session->vcodec)));
				json_object_set_new(result, "temporal", json_integer(session->sim_context.templayer));
				json_object_set_new(event, "result", result);
				gateway->push_event(peer->handle, &janus_videocall_plugin, NULL, event, NULL);
				json_decref(event);
			}
			/* If we got here, update the RTP header and send the packet */
			janus_rtp_header_update(header, &peer->context, TRUE, 0);
			if (session->vcodec == JANUS_VIDEOCODEC_VP8)
			{
				int plen = 0;
				char *payload = janus_rtp_payload(buf, len, &plen);
				janus_vp8_simulcast_descriptor_update(payload, plen, &peer->vp8_context, peer->sim_context.changed_substream);
			}
			/* Save the frame if we're recording (and make sure the SSRC never changes even if the substream does) */
			header->ssrc = htonl(1);
			janus_recorder_save_frame(session->vrc, buf, len);
			/* Send the frame back */
			gateway->relay_rtp(peer->handle, packet);
			/* Restore header or core statistics will be messed up */
			header->ssrc = htonl(ssrc);
			header->timestamp = htonl(timestamp);
			header->seq_number = htons(seq_number);
		}
		else
		{
			if ((!video && session->audio_active) || (video && session->video_active))
			{
				/* Save the frame if we're recording */
				if (call->record)
					janus_recorder_save_frame(video ? session->vrc : session->arc, buf, len);
				/* Forward the packet to the peer */
				gateway->relay_rtp(peer->handle, packet);
			}
		}
	}
}

void janus_videocall_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet)
{
	if (handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if (gateway)
	{
		janus_user_session *session = (janus_user_session *)handle->plugin_handle;
		if (!session)
		{
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		janus_user_session *peer = session->peer;
		if (!peer)
		{
			JANUS_LOG(LOG_ERR, "Session has no peer...\n");
			return;
		}
		if (g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&peer->destroyed))
			return;
		guint32 bitrate = janus_rtcp_get_remb(packet->buffer, packet->length);
		if (bitrate > 0)
		{
			/* If a REMB arrived, make sure we cap it to our configuration, and send it as a video RTCP */
			session->peer_bitrate = bitrate;
			/* No limit ~= 10000000 */
			gateway->send_remb(handle, session->bitrate ? session->bitrate : 10000000);
			return;
		}
		gateway->relay_rtcp(peer->handle, packet);
	}
}

void janus_videocall_incoming_data(janus_plugin_session *handle, janus_plugin_data *packet)
{
	if (handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if (gateway)
	{
		janus_user_session *session = (janus_user_session *)handle->plugin_handle;
		if (!session)
		{
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		janus_user_session *peer = session->peer;
		if (!peer)
		{
			JANUS_LOG(LOG_ERR, "Session has no peer...\n");
			return;
		}
		if (g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&peer->destroyed))
			return;
		if (packet->buffer == NULL || packet->length == 0)
			return;
		char *label = packet->label;
		char *buf = packet->buffer;
		uint16_t len = packet->length;
		JANUS_LOG(LOG_VERB, "Got a %s DataChannel message (%d bytes) to forward\n",
				  !packet->binary ? "text" : "binary", len);
		/* Save the frame if we're recording */
		janus_recorder_save_frame(session->drc, buf, len);
		/* Forward the packet to the peer */
		janus_plugin_data r = {
			.label = label,
			.binary = packet->binary,
			.buffer = buf,
			.length = len};
		gateway->relay_data(peer->handle, &r);
	}
}

void janus_videocall_slow_link(janus_plugin_session *handle, int uplink, int video)
{
	/* The core is informing us that our peer got or sent too many NACKs, are we pushing media too hard? */
	if (handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_user_session *session = (janus_user_session *)handle->plugin_handle;
	if (!session)
	{
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if (g_atomic_int_get(&session->destroyed))
		return;
	session->slowlink_count++;
	if (uplink && !video && !session->audio_active)
	{
		/* We're not relaying audio and the peer is expecting it, so NACKs are normal */
		JANUS_LOG(LOG_VERB, "Getting a lot of lost packets (slow uplink) for audio, but that's expected, a configure disabled the audio forwarding\n");
	}
	else if (uplink && video && !session->video_active)
	{
		/* We're not relaying video and the peer is expecting it, so NACKs are normal */
		JANUS_LOG(LOG_VERB, "Getting a lot of lost packets (slow uplink) for video, but that's expected, a configure disabled the video forwarding\n");
	}
	else
	{
		JANUS_LOG(LOG_WARN, "Getting a lot of lost packets (slow %s) for %s\n",
				  uplink ? "uplink" : "downlink", video ? "video" : "audio");
		if (!uplink)
		{
			/* Send an event on the handle to notify the application: it's
			 * up to the application to then choose a policy and enforce it */
			json_t *event = json_object();
			json_object_set_new(event, "videocall", json_string("event"));
			/* Also add info on what the current bitrate cap is */
			json_t *result = json_object();
			json_object_set_new(result, "event", json_string("slow_link"));
			json_object_set_new(result, "media", json_string(video ? "video" : "audio"));
			if (video)
				json_object_set_new(result, "current-bitrate", json_integer(session->bitrate));
			json_object_set_new(event, "result", result);
			gateway->push_event(session->handle, &janus_videocall_plugin, NULL, event, NULL);
			json_decref(event);
		}
	}
}

static void janus_videocall_recorder_close(janus_user_session *session)
{

	if (session->arc)
	{
		janus_recorder *rc = session->arc;
		session->arc = NULL;
		janus_recorder_close(rc);
		JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", rc->filename ? rc->filename : "??");
		janus_recorder_destroy(rc);
	}
	if (session->vrc)
	{
		janus_recorder *rc = session->vrc;
		session->vrc = NULL;
		janus_recorder_close(rc);
		JANUS_LOG(LOG_INFO, "Closed video recording %s\n", rc->filename ? rc->filename : "??");
		janus_recorder_destroy(rc);
	}
	if (session->drc)
	{
		janus_recorder *rc = session->drc;
		session->drc = NULL;
		janus_recorder_close(rc);
		JANUS_LOG(LOG_INFO, "Closed data recording %s\n", rc->filename ? rc->filename : "??");
		janus_recorder_destroy(rc);
	}
	// if (!session->vrc && !session->arc && !session->peer->vrc && !session->peer->arc && !session->drc && !session->peer->drc){
	// 	if(notify_events && gateway->events_is_enabled()) {
	// 			JANUS_LOG(LOG_ERR, "\nSend a record event %s\n", session->record_path);
	// 			json_t *info = json_object();
	// 			json_object_set_new(info, "event", json_string("record"));
	// 			json_object_set_new(info, "path", json_string(session->record_path));
	// 			gateway->notify_event(&janus_videocall_plugin, session->handle, info);
	// 	}
	// }
}

void janus_videocall_hangup_media(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_INFO, "[%s-%p] No WebRTC media anymore\n", JANUS_VIDEOCALL_PACKAGE, handle);
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_user_session *session = (janus_user_session *)handle->plugin_handle;
	if (!session)
	{
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}

	if (g_atomic_int_get(&session->destroyed))
		return;
	if (!g_atomic_int_compare_and_exchange(&session->hangingup, 0, 1))
		return;
	if (handle != session->handle)
	{
		JANUS_LOG(LOG_WARN, "Session isn't handling this handler\n");
		return;
	}
	/* Get rid of the recorders, if available */
	janus_videocall_session *call = session->call;
	if (call)
	{
		janus_mutex_lock(&call->mutex);
		if (call->state == CALL_STARTED || call->state == CALL_TIMEOUT)
		{
			call->stop_time = janus_get_real_time();
			json_t *result = NULL;
			result = json_object();
			if (g_atomic_int_compare_and_exchange(&call->record, 1, 0))
			{
				janus_videocall_record *record = g_malloc(sizeof(janus_videocall_record));
				record->dir = g_strdup(record_dir);
				record->is_video = call->is_video;
				janus_mutex_lock(&session->rec_mutex);
				if (session->arc)
					record->audio_1 = g_strdup(session->arc->filename);
				if (session->vrc && call->is_video)
					record->video_1 = g_strdup(session->vrc->filename);
				janus_videocall_recorder_close(session);
				janus_mutex_unlock(&session->rec_mutex);

				janus_mutex_lock(&session->peer->rec_mutex);
				if (session->peer->arc)
					record->audio_2 = g_strdup(session->peer->arc->filename);
				if (session->peer->vrc && call->is_video)
					record->video_2 = g_strdup(session->peer->vrc->filename);
				janus_videocall_recorder_close(session->peer);
				janus_mutex_unlock(&session->peer->rec_mutex);
				record->output = g_strdup_printf("%s_%s-%s-%ld", call->is_video ? "videocall" : "audiocall",
												 session->username, session->peer->username, call->start_time);
				json_object_set_new(result, "record_path", json_string(g_strdup_printf("%s/%s.%s", record_dir, record->output, call->is_video ? "webm" : "mp3")));
				g_async_queue_push(records, record);
			}
			if (call->state == CALL_STARTED)
				call->state = CALL_ENDED;
			/* Prepare JSON event */
			json_object_set_new(result, "event", json_string("stop"));
			json_object_set_new(result, "start_time", json_integer(call->start_time));
			json_object_set_new(result, "stop_time", json_integer(call->stop_time));
			json_object_set_new(result, "call_state", json_integer(call->state));
			json_t *event = json_object();
			json_object_set_new(event, "videocall", json_string("event"));
			json_object_set_new(event, "result", result);
			int ret = gateway->push_event(session->handle, &janus_videocall_plugin, NULL, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			ret = gateway->push_event(session->peer->handle, &janus_videocall_plugin, NULL, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);

			call->state = CALL_ENDED;
			JANUS_LOG(LOG_INFO, "The call has stopped...\n");
		}
		janus_mutex_unlock(&call->mutex);
	}

	janus_user_session *peer = session->peer;
	session->peer = NULL;

	if (peer)
		gateway->close_pc(peer->handle);

	/* Reset controls */
	session->has_audio = FALSE;
	session->has_video = FALSE;
	session->has_data = FALSE;
	session->audio_active = TRUE;
	session->video_active = TRUE;
	session->acodec = JANUS_AUDIOCODEC_NONE;
	session->vcodec = JANUS_VIDEOCODEC_NONE;
	session->bitrate = 0;
	session->peer_bitrate = 0;
	int i = 0;
	for (i = 0; i < 3; i++)
	{
		session->ssrc[i] = 0;
		g_free(session->rid[i]);
		session->rid[i] = NULL;
	}
	janus_rtp_switching_context_reset(&session->context);
	janus_rtp_simulcasting_context_reset(&session->sim_context);
	janus_vp8_simulcast_context_reset(&session->vp8_context);
	if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
	{
		janus_refcount_decrease(&peer->ref);
	}
	janus_rtp_switching_context_reset(&session->context);
	g_atomic_int_set(&session->hangingup, 0);
	g_atomic_int_set(&session->has_started, 0);
	janus_refcount_decrease(&session->handle->ref);
	session->handle = NULL;
	if (session->call)
	{
		janus_refcount_decrease(&session->call->ref);
		session->call = NULL;
	}
}

/* Thread to handle incoming messages */
static void *janus_videocall_handler(void *data)
{
	JANUS_LOG(LOG_VERB, "Joining VideoCall handler thread\n");
	janus_videocall_message *msg = NULL;
	int error_code = 0;
	char error_cause[512];
	json_t *root = NULL;
	while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping))
	{
		msg = g_async_queue_pop(messages);
		if (msg == &exit_message)
			break;
		if (msg->handle == NULL)
		{
			janus_videocall_message_free(msg);
			continue;
		}
		janus_user_session *session = (janus_user_session *)msg->handle->plugin_handle;
		if (!session)
		{
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_videocall_message_free(msg);
			continue;
		}
		if (g_atomic_int_get(&session->destroyed))
		{
			janus_videocall_message_free(msg);
			continue;
		}
		/* Handle request */
		error_code = 0;
		root = msg->message;
		if (msg->message == NULL)
		{
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_VIDEOCALL_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		if (!json_is_object(root))
		{
			JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
			error_code = JANUS_VIDEOCALL_ERROR_INVALID_JSON;
			g_snprintf(error_cause, 512, "JSON error: not an object");
			goto error;
		}
		JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
								   error_code, error_cause, TRUE,
								   JANUS_VIDEOCALL_ERROR_MISSING_ELEMENT, JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT);
		if (error_code != 0)
			goto error;
		const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
		const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
		json_t *request = json_object_get(root, "request");
		const char *request_text = json_string_value(request);
		json_t *result = NULL;
		gboolean sdp_update = FALSE;
		if (json_object_get(msg->jsep, "update") != NULL)
			sdp_update = json_is_true(json_object_get(msg->jsep, "update"));
		if (!strcasecmp(request_text, "list"))
		{
			result = json_object();
			json_t *list = json_array();
			JANUS_LOG(LOG_VERB, "Request for the list of peers\n");
			/* Return a list of all available mountpoints */
			janus_mutex_lock(&sessions_mutex);
			GHashTableIter iter;
			gpointer value;
			g_hash_table_iter_init(&iter, sessions);
			while (g_hash_table_iter_next(&iter, NULL, &value))
			{
				janus_user_session *user = value;
				if (user != NULL)
				{
					janus_refcount_increase(&user->ref);
					if (user->username != NULL)
						json_array_append_new(list, json_string(user->username));
					janus_refcount_decrease(&user->ref);
				}
			}
			json_object_set_new(result, "list", list);
			janus_mutex_unlock(&sessions_mutex);
		}
		else if (!strcasecmp(request_text, "login"))
		{
			/* Map this handle to a username */
			if (session->username != NULL)
			{
				JANUS_LOG(LOG_ERR, "Already logged-in (%s)\n", session->username);
				error_code = JANUS_VIDEOCALL_ERROR_ALREADY_REGISTERED;
				g_snprintf(error_cause, 512, "Already logged-in  (%s)", session->username);
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, username_parameters,
									   error_code, error_cause, TRUE,
									   JANUS_VIDEOCALL_ERROR_MISSING_ELEMENT, JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT);
			if (error_code != 0)
				goto error;
			json_t *username = json_object_get(root, "username");
			const char *username_text = json_string_value(username);

			if (!janus_auth_check_token(username_text))
			{
				janus_mutex_unlock(&sessions_mutex);
				JANUS_LOG(LOG_ERR, "Username '%s' hasn't registeded a token\n", username_text);
				error_code = JANUS_VIDEOCALL_ERROR_USERNAME_TAKEN;
				g_snprintf(error_cause, 512, "Username '%s' has not registeded a token", username_text);
				goto error;
			}
			janus_refcount_increase(&msg->handle->ref);
			janus_mutex_lock(&sessions_mutex);
			janus_user_session *exist_session = (janus_user_session *)g_hash_table_lookup(sessions, username_text);
			janus_mutex_unlock(&sessions_mutex);
			if (exist_session != NULL)
			{
				JANUS_LOG(LOG_WARN, "Username '%s' already logged-in \n", username_text);
				msg->handle->plugin_handle = exist_session;
				exist_session->handles = g_list_append(exist_session->handles, msg->handle);
				janus_refcount_increase(&exist_session->ref);
				janus_refcount_decrease(&session->ref);
				janus_refcount_decrease(&session->ref);
				// janus_mutex_unlock(&sessions_mutex);
				// JANUS_LOG(LOG_ERR, "Username '%s' already logged-in \n", username_text);
				// error_code = JANUS_VIDEOCALL_ERROR_USERNAME_TAKEN;
				// g_snprintf(error_cause, 512, "Username '%s' already logged-in ", username_text);
				// int error;
				// janus_videocall_destroy_session(msg->handle, &error);
				// goto error;
			}
			else
			{
				session->username = g_strdup(username_text);
				session->handles = g_list_append(session->handles, msg->handle);
				janus_mutex_lock(&sessions_mutex);
				g_hash_table_insert(sessions, (gpointer)session->username, session);
				janus_mutex_unlock(&sessions_mutex);
			}
			result = json_object();
			json_object_set_new(result, "event", json_string("connected"));
			json_object_set_new(result, "username", json_string(username_text));
			/* Also notify event handlers */
			if (notify_events && gateway->events_is_enabled())
			{
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("connected"));
				json_object_set_new(info, "username", json_string(username_text));
				gateway->notify_event(&janus_videocall_plugin, session->handle, info);
			}
		}
		else if (!strcasecmp(request_text, "call"))
		{
			janus_mutex_lock(&session->mutex);
			/* Call another peer */
			if (session->username == NULL) // check if username exits
			{
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "Register a username first\n");
				error_code = JANUS_VIDEOCALL_ERROR_REGISTER_FIRST;
				g_snprintf(error_cause, 512, "Register a username first");
				/* Hangup the call attempt of the user */
				gateway->close_pc(msg->handle);
				goto error;
			}
			if (session->peer != NULL) // Already in a call
			{
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "Already in a call\n");
				error_code = JANUS_VIDEOCALL_ERROR_ALREADY_IN_CALL;
				g_snprintf(error_cause, 512, "Already in a call");
				/* Hangup the call attempt of the user */
				gateway->close_pc(msg->handle);
				goto error;
			}
			if (!g_atomic_int_compare_and_exchange(&session->incall, 0, 1)) // Already in a call (but no peer?)
			{
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "Already in a call (but no peer?)\n");
				error_code = JANUS_VIDEOCALL_ERROR_ALREADY_IN_CALL;
				g_snprintf(error_cause, 512, "Already in a call (but no peer)");
				/* Hangup the call attempt of the user */
				gateway->close_pc(msg->handle);
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, username_parameters,
									   error_code, error_cause, TRUE,
									   JANUS_VIDEOCALL_ERROR_MISSING_ELEMENT, JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT);
			if (error_code != 0)
			{
				/* Hangup the call attempt of the user */
				janus_mutex_unlock(&session->mutex);
				g_atomic_int_set(&session->incall, 0);
				gateway->close_pc(msg->handle);
				goto error;
			}
			json_t *username = json_object_get(root, "username");
			const char *username_text = json_string_value(username);
			if (!strcmp(username_text, session->username)) // check if call yourself
			{
				g_atomic_int_set(&session->incall, 0);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "You can't call yourself... use the EchoTest for that\n");
				error_code = JANUS_VIDEOCALL_ERROR_USE_ECHO_TEST;
				g_snprintf(error_cause, 512, "You can't call yourself... use the EchoTest for that");
				/* Hangup the call attempt of the user */
				gateway->close_pc(msg->handle);
				goto error;
			}
			janus_mutex_lock(&sessions_mutex);
			janus_user_session *peer = g_hash_table_lookup(sessions, username_text);
			if (peer == NULL || g_atomic_int_get(&peer->destroyed)) // peer not exits
			{
				g_atomic_int_set(&session->incall, 0);
				janus_mutex_unlock(&sessions_mutex);
				JANUS_LOG(LOG_ERR, "Username '%s' doesn't exist\n", username_text);
				error_code = JANUS_VIDEOCALL_ERROR_NO_SUCH_USERNAME;
				g_snprintf(error_cause, 512, "Username '%s' doesn't exist", username_text);
				/* Hangup the call attempt of the user */
				gateway->close_pc(msg->handle);
				goto error;
			}
			/* If the call attempt proceeds we keep the references */
			janus_refcount_increase(&session->ref);
			janus_refcount_increase(&peer->ref);
			if (g_atomic_int_get(&peer->incall) || peer->peer != NULL) // check if 2 user is busy
			{
				janus_mutex_unlock(&session->mutex);
				if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
				{
					janus_refcount_decrease(&session->ref);
					janus_refcount_decrease(&peer->ref);
				}
				janus_mutex_unlock(&sessions_mutex);
				JANUS_LOG(LOG_VERB, "%s is busy\n", username_text);

				// send stop event
				result = json_object();
				json_object_set_new(result, "event", json_string("stop"));
				json_object_set_new(result, "call_state", json_integer(CALL_BUSY));
				json_t *event = json_object();
				json_object_set_new(event, "videocall", json_string("event"));
				json_object_set_new(event, "result", result);
				int ret = gateway->push_event(msg->handle, &janus_videocall_plugin, NULL, event, NULL);
				JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
				json_decref(result);

				// result = json_object();
				// json_object_set_new(result, "event", json_string("hangup"));
				// json_object_set_new(result, "username", json_string(session->username));
				// json_object_set_new(result, "reason", json_string("User busy"));
				// /* Also notify event handlers */
				// if (notify_events && gateway->events_is_enabled())
				// {
				// 	json_t *info = json_object();
				// 	json_object_set_new(info, "event", json_string("hangup"));
				// 	json_object_set_new(info, "reason", json_string("User busy"));
				// 	gateway->notify_event(&janus_videocall_plugin, msg->handle, info);
				// }
				/* Hangup the call attempt of the user */
				gateway->close_pc(msg->handle);
			}
			else
			{
				/* Any SDP to handle? if not, something's wrong */
				if (!msg_sdp) // check if missing SDP
				{
					janus_mutex_unlock(&session->mutex);
					if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
					{
						janus_refcount_decrease(&session->ref);
						janus_refcount_decrease(&peer->ref);
					}
					janus_mutex_unlock(&sessions_mutex);
					JANUS_LOG(LOG_ERR, "Missing SDP\n");
					error_code = JANUS_VIDEOCALL_ERROR_MISSING_SDP;
					g_snprintf(error_cause, 512, "Missing SDP");
					goto error;
				}
				char error_str[512];
				janus_sdp *offer = janus_sdp_parse(msg_sdp, error_str, sizeof(error_str));
				if (offer == NULL) // check if error parsing offer
				{
					janus_mutex_unlock(&session->mutex);
					if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
					{
						janus_refcount_decrease(&session->ref);
						janus_refcount_decrease(&peer->ref);
					}
					janus_mutex_unlock(&sessions_mutex);
					JANUS_LOG(LOG_ERR, "Error parsing offer: %s\n", error_str);
					error_code = JANUS_VIDEOCALL_ERROR_INVALID_SDP;
					g_snprintf(error_cause, 512, "Error parsing offer: %s", error_str);
					goto error;
				}
				janus_sdp_destroy(offer);
				json_t *is_video = json_object_get(root, "videocall");
				json_t *isRecord = json_object_get(root, "record");
				json_t *duration = json_object_get(root, "duration");
				if (!is_video && !isRecord)
				{
					if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
					{
						janus_refcount_decrease(&session->ref);
						janus_refcount_decrease(&peer->ref);
					}
					janus_mutex_unlock(&sessions_mutex);
					JANUS_LOG(LOG_ERR, "Missing parameters (videocall, record)\n");
					error_code = JANUS_VIDEOCALL_ERROR_INVALID_SDP;
					g_snprintf(error_cause, 512, "Missing parameters (videocall, record)");
					goto error;
				}
				g_atomic_int_set(&peer->incall, 1);
				session->peer = peer;
				peer->peer = session;
				session->handle = msg->handle;
				janus_refcount_increase(&msg->handle->ref);
				session->has_audio = (strstr(msg_sdp, "m=audio") != NULL);
				session->has_video = (strstr(msg_sdp, "m=video") != NULL);
				session->has_data = (strstr(msg_sdp, "DTLS/SCTP") != NULL);
				janus_mutex_unlock(&sessions_mutex);
				JANUS_LOG(LOG_VERB, "%s is calling %s\n", session->username, peer->username);
				JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
				/* Check if this user will simulcast */
				json_t *msg_simulcast = json_object_get(msg->jsep, "simulcast");
				if (msg_simulcast)
				{
					JANUS_LOG(LOG_VERB, "VideoCall caller (%s) is going to do simulcasting\n", session->username);
					int rid_ext_id = -1, framemarking_ext_id = -1;
					janus_rtp_simulcasting_prepare(msg_simulcast, &rid_ext_id, &framemarking_ext_id, session->ssrc, session->rid);
					session->sim_context.rid_ext_id = rid_ext_id;
					session->sim_context.framemarking_ext_id = framemarking_ext_id;
				}
				/* Send SDP to our peer */
				json_t *call = json_object();
				json_object_set_new(call, "videocall", json_string("event"));
				json_t *calling = json_object();
				json_object_set_new(calling, "event", json_string("incomingcall"));
				json_object_set_new(calling, "username", json_string(session->username));
				json_object_set_new(call, "result", calling);
				json_t *jsep = json_pack("{ssss}", "type", msg_sdp_type, "sdp", msg_sdp);
				g_atomic_int_set(&session->hangingup, 0);
				GList *curr_handle = session->peer->handles;
				while (curr_handle)
				{
					janus_plugin_session *handle = (janus_plugin_session *)curr_handle->data;
					int ret = gateway->push_event(handle, &janus_videocall_plugin, NULL, call, jsep);
					JANUS_LOG(LOG_VERB, "  >> Pushing event to peer: %d (%s)\n", ret, janus_get_api_error(ret));
					curr_handle = curr_handle->next;
				}
				json_decref(call);
				json_decref(jsep);
				/* Send an ack back */
				result = json_object();
				json_object_set_new(result, "event", json_string("calling"));
				/* Also notify event handlers */
				if (notify_events && gateway->events_is_enabled())
				{
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("calling"));
					gateway->notify_event(&janus_videocall_plugin, session->handle, info);
				}
				// setup a call
				janus_videocall_session *newCall = g_malloc0(sizeof(janus_videocall_session));
				janus_mutex_init(&newCall->mutex);
				janus_refcount_init(&newCall->ref, janus_videocall_session_free);
				newCall->start_time = 0;
				newCall->start_ringtime = janus_get_real_time();
				newCall->duration = duration ? json_integer_value(duration) : NULL;
				newCall->is_video = json_is_true(is_video);
				newCall->state = CALL_INIT;
				g_atomic_int_set(&newCall->record, json_is_true(isRecord) ? 1 : 0);
				g_atomic_int_set(&newCall->has_started, 0);
				g_atomic_int_set(&newCall->timeout, 0);

				session->call = newCall;
				peer->call = newCall;
				janus_refcount_increase(&newCall->ref);
				janus_mutex_unlock(&session->mutex);
			}
		}
		else if (!strcasecmp(request_text, "accept"))
		{
			janus_mutex_lock(&session->mutex);
			/* Accept a call from another peer */
			janus_user_session *peer = session->peer;
			if (peer == NULL || !g_atomic_int_get(&session->incall) || !g_atomic_int_get(&peer->incall))
			{
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "No incoming call to accept\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "No incoming call to accept");
				goto error;
			}
			if (session->call == NULL)
			{
				JANUS_LOG(LOG_ERR, "The call not exits...\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "The call not exits");
				goto error;
			}
			janus_mutex_lock(&session->call->mutex);
			if (session->call->state == CALL_ACCEPTED)
			{
				janus_mutex_unlock(&session->mutex);
				janus_mutex_unlock(&session->call->mutex);
				JANUS_LOG(LOG_ERR, "The call has started...\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "The call has started");
				goto error;
			}
			janus_mutex_unlock(&session->call->mutex);

			janus_refcount_increase(&peer->ref);
			/* Any SDP to handle? if not, something's wrong */
			if (!msg_sdp)
			{
				janus_mutex_unlock(&session->mutex);
				janus_refcount_decrease(&peer->ref);
				JANUS_LOG(LOG_ERR, "Missing SDP\n");
				error_code = JANUS_VIDEOCALL_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "Missing SDP");
				goto error;
			}
			char error_str[512];
			janus_sdp *answer = janus_sdp_parse(msg_sdp, error_str, sizeof(error_str));
			if (answer == NULL)
			{
				janus_mutex_unlock(&session->mutex);
				janus_refcount_decrease(&peer->ref);
				JANUS_LOG(LOG_ERR, "Error parsing answer: %s\n", error_str);
				error_code = JANUS_VIDEOCALL_ERROR_INVALID_SDP;
				g_snprintf(error_cause, 512, "Error parsing answer: %s", error_str);
				goto error;
			}
			JANUS_LOG(LOG_VERB, "%s is accepting a call from %s\n", session->username, peer->username);
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
			session->has_audio = (strstr(msg_sdp, "m=audio") != NULL);
			session->has_video = (strstr(msg_sdp, "m=video") != NULL);
			session->has_data = (strstr(msg_sdp, "DTLS/SCTP") != NULL);
			/* Check if this user will simulcast */
			json_t *msg_simulcast = json_object_get(msg->jsep, "simulcast");
			if (msg_simulcast && janus_get_codec_pt(msg_sdp, "vp8") > 0)
			{
				JANUS_LOG(LOG_VERB, "VideoCall callee (%s) is going to do simulcasting\n", session->username);
				session->ssrc[0] = json_integer_value(json_object_get(msg_simulcast, "ssrc-0"));
				session->ssrc[1] = json_integer_value(json_object_get(msg_simulcast, "ssrc-1"));
				session->ssrc[2] = json_integer_value(json_object_get(msg_simulcast, "ssrc-2"));
			}
			else
			{
				int i = 0;
				for (i = 0; i < 3; i++)
				{
					session->ssrc[i] = 0;
					g_free(session->rid[0]);
					session->rid[0] = NULL;
					if (peer)
					{
						peer->ssrc[i] = 0;
						g_free(peer->rid[0]);
						peer->rid[0] = NULL;
					}
				}
			}

			/* Check which codecs we ended up using */
			const char *acodec = NULL, *vcodec = NULL;
			janus_sdp_find_first_codecs(answer, &acodec, &vcodec);
			session->acodec = janus_audiocodec_from_name(acodec);
			session->vcodec = janus_videocodec_from_name(vcodec);
			if (session->acodec == JANUS_AUDIOCODEC_NONE)
			{
				session->has_audio = FALSE;
				if (peer)
					peer->has_audio = FALSE;
			}
			else if (peer)
			{
				peer->acodec = session->acodec;
			}
			if (session->vcodec == JANUS_VIDEOCODEC_NONE)
			{
				session->has_video = FALSE;
				if (peer)
					peer->has_video = FALSE;
			}
			else if (peer)
			{
				peer->vcodec = session->vcodec;
			}

			session->handle = msg->handle;
			janus_refcount_increase(&msg->handle->ref);
			janus_mutex_lock(&session->call->mutex);
			session->call->state = CALL_ACCEPTED;
			janus_mutex_unlock(&session->call->mutex);

			janus_sdp_destroy(answer);
			/* Send SDP to our peer */
			json_t *jsep = json_pack("{ssss}", "type", msg_sdp_type, "sdp", msg_sdp);
			json_t *call = json_object();
			json_object_set_new(call, "videocall", json_string("event"));
			json_t *calling = json_object();
			json_object_set_new(calling, "event", json_string("accepted"));
			json_object_set_new(calling, "username", json_string(session->username));
			json_object_set_new(call, "result", calling);
			g_atomic_int_set(&session->hangingup, 0);
			int ret = gateway->push_event(peer->handle, &janus_videocall_plugin, NULL, call, jsep);
			JANUS_LOG(LOG_VERB, "  >> Pushing event to peer: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(call);
			json_decref(jsep);

			/* send other handles */
			GList *curr_handle = session->handles;
			while (curr_handle)
			{
				janus_plugin_session *handle = (janus_plugin_session *)curr_handle->data;
				if (handle != session->handle)
				{
					json_t *event = json_object();
					json_object_set_new(event, "videocall", json_string("event"));
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("stop"));
					json_object_set_new(info, "call_state", json_integer(CALL_ACCEPTED));
					json_object_set_new(event, "result", info);
					int ret = gateway->push_event(handle, &janus_videocall_plugin, NULL, event, NULL);
					JANUS_LOG(LOG_VERB, "  >> Pushing event to other handle: %d (%s)\n", ret, janus_get_api_error(ret));
				}
				curr_handle = curr_handle->next;
			}

			/* Send an ack back */
			result = json_object();
			json_object_set_new(result, "event", json_string("accepted"));

			/* Also notify event handlers */
			if (notify_events && gateway->events_is_enabled())
			{
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("accepted"));
				gateway->notify_event(&janus_videocall_plugin, session->handle, info);
			}
			/* Is simulcasting involved on either side? */
			if (session->ssrc[0] || session->rid[0])
			{
				peer->sim_context.substream_target = 2; /* Let's aim for the highest quality */
				peer->sim_context.templayer_target = 2; /* Let's aim for all temporal layers */
			}
			if (peer->ssrc[0] || peer->rid[0])
			{
				session->sim_context.substream_target = 2; /* Let's aim for the highest quality */
				session->sim_context.templayer_target = 2; /* Let's aim for all temporal layers */
			}
			/* We don't need this reference anymore, it was already increased by the peer calling us */
			janus_refcount_decrease(&peer->ref);
			janus_mutex_unlock(&session->mutex);
		}
		else if (!strcasecmp(request_text, "reject"))
		{
			/* Reject a call from another peer */
			janus_mutex_lock(&session->mutex);
			janus_mutex_lock(&session->call->mutex);
			janus_user_session *peer = session->peer;
			if (peer == NULL || !g_atomic_int_get(&session->incall) || !g_atomic_int_get(&peer->incall))
			{
				janus_mutex_unlock(&session->call->mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "No incoming call to reject\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "No incoming call to reject");
				goto error;
			}
			if (session->call == NULL)
			{
				janus_mutex_unlock(&session->call->mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "The call not exits...\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "The call not exits");
				goto error;
			}
			if (session->call->state == CALL_ACCEPTED)
			{
				janus_mutex_unlock(&session->call->mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "The call has started...\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "The call has started");
				goto error;
			}

			JANUS_LOG(LOG_VERB, "%s is reject the call with %s\n", session->username, peer->username);
			/* Check if we still need to remove any reference */
			if (peer && g_atomic_int_compare_and_exchange(&peer->incall, 1, 0))
			{
				janus_refcount_decrease(&session->ref);
			}
			if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
			{
				janus_refcount_decrease(&peer->ref);
			}
			session->handle = msg->handle;
			session->call->state = CALL_REJECT;

			// notify stop event to handlers
			json_t *event = json_object();
			json_object_set_new(event, "videocall", json_string("event"));
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("stop"));
			json_object_set_new(info, "call_state", json_integer(session->call->state));
			json_object_set_new(event, "result", info);
			int ret = gateway->push_event(session->peer->handle, &janus_videocall_plugin, NULL, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event to peer: %d (%s)\n", ret, janus_get_api_error(ret));
			GList *curr_handle = session->handles;
			while (curr_handle)
			{
				if (curr_handle != session->handle)
				{
					janus_plugin_session *handle = (janus_plugin_session *)curr_handle->data;
					int ret = gateway->push_event(handle, &janus_videocall_plugin, NULL, event, NULL);
					JANUS_LOG(LOG_VERB, "  >> Pushing event to peer: %d (%s)\n", ret, janus_get_api_error(ret));
				}
				curr_handle = curr_handle->next;
			}
			json_decref(event);

			// close pc
			gateway->close_pc(session->handle);
			janus_mutex_unlock(&session->call->mutex);
			janus_mutex_unlock(&session->mutex);
		}
		else if (!strcasecmp(request_text, "ringing"))
		{
			janus_videocall_session *call = session->call;
			if (call)
			{
				janus_mutex_lock(&call->mutex);
				if (call->state == CALL_INIT || call->state == CALL_RINGING)
				{
					if (call->state == CALL_INIT)
						call->state = CALL_RINGING;
					gint64 now = janus_get_real_time();
					if ((now - call->start_ringtime) >= 60 * G_USEC_PER_SEC) // timeout
					{
						call->state = CALL_MISSED;
						json_t *event = json_object();
						json_object_set_new(event, "videocall", json_string("event"));
						json_t *info = json_object();
						json_object_set_new(info, "event", json_string("stop"));
						json_object_set_new(info, "call_state", json_integer(call->state));
						json_object_set_new(event, "result", info);
						int ret = gateway->push_event(session->peer->handle, &janus_videocall_plugin, NULL, event, NULL);
						JANUS_LOG(LOG_VERB, "  >> Pushing event to peer: %d (%s)\n", ret, janus_get_api_error(ret));
						json_decref(event);
						gateway->close_pc(session->peer->handle);
					}
				}
				janus_mutex_unlock(&call->mutex);
			}
		}
		else if (!strcasecmp(request_text, "set"))
		{
			/* Update the local configuration (audio/video mute/unmute, bitrate cap or recording) */
			JANUS_VALIDATE_JSON_OBJECT(root, set_parameters,
									   error_code, error_cause, TRUE,
									   JANUS_VIDEOCALL_ERROR_MISSING_ELEMENT, JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT);
			if (error_code != 0)
				goto error;
			json_t *audio = json_object_get(root, "audio");
			json_t *video = json_object_get(root, "video");
			json_t *bitrate = json_object_get(root, "bitrate");
			json_t *record = json_object_get(root, "record");
			json_t *recfile = json_object_get(root, "filename");
			json_t *restart = json_object_get(root, "restart");
			json_t *substream = json_object_get(root, "substream");
			json_t *duration = json_object_get(root, "time");
			if (substream && (!json_is_integer(substream) || json_integer_value(substream) < 0 || json_integer_value(substream) > 2))
			{
				JANUS_LOG(LOG_ERR, "Invalid element (substream should be 0, 1 or 2)\n");
				error_code = JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid value (substream should be 0, 1 or 2)");
				goto error;
			}
			json_t *temporal = json_object_get(root, "temporal");
			if (temporal && (!json_is_integer(temporal) || json_integer_value(temporal) < 0 || json_integer_value(temporal) > 2))
			{
				JANUS_LOG(LOG_ERR, "Invalid element (temporal should be 0, 1 or 2)\n");
				error_code = JANUS_VIDEOCALL_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid value (temporal should be 0, 1 or 2)");
				goto error;
			}
			if (audio)
			{
				session->audio_active = json_is_true(audio);
				JANUS_LOG(LOG_VERB, "Setting audio property: %s\n", session->audio_active ? "true" : "false");
			}
			if (video)
			{
				if (!session->video_active && json_is_true(video))
				{
					/* Send a PLI */
					JANUS_LOG(LOG_VERB, "Just (re-)enabled video, sending a PLI to recover it\n");
					gateway->send_pli(session->handle);
				}
				session->video_active = json_is_true(video);
				JANUS_LOG(LOG_VERB, "Setting video property: %s\n", session->video_active ? "true" : "false");
			}
			if (bitrate)
			{
				session->bitrate = json_integer_value(bitrate);
				JANUS_LOG(LOG_VERB, "Setting video bitrate: %" SCNu32 "\n", session->bitrate);
				gateway->send_remb(session->handle, session->bitrate ? session->bitrate : 10000000);
			}
			janus_user_session *peer = session->peer;
			if (substream)
			{
				session->sim_context.substream_target = json_integer_value(substream);
				JANUS_LOG(LOG_VERB, "Setting video SSRC to let through (simulcast): %" SCNu32 " (index %d, was %d)\n",
						  session->ssrc[session->sim_context.substream], session->sim_context.substream_target, session->sim_context.substream);
				if (session->sim_context.substream_target == session->sim_context.substream)
				{
					/* No need to do anything, we're already getting the right substream, so notify the user */
					json_t *event = json_object();
					json_object_set_new(event, "videocall", json_string("event"));
					json_t *result = json_object();
					json_object_set_new(result, "event", json_string("simulcast"));
					json_object_set_new(result, "videocodec", json_string(janus_videocodec_name(session->vcodec)));
					json_object_set_new(result, "substream", json_integer(session->sim_context.substream));
					json_object_set_new(event, "result", result);
					gateway->push_event(session->handle, &janus_videocall_plugin, NULL, event, NULL);
					json_decref(event);
				}
				else
				{
					/* We need to change substream, send the peer a PLI */
					JANUS_LOG(LOG_VERB, "Simulcasting substream change, sending a PLI to kickstart it\n");
					if (peer && peer->handle)
						gateway->send_pli(peer->handle);
				}
			}
			if (temporal)
			{
				session->sim_context.templayer_target = json_integer_value(temporal);
				JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (simulcast): %d (was %d)\n",
						  session->sim_context.templayer_target, session->sim_context.templayer);
				if (session->vcodec == JANUS_VIDEOCODEC_VP8 && session->sim_context.templayer_target == session->sim_context.templayer)
				{
					/* No need to do anything, we're already getting the right temporal, so notify the user */
					json_t *event = json_object();
					json_object_set_new(event, "videocall", json_string("event"));
					json_t *result = json_object();
					json_object_set_new(result, "event", json_string("simulcast"));
					json_object_set_new(result, "videocodec", json_string(janus_videocodec_name(session->vcodec)));
					json_object_set_new(result, "temporal", json_integer(session->sim_context.templayer));
					json_object_set_new(event, "result", result);
					gateway->push_event(session->handle, &janus_videocall_plugin, NULL, event, NULL);
					json_decref(event);
				}
				else
				{
					/* We need to change temporal, send a PLI */
					JANUS_LOG(LOG_VERB, "Simulcasting temporal layer change, sending a PLI to kickstart it\n");
					if (peer && peer->handle)
						gateway->send_pli(peer->handle);
				}
			}
			// set time *
			if (duration)
			{
				janus_mutex_lock(&session->call->mutex);
				if (session->call->duration == 0)
					session->call->duration = json_integer_value(duration);
				JANUS_LOG(LOG_ERR, "Setting video time: %d\n", session->call->duration);
				janus_mutex_unlock(&session->call->mutex);
			}
			/* Also notify event handlers */
			if (notify_events && gateway->events_is_enabled())
			{
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("configured"));
				json_object_set_new(info, "audio_active", session->audio_active ? json_true() : json_false());
				json_object_set_new(info, "video_active", session->video_active ? json_true() : json_false());
				json_object_set_new(info, "bitrate", json_integer(session->bitrate));
				if (session->arc || session->vrc || session->drc)
				{
					json_t *recording = json_object();
					if (session->arc && session->arc->filename)
						json_object_set_new(recording, "audio", json_string(session->arc->filename));
					if (session->vrc && session->vrc->filename)
						json_object_set_new(recording, "video", json_string(session->vrc->filename));
					if (session->drc && session->drc->filename)
						json_object_set_new(recording, "data", json_string(session->drc->filename));
					json_object_set_new(info, "recording", recording);
				}
				gateway->notify_event(&janus_videocall_plugin, session->handle, info);
			}
			/* Send an ack back */
			result = json_object();
			json_object_set_new(result, "event", json_string("set"));
			/* If this is for an ICE restart, prepare the SDP to send back too */
			gboolean do_restart = restart ? json_is_true(restart) : FALSE;
			if (do_restart && !sdp_update)
			{
				JANUS_LOG(LOG_WARN, "Got a 'restart' request, but no SDP update? Ignoring...\n");
			}
			if (sdp_update && peer != NULL)
			{
				/* Forward new SDP to the peer */
				json_t *event = json_object();
				json_object_set_new(event, "videocall", json_string("event"));
				json_t *update = json_object();
				json_object_set_new(update, "event", json_string("update"));
				json_object_set_new(event, "result", update);
				json_t *jsep = json_pack("{ssss}", "type", msg_sdp_type, "sdp", msg_sdp);
				int ret = gateway->push_event(peer->handle, &janus_videocall_plugin, NULL, event, jsep);
				JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
				json_decref(event);
				json_decref(jsep);
			}
		}
		else if (!strcasecmp(request_text, "hangup"))
		{
			/* Reject a call from another peer */
			janus_mutex_lock(&session->mutex);
			janus_mutex_lock(&session->call->mutex);
			janus_user_session *peer = session->peer;
			if (peer == NULL || !g_atomic_int_get(&session->incall) || !g_atomic_int_get(&peer->incall) || session->call == NULL)
			{
				janus_mutex_unlock(&session->call->mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "No call to hangup\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "No call to hangup");
				goto error;
			}
			if (session->call->state != CALL_STARTED)
			{
				janus_mutex_unlock(&session->call->mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_ERR, "The call hasn't started...\n");
				error_code = JANUS_VIDEOCALL_ERROR_NO_CALL;
				g_snprintf(error_cause, 512, "The call hasn't started");
				goto error;
			}

			JANUS_LOG(LOG_VERB, "%s is hanging up the call with %s (%s)\n", session->username, peer->username);
			/* Check if we still need to remove any reference */
			if (peer && g_atomic_int_compare_and_exchange(&peer->incall, 1, 0))
			{
				janus_refcount_decrease(&session->ref);
			}
			if (g_atomic_int_compare_and_exchange(&session->incall, 1, 0) && peer)
			{
				janus_refcount_decrease(&peer->ref);
			}
			gateway->close_pc(session->handle);
			janus_mutex_unlock(&session->call->mutex);
			janus_mutex_unlock(&session->mutex);
		}
		else
		{
			JANUS_LOG(LOG_ERR, "Unknown request (%s)\n", request_text);
			error_code = JANUS_VIDEOCALL_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Unknown request (%s)", request_text);
			goto error;
		}

		/* Prepare JSON event */
		if (result != NULL)
		{
			json_t *event = json_object();
			json_object_set_new(event, "videocall", json_string("event"));
			json_object_set_new(event, "result", result);
			int ret = gateway->push_event(msg->handle, &janus_videocall_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
		}
		janus_videocall_message_free(msg);
		continue;

	error:
	{
		/* Prepare JSON error event */
		json_t *event = json_object();
		json_object_set_new(event, "videocall", json_string("event"));
		json_object_set_new(event, "error_code", json_integer(error_code));
		json_object_set_new(event, "error", json_string(error_cause));
		int ret = gateway->push_event(msg->handle, &janus_videocall_plugin, msg->transaction, event, NULL);
		JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
		json_decref(event);
		janus_videocall_message_free(msg);
	}
	}
	JANUS_LOG(LOG_VERB, "Leaving VideoCall handler thread\n");
	return NULL;
}
static void *janus_videocall_record_handler(void *data)
{
	JANUS_LOG(LOG_INFO, "Joining VideoCall record handler thread...\n");
	janus_videocall_record *record = NULL;
	int error_code = 0;
	char error_cause[512];
	while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping))
	{
		record = g_async_queue_pop(records);
		if (record == &exit_record_event)
			break;
		JANUS_LOG(LOG_ERR, "Record dir: %s\n", record->dir);
		JANUS_LOG(LOG_ERR, "Record type: %s\n", record->is_video ? "video" : "audio");
		gchar *record_script;
		if (record->is_video)
		{
			JANUS_LOG(LOG_ERR, "Audio mjr 1: %s\n", record->audio_1);
			JANUS_LOG(LOG_ERR, "Video mjr 1: %s\n", record->video_1);
			JANUS_LOG(LOG_ERR, "Audio mjr 2: %s\n", record->audio_2);
			JANUS_LOG(LOG_ERR, "Video mjr 2: %s\n", record->video_2);
			record_script = g_strdup_printf("./record.sh -t v -d %s -v1 %s -a1 %s -v2 %s -a2 %s -o %s", record->dir, record->video_1, record->audio_1,
											record->video_2, record->audio_2, record->output);
		}
		else
		{
			JANUS_LOG(LOG_ERR, "Audio mjr 1: %s\n", record->audio_1);
			JANUS_LOG(LOG_ERR, "Video mjr 1: %s\n", record->audio_2);
			record_script = g_strdup_printf("./record.sh -t a -d %s -v1 %s -v2 %s -o %s", record->dir, record->audio_1, record->audio_2, record->output);
		}
		JANUS_LOG(LOG_ERR, "Excuting shell script: %s\n", record_script);
		if (system(record_script) == -1)
			JANUS_LOG(LOG_ERR, "Record failed...\n");
		else
			JANUS_LOG(LOG_ERR, "Record successfully...\n");
	}
}
