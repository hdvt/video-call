
#include <string.h>

#include "database.h"
#include "debug.h"
#include "mutex.h"
#include "utils.h"

static MYSQL *conn;
static gboolean is_connected = FALSE;
static janus_mutex mutex;

void mysql_handler_connect(const char *hostname, const char *username, const char *password, const char *dbname)
{
    conn = mysql_init(NULL);
    if (!conn)
    {
        JANUS_LOG(LOG_ERR, "%s\n", mysql_error(conn));
        return;
    }
    if (!mysql_real_connect(conn, hostname, username, password, dbname, 0, NULL, 0))
    {
        JANUS_LOG(LOG_ERR, "%s\n", mysql_error(conn));
        mysql_close(conn);
        return;
    }
    is_connected = TRUE;
    janus_mutex_init(&mutex);
}
gboolean mysql_handler_insert(const char *query)
{   
    janus_mutex_lock(&mutex);
    if (!mysql_is_connected())
    {
        JANUS_LOG(LOG_ERR, "MySQL has not connected yet!\n");
        janus_mutex_unlock(&mutex);
        return FALSE;
    }

    if (mysql_query(conn, query))
    {
        JANUS_LOG(LOG_ERR, "%s\n", mysql_error(conn));
        janus_mutex_unlock(&mutex);
        return FALSE;
    }
    janus_mutex_unlock(&mutex);
    return TRUE;
}

MYSQL_RES *mysql_handler_get(const char *query)
{
    janus_mutex_lock(&mutex);
    if (!mysql_is_connected())
    {
        JANUS_LOG(LOG_ERR, "MySQL has not connected yet!\n");
        janus_mutex_unlock(&mutex);
        return NULL;
    }
    if (mysql_query(conn, query))
    {
        JANUS_LOG(LOG_ERR, "%s\n", mysql_error(conn));
        janus_mutex_unlock(&mutex);
        return NULL;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    janus_mutex_unlock(&mutex);

    if (res == NULL)
    {
        JANUS_LOG(LOG_ERR, "%s\n", mysql_error(conn));
        return NULL;
    }
    return res;
}

void mysql_handler_close()
{
    janus_mutex_lock(&mutex);
    mysql_close(conn);
    janus_mutex_unlock(&mutex);
}

gboolean mysql_is_connected()
{
    return is_connected;
}