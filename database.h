#ifndef DATABASE_H
#define DATABASE_H

#include <glib.h>
#include <mysql.h>

void mysql_handler_connect(const char *hostname, const char *username, const char *password, const char *dbname);
void mysql_handler_close();
MYSQL_RES *mysql_handler_get(const char *query);
gboolean mysql_handler_insert(const char *query);
gboolean mysql_is_connected();

#endif

