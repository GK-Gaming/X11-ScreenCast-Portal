#include <spa/param/video/format-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/param/dict-utils.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#include <drmtap.h>

#include <sys/types.h>
#include <sys/stat.h>

#define MAX_SESSIONS 20

enum cursor_modes {
  HIDDEN = 1,
  EMBEDDED = 2,
  METADATA = 4,
};

enum ipc_id {
	IPC_START_CAP_IN,
	IPC_START_CAP_OUT,
	IPC_STOP_CAP_IN,
	IPC_STOP_CAP_OUT
};

struct ipc_start_capture_input {
	char device[64];
	drmtap_display display;
	enum cursor_modes cursor_mode;
};
struct ipc_start_capture_output {
	int ret;
	uint32_t node_id;
	uint64_t pipewire_serial;
};


struct ipc_stop_capture_input {
	uint32_t node_id;
};

struct portal_ipc {
	enum ipc_id id;
	union {
		struct ipc_start_capture_input start_capture_input;
		struct ipc_start_capture_output start_capture_output;
		struct ipc_stop_capture_input stop_capture_input;
	};
};

