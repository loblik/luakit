/*
 * clib/download.c - wrapper for the WebKitDownload class
 *
 * Copyright © 2011 Fabian Streitel <karottenreibe@gmail.com>
 * Copyright © 2011 Mason Larobina <mason.larobina@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "common/luaobject.h"
#include "clib/download.h"
#include "luah.h"
#include "globalconf.h"

#if WITH_WEBKIT2
# include <webkit2/webkit2.h>
#else
# include <webkit/webkitdownload.h>
# include <webkit/webkitnetworkrequest.h>
# include <webkit/webkitnetworkresponse.h>
# include <libsoup/soup-message.h>
#endif
#include <glib/gstdio.h>

/** Internal data structure for luakit's downloads. */
typedef struct {
    /** Common \ref lua_object_t header. \see LUA_OBJECT_HEADER */
    LUA_OBJECT_HEADER
    /** \privatesection */
    /** The \c WebKitDownload that handles the actual data transfer. */
    WebKitDownload* webkit_download;
    /** The reference to the Lua object representing the download.
     * As long as the download is running, the object will be reffed to
     * prevent its garbage-collection.
     */
    gpointer ref;
    /** The URI that is being downloaded. */
    gchar *uri;
    /** The destination path in the filesystem where the file is save to. */
    gchar *destination;
    /** The error message in case of a download failure. */
    gchar *error;
    gboolean is_started;
    enum luakit_download_status_t {
        LUAKIT_DOWNLOAD_STATUS_FINISHED,
        LUAKIT_DOWNLOAD_STATUS_CREATED,
        LUAKIT_DOWNLOAD_STATUS_STARTED,
        LUAKIT_DOWNLOAD_STATUS_CANCELLED,
        LUAKIT_DOWNLOAD_STATUS_FAILED
    } status;
} download_t;

static lua_class_t download_class;
LUA_OBJECT_FUNCS(download_class, download_t, download)

#define luaH_checkdownload(L, idx) luaH_checkudata(L, idx, &(download_class))

/**
 * Allow garbage collection of the download.
 *
 * This function unrefs the download from the object registry.
 * It also deletes the backup files that may be created by WebKit while
 * downloading.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t to unref.
 */
static void
luaH_download_unref(lua_State *L, download_t *download)
{
    if (download->ref) {
        luaH_object_unref(L, download->ref);
        download->ref = NULL;
    }

    /* delete the annoying backup file generated while downloading */
    gchar *backup = g_strdup_printf("%s~", download->destination);
    g_unlink(backup);
    g_free(backup);
}

/**
 * Returns true if the download is currently in progress.
 * "Started" here means that a response has been received, and a destination
 * has been decided
 *
 * \param download The \ref download_t whose progress to check.
 */
static gboolean
download_is_started(download_t *download)
{
#if WITH_WEBKIT2
    return download->is_started;
#else
    WebKitDownloadStatus status = webkit_download_get_status(
            download->webkit_download);
    return status == WEBKIT_DOWNLOAD_STATUS_STARTED;
#endif
}

/**
 * Frees all data associated with the download and disposes
 * of the Lua object.
 *
 * \param L The Lua VM state.
 *
 * \luastack
 * \lparam A \c download object to free.
 */
static gint
luaH_download_gc(lua_State *L)
{
    download_t *download = luaH_checkdownload(L, 1);
    g_object_unref(G_OBJECT(download->webkit_download));

    if (download->destination)
        g_free(download->destination);
    if (download->uri)
        g_free(download->uri);
    if (download->error)
        g_free(download->error);

    return luaH_object_gc(L);
}

#if WITH_WEBKIT2
/**
 * Callback from the \c WebKitDownload when a destination needs to be
 * selected.
 *
 * \returns whether or not other handlers should be invoked for the
 * \c decide-destination signal
 */
static gboolean
decide_destination_cb(WebKitDownload* UNUSED(dl), gchar *suggested_filename, download_t *download)
{
    lua_State *L = globalconf.L;
    luaH_object_push(L, download->ref);
    lua_pushstring(L, suggested_filename);

    gint ret = luaH_object_emit_signal(L, -2, "decide-destination", 1, 1);
    gboolean handled = (ret && lua_toboolean(L, -1));
    lua_pop(L, 1 + ret);
    return handled;
}

/**
 * Callback from the \c WebKitDownload after a destination has been
 * selected.
 */
static void
created_destination_cb(WebKitDownload* UNUSED(dl), gchar *destination, download_t *download)
{
    lua_State *L = globalconf.L;
    luaH_object_push(L, download->ref);
    lua_pushstring(L, destination);

    download->status = LUAKIT_DOWNLOAD_STATUS_CREATED;
    download->is_started = TRUE;

    /* clear last download error message */
    if (download->error) {
        g_free(download->error);
        download->error = NULL;
    }

    luaH_object_emit_signal(L, 1, "created-destination", 1, 0);
    lua_pop(L, 1);
    return;
}

/**
 * Callback from the \c WebKitDownload in case of failure.
 *
 * Fills the \c error member of \ref download_t.
 */
static void
failed_cb(WebKitDownload* UNUSED(d), GError *error, download_t *download)
{
    // TODO does the GError error need a g_error_free(error)?
    /* save error message */
    if (download->error)
        g_free(download->error);
    download->error = g_strdup(error->message);
    if (error->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER) {
        download->status = LUAKIT_DOWNLOAD_STATUS_CANCELLED;
    } else {
        luaH_warn(globalconf.L, "download %p failed: %s", download, error->message);
        download->status = LUAKIT_DOWNLOAD_STATUS_FAILED;

        /* emit error signal if able */
        if (download->ref) {
            lua_State *L = globalconf.L;
            luaH_object_push(L, download->ref);
            lua_pushstring(L, error->message);
            luaH_object_emit_signal(L, -2, "error", 1, 0);
            lua_pop(L, 1);
            /* unreffing of download happens in finished_cb */
        }
    }
}

/**
 * Callback from the \c WebKitDownload once the download is complete
 * and after the failed callback (if there is a failure)
 */
static void
finished_cb(WebKitDownload* UNUSED(dl), download_t *download) {
    lua_State *L = globalconf.L;
    luaH_object_push(L, download->ref);

    if (download->status == LUAKIT_DOWNLOAD_STATUS_CREATED)
        download->status = LUAKIT_DOWNLOAD_STATUS_FINISHED;

    gint ret = luaH_object_emit_signal(L, 1, "finished", 0, 0);
    lua_pop(L, 1 + ret);

    luaH_download_unref(L, download);
    return;
}
#else
/**
 * Callback from the \c WebKitDownload in case of errors.
 *
 * Fills the \c error member of \ref download_t.
 *
 * \returns \c FALSE
 */
static gboolean
error_cb(WebKitDownload* UNUSED(d), gint UNUSED(error_code),
        gint UNUSED(error_detail), gchar *reason, download_t *download)
{
    /* save error message */
    if (download->error)
        g_free(download->error);
    download->error = g_strdup(reason);

    /* emit error signal if able */
    if (download->ref) {
        lua_State *L = globalconf.L;
        luaH_object_push(L, download->ref);
        lua_pushstring(L, reason);
        luaH_object_emit_signal(L, -2, "error", 1, 0);
        lua_pop(L, 1);
        /* unref download */
        luaH_download_unref(L, download);
    }
    return FALSE;
}
#endif

/**
 * Creates a new download on the stack.
 *
 * \param L The Lua VM state.
 *
 * \luastack
 * \lvalue A table containing properties to set on the download.
 * \lreturn A new \c download object.
 */
static gint
luaH_download_new(lua_State *L)
{
    luaH_class_new(L, &download_class);
    download_t *download = luaH_checkdownload(L, -1);

    /* create download from constructor properties */
#if WITH_WEBKIT2
    download->is_started = FALSE;
#else
    WebKitNetworkRequest *request = webkit_network_request_new(
            download->uri);
    download->webkit_download = webkit_download_new(request);
#endif
    g_object_ref(G_OBJECT(download->webkit_download));

#if WITH_WEBKIT2
    /* raise corresponding luakit signals when the webkit signals are
     * emitted
     */
    g_signal_connect(G_OBJECT(download->webkit_download),
            "decide-destination",
            G_CALLBACK(decide_destination_cb), download);

    g_signal_connect(G_OBJECT(download->webkit_download),
            "created-destination",
            G_CALLBACK(created_destination_cb), download);

    g_signal_connect(G_OBJECT(download->webkit_download),
            "finished", G_CALLBACK(finished_cb), download);

    /* raise failed signal on failure or cancellation */
    g_signal_connect(G_OBJECT(download->webkit_download),
            "failed", G_CALLBACK(failed_cb), download);
#else
    /* raise error signal on error */
    g_signal_connect(G_OBJECT(download->webkit_download), "error",
            G_CALLBACK(error_cb), download);
#endif

#if WITH_WEBKIT2
    /* save ref to the lua class instance */
    lua_pushvalue(L, -1);
    download->ref = luaH_object_ref_class(L, -1, &download_class);
#endif

    /* return download */
    return 1;
}

/**
 * Pushes the given download onto the Lua stack.
 *
 * Obtains a GTK reference on the \c WebKitDownload.
 *
 * \param L The Lua VM state.
 * \param d The \c WebKitDownload to push onto the stack.
 *
 * \luastack
 * \lreturn A \c download object.
 */
gint
luaH_download_push(lua_State *L, WebKitDownload *d)
{
    download_class.allocator(L);
    download_t *download = luaH_checkdownload(L, -1);

    /* steal webkit download */
#if WITH_WEBKIT2
    download->is_started = FALSE;

    WebKitURIRequest *r = webkit_download_get_request(d);
    download->uri = g_strdup(webkit_uri_request_get_uri(r));
#else
    download->uri = g_strdup(webkit_download_get_uri(d));
#endif
    download->webkit_download = d;
    g_object_ref(G_OBJECT(download->webkit_download));

#if WITH_WEBKIT2
    /* raise corresponding luakit signals when the webkit signals are
     * emitted
     */
    g_signal_connect(G_OBJECT(download->webkit_download),
            "decide-destination",
            G_CALLBACK(decide_destination_cb), download);

    g_signal_connect(G_OBJECT(download->webkit_download),
            "created-destination",
            G_CALLBACK(created_destination_cb), download);

    g_signal_connect(G_OBJECT(download->webkit_download),
            "finished", G_CALLBACK(finished_cb), download);

    /* raise failed signal on failure or cancellation */
    g_signal_connect(G_OBJECT(download->webkit_download),
            "failed", G_CALLBACK(failed_cb), download);
#else
    /* raise error signal on error */
    g_signal_connect(G_OBJECT(download->webkit_download), "error",
            G_CALLBACK(error_cb), download);
#endif

#if WITH_WEBKIT2
    /* save ref to the lua class instance */
    lua_pushvalue(L, -1);
    download->ref = luaH_object_ref_class(L, -1, &download_class);

    // TODO confirm that it's okay to put this here
    /* line below assumes this function is only called in download_start_cb() */
    download->status = LUAKIT_DOWNLOAD_STATUS_STARTED;
#endif

    /* return download */
    return 1;
}

static gint
luaH_download_set_allow_overwrite(lua_State *L, download_t *download)
{
    gboolean allow = lua_toboolean(L, -1);
    webkit_download_set_allow_overwrite(download->webkit_download, allow);
    luaH_object_emit_signal(L, -3, "property::allow-overwrite", 0, 0);
    return 0;
}

static gint
luaH_download_get_allow_overwrite(lua_State *L, download_t *download)
{
    lua_pushboolean(L, webkit_download_get_allow_overwrite(download->webkit_download));
    return 1;
}

/**
 * Sets the destination of a download.
 *
 * Converts the given destination to a \c file:// URI.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lvalue A string containing the new destination for the download.
 */
static gint
luaH_download_set_destination(lua_State *L, download_t *download)
{
    if (download_is_started(download)) {
        luaH_warn(L, "cannot change destination while download %p is running", download);
        return 0;
    }

    const gchar *destination = luaL_checkstring(L, -1);
    gchar *uri = g_filename_to_uri(destination, NULL, NULL);
    if (uri) {
        download->destination = g_strdup(destination);
#if WITH_WEBKIT2
        webkit_download_set_destination(download->webkit_download, uri);
#else
        webkit_download_set_destination_uri(download->webkit_download, uri);
#endif
        g_free(uri);
        luaH_object_emit_signal(L, -3, "property::destination", 0, 0);

    /* g_filename_to_uri failed on destination path */
    } else {
        lua_pushfstring(L, "invalid destination: '%s'", destination);
        lua_error(L);
    }
    return 0;
}

/**
 * Returns the destination URI of the given download.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 */
LUA_OBJECT_EXPORT_PROPERTY(download, download_t, destination, lua_pushstring)

/**
 * Returns the current progress in percent of the given download.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The progress of the download as a number between 0.0 and 1.0
 */
static gint
luaH_download_get_progress(lua_State *L, download_t *download)
{
#if WITH_WEBKIT2
    gdouble progress = webkit_download_get_estimated_progress(download->webkit_download);
#else
    gdouble progress = webkit_download_get_progress(download->webkit_download);
    if (progress == 1)
        luaH_download_unref(L, download);
#endif
    lua_pushnumber(L, progress);
    return 1;
}

/**
 * Returns the value of the Content-Type webkit network reponse header.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The network request Content-Type.
 */
static gint
luaH_download_get_mime_type(lua_State *L, download_t *download)
{
#if WITH_WEBKIT2
    WebKitURIResponse *response = webkit_download_get_response(
            download->webkit_download);
#else
    WebKitNetworkResponse *response = webkit_download_get_network_response(
            download->webkit_download);
#endif
    if (!response)
        return 0;
#if WITH_WEBKIT2
    const gchar *mime_type = webkit_uri_response_get_mime_type(response);
#else
    SoupMessage *msg = webkit_network_response_get_message(response);
    SoupMessageHeaders *headers;
    g_object_get(G_OBJECT(msg), "response-headers", &headers, NULL);
    const gchar *mime_type = soup_message_headers_get_one(headers, "Content-Type");
#endif
    if (mime_type) {
        lua_pushstring(L, mime_type);
        return 1;
    }
    return 0;
}

/**
 * Returns the status of the given download.
 *
 * The status will be one of the following:
 * - \c finished
 * - \c created
 * - \c started
 * - \c cancelled
 * - \c failed
 *
 * Returns nothing if an error occurs.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The status of the download as a string or \c nil.
 */
static gint
luaH_download_get_status(lua_State *L, download_t *download)
{
#if WITH_WEBKIT2
    switch(download->status) {
      case LUAKIT_DOWNLOAD_STATUS_FINISHED:
        lua_pushstring(L, "finished");
        break;
      case LUAKIT_DOWNLOAD_STATUS_CREATED:
        lua_pushstring(L, "created");
        break;
      case LUAKIT_DOWNLOAD_STATUS_STARTED:
        lua_pushstring(L, "started");
        break;
      case LUAKIT_DOWNLOAD_STATUS_CANCELLED:
        lua_pushstring(L, "cancelled");
        break;
      case LUAKIT_DOWNLOAD_STATUS_FAILED:
        lua_pushstring(L, "failed");
        break;
      default:
        luaH_warn(L, "unknown download status");
        return 0;
    }
#else
    WebKitDownloadStatus status = webkit_download_get_status(
            download->webkit_download);

    switch (status) {
      case WEBKIT_DOWNLOAD_STATUS_FINISHED:
        luaH_download_unref(L, download);
        lua_pushstring(L, "finished");
        break;
      case WEBKIT_DOWNLOAD_STATUS_CREATED:
        lua_pushstring(L, "created");
        break;
      case WEBKIT_DOWNLOAD_STATUS_STARTED:
        lua_pushstring(L, "started");
        break;
      case WEBKIT_DOWNLOAD_STATUS_CANCELLED:
        luaH_download_unref(L, download);
        lua_pushstring(L, "cancelled");
        break;
      case WEBKIT_DOWNLOAD_STATUS_ERROR:
        luaH_download_unref(L, download);
        lua_pushstring(L, "error");
        break;
      default:
        luaH_warn(L, "unknown download status");
        return 0;
    }
#endif

    return 1;
}

/* \fn static gint luaH_download_get_error(lua_State *L)
 * Returns the message of the last error that occurred for the given download.
 *
 * If no error occurred so far, returns \c nil.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The message of the last download error as a string or \c nil.
 */
LUA_OBJECT_EXPORT_PROPERTY(download, download_t, error, lua_pushstring)

/**
 * Returns the expected total size of the download.
 *
 * May vary during downloading as not all servers send this correctly.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The total size of the download in bytes as a number.
 */
static gint
#if WITH_WEBKIT2
luaH_download_get_content_length(lua_State *L, download_t *download)
#else
luaH_download_get_total_size(lua_State *L, download_t *download)
#endif
{
#if WITH_WEBKIT2
    gdouble total_size = webkit_uri_response_get_content_length(
            webkit_download_get_response(download->webkit_download));
#else
    gdouble total_size = webkit_download_get_total_size(
            download->webkit_download);
#endif
    lua_pushnumber(L, total_size);
    return 1;
}

/**
 * Returns the current size of the download, i.e. the bytes already downloaded.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The current size of the download in bytes as a number.
 */
static gint
#if WITH_WEBKIT2
luaH_download_get_received_data_length(lua_State *L, download_t *download)
#else
luaH_download_get_current_size(lua_State *L, download_t *download)
#endif
{
#if WITH_WEBKIT2
    gdouble current_size = webkit_download_get_received_data_length(
            download->webkit_download);
#else
    gdouble current_size = webkit_download_get_current_size(
            download->webkit_download);
#endif
    lua_pushnumber(L, current_size);
    return 1;
}

/**
 * Returns the elapsed time since starting the download.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The elapsed time since starting the download in seconds as a number.
 */
static gint
luaH_download_get_elapsed_time(lua_State *L, download_t *download)
{
    gdouble elapsed_time = webkit_download_get_elapsed_time(
            download->webkit_download);
    lua_pushnumber(L, elapsed_time);
    return 1;
}

/**
 * Returns the suggested filename for the download.
 * This is provided by \c WebKit and inferred from the URI and response headers.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The filename that WebKit suggests for the download as a string.
 */
static gint
luaH_download_get_suggested_filename(lua_State *L, download_t *download)
{
#if WITH_WEBKIT2
    /* webkit_download_get_response() returns NULL if the response hasn't been
     * received yet. This function should only be called after the
     * decide-destination signal is raised. */
    const gchar *suggested_filename = webkit_uri_response_get_suggested_filename(
            webkit_download_get_response(download->webkit_download));
#else
    const gchar *suggested_filename = webkit_download_get_suggested_filename(
            download->webkit_download);
#endif
    lua_pushstring(L, suggested_filename);
    return 1;
}

/**
 * Sets the URI of the download.
 * This does not have any effect if the download is already running.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lvalue The new URI to download.
 */
static gint
luaH_download_set_uri(lua_State *L, download_t *download)
{
    gchar *uri = (gchar*) luaL_checkstring(L, -1);
    /* use http protocol if none specified */
    if (g_strrstr(uri, "://"))
        uri = g_strdup(uri);
    else
        uri = g_strdup_printf("http://%s", uri);
    download->uri = uri;
    return 0;
}

/**
 * Returns the URI that is being downloaded.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \luastack
 * \lparam A \c download object.
 * \lreturn The URI of this download as a string.
 */
LUA_OBJECT_EXPORT_PROPERTY(download, download_t, uri, lua_pushstring)

#if !WITH_WEBKIT2
/**
 * Checks prerequisites for downloading.
 * - clears the last error message of the download.
 * - checks that a destination has been set.
 *
 * \param L The Lua VM state.
 * \param download The \ref download_t of the download.
 *
 * \returns \c TRUE if the download is ready to begin.
 */
static gboolean
download_check_prerequisites(lua_State* L, download_t *download)
{
    /* clear last download error message */
    if (download->error) {
        g_free(download->error);
        download->error = NULL;
    }

    /* get download destination */
    const gchar *destination = webkit_download_get_destination_uri(
        download->webkit_download);

    if (!destination) {
        download->error = g_strdup("Download destination not set");
        luaH_warn(L, "%s", download->error);
        return FALSE;
    }

    /* ready to go */
    return TRUE;
}
#endif

/**
 * Starts the download.
 *
 * Will produce a warning if the download is already running.
 * References the download to prevent its garbage collection.
 * Will raise a Lua error if the start failed.
 *
 * \param L The Lua VM state.
 *
 * \luastack
 * \lparam A \c download object to start.
 */
static gint
#if WITH_WEBKIT2
luaH_download_start(lua_State* UNUSED(L))
{
    /* all this stuff moved to download_start_cb() in
     * widgets/webview/downloads.c */
#else
luaH_download_start(lua_State *L)
{
    download_t *download = luaH_checkdownload(L, 1);
    if (!download_is_started(download)) {
        /* prevent lua garbage collection while downloading */
        lua_pushvalue(L, 1);
        download->ref = luaH_object_ref(L, -1);

        /* check if we can download to destination */
        if (download_check_prerequisites(L, download))
            webkit_download_start(download->webkit_download);

        /* check for webkit/glib errors from download start */
        if (download->error) {
            lua_pushstring(L, download->error);
            lua_error(L);
        }

    } else
        luaH_warn(L, "download already started");
#endif
    return 0;
}

/**
 * Aborts the download.
 *
 * Will produce a warning if the download is not running.
 * Unreferences the download to allow its garbage collection.
 *
 * \param L The Lua VM state.
 *
 * \luastack
 * \lparam A \c download object to abort.
 */
static gint
luaH_download_cancel(lua_State *L)
{
    download_t *download = luaH_checkdownload(L, 1);
#if WITH_WEBKIT2
    if (download_is_started(download))
        webkit_download_cancel(download->webkit_download);
    else
        luaH_warn(L, "trying to cancel download, but download not started");

    download->status = LUAKIT_DOWNLOAD_STATUS_CANCELLED;
#else
    if (download_is_started(download)) {
        webkit_download_cancel(download->webkit_download);
        luaH_download_unref(L, download);
    } else
        luaH_warn(L, "download not started");
#endif
    return 0;
}

/**
 * Creates the Lua download class.
 *
 * \param L The Lua VM state.
 */
void
download_class_setup(lua_State *L)
{
    static const struct luaL_reg download_methods[] =
    {
        LUA_CLASS_METHODS(download)
        { "__call", luaH_download_new },
        { NULL, NULL }
    };

    static const struct luaL_reg download_meta[] =
    {
        LUA_OBJECT_META(download)
        LUA_CLASS_META
        { "start", luaH_download_start },
        { "cancel", luaH_download_cancel },
        { "__gc", luaH_download_gc },
        { NULL, NULL },
    };

    luaH_class_setup(L, &download_class, "download",
             (lua_class_allocator_t) download_new,
             NULL, NULL,
             download_methods, download_meta);

    luaH_class_add_property(&download_class, L_TK_ALLOW_OVERWRITE,
            (lua_class_propfunc_t) luaH_download_set_allow_overwrite,
            (lua_class_propfunc_t) luaH_download_get_allow_overwrite,
            (lua_class_propfunc_t) luaH_download_set_allow_overwrite);

    luaH_class_add_property(&download_class, L_TK_DESTINATION,
            (lua_class_propfunc_t) luaH_download_set_destination,
            (lua_class_propfunc_t) luaH_download_get_destination,
            (lua_class_propfunc_t) luaH_download_set_destination);

    luaH_class_add_property(&download_class, L_TK_PROGRESS,
            NULL,
            (lua_class_propfunc_t) luaH_download_get_progress,
            NULL);

    luaH_class_add_property(&download_class, L_TK_STATUS,
            NULL,
            (lua_class_propfunc_t) luaH_download_get_status,
            NULL);

    luaH_class_add_property(&download_class, L_TK_ERROR,
            NULL,
            (lua_class_propfunc_t) luaH_download_get_error,
            NULL);

// TODO rename token, possibly to L_TK_CONTENT_LENGTH?
    luaH_class_add_property(&download_class, L_TK_TOTAL_SIZE,
            NULL,
#if WITH_WEBKIT2
            (lua_class_propfunc_t) luaH_download_get_content_length,
#else
            (lua_class_propfunc_t) luaH_download_get_total_size,
#endif
            NULL);

    luaH_class_add_property(&download_class, L_TK_CURRENT_SIZE,
            NULL,
#if WITH_WEBKIT2
            (lua_class_propfunc_t) luaH_download_get_received_data_length,
#else
            (lua_class_propfunc_t) luaH_download_get_current_size,
#endif
            NULL);

    luaH_class_add_property(&download_class, L_TK_ELAPSED_TIME,
            NULL,
            (lua_class_propfunc_t) luaH_download_get_elapsed_time,
            NULL);

    luaH_class_add_property(&download_class, L_TK_MIME_TYPE,
            NULL,
            (lua_class_propfunc_t) luaH_download_get_mime_type,
            NULL);

    luaH_class_add_property(&download_class, L_TK_SUGGESTED_FILENAME,
            NULL,
            (lua_class_propfunc_t) luaH_download_get_suggested_filename,
            NULL);

    luaH_class_add_property(&download_class, L_TK_URI,
            (lua_class_propfunc_t) luaH_download_set_uri,
            (lua_class_propfunc_t) luaH_download_get_uri,
            (lua_class_propfunc_t) NULL);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
