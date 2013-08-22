/*
 *  Copyright (c) 2013 Sergey 'Jin' Bostandzhyan <jin at mediatomb dot cc>
 *
 *  tox-prlp - libpurple protocol plugin or Tox (see http://tox.im)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This plugin is based on the Nullprpl mockup from Pidgin / Finch / libpurple
 *  which is disributed under GPL v2 or later.  See http://pidgin.im/
 */

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <Messenger.h>
#include <network.h>

#define PURPLE_PLUGINS

#ifdef HAVE_CONFIG_H
#include "autoconfig.h"
#endif

#include <account.h>
#include <accountopt.h>
#include <blist.h>
#include <cmds.h>
#include <conversation.h>
#include <connection.h>
#include <debug.h>
#include <notify.h>
#include <privacy.h>
#include <prpl.h>
#include <roomlist.h>
#include <request.h>
#include <status.h>
#include <util.h>
#include <version.h>
#include <arpa/inet.h>

#define _(msg) msg // might add gettext later

#define TOXPRPL_ID "prpl-jin_eld-tox"
#define DEFAULT_SERVER_KEY "5CD7EB176C19A2FD840406CD56177BB8E75587BB366F7BB3004B19E3EDC04143"
#define DEFAULT_SERVER_PORT 33445
#define DEFAULT_SERVER_IP   "192.184.81.118"

// todo: allow user to specify a contact request message
#define DEFAULT_REQUEST_MESSAGE _("Please allow me to add you as a friend!")

#define MAX_ACCOUNT_DATA_SIZE   1*1024*1024
static const char *g_HEX_CHARS = "0123456789abcdef";

typedef struct
{
    PurpleStatusPrimitive primitive;
    uint8_t tox_status;
    gchar *id;
    gchar *title;
} toxprpl_status;

typedef struct
{
    int tox_friendlist_number;
} toxprpl_buddy_data;

typedef struct
{
    PurpleConnection *gc;
    char *buddy_key;
} toxprpl_accept_friend_data;

typedef struct
{
    Messenger *m;
    guint messenger_timer;
    guint connection_timer;
    guint connected;
} toxprpl_plugin_data;

#define TOXPRPL_MAX_STATUS          4
#define TOXPRPL_STATUS_ONLINE       0
#define TOXPRPL_STATUS_AWAY         1
#define TOXPRPL_STATUS_BUSY         2
#define TOXPRPL_STATUS_OFFLINE      3

static toxprpl_status toxprpl_statuses[] =
{
    {
        PURPLE_STATUS_AVAILABLE, TOXPRPL_STATUS_ONLINE,
        "tox_online", _("Online")
    },
    {
        PURPLE_STATUS_AWAY, TOXPRPL_STATUS_ONLINE,
        "tox_away", _("Away")
    },
    {
        PURPLE_STATUS_UNAVAILABLE, TOXPRPL_STATUS_BUSY,
        "tox_busy", _("Busy")
    },
    {
        PURPLE_STATUS_OFFLINE, TOXPRPL_STATUS_OFFLINE,
        "tox_offline", _("Offline")
    }
};

/*
 * stores offline messages that haven't been delivered yet. maps username
 * (char *) to GList * of GOfflineMessages. initialized in toxprpl_init.
 */
GHashTable* goffline_messages = NULL;

typedef struct
{
    char *from;
    char *message;
    time_t mtime;
    PurpleMessageFlags flags;
} GOfflineMessage;

static void toxprpl_add_to_buddylist(toxprpl_accept_friend_data *data);
static void toxprpl_do_not_add_to_buddylist(toxprpl_accept_friend_data *data);

static void toxprpl_login(PurpleAccount *acct);
static void toxprpl_query_buddy_info(gpointer data, gpointer user_data);

// utilitis

// returned buffer must be freed by the caller
static char *toxprpl_data_to_hex_string(const unsigned char *data,
                                        const size_t len)
{
    unsigned char *chars;
    unsigned char hi, lo;
    size_t i;
    char *buf = malloc((len * 2) + 1);
    char *p = buf;
    chars = (unsigned char *)data;
    chars = (unsigned char *)data;
    for (i = 0; i < len; i++)
    {
        unsigned char c = chars[i];
        hi = c >> 4;
        lo = c & 0xF;
        *p = g_HEX_CHARS[hi];
        p++;
        *p = g_HEX_CHARS[lo];
        p++;
    }
    buf[len*2] = '\0';
    return buf;
}

unsigned char *toxprpl_hex_string_to_data(const char *s)
{
    size_t len = strlen(s);
    unsigned char *buf = malloc(len / 2);
    unsigned char *p = buf;

    size_t i;
    for (i = 0; i < len; i += 2)
    {
        const char *chi = strchr(g_HEX_CHARS, g_ascii_tolower(s[i]));
        const char *clo = strchr(g_HEX_CHARS, g_ascii_tolower(s[i + 1]));
        int hi, lo;
        if (chi)
        {
            hi = chi - g_HEX_CHARS;
        }
        else
        {
            hi = 0;
        }

        if (clo)
        {
            lo = clo - g_HEX_CHARS;
        }
        else
        {
            lo = 0;
        }

        unsigned char ch = (unsigned char)(hi << 4 | lo);
        *p = ch;
        p++;
    }
    return buf;
}

// stay independent from the lib
static int toxprpl_get_status_index(Messenger *m, int fnum, USERSTATUS status)
{
    switch (status)
    {
        case USERSTATUS_AWAY:
            return TOXPRPL_STATUS_AWAY;
        case USERSTATUS_BUSY:
            return TOXPRPL_STATUS_BUSY;
        case USERSTATUS_NONE:
        case USERSTATUS_INVALID:
        default:
            if (fnum != -1)
            {
                if (m_friendstatus(m, fnum) == FRIEND_ONLINE)
                {
                    return TOXPRPL_STATUS_ONLINE;
                }
            }
    }
    return TOXPRPL_STATUS_OFFLINE;
}

static USERSTATUS toxprpl_get_tox_status_from_id(const char *status_id)
{
    int i;
    for (i = 0; i < TOXPRPL_MAX_STATUS; i++)
    {
        if (strcmp(toxprpl_statuses[i].id, status_id) == 0)
        {
            return toxprpl_statuses[i].tox_status;
        }
    }
    return USERSTATUS_INVALID;
}

/* tox helpers */
static gchar *toxprpl_tox_bin_id_to_string(uint8_t *bin_id)
{
    return toxprpl_data_to_hex_string(bin_id, CLIENT_ID_SIZE);
}

static gchar *toxprpl_tox_friend_id_to_string(uint8_t *bin_id)
{
    return toxprpl_data_to_hex_string(bin_id, FRIEND_ADDRESS_SIZE);
}

/* tox specific stuff */
static void on_connectionstatus(Messenger *m, int fnum, uint8_t status,
                                void *user_data)
{
    PurpleConnection *gc = (PurpleConnection *)user_data;
    int tox_status = TOXPRPL_STATUS_OFFLINE;
    if (status == 1)
    {
        tox_status = TOXPRPL_STATUS_ONLINE;
    }

    purple_debug_info("toxprpl", "Friend status change: %d\n", status);
    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, fnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend #%d\n",
                          fnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    PurpleAccount *account = purple_connection_get_account(gc);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[tox_status].id, NULL);
    g_free(buddy_key);
}

static void on_request(uint8_t* public_key, uint8_t* data,
                       uint16_t length, void *user_data)
{
    purple_debug_info("toxprpl", "incoming friend request!\n");
    gchar *dialog_message;
    PurpleConnection *gc = (PurpleConnection *)user_data;

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(public_key);
    purple_debug_info("toxprpl", "Buddy request from %s: %s\n",
                      buddy_key, data);

    PurpleAccount *account = purple_connection_get_account(gc);
    PurpleBuddy *buddy = purple_find_buddy(account, buddy_key);
    if (buddy != NULL)
    {
        purple_debug_info("toxprpl", "Buddy %s already in buddy list!\n",
                          buddy_key);
        g_free(buddy_key);
        return;
    }

    dialog_message = g_strdup_printf("The user %s has sent you a friend "
                                    "request, do you want to add him?",
                                    buddy_key);

    gchar *request_msg = NULL;
    if (length > 0)
    {
        request_msg = g_strndup((const gchar *)data, length);
    }

    toxprpl_accept_friend_data *fdata = g_new0(toxprpl_accept_friend_data, 1);
    fdata->gc = gc;
    fdata->buddy_key = buddy_key;
    purple_request_yes_no(gc, "New friend request", dialog_message,
                          request_msg,
                          PURPLE_DEFAULT_ACTION_NONE,
                          account, NULL,
                          NULL,
                          fdata, // buddy key will be freed elsewhere
                          G_CALLBACK(toxprpl_add_to_buddylist),
                          G_CALLBACK(toxprpl_do_not_add_to_buddylist));
    g_free(dialog_message);
    g_free(request_msg);
}

static void on_friend_action(Messenger *m, int friendnum, uint8_t* string,
                             uint16_t length, void *user_data)
{
    purple_debug_info("toxprpl", "action received\n");
    PurpleConnection *gc = (PurpleConnection *)user_data;

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    gchar *message = g_strdup_printf("/me %s", string);

    serv_got_im(gc, buddy_key, message, PURPLE_MESSAGE_RECV,
                time(NULL));
    g_free(buddy_key);
    g_free(message);
}

static void on_incoming_message(Messenger *m, int friendnum, uint8_t* string,
                                uint16_t length, void *user_data)
{
    purple_debug_info("toxprpl", "Message received!\n");
    PurpleConnection *gc = (PurpleConnection *)user_data;

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    serv_got_im(gc, buddy_key, (const char *)string, PURPLE_MESSAGE_RECV,
                time(NULL));
    g_free(buddy_key);
}

static void on_nick_change(Messenger *m, int friendnum, uint8_t* data,
                           uint16_t length, void *user_data)
{
    purple_debug_info("toxprpl", "Nick change!\n");

    PurpleConnection *gc = (PurpleConnection *)user_data;

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    PurpleAccount *account = purple_connection_get_account(gc);
    PurpleBuddy *buddy = purple_find_buddy(account, buddy_key);
    if (buddy == NULL)
    {
        purple_debug_info("toxprpl", "Ignoring nick change because buddy %s was not found\n", buddy_key);
        g_free(buddy_key);
        return;
    }

    g_free(buddy_key);
    purple_blist_alias_buddy(buddy, (const char *)data);
}

static void on_status_change(Messenger *m, int friendnum, USERSTATUS userstatus,
                             void *user_data)
{
    purple_debug_info("toxprpl", "Status change: %d\n", userstatus);
    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);

    PurpleConnection *gc = (PurpleConnection *)user_data;
    PurpleAccount *account = purple_connection_get_account(gc);
    purple_debug_info("toxprpl", "Setting user status for user %s to %s\n",
        buddy_key, toxprpl_statuses[
                        toxprpl_get_status_index(m, friendnum, userstatus)].id);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[toxprpl_get_status_index(m, friendnum, userstatus)].id,
        NULL);
    g_free(buddy_key);
}

static gboolean tox_messenger_loop(gpointer data)
{
    PurpleConnection *gc = (PurpleConnection *)data;
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    if ((plugin != NULL) && (plugin->m != NULL))
    {
        doMessenger(plugin->m);
    }
    return TRUE;
}

static gboolean tox_connection_check(gpointer gc)
{
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    if ((plugin->connected == 0) && DHT_isconnected(plugin->m->dht))
    {
        plugin->connected = 1;
        purple_connection_update_progress(gc, _("Connected"),
                1,   /* which connection step this is */
                2);  /* total number of steps */
        purple_connection_set_state(gc, PURPLE_CONNECTED);
        purple_debug_info("toxprpl", "DHT connected!\n");

        // query status of all buddies
        PurpleAccount *account = purple_connection_get_account(gc);
        GSList *buddy_list = purple_find_buddies(account, NULL);
        g_slist_foreach(buddy_list, toxprpl_query_buddy_info, gc);
        g_slist_free(buddy_list);

        uint8_t our_name[MAX_NAME_LENGTH];
        uint16_t name_len = getself_name(plugin->m, our_name, MAX_NAME_LENGTH);
        // bug in the library?
        if (name_len == 0)
        {
            our_name[0] = '\0';
        }

        const char *nick = purple_account_get_string(account, "nickname", NULL);
        if (nick == NULL)
        {
            if (strlen((const char *)our_name) > 0)
            {
                purple_connection_set_display_name(gc, (const char *)our_name);
                purple_account_set_string(account, "nickname",
                                                      (const char *)our_name);
            }
        }
        else
        {
            purple_connection_set_display_name(gc, nick);
            if (strcmp(nick, (const char *)our_name) != 0)
            {
                setname(plugin->m, (uint8_t *)nick, strlen(nick) + 1);
            }
        }
    }
    else if ((plugin->connected == 1) && !DHT_isconnected(plugin->m->dht))
    {
        plugin->connected = 0;
        purple_debug_info("toxprpl", "DHT not connected!\n");
        purple_connection_update_progress(gc, _("Connecting"),
                0,   /* which connection step this is */
                2);  /* total number of steps */
    }
    return TRUE;
}

static void toxprpl_set_status(PurpleAccount *account, PurpleStatus *status)
{
    const char* status_id = purple_status_get_id(status);
    const char *message = purple_status_get_attr_string(status, "message");

    PurpleConnection *gc = purple_account_get_connection(account);
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);

    purple_debug_info("toxprpl", "setting status %s\n", status_id);

    USERSTATUS tox_status = toxprpl_get_tox_status_from_id(status_id);
    if (tox_status == USERSTATUS_INVALID)
    {
        purple_debug_info("toxprpl", "status %s is invalid\n", status_id);
        return;
    }

    m_set_userstatus(plugin->m, tox_status);
    if ((message != NULL) && (strlen(message) > 0))
    {
        m_set_statusmessage(plugin->m, (uint8_t *)message, strlen(message) + 1);
    }
    // FOKEL
}
// query buddy status
static void toxprpl_query_buddy_info(gpointer data, gpointer user_data)
{
    purple_debug_info("toxprpl", "toxprpl_query_buddy_info\n");
    PurpleBuddy *buddy = (PurpleBuddy *)data;
    PurpleConnection *gc = (PurpleConnection *)user_data;
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);

    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data == NULL)
    {
        unsigned char *bin_key = toxprpl_hex_string_to_data(buddy->name);
        int fnum = getfriend_id(plugin->m, bin_key);
        buddy_data = g_new0(toxprpl_buddy_data, 1);
        buddy_data->tox_friendlist_number = fnum;
        purple_buddy_set_protocol_data(buddy, buddy_data);
        g_free(bin_key);
    }

    PurpleAccount *account = purple_connection_get_account(gc);
    purple_debug_info("toxprpl", "Setting user status for user %s to %s\n",
        buddy->name, toxprpl_statuses[toxprpl_get_status_index(plugin->m,
            buddy_data->tox_friendlist_number,
            m_get_userstatus(plugin->m,buddy_data->tox_friendlist_number))].id);
    purple_prpl_got_user_status(account, buddy->name,
        toxprpl_statuses[toxprpl_get_status_index(plugin->m,
            buddy_data->tox_friendlist_number,
            m_get_userstatus(plugin->m, buddy_data->tox_friendlist_number))].id,
        NULL);

    uint8_t alias[MAX_NAME_LENGTH];
    if (getname(plugin->m, buddy_data->tox_friendlist_number, alias) == 0)
    {
        purple_blist_alias_buddy(buddy, (const char*)alias);
    }
}

static const char *toxprpl_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
    return "tox";
}

static GList *toxprpl_status_types(PurpleAccount *acct)
{
    GList *types = NULL;
    PurpleStatusType *type;
    int i;

    purple_debug_info("toxprpl", "setting up status types\n");

    for (i = 0; i < TOXPRPL_MAX_STATUS; i++)
    {
        type = purple_status_type_new_with_attrs(toxprpl_statuses[i].primitive,
            toxprpl_statuses[i].id, toxprpl_statuses[i].title, TRUE, TRUE,
            FALSE,
            "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
            NULL);
        types = g_list_append(types, type);
    }

    return types;
}

static void toxprpl_login_after_setup(PurpleAccount *acct)
{
    IP_Port dht;

    purple_debug_info("toxprpl", "logging in...\n");

    PurpleConnection *gc = purple_account_get_connection(acct);

    Messenger *m = initMessenger();
    if (m == NULL)
    {
        purple_debug_info("toxprpl", "Fatal error, could not allocate memory "
                                     "for messenger!\n");
        return;

    }


    m_callback_friendmessage(m, on_incoming_message, gc);
    m_callback_namechange(m, on_nick_change, gc);
    m_callback_userstatus(m, on_status_change, gc);
    m_callback_friendrequest(m, on_request, gc);
    m_callback_connectionstatus(m, on_connectionstatus, gc);
    m_callback_action(m, on_friend_action, gc);

    purple_debug_info("toxprpl", "initialized tox callbacks\n");

    gc->flags |= PURPLE_CONNECTION_NO_FONTSIZE | PURPLE_CONNECTION_NO_URLDESC;
    gc->flags |= PURPLE_CONNECTION_NO_IMAGES | PURPLE_CONNECTION_NO_NEWLINES;

    purple_debug_info("toxprpl", "logging in %s\n", acct->username);

    const char *msg64 = purple_account_get_string(acct, "messenger", NULL);
    if ((msg64 != NULL) && (strlen(msg64) > 0))
    {
        purple_debug_info("toxprpl", "found existing account data\n");
        gsize out_len;
        guchar *msg_data = g_base64_decode(msg64, &out_len);
        if (msg_data && (out_len > 0))
        {
            if (Messenger_load(m, (uint8_t *)msg_data, (uint32_t)out_len) != 0)
            {
                purple_debug_info("toxprpl", "Invalid account data\n");
                purple_account_set_string(acct, "messenger", NULL);
            }
            g_free(msg_data);
        }
    }

    purple_connection_update_progress(gc, _("Connecting"),
            0,   /* which connection step this is */
            2);  /* total number of steps */

    const char* ip = purple_account_get_string(acct, "dht_server",
                                               DEFAULT_SERVER_IP);
    dht.port = htons(
            purple_account_get_int(acct, "dht_server_port",
                                   DEFAULT_SERVER_PORT));
    const char *key = purple_account_get_string(acct, "dht_server_key",
                                          DEFAULT_SERVER_KEY);
    uint32_t resolved = resolve_addr(ip);
    dht.ip.i = resolved;
    unsigned char *bin_str = toxprpl_hex_string_to_data(key);
    DHT_bootstrap(m->dht, dht, bin_str);
    g_free(bin_str);
    purple_debug_info("toxprpl", "Will connect to %s:%d (%s)\n" ,
                      ip, ntohs(dht.port), key);


    toxprpl_plugin_data *plugin = g_new0(toxprpl_plugin_data, 1);

    plugin->m = m;
    plugin->messenger_timer = purple_timeout_add(100, tox_messenger_loop, gc);
    purple_debug_info("toxprpl", "added messenger timer as %d\n",
                      plugin->messenger_timer);
    plugin->connection_timer = purple_timeout_add_seconds(2,
                                                        tox_connection_check,
                                                        gc);
    purple_debug_info("toxprpl", "added connection timer as %d\n",
                      plugin->connection_timer);


    purple_connection_set_protocol_data(gc, plugin);
}

static void toxprpl_user_import(PurpleAccount *acct, const char *filename)
{
    purple_debug_info("toxprpl", "import user account: %s\n", filename);

    PurpleConnection *gc = purple_account_get_connection(acct);

    GStatBuf sb;
    if (g_stat(filename, &sb) != 0)
    {
        purple_notify_message(gc,
                PURPLE_NOTIFY_MSG_ERROR,
                _("Error"),
                _("Could not access account data file:"),
                filename,
                (PurpleNotifyCloseCallback)toxprpl_login,
                acct);
        return;
    }

    if ((sb.st_size == 0) || (sb.st_size > MAX_ACCOUNT_DATA_SIZE))
    {
        purple_notify_message(gc,
                PURPLE_NOTIFY_MSG_ERROR,
                _("Error"),
                _("Account data file seems to be invalid"),
                NULL,
                (PurpleNotifyCloseCallback)toxprpl_login,
                acct);
        return;
    }

    int fd = open(filename, O_RDONLY);
    if (fd == -1)
    {
        purple_notify_message(gc,
                PURPLE_NOTIFY_MSG_ERROR,
                _("Error"),
                _("Could not open account data file:"),
                strerror(errno),
                (PurpleNotifyCloseCallback)toxprpl_login,
                acct);
        return;
    }

    guchar *account_data = g_malloc0(sb.st_size);

    size_t rb = read(fd, account_data, sb.st_size);
    if (rb != sb.st_size)
    {
        const char *msg2 = _("short read");
        if (rb < 0)
        {
            msg2 = strerror(errno);
        }

        purple_notify_message(gc,
                PURPLE_NOTIFY_MSG_ERROR,
                _("Error"),
                _("Could not read account data file:"),
                msg2,
                (PurpleNotifyCloseCallback)toxprpl_login,
                acct);
        g_free(account_data);
        close(fd);
        return;
    }

    gchar *msg64 = g_base64_encode(account_data, sb.st_size);
    purple_account_set_string(acct, "messenger", msg64);
    g_free(msg64);
    g_free(account_data);
    toxprpl_login(acct);
    close(fd);
}

static void toxprpl_user_ask_import(PurpleAccount *acct)
{
    purple_debug_info("toxprpl", "ask to import user account\n");
    PurpleConnection *gc = purple_account_get_connection(acct);

    purple_request_file(gc,
        _("Import existing Tox account data"),
        NULL,
        FALSE,
        G_CALLBACK(toxprpl_user_import),
        G_CALLBACK(toxprpl_login),
        acct,
        NULL,
        NULL,
        acct);
}

static void toxprpl_login(PurpleAccount *acct)
{
    PurpleConnection *gc = purple_account_get_connection(acct);

    // check if we need to run first time setup
    if (purple_account_get_string(acct, "messenger", NULL) == NULL)
    {
        purple_request_action(gc,
            _("Setup Tox account"),
            _("This appears to be your first login to the Tox network, "
              "would you like to start with a new user ID or would you "
              "like to import an existing one?"),
            _("Note: you can export / backup your account via the account "
              "actions menu."),
            PURPLE_DEFAULT_ACTION_NONE,
            acct, NULL, NULL,
            acct, // user data
            2,    // 2 choices
            _("Import existing Tox account"),
            G_CALLBACK(toxprpl_user_ask_import),
            _("Create new Tox account"),
            G_CALLBACK(toxprpl_login_after_setup));

    }
    else
    {
        toxprpl_login_after_setup(acct);
    }
}


static void toxprpl_close(PurpleConnection *gc)
{
    /* notify other toxprpl accounts */
    purple_debug_info("toxprpl", "Closing!\n");

    PurpleAccount *account = purple_connection_get_account(gc);
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    if (plugin == NULL)
    {
        return;
    }

    if (plugin->m == NULL)
    {
        g_free(plugin);
        purple_connection_set_protocol_data(gc, NULL);
        return;
    }

    purple_debug_info("toxprpl", "removing timers %d and %d\n",
            plugin->messenger_timer, plugin->connection_timer);
    purple_timeout_remove(plugin->messenger_timer);
    purple_timeout_remove(plugin->connection_timer);

    uint32_t msg_size = Messenger_size(plugin->m);
    if (msg_size > 0)
    {
        guchar *msg_data = g_malloc0(msg_size);
        Messenger_save(plugin->m, (uint8_t *)msg_data);
        gchar *msg64 = g_base64_encode(msg_data, msg_size);
        purple_account_set_string(account, "messenger", msg64);
        g_free(msg64);
        g_free(msg_data);
    }
    else
    {
        purple_account_set_string(account, "messenger", "");
    }

    purple_debug_info("toxprpl", "shutting down\n");
    purple_connection_set_protocol_data(gc, NULL);
    cleanupMessenger(plugin->m);
    g_free(plugin);
}

static int toxprpl_send_im(PurpleConnection *gc, const char *who,
        const char *message, PurpleMessageFlags flags)
{
    const char *from_username = gc->account->username;

    purple_debug_info("toxprpl", "sending message from %s to %s\n",
            from_username, who);

    PurpleAccount *account = purple_connection_get_account(gc);
    PurpleBuddy *buddy = purple_find_buddy(account, who);
    if (buddy == NULL)
    {
        purple_debug_info("toxprpl", "Can't send message because buddy %s was not found\n", who);
        return 0;
    }
    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data == NULL)
    {
         purple_debug_info("toxprpl", "Can't send message because tox friend number is unknown\n");
        return 0;
    }
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    char *no_html = purple_markup_strip_html(message);

    if (purple_message_meify(no_html, -1))
    {
        m_sendaction(plugin->m, buddy_data->tox_friendlist_number,
                                    (uint8_t *)no_html, strlen(message)+1);
    }
    else
    {
        m_sendmessage(plugin->m, buddy_data->tox_friendlist_number,
                                   (uint8_t *)no_html, strlen(message)+1);
    }
    if (no_html)
    {
        free(no_html);
    }
    return 1;
}

static int toxprpl_tox_addfriend(Messenger *m, PurpleConnection *gc,
                                 const char *buddy_key,
                                 gboolean sendrequest,
                                 const char *message)
{
    unsigned char *bin_key = toxprpl_hex_string_to_data(buddy_key);
    int ret;

    if (sendrequest == TRUE)
    {
        if ((message == NULL) || (strlen(message) == 0))
        {
            message = DEFAULT_REQUEST_MESSAGE;
        }
        ret = m_addfriend(m, bin_key, (uint8_t *)message,
                          (uint16_t)strlen(message) + 1);
    }
    else
    {
        ret = m_addfriend_norequest(m, bin_key);
    }

    g_free(bin_key);
    const char *msg;
    switch (ret)
    {
        case FAERR_TOOLONG:
            msg = "Message too long";
            break;
        case FAERR_NOMESSAGE:
            msg = "Missing request message";
            break;
        case FAERR_OWNKEY:
            msg = "You're trying to add yourself as a friend";
            break;
        case FAERR_ALREADYSENT:
            msg = "Friend request already sent";
            break;
        case FAERR_BADCHECKSUM:
            msg = "Can't add friend: bad checksum in ID";
            break;
        case FAERR_SETNEWNOSPAM:
            msg = "Can't add friend: wrong nospam ID";
            break;
        case FAERR_NOMEM:
            msg = "Could not allocate memory for friendlist";
            break;
        case FAERR_UNKNOWN:
            msg = "Error adding friend";
            break;
        default:
            break;
    }

    if (ret < 0)
    {
        purple_notify_error(gc, _("Error"), msg, NULL);
    }
    else
    {
        purple_debug_info("toxprpl", "Friend %s added as %d\n", buddy_key, ret);
    }

    return ret;
}

static void toxprpl_do_not_add_to_buddylist(toxprpl_accept_friend_data *data)
{
    g_free(data->buddy_key);
    g_free(data);
}

static void toxprpl_add_to_buddylist(toxprpl_accept_friend_data *data)
{
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(data->gc);

    int ret = toxprpl_tox_addfriend(plugin->m, data->gc, data->buddy_key,
                                    FALSE, NULL);
    if (ret < 0)
    {
        g_free(data->buddy_key);
        g_free(data);
        // error dialogs handled in toxprpl_tox_addfriend()
        return;
    }

    PurpleAccount *account = purple_connection_get_account(data->gc);

    uint8_t alias[MAX_NAME_LENGTH];

    PurpleBuddy *buddy;
    if ((getname(plugin->m, ret, alias) == 0) &&
        (strlen((const char *)alias) > 0))
    {
        purple_debug_info("toxprpl", "Got friend alias %s\n", alias);
        buddy = purple_buddy_new(account, data->buddy_key, (const char*)alias);
    }
    else
    {
        purple_debug_info("toxprpl", "Adding [%s]\n", data->buddy_key);
        buddy = purple_buddy_new(account, data->buddy_key, NULL);
    }

    toxprpl_buddy_data *buddy_data = g_new0(toxprpl_buddy_data, 1);
    buddy_data->tox_friendlist_number = ret;
    purple_buddy_set_protocol_data(buddy, buddy_data);
    purple_blist_add_buddy(buddy, NULL, NULL, NULL);
    USERSTATUS userstatus = m_get_userstatus(plugin->m, ret);
    purple_debug_info("toxprpl", "Friend %s has status %d\n",
            data->buddy_key, userstatus);
    purple_prpl_got_user_status(account, data->buddy_key,
        toxprpl_statuses[toxprpl_get_status_index(plugin->m,ret,userstatus)].id,
        NULL);

    g_free(data->buddy_key);
    g_free(data);
}

static void toxprpl_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
        PurpleGroup *group, const char *msg)
{
    purple_debug_info("toxprpl", "adding %s to buddy list\n", buddy->name);

    buddy->name = g_strstrip(buddy->name);
    if (strlen(buddy->name) != (FRIEND_ADDRESS_SIZE * 2))
    {
        purple_notify_error(gc, _("Error"),
                            _("Invalid buddy ID given (must be 76 characters "
                              "long)"), NULL);
        purple_blist_remove_buddy(buddy);
        return;
    }

    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    int ret = toxprpl_tox_addfriend(plugin->m, gc, buddy->name, TRUE, msg);
    if ((ret < 0) && (ret != FAERR_ALREADYSENT))
    {
        purple_blist_remove_buddy(buddy);
        return;
    }

    gchar *cut = g_ascii_strdown(buddy->name, CLIENT_ID_SIZE * 2 + 1);
    cut[CLIENT_ID_SIZE * 2] = '\0';
    purple_debug_info("toxprpl", "converted %s to %s\n", buddy->name, cut);
    purple_blist_rename_buddy(buddy, cut);
    g_free(cut);
    // buddy data will be added by the query_buddy_info function
    toxprpl_query_buddy_info((gpointer)buddy, (gpointer)gc);
}

static void toxprpl_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
        PurpleGroup *group)
{
    purple_debug_info("toxprpl", "removing buddy %s\n", buddy->name);
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data != NULL)
    {
        purple_debug_info("toxprpl", "removing tox friend #%d\n", buddy_data->tox_friendlist_number);
        m_delfriend(plugin->m, buddy_data->tox_friendlist_number);
    }
}

static void toxprpl_set_nick_action(PurpleConnection *gc,
                                    PurpleRequestFields *fields)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    const char *nickname = purple_request_fields_get_string(fields,
                                                            "text_nickname");
    if (nickname != NULL)
    {
        purple_connection_set_display_name(gc, nickname);
        setname(plugin->m, (uint8_t *)nickname, strlen(nickname) + 1);
        purple_account_set_string(account, "nickname", nickname);
    }
}

static void toxprpl_show_id_dialog_closed(gchar *id)
{
    g_free(id);
}

static void toxprpl_action_show_id_dialog(PurplePluginAction *action)
{
    PurpleConnection *gc = (PurpleConnection*)action->context;

    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);

    uint8_t bin_id[FRIEND_ADDRESS_SIZE];
    getaddress(plugin->m, bin_id);
    gchar *id = toxprpl_tox_friend_id_to_string(bin_id);

    purple_notify_message(gc,
            PURPLE_NOTIFY_MSG_INFO,
            _("Account ID"),
            _("If someone wants to add you, give them this ID:"),
            id,
            (PurpleNotifyCloseCallback)toxprpl_show_id_dialog_closed,
            id);
}

static void toxprpl_action_set_nick_dialog(PurplePluginAction *action)
{
    PurpleConnection *gc = (PurpleConnection*)action->context;
    PurpleAccount *account = purple_connection_get_account(gc);

    PurpleRequestFields *fields;
    PurpleRequestFieldGroup *group;
    PurpleRequestField *field;

    fields = purple_request_fields_new();
    group = purple_request_field_group_new(NULL);
    purple_request_fields_add_group(fields, group);

    field = purple_request_field_string_new("text_nickname",
                    _("Nickname"),
                    purple_account_get_string(account, "nickname", ""), FALSE);

    purple_request_field_group_add_field(group, field);
    purple_request_fields(gc, _("Set your nickname"), NULL, NULL, fields,
            _("_Set"), G_CALLBACK(toxprpl_set_nick_action),
            _("_Cancel"), NULL,
            account, account->username, NULL, gc);
}


static void toxprpl_user_export(PurpleConnection *gc, const char *filename)
{
    purple_debug_info("toxprpl", "export account to %s\n", filename);

    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    if (plugin == NULL)
    {
        return;
    }

    if (plugin->m == NULL)
    {
        return;
    }

    uint32_t msg_size = Messenger_size(plugin->m);
    if (msg_size > 0)
    {
        uint8_t *account_data = g_malloc0(msg_size);
        Messenger_save(plugin->m, account_data);

        int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (fd == -1)
        {
            g_free(account_data);
            purple_notify_message(gc,
                    PURPLE_NOTIFY_MSG_ERROR,
                    _("Error"),
                    _("Could not save account data file:"),
                    strerror(errno),
                    NULL, NULL);
            return;
        }

        size_t wb = write(fd, account_data, msg_size);
        if (wb != msg_size)
        {
            const char *msg2 = NULL;
            if (wb < 0)
            {
                msg2 = strerror(errno);
            }
            purple_notify_message(gc,
                    PURPLE_NOTIFY_MSG_ERROR,
                    _("Error"),
                    _("Could not save account data file:"),
                    msg2,
                    (PurpleNotifyCloseCallback)toxprpl_login,
                    acct);
            g_free(account_data);
            close(fd);
            return;
        }

        g_free(account_data);
        close(fd);
    }
}

static void toxprpl_export_account_dialog(PurplePluginAction *action)
{
    purple_debug_info("toxprpl", "ask to export account\n");

    PurpleConnection *gc = (PurpleConnection*)action->context;
    PurpleAccount *account = purple_connection_get_account(gc);
    toxprpl_plugin_data *plugin = purple_connection_get_protocol_data(gc);
    if (plugin == NULL)
    {
        return;
    }

    if (plugin->m == NULL)
    {
        return;
    }

    uint8_t bin_id[FRIEND_ADDRESS_SIZE];
    getaddress(plugin->m, bin_id);
    gchar *id = toxprpl_tox_friend_id_to_string(bin_id);
    strcpy(id+CLIENT_ID_SIZE, ".tox\0"); // insert extension instead of nospam

    purple_request_file(gc,
        _("Export existing Tox account data"),
        id,
        TRUE,
        G_CALLBACK(toxprpl_user_export),
        NULL,
        account,
        NULL,
        NULL,
        gc);
    g_free(id);
}

static GList *toxprpl_account_actions(PurplePlugin *plugin, gpointer context)
{
    purple_debug_info("toxprpl", "setting up account actions\n");

    GList *actions = NULL;
    PurplePluginAction *action;

    action = purple_plugin_action_new(_("Show my id..."),
             toxprpl_action_show_id_dialog);
    actions = g_list_append(actions, action);

    action = purple_plugin_action_new(_("Set nickname..."),
             toxprpl_action_set_nick_dialog);
    actions = g_list_append(actions, action);

    action = purple_plugin_action_new(_("Export account data..."),
            toxprpl_export_account_dialog);
    actions = g_list_append(actions, action);
    return actions;
}

static void toxprpl_free_buddy(PurpleBuddy *buddy)
{
    if (buddy->proto_data)
    {
        toxprpl_buddy_data *buddy_data = buddy->proto_data;
        g_free(buddy_data);
    }
}

static gboolean toxprpl_offline_message(const PurpleBuddy *buddy)
{
    return FALSE;
}


static PurplePluginProtocolInfo prpl_info =
{
    OPT_PROTO_NO_PASSWORD | OPT_PROTO_REGISTER_NOSCREENNAME | OPT_PROTO_INVITE_MESSAGE,  /* options */
    NULL,               /* user_splits, initialized in toxprpl_init() */
    NULL,               /* protocol_options, initialized in toxprpl_init() */
    NO_BUDDY_ICONS,
    toxprpl_list_icon,                   /* list_icon */
    NULL,                                      /* list_emblem */
    NULL,                                      /* status_text */
    NULL,                                      /* tooltip_text */
    toxprpl_status_types,               /* status_types */
    NULL,                                      /* blist_node_menu */
    NULL,                                      /* chat_info */
    NULL,                                      /* chat_info_defaults */
    toxprpl_login,                      /* login */
    toxprpl_close,                      /* close */
    toxprpl_send_im,                    /* send_im */
    NULL,                                      /* set_info */
    NULL,                                      /* send_typing */
    NULL,                                      /* get_info */
    toxprpl_set_status,                 /* set_status */
    NULL,                                      /* set_idle */
    NULL,                                      /* change_passwd */
    NULL,                                      /* add_buddy */
    NULL,                                      /* add_buddies */
    toxprpl_remove_buddy,               /* remove_buddy */
    NULL,                                      /* remove_buddies */
    NULL,                                      /* add_permit */
    NULL,                                      /* add_deny */
    NULL,                                      /* rem_permit */
    NULL,                                      /* rem_deny */
    NULL,                                      /* set_permit_deny */
    NULL,                                      /* join_chat */
    NULL,                                      /* reject_chat */
    NULL,                                      /* get_chat_name */
    NULL,                                      /* chat_invite */
    NULL,                                      /* chat_leave */
    NULL,                                      /* chat_whisper */
    NULL,                                      /* chat_send */
    NULL,                                      /* keepalive */
    NULL,                                      /* register_user */
    NULL,                                      /* get_cb_info */
    NULL,                                      /* get_cb_away */
    NULL,                                      /* alias_buddy */
    NULL,                                      /* group_buddy */
    NULL,                                      /* rename_group */
    toxprpl_free_buddy,                  /* buddy_free */
    NULL,                                      /* convo_closed */
    NULL,                                      /* normalize */
    NULL,                                      /* set_buddy_icon */
    NULL,                                      /* remove_group */
    NULL,                                      /* get_cb_real_name */
    NULL,                                      /* set_chat_topic */
    NULL,                                      /* find_blist_chat */
    NULL,                                      /* roomlist_get_list */
    NULL,                                      /* roomlist_cancel */
    NULL,                                      /* roomlist_expand_category */
    NULL,                                      /* can_receive_file */
    NULL,                                /* send_file */
    NULL,                                /* new_xfer */
    toxprpl_offline_message,             /* offline_message */
    NULL,                                /* whiteboard_prpl_ops */
    NULL,                                /* send_raw */
    NULL,                                /* roomlist_room_serialize */
    NULL,                                /* unregister_user */
    NULL,                                /* send_attention */
    NULL,                                /* get_attention_types */
    sizeof(PurplePluginProtocolInfo),    /* struct_size */
    NULL,                                /* get_account_text_table */
    NULL,                                /* initiate_media */
    NULL,                                /* get_media_caps */
    NULL,                                /* get_moods */
    NULL,                                /* set_public_alias */
    NULL,                                /* get_public_alias */
    toxprpl_add_buddy,                   /* add_buddy_with_invite */
    NULL                                 /* add_buddies_with_invite */
};

static void toxprpl_init(PurplePlugin *plugin)
{
    purple_debug_info("toxprpl", "starting up\n");

    PurpleAccountOption *option = purple_account_option_string_new(
        _("Nickname"), "nickname", "");
    prpl_info.protocol_options = g_list_append(NULL, option);

    option = purple_account_option_string_new(
        _("Server"), "dht_server", DEFAULT_SERVER_IP);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);

    option = purple_account_option_int_new(_("Port"), "dht_server_port",
            DEFAULT_SERVER_PORT);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);

    option = purple_account_option_string_new(_("Server key"),
        "dht_server_key", DEFAULT_SERVER_KEY);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);
    purple_debug_info("toxprpl", "initialization complete\n");
}

static PurplePluginInfo info =
{
    PURPLE_PLUGIN_MAGIC,                                /* magic */
    PURPLE_MAJOR_VERSION,                               /* major_version */
    PURPLE_MINOR_VERSION,                               /* minor_version */
    PURPLE_PLUGIN_PROTOCOL,                             /* type */
    NULL,                                               /* ui_requirement */
    0,                                                  /* flags */
    NULL,                                               /* dependencies */
    PURPLE_PRIORITY_DEFAULT,                            /* priority */
    TOXPRPL_ID,                                         /* id */
    "Tox",                                              /* name */
    VERSION,                                            /* version */
    "Tox Protocol Plugin",                              /* summary */
    "Tox Protocol Plugin http://tox.im/",              /* description */
    "Sergey 'Jin' Bostandzhyan",                        /* author */
    PACKAGE_URL,                                        /* homepage */
    NULL,                                               /* load */
    NULL,                                               /* unload */
    NULL,                                               /* destroy */
    NULL,                                               /* ui_info */
    &prpl_info,                                         /* extra_info */
    NULL,                                               /* prefs_info */
    toxprpl_account_actions,                            /* actions */
    NULL,                                               /* padding... */
    NULL,
    NULL,
    NULL,
};

PURPLE_INIT_PLUGIN(tox, toxprpl_init, info);
