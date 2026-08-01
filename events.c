
#define _GNU_SOURCE
#include "both.h"


drmtap_ctx* displays_ctx;
drmtap_display displays[8];
int n_displays;


#include <poll.h>

#define POLLS 1

enum event_loop_fd {
	EVENT_LOOP_DBUS,
};

struct pollfd pollfds[MAX_SESSIONS + POLLS] = {
	[EVENT_LOOP_DBUS] = {0}, // Filled in later
};

#include <systemd/sd-bus.h>

struct portal_dbus_session {
	bool active;
	bool metadata_cursor;
	char* session_handle;
	sd_bus_slot* slot;
	void* state;
	int sess_id;
	int portal_sess_id;
	uint32_t node_id;
	char* device;
	drmtap_display* display;
};

struct portal_session {
	int connections;
	int sess_id;
	int ipc_fd_write;
	int ipc_fd_read;
	pid_t pid;
	char* device;
	drmtap_display* display;
	void* state;
};

struct portal_state {
	// xdpw
	uint32_t refcount;
	bool initialized;
	bool first_cap;
	sd_bus *bus;
	drmtap_ctx *ctx;
	int timer_poll_fd;
	struct spa_list timers;
	bool dma_buf;
	void* datass;
	int datass_size;
	
	uint32_t source_types;
	uint32_t cursor_modes;
	uint32_t screencast_version;
	
	struct portal_session session[MAX_SESSIONS];
	int sessions;
	struct portal_dbus_session dbus_session[MAX_SESSIONS];
	int dbus_sessions;
};

struct portal_ipc ipc_buffer;

struct ipc_start_capture_output start_capture(struct portal_dbus_session* dbus_sess, struct portal_session* sess) {
	struct ipc_start_capture_input input;
	struct ipc_start_capture_output output;
	
	if (sess->ipc_fd_read == -1) {
		static char fifo_name[60];
		pid_t pid;
		
		snprintf(fifo_name, 60, "/tmp/x11-portal-fifo-%i-0", sess->sess_id);
		unlink(fifo_name);
		if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) goto err;
		snprintf(fifo_name, 60, "/tmp/x11-portal-fifo-%i-1", sess->sess_id);
		unlink(fifo_name);
		if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) goto err;
		
		pid = vfork();
		if (pid == 0) {
			static char fifo_name_pass[60];
			static char* argv[] = {"xdg-desktop-portal-x11-session", fifo_name_pass, NULL};
			snprintf(fifo_name_pass, 60, "/tmp/x11-portal-fifo-%i", sess->sess_id);
			if (execv("xdg-desktop-portal-x11-session", argv)) {
				printf("failed to start xdg-desktop-portal-x11-session!!\nEXECV FAILED: (%s)\n", strerror(errno));
				kill(0, SIGTERM);
			}
		}
		if (pid == -1) {
			printf("failed to start xdg-desktop-portal-x11-session!!\nVFORK FAILED: (%s)\n", strerror(errno));
			exit(1);
		}
		
		sess->pid = pid;
		
		snprintf(fifo_name, 60, "/tmp/x11-portal-fifo-%i-0", sess->sess_id);
		sess->ipc_fd_read = open(fifo_name, O_RDONLY);
		if (sess->ipc_fd_read == -1) goto err;
		snprintf(fifo_name, 60, "/tmp/x11-portal-fifo-%i-1", sess->sess_id);
		sess->ipc_fd_write = open(fifo_name, O_WRONLY);
		if (sess->ipc_fd_write == -1) goto err;
		
		pollfds[sess->sess_id + POLLS].fd = sess->ipc_fd_read;
	}
	
	ipc_buffer.id = IPC_START_CAP_IN;
	strncpy(ipc_buffer.start_capture_input.device, sess->device, 64);
	ipc_buffer.start_capture_input.display = *sess->display;
	ipc_buffer.start_capture_input.metadata_cursor = dbus_sess->metadata_cursor;
	
	if (write(sess->ipc_fd_write, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer)) goto err;
	if (read(sess->ipc_fd_read, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer) || ipc_buffer.id != IPC_START_CAP_OUT) goto err;
	
	return ipc_buffer.start_capture_output;
	
	err:;
	
	ipc_buffer.start_capture_output.ret = -1;
	return ipc_buffer.start_capture_output;
	
	ipc_err:;
	
	printf("IPC IPC_START_CAP_IN FAILED!\n");
	goto err;
}

void stop_capture(struct portal_session* sess, uint32_t node_id) {
	ipc_buffer.id = IPC_STOP_CAP_IN;
	ipc_buffer.stop_capture_input.node_id = node_id;
	
	if (!sess->connections) return;
	
	if (sess->ipc_fd_write != -1 && sess->ipc_fd_read != -1) {
		if (write(sess->ipc_fd_write, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer)) goto ipc_err;
		if (read(sess->ipc_fd_read, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer)) goto ipc_err;
		
		if (!--sess->connections) {
			waitpid(sess->pid, NULL, 0);
			
			close(sess->ipc_fd_write);
			close(sess->ipc_fd_read);
			((struct portal_state*) (sess->state))->sessions--;
			if (sess->device) {
				free(sess->device);
				sess->device = NULL;
			}
			
			pollfds[sess->sess_id + POLLS].fd = -1;
		}
	}
	
	return;
	
	ipc_err:;
	printf("IPC IPC_STOP_CAP_IN FAILED!\n");
	exit(1);
}

// DBUS

enum LOGLEVEL { QUIET, ERROR, WARN, INFO, DEBUG, TRACE };
void logprint(enum LOGLEVEL level, char *msg, ...) {
	va_list args;
	va_start(args, msg);
	vprintf(msg, args);
	va_end(args);
	printf("\n");
}


static const char service_name[] = "org.freedesktop.impl.portal.desktop.x11";
static const char object_path2[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.Session";
static const char interface_name1[] = "org.freedesktop.impl.portal.Request";
static const char interface_name2[] = "org.freedesktop.impl.portal.ScreenCast";


// DBUS


static int handle_name_lost(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	logprint(INFO, "dbus: lost name, closing connection");
	sd_bus_close(sd_bus_message_get_bus(m));
	return 1;
}


struct xdpw_request {
	sd_bus_slot *slot;
};

enum {
	PORTAL_RESPONSE_SUCCESS = 0,
	PORTAL_RESPONSE_CANCELLED = 1,
	PORTAL_RESPONSE_ENDED = 2
};

static int method_close2(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error);

static const sd_bus_vtable request_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_close2, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

struct xdpw_request *xdpw_request_create(sd_bus *bus, const char *object_path) {
	struct xdpw_request *req = calloc(1, sizeof(struct xdpw_request));

	if (sd_bus_add_object_vtable(bus, &req->slot, object_path, interface_name1,
			request_vtable, NULL) < 0) {
		free(req);
		logprint(ERROR, "dbus: sd_bus_add_object_vtable failed: %s",
			strerror(-errno));
		return NULL;
	}

	return req;
}

void xdpw_request_destroy(struct xdpw_request *req) {
	if (req == NULL) {
		return;
	}
	sd_bus_slot_unref(req->slot);
	free(req);
}


static int method_close2(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_request *req = data;
	int ret = 0;
	logprint(INFO, "dbus: request closed");

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);

	xdpw_request_destroy(req);

	return 0;
}

static int method_close(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	int ret = 0;
	struct portal_dbus_session *sess = data;
	struct portal_state *state = sess->state;
	logprint(INFO, "dbus: session closed");
	
	if (!sess->active) return -1;
	sess->active = false;

	if (sess->session_handle) {
		free(sess->session_handle);
		sess->session_handle = NULL;
	}
	
	if (sess->device) {
		free(sess->device);
		sess->device = NULL;
	}

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);

	state->dbus_sessions--;
	// Pointers passed to userdata functions shouldnt move
#if 0
	if (sess->sess_id != state->dbus_sessions) {
		state->dbus_session[sess->sess_id] = state->dbus_session[state->dbus_sessions];
		state->dbus_session[sess->sess_id].sess_id = sess->sess_id;
	}
#endif
	
	stop_capture(&state->session[sess->portal_sess_id], sess->node_id);
	

	return 0;
}

static const sd_bus_vtable session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static int method_screencast_create_session(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct portal_state *state = data;

	int ret = 0;

	logprint(INFO, "dbus: create session method invoked");

	char *request_handle, *session_handle, *app_id;
	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	logprint(INFO, "dbus: request_handle: %s", request_handle);
	logprint(INFO, "dbus: session_handle: %s", session_handle);
	logprint(INFO, "dbus: app_id: %s", app_id);

	char *key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		if (strcmp(key, "session_handle_token") == 0) {
			char *token;
			sd_bus_message_read(msg, "v", "s", &token);
			logprint(INFO, "dbus: option token: %s", token);
		} else {
			logprint(WARN, "dbus: unknown option: %s", key);
			sd_bus_message_skip(msg, "v");
		}

		innerRet = sd_bus_message_exit_container(msg);
		if (innerRet < 0) {
			return innerRet;
		}
	}
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	struct xdpw_request *req =
		xdpw_request_create(sd_bus_message_get_bus(msg), request_handle);
	if (req == NULL) {
		return -ENOMEM;
	}
	
	int sess_id;
	for (sess_id = 0; sess_id  < MAX_SESSIONS; sess_id ++)
		if (!state->dbus_session[sess_id].session_handle)
			break;
	
	struct portal_dbus_session* sess = state->dbus_session + sess_id;
	sess->active = true;
	sess->state = state;
	sess->sess_id = sess_id;
	sess->portal_sess_id = -1;
	sess->session_handle = strdup(session_handle);
	
	if (sd_bus_add_object_vtable(state->bus, &sess->slot, session_handle, interface_name,
			session_vtable, sess) < 0) {
		logprint(ERROR, "dbus: sd_bus_add_object_vtable failed: %s",
			strerror(-errno));
		return ENOMEM;
	}

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);
	return 0;
}


enum cursor_modes {
  HIDDEN = 1,
  EMBEDDED = 2,
  METADATA = 4,
};

enum source_types {
  MONITOR = 1,
  WINDOW = 2,
};
enum persist_modes {
  PERSIST_NONE = 0,
  PERSIST_TRANSIENT = 1,
  PERSIST_PERMANENT = 2,
};


int cursor_mode = METADATA;
int source_type = MONITOR;
int persist_mode = PERSIST_NONE;


struct xdpw_screencast_restore_data {
	uint32_t version;
	const char *output_name;
};

static int method_screencast_select_sources(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct portal_state *state = data;

	int ret = 0;
	struct portal_dbus_session *dbus_sess = NULL;
	struct portal_session *sess = NULL;
	sd_bus_message *reply = NULL;

	logprint(INFO, "dbus: select sources method invoked");

	char *request_handle, *session_handle, *app_id;
	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	logprint(INFO, "dbus: request_handle: %s", request_handle);
	logprint(INFO, "dbus: session_handle: %s", session_handle);
	logprint(INFO, "dbus: app_id: %s", app_id);

	dbus_sess = NULL;
	for (int i = 0; i < MAX_SESSIONS; i++) {
		dbus_sess = &state->dbus_session[i];
		if (dbus_sess->active && dbus_sess->session_handle && strcmp(dbus_sess->session_handle, session_handle) == 0) {
				logprint(DEBUG, "dbus: select sources: found matching session %s", dbus_sess->session_handle);
				break;
		}
	}
	if (!dbus_sess) {
		logprint(WARN, "dbus: select sources: no matching session %s found", dbus_sess->session_handle);
		goto error;
	}

	// default to metadata cursor mode if not specified
	cursor_mode = METADATA;
	// default to no persist if not specified
	persist_mode = PERSIST_NONE;

	char *key;
	int innerRet = 0;
	uint32_t type_mask = 0;
	struct xdpw_screencast_restore_data restore_data = {0};
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		if (strcmp(key, "multiple") == 0) {
			int multiple;
			sd_bus_message_read(msg, "v", "b", &multiple);
			logprint(INFO, "dbus: option multiple: %d", multiple);
		} else if (strcmp(key, "types") == 0) {
			sd_bus_message_read(msg, "v", "u", &type_mask);
			if (!(type_mask & (MONITOR | WINDOW))) {
				logprint(INFO, "dbus: non-monitor non-window cast requested, not replying");
				return -1;
			}
			logprint(INFO, "dbus: option types: %x", type_mask);
		} else if (strcmp(key, "cursor_mode") == 0) {
			sd_bus_message_read(msg, "v", "u", &cursor_mode);
			if (cursor_mode & EMBEDDED) {
				logprint(ERROR, "dbus: unsupported cursor mode requested, cancelling");
				goto error;
			}
			logprint(INFO, "dbus: option cursor_mode:%x", cursor_mode);
		} else if (strcmp(key, "restore_data") == 0) {
			logprint(INFO, "dbus: restore data available");
			char *portal_vendor;
			innerRet = sd_bus_message_enter_container(msg, 'v', "(suv)");
			if (innerRet < 0) {
				logprint(ERROR, "dbus: error entering variant");
				return innerRet;
			}
			innerRet = sd_bus_message_enter_container(msg, 'r', "suv");
			if (innerRet < 0) {
				logprint(ERROR, "dbus: error entering struct");
				return innerRet;
			}
			sd_bus_message_read(msg, "s", &portal_vendor);
			if (strcmp(portal_vendor, "wlroots") != 0) {
				logprint(INFO, "dbus: skipping restore_data from another vendor (%s)", portal_vendor);
				sd_bus_message_skip(msg, "uv");
				continue;
			}
			sd_bus_message_read(msg, "u", &restore_data.version);
			if (restore_data.version == 1) {
				innerRet = sd_bus_message_enter_container(msg, 'v', "a{sv}");
				if (innerRet < 0) {
					return innerRet;
				}
				innerRet = sd_bus_message_enter_container(msg, 'a', "{sv}");
				if (innerRet < 0) {
					return innerRet;
				}
				logprint(INFO, "dbus: restoring session from data");
				int rdRet;
				char *rdKey;
				while ((innerRet = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
					rdRet = sd_bus_message_read(msg, "s", &rdKey);
					if (rdRet < 0) {
						return rdRet;
					}
					if (strcmp(rdKey, "output_name") == 0) {
						sd_bus_message_read(msg, "v", "s", &restore_data.output_name);
						logprint(INFO, "dbus: option restore_data.output_name: %s", restore_data.output_name);
					} else {
						logprint(WARN, "dbus: unknown option %s", rdKey);
						sd_bus_message_skip(msg, "v");
					}
					innerRet = sd_bus_message_exit_container(msg); // dictionary
					if (innerRet < 0) {
						return innerRet;
					}
				}
				if (innerRet < 0) {
					return innerRet;
				}
				innerRet = sd_bus_message_exit_container(msg); //array
				if (innerRet < 0) {
					return innerRet;
				}
				innerRet = sd_bus_message_exit_container(msg); //variant
				if (innerRet < 0) {
					return innerRet;
				}
			} else {
				sd_bus_message_skip(msg, "v");
				logprint(ERROR, "Unknown restore_data version: %u", restore_data.version);
			}
			innerRet = sd_bus_message_exit_container(msg); // struct
			if (innerRet < 0) {
				return innerRet;
			}
			innerRet = sd_bus_message_exit_container(msg); // variant
			if (innerRet < 0) {
				return innerRet;
			}
		} else if (strcmp(key, "persist_mode") == 0) {
			sd_bus_message_read(msg, "v", "u", &persist_mode);
			logprint(INFO, "dbus: option persist_mode:%u", persist_mode);
		} else {
			logprint(WARN, "dbus: unknown option %s", key);
			sd_bus_message_skip(msg, "v");
		}

		innerRet = sd_bus_message_exit_container(msg);
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	bool selection_canceled = false; //!setup_target(ctx, sess, restore_data.version > 0 ? &restore_data : NULL, type_mask);

	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	if (selection_canceled) {
		ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_CANCELLED, 0);
	} else {
		ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	
	// ---------------- SELECTION HAPPENS HERE!!
	drmtap_display* target = &displays[0];
	char* device = "";
	
	dbus_sess->metadata_cursor = cursor_mode == METADATA;
	
	dbus_sess->display = target;
	dbus_sess->device = strdup(device);
	
	sd_bus_message_unref(reply);
	return 0;

error:
	if (dbus_sess) {
		sd_bus_emit_signal(sd_bus_slot_get_bus(dbus_sess->slot), dbus_sess->session_handle,
			"org.freedesktop.impl.portal.Session", "Closed", "");
	}

	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_CANCELLED, 0);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_unref(reply);
	return -1;
}
#define XDP_CAST_DATA_VER 1

static int method_screencast_start(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct portal_state *state = data;

	int ret = 0;

	logprint(INFO, "dbus: start method invoked");

	char *request_handle, *session_handle, *app_id, *parent_window;
	ret = sd_bus_message_read(msg, "ooss", &request_handle, &session_handle, &app_id, &parent_window);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	logprint(INFO, "dbus: request_handle: %s", request_handle);
	logprint(INFO, "dbus: session_handle: %s", session_handle);
	logprint(INFO, "dbus: app_id: %s", app_id);
	logprint(INFO, "dbus: parent_window: %s", parent_window);

	char *key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}
		logprint(WARN, "dbus: unknown option: %s", key);
		sd_bus_message_skip(msg, "v");
		innerRet = sd_bus_message_exit_container(msg);
		if (innerRet < 0) {
			return innerRet;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	bool found = false;
	struct portal_dbus_session* dbus_sess;
	struct portal_session* sess = NULL;
	for (int i = 0; i < MAX_SESSIONS; i++) {
		dbus_sess = state->dbus_session + i;
		if (dbus_sess->active && dbus_sess->session_handle && strcmp(dbus_sess->session_handle, session_handle) == 0) {
				logprint(DEBUG, "dbus: start: found matching session %s", dbus_sess->session_handle);
				found = true;
				break;
		}
	}
	if (!found) {
		return -1;
	}
	
	// Associate dbus_session with portal_session
	{
		int p_sess_id;
		for (p_sess_id = 0; p_sess_id < MAX_SESSIONS; p_sess_id++) {
			struct portal_session* t_sess;
			t_sess = &state->session[p_sess_id];
			if (t_sess->connections && t_sess->display->crtc_id == dbus_sess->display->crtc_id && !strcmp(t_sess->device, dbus_sess->device)) {
				sess = t_sess;
				break;
			}
		}
		if (!sess) {
			for (p_sess_id = 0; p_sess_id < MAX_SESSIONS; p_sess_id++)
				if (!state->session[p_sess_id].connections)
					break;
			state->sessions++;
			sess = &state->session[p_sess_id];
			// First start
			sess->ipc_fd_write = -1;
			sess->ipc_fd_read = -1;
		}
		
		sess->connections++;
		
		sess->state = state;
		if (!sess->device)
			sess->device = strdup(dbus_sess->device);
		sess->display = dbus_sess->display;
		
		dbus_sess->portal_sess_id = p_sess_id;
	}
	

	struct ipc_start_capture_output output = start_capture(dbus_sess, sess);
	if (output.ret < 0) {
		return output.ret;
	}
	dbus_sess->node_id = output.node_id;

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "dbus: start: returning node %d", (int)output.node_id);
	ret = sd_bus_message_append(reply, "u", PORTAL_RESPONSE_SUCCESS);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'e', "sv");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "s", "streams");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'v', "a(ua{sv})");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'a', "(ua{sv})");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'r', "ua{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "u", output.node_id);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "{sv}",
		"position", "(ii)", 0, 0);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "{sv}",
		"size", "(ii)", sess->display->width, sess->display->height);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "{sv}", "source_type", "u", MONITOR);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "{sv}",
		"mapping_id", "s", sess->display->name);
	if (ret < 0) {
		return ret;
	}
	if (output.pipewire_serial != 0) {
		ret = sd_bus_message_append(reply, "{sv}",
			"pipewire-serial", "t", output.pipewire_serial);
		if (ret < 0) {
			return ret;
		}
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "{sv}",
		"persist_mode", "u", persist_mode);
	if (ret < 0) {
		return ret;
	}
	if (persist_mode != PERSIST_NONE) {
		struct xdpw_screencast_restore_data restore_data;
		restore_data.output_name = sess->display->name;
		ret = sd_bus_message_append(reply, "{sv}",
			"restore_data", "(suv)",
			"wlroots", XDP_CAST_DATA_VER,
			"a{sv}", 1, "output_name", "s", restore_data.output_name);
		if (ret < 0) {
			return ret;
		}
	}

	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_unref(reply);

	return 0;
}

#define XDP_CAST_PROTO_VER 6

const uint32_t source_types_ = MONITOR;
const uint32_t cursor_modes_ = HIDDEN | METADATA;
const uint32_t screencast_version_ = XDP_CAST_PROTO_VER;

static const sd_bus_vtable screencast_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}",
		method_screencast_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}",
		method_screencast_select_sources, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}",
		method_screencast_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("AvailableSourceTypes", "u", NULL,
		offsetof(struct portal_state, source_types),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("AvailableCursorModes", "u", NULL,
		offsetof(struct portal_state, cursor_modes),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("version", "u", NULL,
		offsetof(struct portal_state, screencast_version),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};


void dbus_basic_setup(struct portal_state *cast) {
	sd_bus *bus = NULL;
	sd_bus_slot *slot = NULL;
	
	int ret =  sd_bus_open_user(&bus);
	
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to connect to user bus: %s", strerror(-ret));
		exit(EXIT_FAILURE);
	}
	logprint(DEBUG, "dbus: connected");
	cast->bus = bus;
	
	
	
	ret = sd_bus_add_object_vtable(bus, NULL, object_path2, interface_name2,
		screencast_vtable, cast);
	if (ret < 0) {
		logprint(ERROR, "dbus: sd_bus_add_object_vtable failed: %s",
			strerror(-ret));
		exit(EXIT_FAILURE);
	}
	
	
	
	uint64_t flags = SD_BUS_NAME_ALLOW_REPLACEMENT;
	if (1) {
		flags |= SD_BUS_NAME_REPLACE_EXISTING;
	}

	ret = sd_bus_request_name(bus, service_name, flags);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to acquire service name: %s", strerror(-ret));
		exit(EXIT_FAILURE);
	}

	const char *unique_name;
	ret = sd_bus_get_unique_name(bus, &unique_name);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to get unique bus name: %s", strerror(-ret));
		exit(EXIT_FAILURE);
	}

	static char match[1024];
	snprintf(match, sizeof(match), "sender='org.freedesktop.DBus',"
		"type='signal',"
		"interface='org.freedesktop.DBus',"
		"member='NameOwnerChanged',"
		"path='/org/freedesktop/DBus',"
		"arg0='%s',"
		"arg1='%s'",
		service_name, unique_name);

	ret = sd_bus_add_match(bus, &slot, match, handle_name_lost, NULL);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to add NameOwnerChanged signal match: %s", strerror(-ret));
		exit(EXIT_FAILURE);
	}
}






int main(int argc, char *argv[]) {
	struct portal_state cast = {0};
	
	drmtap_config cfg = {0};
	cfg.debug = 0;
	displays_ctx = drmtap_open(&cfg);
	
	n_displays = drmtap_list_displays(displays_ctx, displays, 8);
	
	cast.source_types = MONITOR;
	cast.cursor_modes = HIDDEN | METADATA;
	cast.screencast_version = XDP_CAST_PROTO_VER;
	
	dbus_basic_setup(&cast);
	
	uint64_t usec_timeout = 0;
	int ret = sd_bus_get_timeout(cast.bus, &usec_timeout);
	if (ret < 0) {
		logprint(ERROR, "sd_bus_get_timeout failed: %s", strerror(-ret));
		goto error;
	}
	// Convert timestamp from usec to msec.  Value of -1 indicates no
	// timeout, i.e. poll forever.
	int msec_timeout = usec_timeout == UINT64_MAX ? -1 : (int)((usec_timeout + 999) / 1000);
	
	for (int s = 0; s < MAX_SESSIONS; s++) {
		pollfds[s + POLLS].fd = -1;
		pollfds[s + POLLS].events = POLLIN;
	}
	
	while (1) {
		// sd-bus requires that we update FD/events/timeout every time we poll
		pollfds[EVENT_LOOP_DBUS].fd = sd_bus_get_fd(cast.bus);
		if (pollfds[EVENT_LOOP_DBUS].fd < 0) {
			logprint(ERROR, "sd_bus_get_fd failed: %s",
				strerror(-pollfds[EVENT_LOOP_DBUS].fd));
			goto error;
		}
		pollfds[EVENT_LOOP_DBUS].events = sd_bus_get_events(cast.bus);
		if (pollfds[EVENT_LOOP_DBUS].events < 0) {
			logprint(ERROR, "sd_bus_get_events failed: %s",
				strerror(-pollfds[EVENT_LOOP_DBUS].events));
			goto error;
		}
		uint64_t usec_timeout = 0;
		ret = sd_bus_get_timeout(cast.bus, &usec_timeout);
		if (ret < 0) {
			logprint(ERROR, "sd_bus_get_timeout failed: %s", strerror(-ret));
			goto error;
		}
		// Convert timestamp from usec to msec.  Value of -1 indicates no
		// timeout, i.e. poll forever.
		int msec_timeout = usec_timeout == UINT64_MAX ? -1 : (int)((usec_timeout + 999) / 1000);
		
		
		int ret = poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), msec_timeout);
		if (ret < 0) {
			logprint(ERROR, "poll failed: %s", strerror(errno));
			goto error;
		}
		
		if (pollfds[EVENT_LOOP_DBUS].revents & POLLHUP) {
			logprint(INFO, "event-loop: disconnected from dbus");
			break;
		}
		
		// sd-bus sets events=0 if it already has messages to process
		if (pollfds[EVENT_LOOP_DBUS].revents ||
				pollfds[EVENT_LOOP_DBUS].events == 0) {
			logprint(TRACE, "event-loop: got dbus event");
			do {
				ret = sd_bus_process(cast.bus, NULL);
			} while (ret > 0);
			if (ret < 0) {
				logprint(ERROR, "sd_bus_process failed: %s", strerror(-ret));
				goto error;
			}
		}
		
		for (int s = 0; s < MAX_SESSIONS; s++) {
			if (pollfds[s + POLLS].revents & POLLHUP) {
				logprint(INFO, "event-loop: disconnected from session %i", s);
			}
			if (pollfds[s + POLLS].revents & POLLIN) {
				logprint(TRACE, "event-loop: got session %i event", s);
				
				switch ((int)ipc_buffer.id) {
					// Nothing yet
				}
			}
		}
		
		sd_bus_flush(cast.bus);
	}
	
	error:;
	
	drmtap_close(displays_ctx);
	
	return 0;
}

