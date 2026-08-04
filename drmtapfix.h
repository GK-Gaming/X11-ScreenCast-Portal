#include <sys/wait.h>
#include <sys/socket.h>

#include "libdrmtap/src/wire.h"
#include "libdrmtap/src/drmtap_internal.h"

// Kill the helper process
void drmtap_helper_stop(drmtap_ctx *ctx) {
    if (ctx->helper_fd >= 0) {
        /* Send a full-size quit command (best effort). The helper reads one whole
         * fixed-size command frame per iteration, so a 1-byte quit would be a
         * short read that the helper now rejects; send the complete struct. */
        helper_cmd_grab_t hcmd = wire_cmd(CMD_QUIT, 0);
        ssize_t n = send(ctx->helper_fd, &hcmd, sizeof(hcmd), MSG_NOSIGNAL);
        (void)n;

        close(ctx->helper_fd);
        ctx->helper_fd = -1;
    }

    if (ctx->helper_pid > 0) {
        /* Give helper 100ms to exit, then SIGKILL */
        int status;
        usleep(100000);
        if (waitpid(ctx->helper_pid, &status, WNOHANG) == 0) {
            kill(ctx->helper_pid, SIGKILL);
            waitpid(ctx->helper_pid, &status, 0);
        }
        ctx->helper_pid = -1;
    }
}
