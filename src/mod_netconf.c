/*!
 * \file mod_netconf.c
 * \brief NETCONF Apache modul for Netopeer
 * \author Tomas Cejka <cejkat@cesnet.cz>
 * \author Radek Krejci <rkrejci@cesnet.cz>
 * \date 2011
 * \date 2012
 * \date 2013
 */
/*
 * Copyright (C) 2011-2013 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */
static const char rcsid[] __attribute__((used)) ="$Id: "__FILE__": "ARCSID" $";

#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <pthread.h>
#include <ctype.h>

#include <unixd.h>
#include <httpd.h>
#include <http_log.h>
#include <http_config.h>

#include <apr_sha1.h>
#include <apr_hash.h>
#include <apr_signal.h>
#include <apr_strings.h>

#include <json/json.h>

#include <libnetconf.h>
#include <libnetconf_ssh.h>

#ifdef WITH_NOTIFICATIONS
#include "notification_module.h"
#endif

#include "message_type.h"
#include "mod_netconf.h"

#define MAX_PROCS 5
#define SOCKET_FILENAME "/tmp/mod_netconf.sock"
#define MAX_SOCKET_CL 10
#define BUFFER_SIZE 4096
#define NOTIFICATION_QUEUE_SIZE 10
#define ACTIVITY_CHECK_INTERVAL	10  /**< timeout in seconds, how often activity is checked */
#define ACTIVITY_TIMEOUT	(60*60)  /**< timeout in seconds, after this time, session is automaticaly closed. */

/* sleep in master process for non-blocking socket reading */
#define SLEEP_TIME 200

#ifndef offsetof
#define offsetof(type, member) ((size_t) ((type *) 0)->member)
#endif

/* timeout in msec */
struct timeval timeout = { 1, 0 };

#define NCWITHDEFAULTS	NCWD_MODE_NOTSET


#define MSG_OK 0
#define MSG_OPEN  1
#define MSG_DATA  2
#define MSG_CLOSE 3
#define MSG_ERROR 4
#define MSG_UNKNOWN 5

module AP_MODULE_DECLARE_DATA netconf_module;

pthread_rwlock_t session_lock; /**< mutex protecting netconf_sessions_list from multiple access errors */
pthread_mutex_t ntf_history_lock; /**< mutex protecting notification history list */
apr_hash_t *netconf_sessions_list = NULL;

static pthread_key_t notif_history_key;
server_rec *http_server = NULL;

volatile int isterminated = 0;

static char* password;

static void signal_handler(int sign)
{
	switch (sign) {
	case SIGTERM:
		isterminated = 1;
		break;
	}
}

static char* gen_ncsession_hash( const char* hostname, const char* port, const char* sid)
{
	unsigned char hash_raw[APR_SHA1_DIGESTSIZE];
	int i;
	char* hash;

	apr_sha1_ctx_t sha1_ctx;
	apr_sha1_init(&sha1_ctx);
	apr_sha1_update(&sha1_ctx, hostname, strlen(hostname));
	apr_sha1_update(&sha1_ctx, port, strlen(port));
	apr_sha1_update(&sha1_ctx, sid, strlen(sid));
	apr_sha1_final(hash_raw, &sha1_ctx);

	/* convert binary hash into hex string, which is printable */
	hash = malloc(sizeof(char) * ((2*APR_SHA1_DIGESTSIZE)+1));
	for (i = 0; i < APR_SHA1_DIGESTSIZE; i++) {
		snprintf(hash + (2*i), 3, "%02x", hash_raw[i]);
	}
	//hash[2*APR_SHA1_DIGESTSIZE] = 0;

	return (hash);
}

int netconf_callback_ssh_hostkey_check (const char* hostname, int keytype, const char* fingerprint)
{
	/* always approve */
	return (EXIT_SUCCESS);
}

char* netconf_callback_sshauth_password (const char* username, const char* hostname)
{
	char* buf;

	buf = malloc ((strlen(password) + 1) * sizeof(char));
	apr_cpystrn(buf, password, strlen(password) + 1);

	return (buf);
}

void netconf_callback_sshauth_interactive (const char* name,
		int name_len,
		const char* instruction,
		int instruction_len,
		int num_prompts,
		const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
		LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
		void** abstract)
{
	int i;

	for (i=0; i<num_prompts; i++) {
		responses[i].text = malloc ((strlen(password) + 1) * sizeof(char));
		apr_cpystrn(responses[i].text, password, strlen(password) + 1);
		responses[i].length = strlen(responses[i].text) + 1;
	}

	return;
}

static json_object *err_reply = NULL;
void netconf_callback_error_process(const char* tag,
		const char* type,
		const char* severity,
		const char* apptag,
		const char* path,
		const char* message,
		const char* attribute,
		const char* element,
		const char* ns,
		const char* sid)
{
	err_reply = json_object_new_object();
	json_object_object_add(err_reply, "type", json_object_new_int(REPLY_ERROR));
	if (tag) json_object_object_add(err_reply, "error-tag", json_object_new_string(tag));
	if (type) json_object_object_add(err_reply, "error-type", json_object_new_string(type));
	if (severity) json_object_object_add(err_reply, "error-severity", json_object_new_string(severity));
	if (apptag) json_object_object_add(err_reply, "error-app-tag", json_object_new_string(apptag));
	if (path) json_object_object_add(err_reply, "error-path", json_object_new_string(path));
	if (message) json_object_object_add(err_reply, "error-message", json_object_new_string(message));
	if (attribute) json_object_object_add(err_reply, "bad-attribute", json_object_new_string(attribute));
	if (element) json_object_object_add(err_reply, "bad-element", json_object_new_string(element));
	if (ns) json_object_object_add(err_reply, "bad-namespace", json_object_new_string(ns));
	if (sid) json_object_object_add(err_reply, "session-id", json_object_new_string(sid));
}

/**
 * should be used in locked area
 */
void prepare_status_message(server_rec* server, struct session_with_mutex *s, struct nc_session *session)
{
	json_object *json_obj = NULL;
	//json_object *old_sid = NULL;
	const char *cpbltstr;
	struct nc_cpblts* cpblts = NULL;

	if (s == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "No session given.");
		return;
	}

	if (s->hello_message != NULL) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "clean previous hello message");
		//json_object_put(s->hello_message);
		s->hello_message = NULL;

		//old_sid = json_object_object_get(s->hello_message, "sid");
	}
	s->hello_message = json_object_new_object();
	if (session != NULL) {
		/** \todo reload hello - save old sid */
		//if (old_sid != NULL) {
		//	/* use previous sid */
		//	json_object_object_add(s->hello_message, "sid", old_sid);
		//} else {
			/* we don't have old sid */
		json_object_object_add(s->hello_message, "sid", json_object_new_string(nc_session_get_id(session)));
		//}
		json_object_object_add(s->hello_message, "version", json_object_new_string((nc_session_get_version(session) == 0)?"1.0":"1.1"));
		json_object_object_add(s->hello_message, "host", json_object_new_string(nc_session_get_host(session)));
		json_object_object_add(s->hello_message, "port", json_object_new_string(nc_session_get_port(session)));
		json_object_object_add(s->hello_message, "user", json_object_new_string(nc_session_get_user(session)));
		cpblts = nc_session_get_cpblts (session);
		if (cpblts != NULL) {
			json_obj = json_object_new_array();
			nc_cpblts_iter_start (cpblts);
			while ((cpbltstr = nc_cpblts_iter_next (cpblts)) != NULL) {
				json_object_array_add(json_obj, json_object_new_string(cpbltstr));
			}
			json_object_object_add(s->hello_message, "capabilities", json_obj);
		}
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "%s", json_object_to_json_string(s->hello_message));
	} else {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Session was not given.");
		json_object_object_add(s->hello_message, "type", json_object_new_int(REPLY_ERROR));
		json_object_object_add(s->hello_message, "error-message", json_object_new_string("Invalid session identifier."));
	}
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Status info from hello message prepared");

}


/**
 * \defgroup netconf_operations NETCONF operations
 * The list of NETCONF operations that mod_netconf supports.
 * @{
 */

/**
 * \brief Connect to NETCONF server
 *
 * \warning Session_key hash is not bound with caller identification. This could be potential security risk.
 */
static char* netconf_connect(server_rec* server, apr_pool_t* pool, const char* host, const char* port, const char* user, const char* pass, struct nc_cpblts * cpblts)
{
	struct nc_session* session = NULL;
	struct session_with_mutex * locked_session;
	char *session_key;

	/* connect to the requested NETCONF server */
	password = (char*)pass;
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "prepare to connect %s@%s:%s", user, host, port);
	nc_verbosity(NC_VERB_DEBUG);
	session = nc_session_connect(host, (unsigned short) atoi (port), user, cpblts);
	ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "nc_session_connect done");

	/* if connected successful, add session to the list */
	if (session != NULL) {
		/* generate hash for the session */
		session_key = gen_ncsession_hash(
				(host==NULL) ? "localhost" : host,
				(port==NULL) ? "830" : port,
				nc_session_get_id(session));

		/** \todo allocate from apr_pool */
		if ((locked_session = malloc (sizeof (struct session_with_mutex))) == NULL || pthread_mutex_init (&locked_session->lock, NULL) != 0) {
			nc_session_free(session);
			free (locked_session);
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Creating structure session_with_mutex failed %d (%s)", errno, strerror(errno));
			return NULL;
		}
		locked_session->session = session;
		locked_session->last_activity = apr_time_now();
		locked_session->hello_message = NULL;
		locked_session->closed = 0;
		pthread_mutex_init (&locked_session->lock, NULL);
		ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "Before session_lock");
		/* get exclusive access to sessions_list (conns) */
		if (pthread_rwlock_wrlock (&session_lock) != 0) {
			nc_session_free(session);
			free (locked_session);
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock: %d (%s)", errno, strerror(errno));
			return NULL;
		}
		locked_session->notifications = apr_array_make(pool, NOTIFICATION_QUEUE_SIZE, sizeof(notification_t));
		locked_session->ntfc_subscribed = 0;
		ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "Add connection to the list");
		apr_hash_set(netconf_sessions_list, apr_pstrdup(pool, session_key), APR_HASH_KEY_STRING, (void *) locked_session);
		ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "Before session_unlock");

		/* lock session */
		pthread_mutex_lock(&locked_session->lock);

		/* unlock session list */
		if (pthread_rwlock_unlock (&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}

		/* store information about session from hello message for future usage */
		prepare_status_message(server, locked_session, session);

		pthread_mutex_unlock(&locked_session->lock);

		ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "NETCONF session established");
		return (session_key);
	} else {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Connection could not be established");
		return (NULL);
	}

}

static int close_and_free_session(server_rec *server, struct session_with_mutex *locked_session)
{
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "lock private lock.");
	if (pthread_mutex_lock(&locked_session->lock) != 0) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock");
	}
	locked_session->ntfc_subscribed = 0;
	locked_session->closed = 1;
	nc_session_free(locked_session->session);
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "session closed.");
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "unlock private lock.");
	if (pthread_mutex_unlock(&locked_session->lock) != 0) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock");
	}

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "unlock session lock.");
	ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "closed session, disabled notif(?), wait 2s");
	usleep(500000); /* let notification thread stop */

	/* session shouldn't be used by now */
	/** \todo free all notifications from queue */
	apr_array_clear(locked_session->notifications);
	pthread_mutex_destroy(&locked_session->lock);
	if (locked_session->hello_message != NULL) {
		/** \todo free hello_message */
		//json_object_put(locked_session->hello_message);
		locked_session->hello_message = NULL;
	}
	locked_session->session = NULL;
	free(locked_session);
	locked_session = NULL;
	ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "NETCONF session closed, everything cleared.");
	return (EXIT_SUCCESS);
}

static int netconf_close(server_rec* server, const char* session_key)
{
	struct session_with_mutex * locked_session;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Key in hash to close: %s", session_key);

	/* get exclusive (write) access to sessions_list (conns) */
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "lock session lock.");
	if (pthread_rwlock_wrlock (&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock");
		return EXIT_FAILURE;
	}
	/* get session to close */
	locked_session = (struct session_with_mutex *)apr_hash_get(netconf_sessions_list, session_key, APR_HASH_KEY_STRING);
	/* remove session from the active sessions list -> nobody new can now work with session */
	apr_hash_set(netconf_sessions_list, session_key, APR_HASH_KEY_STRING, NULL);

	if (pthread_rwlock_unlock (&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock");
	}

	if ((locked_session != NULL) && (locked_session->session != NULL)) {
		return close_and_free_session(server, locked_session);
	} else {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Unknown session to close");
		return (EXIT_FAILURE);
	}
}

/**
 * Test reply message type and return error message.
 *
 * \param[in] server	httpd server for logging
 * \param[in] session	nc_session internal struct
 * \param[in] session_key session key, NULL to disable disconnect on error
 * \param[in] msgt	RPC-REPLY message type
 * \param[out] data
 * \return NULL on success
 */
json_object *netconf_test_reply(server_rec *server, struct nc_session *session, const char *session_key, NC_MSG_TYPE msgt, nc_reply *reply, char **data)
{
	NC_REPLY_TYPE replyt;

	/* process the result of the operation */
	switch (msgt) {
		case NC_MSG_UNKNOWN:
			if (nc_session_get_status(session) != NC_SESSION_STATUS_WORKING) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: receiving rpc-reply failed");
				if (session_key != NULL) {
					netconf_close(server, session_key);
				}
				return create_error("Internal: Receiving RPC-REPLY failed.");
			}
		case NC_MSG_NONE:
			/* there is error handled by callback */
			(*data) = NULL;
			return NULL;
		case NC_MSG_REPLY:
			switch (replyt = nc_reply_get_type(reply)) {
				case NC_REPLY_OK:
					(*data) = NULL;
					return NULL;
				case NC_REPLY_DATA:
					if (((*data) = nc_reply_get_data (reply)) == NULL) {
						ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: no data from reply");
						return create_error("Internal: No data from reply received.");
					} else {
						return NULL;
					}
					break;
				case NC_REPLY_ERROR:
					ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: unexpected rpc-reply (%d)", replyt);
					(*data) = NULL;
					return create_error(nc_reply_get_errormsg(reply));
				default:
					ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: unexpected rpc-reply (%d)", replyt);
					(*data) = NULL;
					return create_error(nc_reply_get_errormsg(reply));
			}
			break;
		default:
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: unexpected reply message received (%d)", msgt);
			(*data) = NULL;
			return create_error("Internal: Unexpected RPC-REPLY message type.");
	}
}

json_object *netconf_unlocked_op(server_rec* server, struct nc_session *session, nc_rpc* rpc)
{
	nc_reply* reply = NULL;
	NC_MSG_TYPE msgt;

	/* check requests */
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: rpc is not created");
		return create_error("Internal error: RPC is not created");
	}

	if (session != NULL) {
		/* send the request and get the reply */
		msgt = nc_session_send_recv(session, rpc, &reply);
		/* process the result of the operation */
		return netconf_test_reply(server, session, NULL, msgt, reply, NULL);
	} else {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Unknown session to process.");
		return create_error("Internal error: Unknown session to process.");
	}
}

/**
 * Perform RPC method that returns data.
 *
 * \param[in] server	httpd server
 * \param[in] session_key	session identifier
 * \param[in] rpc	RPC message to perform
 * \param[out] received_data	received data string, can be NULL when no data expected, value can be set to NULL if no data received
 * \return NULL on success, json object with error otherwise
 */
static json_object *netconf_op(server_rec* server, const char* session_key, nc_rpc* rpc, char **received_data)
{
	struct nc_session *session = NULL;
	struct session_with_mutex * locked_session;
	nc_reply* reply = NULL;
	json_object *res = NULL;
	char* data;
	NC_MSG_TYPE msgt;

	/* check requests */
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: rpc is not created");
		res = create_error("Internal: RPC could not be created.");
		data = NULL;
		goto finished;
	}

	/* get non-exclusive (read) access to sessions_list (conns) */
	if (pthread_rwlock_rdlock (&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock: %d (%s)", errno, strerror(errno));
		res = create_error("Internal: Lock failed.");
		data = NULL;
		goto finished;
	}
	/* get session where send the RPC */
	locked_session = (struct session_with_mutex *)apr_hash_get(netconf_sessions_list, session_key, APR_HASH_KEY_STRING);
	if (locked_session != NULL) {
		session = locked_session->session;
	}
	if (session != NULL) {
		/* get exclusive access to session */
		if (pthread_mutex_lock(&locked_session->lock) != 0) {
			/* unlock before returning error */
			if (pthread_rwlock_unlock (&session_lock) != 0) {

				ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock: %d (%s)", errno, strerror(errno));
				res = create_error("Internal: Could not unlock.");
				goto finished;
			}
			res = create_error("Internal: Could not unlock.");
		}
		if (pthread_rwlock_unlock(&session_lock) != 0) {

			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock: %d (%s)", errno, strerror(errno));
			res = create_error("Internal: Could not unlock.");
		}

		locked_session->last_activity = apr_time_now();

		/* send the request and get the reply */
		msgt = nc_session_send_recv(session, rpc, &reply);

		/* first release exclusive lock for this session */
		pthread_mutex_unlock(&locked_session->lock);
		/* end of critical section */

		res = netconf_test_reply(server, session, session_key, msgt, reply, &data);
	} else {
		/* release lock on failure */
		if (pthread_rwlock_unlock (&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Unknown session to process.");
		res = create_error("Unknown session to process.");
		data = NULL;
	}
finished:
	nc_reply_free(reply);
	if (received_data != NULL) {
		(*received_data) = data;
	}
	return res;
}

static char* netconf_getconfig(server_rec* server, const char* session_key, NC_DATASTORE source, const char* filter)
{
	nc_rpc* rpc;
	struct nc_filter *f = NULL;
	char* data = NULL;
	json_object *res = NULL;

	/* create filter if set */
	if (filter != NULL) {
		f = nc_filter_new(NC_FILTER_SUBTREE, filter);
	}

	/* create requests */
	rpc = nc_rpc_getconfig (source, f);
	nc_filter_free(f);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return (NULL);
	}

	/* tell server to show all elements even if they have default values */
	if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_ALL)) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: setting withdefaults failed");
	}

	res = netconf_op(server, session_key, rpc, &data);
	nc_rpc_free (rpc);
	if (res == NULL) {
		/* TODO return error message somehow */
	}

	return (data);
}

static char* netconf_getschema(server_rec* server, const char* session_key, const char* identifier, const char* version, const char* format)
{
	nc_rpc* rpc;
	char* data = NULL;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_getschema(identifier, version, format);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return (NULL);
	}

	res = netconf_op(server, session_key, rpc, &data);
	nc_rpc_free (rpc);
	if (res == NULL) {
		/* TODO return error message somehow */
	}

	return (data);
}

static char* netconf_get(server_rec* server, const char* session_key, const char* filter)
{
	nc_rpc* rpc;
	struct nc_filter *f = NULL;
	char* data = NULL;
	json_object *res = NULL;

	/* create filter if set */
	if (filter != NULL) {
		f = nc_filter_new(NC_FILTER_SUBTREE, filter);
	}

	/* create requests */
	rpc = nc_rpc_get (f);
	nc_filter_free(f);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return (NULL);
	}

	/* tell server to show all elements even if they have default values */
	if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, NCWD_MODE_ALL)) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: setting withdefaults failed");
	}

	res = netconf_op(server, session_key, rpc, &data);
	nc_rpc_free (rpc);
	if (res == NULL) {
		/* TODO return error message somehow */
	}

	return (data);
}

static json_object *netconf_copyconfig(server_rec* server, const char* session_key, NC_DATASTORE source, NC_DATASTORE target, const char* config, const char *url)
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	if ((source == NC_DATASTORE_CONFIG) || (source == NC_DATASTORE_URL)) {
		if (target == NC_DATASTORE_URL) {
			rpc = nc_rpc_copyconfig(source, target, config, url);
		} else {
			rpc = nc_rpc_copyconfig(source, target, config);
		}
	} else {
		if (target == NC_DATASTORE_URL) {
			rpc = nc_rpc_copyconfig(source, target, url);
		} else {
			rpc = nc_rpc_copyconfig(source, target);
		}
	}
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(server, session_key, rpc, NULL);
	nc_rpc_free (rpc);

	return res;
}

static json_object *netconf_editconfig(server_rec* server, const char* session_key, NC_DATASTORE target, NC_EDIT_DEFOP_TYPE defop, NC_EDIT_ERROPT_TYPE erropt, NC_EDIT_TESTOPT_TYPE testopt, const char* config)
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	/* TODO source NC_DATASTORE_CONFIG / NC_DATASTORE_URL  */
	rpc = nc_rpc_editconfig(target, NC_DATASTORE_CONFIG, defop, erropt, testopt, config);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(server, session_key, rpc, NULL);
	nc_rpc_free (rpc);

	return res;
}

static json_object *netconf_killsession(server_rec* server, const char* session_key, const char* sid)
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_killsession(sid);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(server, session_key, rpc, NULL);
	nc_rpc_free (rpc);
	return res;
}

static json_object *netconf_onlytargetop(server_rec* server, const char* session_key, NC_DATASTORE target, nc_rpc* (*op_func)(NC_DATASTORE))
{
	nc_rpc* rpc;
	json_object *res = NULL;

	/* create requests */
	rpc = op_func(target);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(server, session_key, rpc, NULL);
	nc_rpc_free (rpc);
	return res;
}

static json_object *netconf_deleteconfig(server_rec* server, const char* session_key, NC_DATASTORE target, const char *url)
{
	nc_rpc *rpc = NULL;
	json_object *res = NULL;
	if (target != NC_DATASTORE_URL) {
		rpc = nc_rpc_deleteconfig(target);
	} else {
		rpc = nc_rpc_deleteconfig(target, url);
	}
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	res = netconf_op(server, session_key, rpc, NULL);
	nc_rpc_free (rpc);
	return res;
}

static json_object *netconf_lock(server_rec* server, const char* session_key, NC_DATASTORE target)
{
	return (netconf_onlytargetop(server, session_key, target, nc_rpc_lock));
}

static json_object *netconf_unlock(server_rec* server, const char* session_key, NC_DATASTORE target)
{
	return (netconf_onlytargetop(server, session_key, target, nc_rpc_unlock));
}

static json_object *netconf_generic(server_rec* server, const char* session_key, const char* content, char** data)
{
	nc_rpc* rpc = NULL;
	json_object *res = NULL;

	/* create requests */
	rpc = nc_rpc_generic(content);
	if (rpc == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
		return create_error("Internal: Creating rpc request failed");
	}

	if (data != NULL) {
		// TODO ?free(*data);
		(*data) = NULL;
	}

	/* get session where send the RPC */
	res = netconf_op(server, session_key, rpc, data);
	nc_rpc_free (rpc);
	return res;
}

/**
 * @}
 *//* netconf_operations */

server_rec* clb_print_server;
void clb_print(NC_VERB_LEVEL level, const char* msg)
{
	switch (level) {
	case NC_VERB_ERROR:
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, clb_print_server, "%s", msg);
		break;
	case NC_VERB_WARNING:
		ap_log_error(APLOG_MARK, APLOG_WARNING, 0, clb_print_server, "%s", msg);
		break;
	case NC_VERB_VERBOSE:
		ap_log_error(APLOG_MARK, APLOG_INFO, 0, clb_print_server, "%s", msg);
		break;
	case NC_VERB_DEBUG:
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, clb_print_server, "%s", msg);
		break;
	}
}

/**
 * Receive message from client over UNIX socket and return pointer to it.
 * Caller should free message memory.
 * \param[in] client	socket descriptor of client
 * \param[in] server	httpd server for logging
 * \return pointer to message
 */
char *get_framed_message(server_rec *server, int client)
{
	/* read json in chunked framing */
	unsigned int buffer_size = 0;
	ssize_t buffer_len = 0;
	char *buffer = NULL;
	char c;
	ssize_t ret;
	int i, chunk_len;
	char chunk_len_str[12];

	while (1) {
		/* read chunk length */
		if ((ret = recv (client, &c, 1, 0)) != 1 || c != '\n') {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		if ((ret = recv (client, &c, 1, 0)) != 1 || c != '#') {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		i=0;
		memset (chunk_len_str, 0, 12);
		while ((ret = recv (client, &c, 1, 0) == 1 && (isdigit(c) || c == '#'))) {
			if (i==0 && c == '#') {
				if (recv (client, &c, 1, 0) != 1 || c != '\n') {
					/* end but invalid */
					if (buffer != NULL) {
						free (buffer);
						buffer = NULL;
					}
				}
				/* end of message, double-loop break */
				goto msg_complete;
			}
			chunk_len_str[i++] = c;
			if (i==11) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Message is too long, buffer for length is not big enought!!!!");
				break;
			}
		}
		if (c != '\n') {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		chunk_len_str[i] = 0;
		if ((chunk_len = atoi (chunk_len_str)) == 0) {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		buffer_size += chunk_len+1;
		buffer = realloc (buffer, sizeof(char)*buffer_size);
		if ((ret = recv (client, buffer+buffer_len, chunk_len, 0)) == -1 || ret != chunk_len) {
			if (buffer != NULL) {
				free (buffer);
				buffer = NULL;
			}
			break;
		}
		buffer_len += ret;
	}
msg_complete:
	return buffer;
}

NC_DATASTORE parse_datastore(const char *ds)
{
	if (strcmp(ds, "running") == 0) {
		return NC_DATASTORE_RUNNING;
	} else if (strcmp(ds, "startup") == 0) {
		return NC_DATASTORE_STARTUP;
	} else if (strcmp(ds, "candidate") == 0) {
		return NC_DATASTORE_CANDIDATE;
	} else if (strcmp(ds, "url") == 0) {
		return NC_DATASTORE_URL;
	}
	return -1;
}

json_object *create_error(const char *errmess)
{
	json_object *reply = json_object_new_object();
	json_object_object_add(reply, "type", json_object_new_int(REPLY_ERROR));
	json_object_object_add(reply, "error-message", json_object_new_string(errmess));
	return reply;

}

json_object *create_data(const char *data)
{
	json_object *reply = json_object_new_object();
	json_object_object_add(reply, "type", json_object_new_int(REPLY_DATA));
	json_object_object_add(reply, "data", json_object_new_string(data));
	return reply;
}

json_object *handle_op_connect(server_rec *server, apr_pool_t *pool, json_object *request)
{
	const char *host = NULL;
	const char *port = NULL;
	const char *user = NULL;
	const char *pass = NULL;
	json_object *capabilities = NULL;
	json_object *reply = NULL;
	char *session_key_hash = NULL;
	struct nc_cpblts* cpblts = NULL;
	unsigned int len, i;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: Connect");
	host = json_object_get_string(json_object_object_get((json_object *) request, "host"));
	port = json_object_get_string(json_object_object_get((json_object *) request, "port"));
	user = json_object_get_string(json_object_object_get((json_object *) request, "user"));
	pass = json_object_get_string(json_object_object_get((json_object *) request, "pass"));

	capabilities = json_object_object_get((json_object *) request, "capabilities");
	if ((capabilities != NULL) && ((len = json_object_array_length(capabilities)) > 0)) {
		cpblts = nc_cpblts_new(NULL);
		for (i=0; i<len; i++) {
			nc_cpblts_add(cpblts, json_object_get_string(json_object_array_get_idx(capabilities, i)));
		}
	} else {
		ap_log_error (APLOG_MARK, APLOG_ERR, 0, server, "no capabilities specified");
	}

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "host: %s, port: %s, user: %s", host, port, user);
	if ((host == NULL) || (user == NULL)) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Cannot connect - insufficient input.");
		session_key_hash = NULL;
	} else {
		session_key_hash = netconf_connect(server, pool, host, port, user, pass, cpblts);
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "hash: %s", session_key_hash);
	}
	if (cpblts != NULL) {
		nc_cpblts_free(cpblts);
	}

	if (session_key_hash == NULL) {
		/* negative reply */
		if (err_reply == NULL) {
			reply = json_object_new_object();
			json_object_object_add(reply, "type", json_object_new_int(REPLY_ERROR));
			json_object_object_add(reply, "error-message", json_object_new_string("Connecting NETCONF server failed."));
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Connection failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Connect - error from libnetconf's callback.");
		}
	} else {
		/* positive reply */
		reply = json_object_new_object();
		json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
		json_object_object_add(reply, "session", json_object_new_string(session_key_hash));

		free(session_key_hash);
	}
	return reply;
}

json_object *handle_op_get(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	const char *filter = NULL;
	const char *data = NULL;
	json_object *reply = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: get (session %s)", session_key);

	filter = json_object_get_string(json_object_object_get(request, "filter"));

	if ((data = netconf_get(server, session_key, filter)) == NULL) {
		if (err_reply == NULL) {
			reply = create_error("Get information from device failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		return create_data(data);
	}
	return reply;
}

json_object *handle_op_getconfig(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	NC_DATASTORE ds_type_s = -1;
	NC_DATASTORE ds_type_t = -1;
	const char *filter = NULL;
	const char *data = NULL;
	const char *source = NULL;
	const char *target = NULL;
	json_object *reply = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: get-config (session %s)", session_key);

	filter = json_object_get_string(json_object_object_get(request, "filter"));

	/* get parameters */
	if ((target = json_object_get_string(json_object_object_get(request, "target"))) != NULL) {
		ds_type_t = parse_datastore(target);
	}
	if ((source = json_object_get_string(json_object_object_get(request, "source"))) != NULL) {
		ds_type_s = parse_datastore(source);
	}
	if (ds_type_s == -1) {
		return create_error("Invalid source repository type requested.");
	}

	if ((data = netconf_getconfig(server, session_key, ds_type_s, filter)) == NULL) {
		if (err_reply == NULL) {
			reply = create_error("Get configuration information from device failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		return create_data(data);
	}
	return reply;
}

json_object *handle_op_getschema(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	const char *data = NULL;
	const char *identifier = NULL;
	const char *version = NULL;
	const char *format = NULL;
	json_object *reply = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: get-schema (session %s)", session_key);
	identifier = json_object_get_string(json_object_object_get(request, "identifier"));
	if (identifier == NULL) {
		return create_error("No identifier for get-schema supplied.");
	}
	version = json_object_get_string(json_object_object_get(request, "version"));
	format = json_object_get_string(json_object_object_get(request, "format"));

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "get-schema(version: %s, format: %s)", version, format);
	if ((data = netconf_getschema(server, session_key, identifier, version, format)) == NULL) {
		if (err_reply == NULL) {
			reply = create_error("Get schema failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		return create_data(data);
	}
	return reply;
}

json_object *handle_op_editconfig(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	NC_DATASTORE ds_type_s = -1;
	NC_DATASTORE ds_type_t = -1;
	NC_EDIT_DEFOP_TYPE defop_type = NC_EDIT_DEFOP_NOTSET;
	NC_EDIT_ERROPT_TYPE erropt_type = 0;
	const char *defop = NULL;
	const char *erropt = NULL;
	const char *config = NULL;
	const char *source = NULL;
	const char *target = NULL;
	json_object *reply = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: edit-config (session %s)", session_key);

	defop = json_object_get_string(json_object_object_get(request, "default-operation"));
	if (defop != NULL) {
		if (strcmp(defop, "merge") == 0) {
			defop_type = NC_EDIT_DEFOP_MERGE;
		} else if (strcmp(defop, "replace") == 0) {
			defop_type = NC_EDIT_DEFOP_REPLACE;
		} else if (strcmp(defop, "none") == 0) {
			defop_type = NC_EDIT_DEFOP_NONE;
		} else {
			return create_error("Invalid default-operation parameter.");
		}
	} else {
		defop_type = NC_EDIT_DEFOP_NOTSET;
	}

	erropt = json_object_get_string(json_object_object_get(request, "error-option"));
	if (erropt != NULL) {
		if (strcmp(erropt, "continue-on-error") == 0) {
			erropt_type = NC_EDIT_ERROPT_CONT;
		} else if (strcmp(erropt, "stop-on-error") == 0) {
			erropt_type = NC_EDIT_ERROPT_STOP;
		} else if (strcmp(erropt, "rollback-on-error") == 0) {
			erropt_type = NC_EDIT_ERROPT_ROLLBACK;
		} else {
			return create_error("Invalid error-option parameter.");
		}
	} else {
		erropt_type = 0;
	}

	/* get parameters */
	if ((target = json_object_get_string(json_object_object_get(request, "target"))) != NULL) {
		ds_type_t = parse_datastore(target);
	}
	if ((source = json_object_get_string(json_object_object_get(request, "source"))) != NULL) {
		ds_type_s = parse_datastore(source);
	}
	if (ds_type_t == -1) {
		return create_error("Invalid target repository type requested.");
	}

	config = json_object_get_string(json_object_object_get(request, "config"));
	if (config == NULL) {
		return create_error("Invalid config data parameter.");
	}

	if (netconf_editconfig(server, session_key, ds_type_t, defop_type, erropt_type, NC_EDIT_TESTOPT_TESTSET, config) != EXIT_SUCCESS) {
		if (err_reply == NULL) {
			reply = create_error("Edit-config failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		reply = json_object_new_object();
		json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
	}
	return reply;
}

json_object *handle_op_copyconfig(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	NC_DATASTORE ds_type_s = -1;
	NC_DATASTORE ds_type_t = -1;
	const char *config = NULL;
	const char *target = NULL;
	const char *source = NULL;
	json_object *reply = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: copy-config (session %s)", session_key);

	/* get parameters */
	if ((target = json_object_get_string(json_object_object_get(request, "target"))) != NULL) {
		ds_type_t = parse_datastore(target);
	}
	if ((source = json_object_get_string(json_object_object_get(request, "source"))) != NULL) {
		ds_type_s = parse_datastore(source);
	}
	if (source == NULL) {
		/* no explicit source specified -> use config data */
		ds_type_s = NC_DATASTORE_CONFIG;
		config = json_object_get_string(json_object_object_get(request, "config"));
	} else if (ds_type_s == -1) {
		/* source datastore specified, but it is invalid */
		return create_error("Invalid source repository type requested.");
	}

	if (ds_type_t == -1) {
		/* invalid target datastore specified */
		return create_error("Invalid target repository type requested.");
	}

	if (source == NULL && config == NULL) {
		reply = create_error("invalid input parameters - one of source and config is required.");
	} else {
		if (netconf_copyconfig(server, session_key, ds_type_s, ds_type_t, config, "") != EXIT_SUCCESS) {
			if (err_reply == NULL) {
				reply = create_error("Copying of configuration failed.");
			} else {
				/* use filled err_reply from libnetconf's callback */
				reply = err_reply;
			}
		} else {
			reply = json_object_new_object();
			json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
		}
	}
	return reply;
}

json_object *handle_op_generic(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	json_object *reply = NULL;
	const char *config = NULL;
	char *data = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: generic request for session %s", session_key);

	config = json_object_get_string(json_object_object_get(request, "content"));

	if (config == NULL) {
		return create_error("Missing content parameter.");
	}

	if (netconf_generic(server, session_key, config, &data) != EXIT_SUCCESS) {
		if (err_reply == NULL) {
			reply = create_error("Killing of session failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		if (data == NULL) {
			reply = json_object_new_object();
			json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
		} else {
			return create_data(data);
		}
	}
	return reply;
}

json_object *handle_op_disconnect(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	json_object *reply = NULL;
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: Disconnect session %s", session_key);

	if (netconf_close(server, session_key) != EXIT_SUCCESS) {
		if (err_reply == NULL) {
			reply = create_error("Invalid session identifier.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		reply = json_object_new_object();
		json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
	}
	return reply;
}

json_object *handle_op_kill(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	json_object *reply = NULL;
	const char *sid = NULL;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: kill-session, session %s", session_key);

	sid = json_object_get_string(json_object_object_get(request, "session-id"));

	if (sid == NULL) {
		return create_error("Missing session-id parameter.");
	}

	if (netconf_killsession(server, session_key, sid) != EXIT_SUCCESS) {
		if (err_reply == NULL) {
			reply = create_error("Killing of session failed.");
		} else {
			/* use filled err_reply from libnetconf's callback */
			reply = err_reply;
		}
	} else {
		reply = json_object_new_object();
		json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
	}
	return reply;
}

json_object *handle_op_reloadhello(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	struct nc_session *temp_session = NULL;
	struct session_with_mutex * locked_session = NULL;
	json_object *reply = NULL;
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: get info about session %s", session_key);

	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		return NULL;
	}

	locked_session = (struct session_with_mutex *)apr_hash_get(netconf_sessions_list, session_key, APR_HASH_KEY_STRING);
	if ((locked_session != NULL) && (locked_session->hello_message != NULL)) {
		pthread_mutex_lock(&locked_session->lock);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "creating temporal NC session.");
		temp_session = nc_session_connect_channel(locked_session->session, NULL);
		if (temp_session != NULL) {
			prepare_status_message(server, locked_session, temp_session);
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "closing temporal NC session.");
			nc_session_free(temp_session);
		} else {
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Reload hello failed due to channel establishment");
			reply = create_error("Reload was unsuccessful, connection failed.");
		}
		pthread_mutex_unlock(&locked_session->lock);
	} else {
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}

	if ((reply == NULL) && (locked_session->hello_message != NULL)) {
		reply = locked_session->hello_message;
	}
	return reply;
}

json_object *handle_op_info(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	json_object *reply = NULL;
	struct session_with_mutex * locked_session = NULL;
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: get info about session %s", session_key);

	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
	}

	locked_session = (struct session_with_mutex *)apr_hash_get(netconf_sessions_list, session_key, APR_HASH_KEY_STRING);
	if (locked_session != NULL) {
		pthread_mutex_lock(&locked_session->lock);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		if (locked_session->hello_message != NULL) {
			reply = locked_session->hello_message;
		} else {
			reply = create_error("Invalid session identifier.");
		}
		pthread_mutex_unlock(&locked_session->lock);
	} else {
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}


	return reply;
}

void notification_history(time_t eventtime, const char *content)
{
	json_object *notif_history_array = (json_object *) pthread_getspecific(notif_history_key);
	if (notif_history_array == NULL) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, http_server, "No list of notification history found.");
		return;
	}
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, http_server, "Got notification from history %lu.", (long unsigned) eventtime);
	json_object *notif = json_object_new_object();
	if (notif == NULL) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, http_server, "Could not allocate memory for notification (json).");
		return;
	}
	json_object_object_add(notif, "eventtime", json_object_new_int64(eventtime));
	json_object_object_add(notif, "content", json_object_new_string(content));
	json_object_array_add(notif_history_array, notif);
}

json_object *handle_op_ntfgethistory(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	json_object *reply = NULL;
	const char *sid = NULL;
	struct session_with_mutex *locked_session = NULL;
	struct nc_session *temp_session = NULL;
	nc_rpc *rpc = NULL;
	time_t start = 0;
	time_t stop = 0;
	int64_t from, to;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: get notification history, session %s", session_key);

	sid = json_object_get_string(json_object_object_get(request, "session"));
	from = json_object_get_int64(json_object_object_get(request, "from"));
	to = json_object_get_int64(json_object_object_get(request, "to"));

	start = time(NULL) + from;
	stop = time(NULL) + to;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "notification history interval %li %li", (long int) from, (long int) to);

	if (sid == NULL) {
		return create_error("Missing session parameter.");
	}

	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		return NULL;
	}

	locked_session = (struct session_with_mutex *)apr_hash_get(netconf_sessions_list, session_key, APR_HASH_KEY_STRING);
	if (locked_session != NULL) {
		pthread_mutex_lock(&locked_session->lock);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "creating temporal NC session.");
		temp_session = nc_session_connect_channel(locked_session->session, NULL);
		if (temp_session != NULL) {
			rpc = nc_rpc_subscribe(NULL /* stream */, NULL /* filter */, &start, &stop);
			if (rpc == NULL) {
				pthread_mutex_unlock(&locked_session->lock);
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "notifications: creating an rpc request failed.");
				return create_error("notifications: creating an rpc request failed.");
			}

			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Send NC subscribe.");
			/** \todo replace with sth like netconf_op(http_server, session_hash, rpc) */
			json_object *res = netconf_unlocked_op(server, temp_session, rpc);
			if (res != NULL) {
				pthread_mutex_unlock(&locked_session->lock);
				ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Subscription RPC failed.");
				return res;
			}
			rpc = NULL; /* just note that rpc is already freed by send_recv_process() */

			pthread_mutex_unlock(&locked_session->lock);
			pthread_mutex_lock(&ntf_history_lock);
			json_object *notif_history_array = json_object_new_array();
			if (pthread_setspecific(notif_history_key, notif_history_array) != 0) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, http_server, "notif_history: cannot set thread-specific hash value.");
			}

			ncntf_dispatch_receive(temp_session, notification_history);

			reply = json_object_new_object();
			json_object_object_add(reply, "notifications", notif_history_array);
			//json_object_put(notif_history_array);

			pthread_mutex_unlock(&ntf_history_lock);
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "closing temporal NC session.");
			nc_session_free(temp_session);
		} else {
			pthread_mutex_unlock(&locked_session->lock);
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Get history of notification failed due to channel establishment");
			reply = create_error("Get history of notification was unsuccessful, connection failed.");
		}
	} else {
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}

	return reply;
}

json_object *handle_op_validate(server_rec *server, apr_pool_t *pool, json_object *request, const char *session_key)
{
	json_object *reply = NULL;
	const char *sid = NULL;
	const char *target = NULL;
	const char *url = NULL;
	struct session_with_mutex *locked_session = NULL;
	nc_rpc *rpc = NULL;
	NC_DATASTORE target_ds;

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: validate datastore, session %s", session_key);

	sid = json_object_get_string(json_object_object_get(request, "session"));
	target = json_object_get_string(json_object_object_get(request, "target"));
	url = json_object_get_string(json_object_object_get(request, "url"));


	if ((sid == NULL) || (target == NULL)) {
		return create_error("Missing session parameter.");
	}

	if (pthread_rwlock_rdlock(&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		return NULL;
	}

	locked_session = (struct session_with_mutex *)apr_hash_get(netconf_sessions_list, session_key, APR_HASH_KEY_STRING);
	if (locked_session != NULL) {
		pthread_mutex_lock(&locked_session->lock);
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		/* validation */
		target_ds = parse_datastore(target);
		if (target_ds == NC_DATASTORE_URL) {
			if (url != NULL) {
				rpc = nc_rpc_validate(target_ds, url);
			}
		} else if ((target_ds == NC_DATASTORE_RUNNING) || (target_ds == NC_DATASTORE_STARTUP)
			|| (target_ds == NC_DATASTORE_CANDIDATE)) {
			rpc = nc_rpc_validate(target_ds);
		}
		if (rpc == NULL) {
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: creating rpc request failed");
			reply = create_error("Creation of RPC request failed.");
			pthread_mutex_unlock(&locked_session->lock);
			return reply;
		}

		if ((reply = netconf_op(server, session_key, rpc, NULL)) == NULL) {
			reply = json_object_new_object();
			json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
		}
		nc_rpc_free (rpc);

		pthread_mutex_unlock(&locked_session->lock);
	} else {
		if (pthread_rwlock_unlock(&session_lock) != 0) {
			ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
		}
		reply = create_error("Invalid session identifier.");
	}

	return reply;
}

void * thread_routine (void * arg)
{
	void * retval = NULL;
	struct pollfd fds;
	json_object *request = NULL;
	json_object *reply = NULL;
	int operation;
	int status = 0;
	const char *msgtext;
	const char *session_key;
	const char *target = NULL;
	const char *source = NULL;
	const char *url = NULL;
	NC_DATASTORE ds_type_t = -1;
	NC_DATASTORE ds_type_s = -1;
	char *chunked_out_msg = NULL;
	apr_pool_t * pool = ((struct pass_to_thread*)arg)->pool;
	server_rec * server = ((struct pass_to_thread*)arg)->server;
	int client = ((struct pass_to_thread*)arg)->client;

	char *buffer = NULL;

	while (!isterminated) {
		fds.fd = client;
		fds.events = POLLIN;
		fds.revents = 0;

		status = poll(&fds, 1, 1000);

		if (status == 0 || (status == -1 && (errno == EAGAIN || (errno == EINTR && isterminated == 0)))) {
			/* poll was interrupted - check if the isterminated is set and if not, try poll again */
			//ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "poll interrupted");
			continue;
		} else if (status < 0) {
			/* 0:  poll time outed
			 *     close socket and ignore this request from the client, it can try it again
			 * -1: poll failed
			 *     something wrong happend, close this socket and wait for another request
			 */
			//ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "poll failed, status %d(%d: %s)", status, errno, strerror(errno));
			close(client);
			break;
		}
		/* status > 0 */

		/* check the status of the socket */

		/* if nothing to read and POLLHUP (EOF) or POLLERR set */
		if ((fds.revents & POLLHUP) || (fds.revents & POLLERR)) {
			/* close client's socket (it's probably already closed by client */
			//ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "socket error (%d)", fds.revents);
			close(client);
			break;
		}

		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Get framed message...");
		buffer = get_framed_message(server, client);

		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Check read buffer.");
		if (buffer != NULL) {
			enum json_tokener_error jerr;
			request = json_tokener_parse_verbose(buffer, &jerr);
			if (jerr != json_tokener_success) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "JSON parsing error");
				continue;
			}
			operation = json_object_get_int(json_object_object_get(request, "type"));

			session_key = json_object_get_string(json_object_object_get(request, "session"));
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "operation %d session_key %s.", operation, session_key);
			/* DO NOT FREE session_key HERE, IT IS PART OF REQUEST */
			if (operation != MSG_CONNECT && session_key == NULL) {
				reply = create_error("Missing session specification.");
				msgtext = json_object_to_json_string(reply);
				send(client, msgtext, strlen(msgtext) + 1, 0);
				json_object_put(reply);
				/* there is some stupid client, so close the connection to give a chance to some other client */
				close(client);
				break;
			}

			/* null global JSON error-reply */
			err_reply = NULL;

			/* prepare reply envelope */
			reply = NULL;

			/* process required operation */
			switch (operation) {
			case MSG_CONNECT:
				reply = handle_op_connect(server, pool, request);
				break;
			case MSG_GET:
				reply = handle_op_get(server, pool, request, session_key);
				break;
			case MSG_GETCONFIG:
				reply = handle_op_getconfig(server, pool, request, session_key);
				break;
			case MSG_GETSCHEMA:
				reply = handle_op_getschema(server, pool, request, session_key);
				break;
			case MSG_EDITCONFIG:
				reply = handle_op_editconfig(server, pool, request, session_key);
				break;
			case MSG_COPYCONFIG:
				reply = handle_op_copyconfig(server, pool, request, session_key);
				break;

			case MSG_DELETECONFIG:
			case MSG_LOCK:
			case MSG_UNLOCK:
				/* get parameters */
				if ((target = json_object_get_string(json_object_object_get(request, "target"))) != NULL) {
					ds_type_t = parse_datastore(target);
				}
				if ((source = json_object_get_string(json_object_object_get(request, "source"))) != NULL) {
					ds_type_s = parse_datastore(source);
				}

				if (ds_type_t == -1) {
					reply = create_error("Invalid target repository type requested.");
					break;
				}
				switch(operation) {
				case MSG_DELETECONFIG:
					ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: delete-config (session %s)", session_key);
					url = json_object_get_string(json_object_object_get(request, "url"));
					reply = netconf_deleteconfig(server, session_key, ds_type_t, url);
					break;
				case MSG_LOCK:
					ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: lock (session %s)", session_key);
					reply = netconf_lock(server, session_key, ds_type_t);
					break;
				case MSG_UNLOCK:
					ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Request: unlock (session %s)", session_key);
					reply = netconf_unlock(server, session_key, ds_type_t);
					break;
				default:
					reply = create_error("Internal: Unknown request type.");
					break;
				}

				if (reply == NULL) {
					if (err_reply == NULL) {
						/** \todo more clever error message wanted */
						reply = json_object_new_object();
						json_object_object_add(reply, "type", json_object_new_int(REPLY_OK));
					} else {
						/* use filled err_reply from libnetconf's callback */
						reply = err_reply;
					}
				}
				break;
			case MSG_KILL:
				reply = handle_op_kill(server, pool, request, session_key);
				break;
			case MSG_DISCONNECT:
				reply = handle_op_disconnect(server, pool, request, session_key);
				break;
			case MSG_RELOADHELLO:
				reply = handle_op_reloadhello(server, pool, request, session_key);
				break;
			case MSG_INFO:
				reply = handle_op_info(server, pool, request, session_key);
				break;
			case MSG_GENERIC:
				reply = handle_op_generic(server, pool, request, session_key);
				break;
			case MSG_NTF_GETHISTORY:
				reply = handle_op_ntfgethistory(server, pool, request, session_key);
				break;
			case MSG_VALIDATE:
				reply = handle_op_validate(server, pool, request, session_key);
				break;
			default:
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Unknown mod_netconf operation requested (%d)", operation);
				reply = create_error("Operation not supported.");
				break;
			}
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Clean request json object.");
			json_object_put(request);

			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Send reply json object.");
			/* send reply to caller */
			if (reply != NULL) {
				msgtext = json_object_to_json_string(reply);
				if (asprintf (&chunked_out_msg, "\n#%d\n%s\n##\n", (int)strlen(msgtext), msgtext) == -1) {
					if (buffer != NULL) {
						free(buffer);
						buffer = NULL;
					}
					break;
				}
				ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Send framed reply json object.");
				send(client, chunked_out_msg, strlen(chunked_out_msg) + 1, 0);
				ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Clean reply json object.");
				json_object_put(reply);
				ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Clean message buffer.");
				free(chunked_out_msg);
				chunked_out_msg = NULL;
				if (buffer != NULL) {
					free(buffer);
					buffer = NULL;
				}
			} else {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Reply is NULL, shouldn't be...");
				break;
			}
		}
	}
	free (arg);

	return retval;
}

/**
 * \brief Close all open NETCONF sessions.
 *
 * During termination of mod_netconf, it is useful to close all remaining
 * sessions. This function iterates over the list of sessions and close them
 * all.
 *
 * \param[in] server  pointer to server_rec for logging
 * \param[in] p  apr pool needed for hash table iterating
 * \param[in] ht  hash table of session_with_mutex structs
 */
static void close_all_nc_sessions(server_rec* server, apr_pool_t *p)
{
	apr_hash_index_t *hi;
	void *val = NULL;
	struct session_with_mutex *swm = NULL;
	const char *hashed_key = NULL;
	apr_ssize_t hashed_key_length;
	int ret;

	/* get exclusive access to sessions_list (conns) */
	if ((ret = pthread_rwlock_wrlock (&session_lock)) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock: %d (%s)", ret, strerror(ret));
		return;
	}
	for (hi = apr_hash_first(p, netconf_sessions_list); hi; hi = apr_hash_next(hi)) {
		apr_hash_this(hi, (const void **) &hashed_key, &hashed_key_length, &val);
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Closing NETCONF session (%s).", hashed_key);
		swm = (struct session_with_mutex *) val;
		if (swm != NULL) {
			apr_hash_set(netconf_sessions_list, hashed_key, APR_HASH_KEY_STRING, NULL);
			pthread_mutex_unlock(&swm->lock);

			/* close_and_free_session handles locking on its own */
			close_and_free_session(server, swm);
		}
	}
	/* get exclusive access to sessions_list (conns) */
	if (pthread_rwlock_unlock (&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
	}
}

static void check_timeout_and_close(server_rec* server, apr_pool_t *p)
{
	apr_hash_index_t *hi;
	void *val = NULL;
	struct nc_session *ns = NULL;
	struct session_with_mutex *swm = NULL;
	const char *hashed_key = NULL;
	apr_ssize_t hashed_key_length;
	apr_time_t current_time = apr_time_now();
	int ret;

	/* get exclusive access to sessions_list (conns) */
	if ((ret = pthread_rwlock_wrlock (&session_lock)) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while locking rwlock: %d (%s)", ret, strerror(ret));
		return;
	}
	for (hi = apr_hash_first(p, netconf_sessions_list); hi; hi = apr_hash_next(hi)) {
		apr_hash_this(hi, (const void **) &hashed_key, &hashed_key_length, &val);
		swm = (struct session_with_mutex *) val;
		if (swm == NULL) {
			continue;
		}
		ns = swm->session;
		if (ns == NULL) {
			continue;
		}
		pthread_mutex_lock(&swm->lock);
		if ((current_time - swm->last_activity) > apr_time_from_sec(ACTIVITY_TIMEOUT)) {
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Closing NETCONF session (%s).", hashed_key);
			/* remove session from the active sessions list */
			apr_hash_set(netconf_sessions_list, hashed_key, APR_HASH_KEY_STRING, NULL);
			pthread_mutex_unlock(&swm->lock);

			/* close_and_free_session handles locking on its own */
			close_and_free_session(server, swm);
		} else {
			pthread_mutex_unlock(&swm->lock);
		}
	}
	/* get exclusive access to sessions_list (conns) */
	if (pthread_rwlock_unlock (&session_lock) != 0) {
		ap_log_error (APLOG_MARK, APLOG_DEBUG, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
	}
}


/**
 * This is actually implementation of NETCONF client
 * - requests are received from UNIX socket in the predefined format
 * - results are replied through the same way
 * - the daemon run as a separate process, but it is started and stopped
 *   automatically by Apache.
 *
 */
static void forked_proc(apr_pool_t * pool, server_rec * server)
{
	struct timeval tv;
	struct sockaddr_un local, remote;
	int lsock, client, ret, i, pthread_count = 0;
	unsigned int olds = 0, timediff = 0;
	socklen_t len;
	mod_netconf_cfg *cfg;
	struct pass_to_thread * arg;
	pthread_t * ptids = calloc (1,sizeof(pthread_t));
	struct timespec maxtime;
	pthread_rwlockattr_t lock_attrs;
	#ifdef WITH_NOTIFICATIONS
	char use_notifications = 0;
	#endif

	http_server = server;

	/* wait at most 5 seconds for every thread to terminate */
	maxtime.tv_sec = 5;
	maxtime.tv_nsec = 0;

	/* change uid and gid of process for security reasons */
	unixd_setup_child();

	cfg = ap_get_module_config(server->module_config, &netconf_module);
	if (cfg == NULL) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Getting mod_netconf configuration failed");
		return;
	}

	/* create listening UNIX socket to accept incoming connections */
	if ((lsock = socket(PF_UNIX, SOCK_STREAM, 0)) == -1) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Creating socket failed (%s)", strerror(errno));
		return;
	}

	local.sun_family = AF_UNIX;
	strncpy(local.sun_path, cfg->sockname, sizeof(local.sun_path));
	unlink(local.sun_path);
	len = offsetof(struct sockaddr_un, sun_path) + strlen(local.sun_path);

	if (bind(lsock, (struct sockaddr *) &local, len) == -1) {
		if (errno == EADDRINUSE) {
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "mod_netconf socket address already in use");
			close(lsock);
			exit(0);
		}
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Binding socket failed (%s)", strerror(errno));
		close(lsock);
		return;
	}

	if (listen(lsock, MAX_SOCKET_CL) == -1) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Setting up listen socket failed (%s)", strerror(errno));
		close(lsock);
		return;
	}

	/* prepare internal lists */
	netconf_sessions_list = apr_hash_make(pool);

	#ifdef WITH_NOTIFICATIONS
	if (notification_init(pool, server) == -1) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "libwebsockets initialization failed");
		use_notifications = 0;
	} else {
		use_notifications = 1;
	}
	#endif

	/* setup libnetconf's callbacks */
	nc_verbosity(NC_VERB_DEBUG);
	clb_print_server = server;
	nc_callback_print(clb_print);
	nc_callback_ssh_host_authenticity_check(netconf_callback_ssh_hostkey_check);
	nc_callback_sshauth_interactive(netconf_callback_sshauth_interactive);
	nc_callback_sshauth_password(netconf_callback_sshauth_password);
	nc_callback_error_reply(netconf_callback_error_process);

	/* disable publickey authentication */
	nc_ssh_pref(NC_SSH_AUTH_PUBLIC_KEYS, -1);

	/* create mutex protecting session list */
	pthread_rwlockattr_init(&lock_attrs);
	/* rwlock is shared only with threads in this process */
	pthread_rwlockattr_setpshared(&lock_attrs, PTHREAD_PROCESS_PRIVATE);
	/* create rw lock */
	if (pthread_rwlock_init(&session_lock, &lock_attrs) != 0) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Initialization of mutex failed: %d (%s)", errno, strerror(errno));
		close (lsock);
		return;
	}
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, http_server, "init of notif_history_key.");
	if (pthread_key_create(&notif_history_key, NULL) != 0) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, http_server, "init of notif_history_key failed");
	}

	fcntl(lsock, F_SETFL, fcntl(lsock, F_GETFL, 0) | O_NONBLOCK);
	while (isterminated == 0) {
		gettimeofday(&tv, NULL);
		timediff = (unsigned int)tv.tv_sec - olds;
		#ifdef WITH_NOTIFICATIONS
		if (timediff > 60) {
			ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "handling notifications");
		}
		if (use_notifications == 1) {
			notification_handle();
		}
		#endif
		if (timediff > ACTIVITY_CHECK_INTERVAL) {
			check_timeout_and_close(server, pool);
		}

		/* open incoming connection if any */
		len = sizeof(remote);
		if (((unsigned int)tv.tv_sec - olds) > 60) {
			ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, "accepting another client");
			olds = tv.tv_sec;
		}
		client = accept(lsock, (struct sockaddr *) &remote, &len);
		if (client == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			apr_sleep(SLEEP_TIME);
			continue;
		} else if (client == -1 && (errno == EINTR)) {
			continue;
		} else if (client == -1) {
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Accepting mod_netconf client connection failed (%s)", strerror(errno));
			continue;
		}

		/* set client's socket as non-blocking */
		//fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);

		arg = malloc (sizeof(struct pass_to_thread));
		arg->client = client;
		arg->pool = pool;
		arg->server = server;
		arg->netconf_sessions_list = netconf_sessions_list;

		/* start new thread. It will serve this particular request and then terminate */
		if ((ret = pthread_create (&ptids[pthread_count], NULL, thread_routine, (void*)arg)) != 0) {
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Creating POSIX thread failed: %d\n", ret);
		} else {
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Thread %lu created", ptids[pthread_count]);
			pthread_count++;
			ptids = realloc (ptids, sizeof(pthread_t)*(pthread_count+1));
			ptids[pthread_count] = 0;
		}

		/* check if some thread already terminated, free some resources by joining it */
		for (i=0; i<pthread_count; i++) {
			if (pthread_tryjoin_np (ptids[i], (void**)&arg) == 0) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Thread %lu joined with retval %p", ptids[i], arg);
				pthread_count--;
				if (pthread_count > 0) {
					/* place last Thread ID on the place of joined one */
					ptids[i] = ptids[pthread_count];
				}
			}
		}
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Running %d threads", pthread_count);
	}

	ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf terminating...");
	/* join all threads */
	for (i=0; i<pthread_count; i++) {
		pthread_timedjoin_np (ptids[i], (void**)&arg, &maxtime);
	}
	free (ptids);

	close(lsock);

	#ifdef WITH_NOTIFICATIONS
	notification_close();
	#endif

	/* close all NETCONF sessions */
	close_all_nc_sessions(server, pool);

	/* destroy rwlock */
	pthread_rwlock_destroy(&session_lock);
	pthread_rwlockattr_destroy(&lock_attrs);

	ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Exiting from the mod_netconf daemon");

	exit(APR_SUCCESS);
}

static void *mod_netconf_create_conf(apr_pool_t * pool, server_rec * s)
{
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "Init netconf module config");

	mod_netconf_cfg *config = apr_pcalloc(pool, sizeof(mod_netconf_cfg));
	apr_pool_create(&config->pool, pool);
	config->forkproc = NULL;
	config->sockname = SOCKET_FILENAME;

	return (void *)config;
}

static int mod_netconf_master_init(apr_pool_t * pconf, apr_pool_t * ptemp,
		  apr_pool_t * plog, server_rec * s)
{
	mod_netconf_cfg *config;
	apr_status_t res;

	/* These two help ensure that we only init once. */
	void *data = NULL;
	const char *userdata_key = "netconf_ipc_init";

	/*
	 * The following checks if this routine has been called before.
	 * This is necessary because the parent process gets initialized
	 * a couple of times as the server starts up.
	 */
	apr_pool_userdata_get(&data, userdata_key, s->process->pool);
	if (!data) {
		apr_pool_userdata_set((const void *)1, userdata_key, apr_pool_cleanup_null, s->process->pool);
		return (OK);
	}

	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "creating mod_netconf daemon");
	config = ap_get_module_config(s->module_config, &netconf_module);

	if (config && config->forkproc == NULL) {
		config->forkproc = apr_pcalloc(config->pool, sizeof(apr_proc_t));
		res = apr_proc_fork(config->forkproc, config->pool);
		switch (res) {
		case APR_INCHILD:
			/* set signal handler */
			apr_signal_init(config->pool);
			apr_signal(SIGTERM, signal_handler);

			/* log start of the separated NETCONF communication process */
			ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, "mod_netconf daemon started (PID %d)", getpid());

			/* start main loop providing NETCONF communication */
			forked_proc(config->pool, s);

			/* I never should be here, wtf?!? */
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "mod_netconf daemon unexpectedly stopped");
			exit(APR_EGENERAL);
			break;
		case APR_INPARENT:
			/* register child to be killed (SIGTERM) when the module config's pool dies */
			apr_pool_note_subprocess(config->pool, config->forkproc, APR_KILL_AFTER_TIMEOUT);
			break;
		default:
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "apr_proc_fork() failed");
			break;
		}
	} else {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "mod_netconf misses configuration structure");
	}

	return OK;
}

/**
 * Register module hooks
 */
static void mod_netconf_register_hooks(apr_pool_t * p)
{
	ap_hook_post_config(mod_netconf_master_init, NULL, NULL, APR_HOOK_LAST);
}

static const char* cfg_set_socket_path(cmd_parms* cmd, void* cfg, const char* arg)
{
	((mod_netconf_cfg*)cfg)->sockname = apr_pstrdup(cmd->pool, arg);
	return NULL;
}

static const command_rec netconf_cmds[] = {
		AP_INIT_TAKE1("NetconfSocket", cfg_set_socket_path, NULL, OR_ALL, "UNIX socket path for mod_netconf communication."),
		{NULL}
};

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA netconf_module = {
	STANDARD20_MODULE_STUFF,
	NULL,			/* create per-dir    config structures */
	NULL,			/* merge  per-dir    config structures */
	mod_netconf_create_conf,	/* create per-server config structures */
	NULL,			/* merge  per-server config structures */
	netconf_cmds,			/* table of config file commands       */
	mod_netconf_register_hooks	/* register hooks                      */
};

