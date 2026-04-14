#define _GNU_SOURCE
#include <errno.h>
#include <linux/limits.h>
#include <sys/mman.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <wayland-client.h>
#include "wlr-data-control-unstable-v1.h"
#include "vassert.h"
#define STRB_IMPLEMENTATION
#include "strb.h"

// #define STB_TRUETYPE_IMPLEMENTATION
// #include "stb_truetype.h"

static char regclip_dir[PATH_MAX];
static char input_file[PATH_MAX];
static char ar_file[PATH_MAX];
struct pending_write {
	int fd;
	const char *data;
	size_t remaining;
	struct zwlr_data_control_source_v1 *source;
	bool active;
} pending_wr = { -1, NULL, 0, NULL, false };

// #undef VINFO
// #define VINFO(msg, ...) (void)0;

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *mgr;
static struct zwlr_data_control_device_v1 *dev;
static struct zwlr_data_control_source_v1 *source;
typedef struct {
	char *data;
	size_t cap;
	size_t lvl;
} Buf;
#define NUM_REGS 4
static Buf last[NUM_REGS] = { 0 };
static char reg_names[NUM_REGS][PATH_MAX];

#define MAX_BUF_SIZE (1024 * 1024 * 64)

static bool in_file_changed = true;
static int inotify_fd = -1;

uint32_t ar = 0;

#define UNUSED(arg) (void)(arg)

bool setup_inotify(void)
{
	int fd = open(input_file, O_CREAT | O_RDONLY, 0644);
	if (fd < 0) {
		VERROR("Couldn't create/open %s", input_file);
		return false;
	}
	close(fd);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd < 0) {
		VERROR("inotify_init1");
		return false;
	}

	int wd = inotify_add_watch(inotify_fd, input_file,
				   IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
	if (wd < 0) {
		VERROR("inotify_add_watch");
		close(inotify_fd);
		inotify_fd = -1;
		return false;
	}
	return true;
}

static bool buf_ensure(Buf *b, size_t need)
{
	if (b->cap >= need)
		return true;

	size_t new_cap = b->cap ? b->cap : 4096;
	while (new_cap < need)
		new_cap *= 2;

	char *new_data = realloc(b->data, new_cap);
	if (!new_data)
		return false;

	b->data = new_data;
	b->cap = new_cap;
	return true;
}

#ifdef NOTIFY_SEND_HACK
void notify(const char *title, const char *body)
{
	pid_t pid = fork();
	if (pid == 0) {
		execlp("notify-send", "notify-send",
		       "--hint", "int:transient:1", title, body, NULL);
		_exit(1);
	} else if (pid > 0) {
		signal(SIGCHLD, SIG_IGN);
	}
}
#endif // NOTIFY_SEND_HACK
void get_ar()
{
	if (!in_file_changed)
		return;
	FILE *f = fopen(input_file, "r");
	if (!f)
		return;

	if (fscanf(f, "%d", &ar) != 1) {
		ar = 0;

		goto cleanup;
	}
cleanup:
	fclose(f);
	in_file_changed = false;
}

void continue_write(void)
{
	if (!pending_wr.active)
		return;

	while (pending_wr.remaining > 0) {
		ssize_t n = write(pending_wr.fd,
				  pending_wr.data + (last[ar].lvl - pending_wr.remaining),
				  pending_wr.remaining);
		if (n > 0) {
			pending_wr.remaining -= n;
		} else if (n == 0) {
			// Should not happen on write fd, but treat as done
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// Wait for main loop to call us again when fd is writable
			return;
		} else if (errno != EINTR) {
			// Real error – abort
			break;
		}
	}

	close(pending_wr.fd);
	pending_wr.active = false;
	pending_wr.fd = -1;
}

bool regfile_write(size_t idx, const void *data, size_t len)
{
	VASSERT(idx < NUM_REGS);
	int fd = open(reg_names[idx], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;

	const char *p = data;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		p += n;
		remaining -= n;
	}

	close(fd);
	return true;
}

void source_send(void *data, struct zwlr_data_control_source_v1 *src,
		 const char *mime_type, int32_t fd)
{
	UNUSED(data);
	UNUSED(mime_type);

	if (!last[ar].data || last[ar].lvl == 0) {
		close(fd);
		return;
	}

	if (pending_wr.active) {
		close(fd);
		return;
	}

	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	pending_wr.fd = fd;
	pending_wr.source = src;
	pending_wr.data = last[ar].data;
	pending_wr.remaining = last[ar].lvl;
	pending_wr.active = true;

	continue_write();
}
void source_cancelled(void *data, struct zwlr_data_control_source_v1 *src)
{
	UNUSED(data);
	zwlr_data_control_source_v1_destroy(src);
	if (source == src)
		source = NULL;
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
	.send = source_send,
	.cancelled = source_cancelled
};
void publish_register(uint32_t reg)
{
	if (!dev || reg >= NUM_REGS)
		return;

	if (source)
		zwlr_data_control_source_v1_destroy(source);

	source = zwlr_data_control_manager_v1_create_data_source(mgr);
	zwlr_data_control_source_v1_add_listener(source, &source_listener, NULL);

	zwlr_data_control_source_v1_offer(source, "text/plain");
	zwlr_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
	zwlr_data_control_source_v1_offer(source, "UTF8_STRING");
	zwlr_data_control_source_v1_offer(source, "STRING");
	zwlr_data_control_source_v1_offer(source, "TEXT");

	zwlr_data_control_device_v1_set_selection(dev, source);
}

#define MIN(a, b) ((a) > (b) ? (b) : (a))

#define PREVIEW_SIZE 256

void write_ar_file()
{
	int fd = open(ar_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;

	dprintf(fd, "%d\n", ar);
	close(fd);
}

void process_inotify(void)
{
	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	ssize_t n;

	bool changed = false;
	while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
		changed = true;
	}
	if (changed) {
		in_file_changed = true;
		uint32_t old_ar = ar;
		get_ar();
		//if (ar != old_ar && ar < NUM_REGS) {
		if (ar < NUM_REGS) {
			publish_register(ar);
			char regstr[16];
			snprintf(regstr, sizeof(regstr), "Reg: %u", ar);
			char datastr[PREVIEW_SIZE];
			snprintf(datastr, sizeof(datastr), "%.*s", (int)MIN(last[ar].lvl, sizeof(datastr)), last[ar].data);
#ifdef NOTIFY_SEND_HACK
			notify(regstr, datastr);
#endif // NOTIFY_SEND_HACK
		} else {
			ar = old_ar;
			char datastr[PREVIEW_SIZE];
			char regentry[PREVIEW_SIZE / NUM_REGS];

			size_t datastr_fill = 0;
			for (size_t i = 0; i < NUM_REGS; i++) {
				regentry[0] = i + '0';
				regentry[1] = ':';
				size_t other_stuff = 1 + 1 + 1;
				size_t data_part_len = MIN(last[i].lvl, sizeof(regentry) - other_stuff);
				size_t ent_len = other_stuff + data_part_len;
				regentry[ent_len - 1] = '\n';
				memcpy(regentry + 2, last[i].data, data_part_len);
				memcpy(datastr + datastr_fill, regentry, ent_len);
				datastr_fill += ent_len;
			}
			VASSERT(datastr_fill <= sizeof(datastr));
			size_t last_byte = datastr_fill == sizeof(datastr) ? datastr_fill - 1 : datastr_fill;
			datastr[last_byte] = '\0';
			char regstr[16];
			snprintf(regstr, sizeof(regstr), "Reg: %u", old_ar);

#ifdef NOTIFY_SEND_HACK
			notify(regstr, datastr);
#endif // NOTIFY_SEND_HACK
		}
		write_ar_file();
	}
}
static struct {
	int fd; // read end of pipe
	struct zwlr_data_control_offer_v1 *offer;
	Buf buf;
} pending = { -1, NULL, { 0 } };

void sel(void *data, struct zwlr_data_control_device_v1 *dev,
	 struct zwlr_data_control_offer_v1 *offer)
{
	UNUSED(data);
	UNUSED(dev);
	if (!offer)
		return;
	if (pending.fd >= 0)
		return;

	int p[2];
	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
		return;

	// zwlr_data_control_offer_v1_receive(offer, "text/plain", p[1]);
	zwlr_data_control_offer_v1_receive(offer, "text/plain;charset=utf-8", p[1]);

	wl_display_flush(dpy);
	close(p[1]);

	pending.fd = p[0];
	pending.offer = offer;
	pending.buf.lvl = 0;
	buf_ensure(&pending.buf, 4096);
}

void process_clipboard(void)
{
	if (pending.buf.lvl == 0)
		goto cleanup;

	pending.buf.data[pending.buf.lvl] = '\0';

	if (pending.buf.lvl != last[ar].lvl ||
	    !last[ar].data ||
	    memcmp(pending.buf.data, last[ar].data, pending.buf.lvl) != 0) {
		size_t need = pending.buf.lvl + 1;
		if (buf_ensure(&last[ar], need)) {
			memcpy(last[ar].data, pending.buf.data, pending.buf.lvl);
			last[ar].data[pending.buf.lvl] = '\0';
			last[ar].lvl = pending.buf.lvl;

			regfile_write(ar, last[ar].data, last[ar].lvl);

			publish_register(ar);
		}
	}

cleanup:
	zwlr_data_control_offer_v1_destroy(pending.offer);
	pending.offer = NULL;
	pending.fd = -1;
}

void data_offer(void *data, struct zwlr_data_control_device_v1 *dev,
		struct zwlr_data_control_offer_v1 *offer)
{
	UNUSED(dev);
	UNUSED(data);
	UNUSED(offer);
}

void finished(void *data, struct zwlr_data_control_device_v1 *dev)
{
	UNUSED(data);
	UNUSED(dev);
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
	.data_offer = data_offer,
	.selection = sel,
	.finished = finished
};

void setup_device()
{
	if (!seat || !mgr || dev)
		return;
	dev = zwlr_data_control_manager_v1_get_data_device(mgr, seat);
	zwlr_data_control_device_v1_add_listener(dev, &device_listener, NULL);
}

void registry_global(void *data, struct wl_registry *reg, uint32_t id,
		     const char *iface, uint32_t ver)
{
	UNUSED(data);
	UNUSED(ver);

	if (!strcmp(iface, "wl_seat")) {
		seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
		setup_device();
	}
	if (!strcmp(iface, "zwlr_data_control_manager_v1")) {
		mgr = wl_registry_bind(reg, id, &zwlr_data_control_manager_v1_interface, 1);
		setup_device();
	}
}

void registry_global_remove(void *data, struct wl_registry *reg, uint32_t id)
{
	UNUSED(data);
	UNUSED(reg);
	UNUSED(id);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove
};

bool mkdir_if_not_exists(const char *path)
{
	int result = mkdir(path, 0755);
	if (result < 0) {
		if (errno == EEXIST) {
			return true;
		}
		return false;
	}
	return true;
}

void write_num_regs()
{
	static char num_regs_file[PATH_MAX];
	strbcpy(num_regs_file, regclip_dir, sizeof(num_regs_file));

	strbcat(num_regs_file, "nreg", sizeof(num_regs_file));
	int fd = open(num_regs_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;

	dprintf(fd, "%d\n", NUM_REGS);
	close(fd);
}

int main()
{
	char *xdg_data = getenv("XDG_RUNTIME_DIR");
	if (!xdg_data) {
		VWARN("No XDG_RUNTIME_DIR");
		xdg_data = "/tmp/";
	}
	strbcpy(regclip_dir, xdg_data, sizeof(input_file));
	strbcat(regclip_dir, "/regclip/", sizeof(input_file));
	// VINFO("%s", regclip_dir);
	VENSURE(mkdir_if_not_exists(regclip_dir));
	strbcpy(input_file, regclip_dir, sizeof(input_file));
	strbcat(input_file, "in", sizeof(input_file));
	strbcpy(ar_file, regclip_dir, sizeof(ar_file));
	strbcat(ar_file, "ar", sizeof(ar_file));
	write_num_regs();
	write_ar_file();

	for (size_t i = 0; i < NUM_REGS; i++) {
		char rname[8];
		snprintf(rname, sizeof(rname), "r%zu", i);
		strbcpy(reg_names[i], regclip_dir, sizeof(reg_names[i]));
		strbcat(reg_names[i], rname, sizeof(reg_names[i]));

		int fd = open(reg_names[i], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		close(fd);
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		VPANIC("Failed to connect to Wayland display");
	}

	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);

	if (!dev) {
		VPANIC("Failed to create data device (wlr-data-control unavailable?)");
	}
	VENSURE(setup_inotify());

	struct pollfd fds[4]; // display, inotify, pending write, clipboard read
	int nfds;

	while (1) {
		wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		nfds = 0;
		fds[nfds].fd = wl_display_get_fd(dpy);
		fds[nfds].events = POLLIN;
		nfds++;

		fds[nfds].fd = inotify_fd;
		fds[nfds].events = POLLIN;
		nfds++;

		if (pending_wr.active) {
			fds[nfds].fd = pending_wr.fd;
			fds[nfds].events = POLLOUT;
			nfds++;
		}

		if (pending.fd >= 0) {
			fds[nfds].fd = pending.fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (wl_display_prepare_read(dpy) < 0)
			continue;

		if (poll(fds, nfds, -1) < 0) {
			wl_display_cancel_read(dpy);
			VERROR("poll");
			break;
		}

		if (fds[0].revents & POLLIN) {
			wl_display_read_events(dpy);
		} else {
			wl_display_cancel_read(dpy);
		}

		if (fds[1].revents & POLLIN) {
			process_inotify();
		}

		int write_idx = pending_wr.active ? 2 : -1;
		if (write_idx >= 0 && (fds[write_idx].revents & POLLOUT)) {
			continue_write();
		}
		if (write_idx >= 0 && (fds[write_idx].revents & (POLLERR | POLLHUP))) {
			close(pending_wr.fd);
			pending_wr.active = false;
		}

		int read_idx = nfds - (pending.fd >= 0 ? 1 : 0);
		if (pending.fd >= 0 && (fds[read_idx].revents & (POLLIN | POLLHUP))) {
			ssize_t n = read(pending.fd,
					 pending.buf.data + pending.buf.lvl,
					 pending.buf.cap - pending.buf.lvl - 1);
			if (n > 0) {
				pending.buf.lvl += n;
				if (!buf_ensure(&pending.buf, pending.buf.cap * 2)) {
					close(pending.fd);
					process_clipboard();
				}
			} else if (n == 0 || (n < 0 && errno != EAGAIN)) {
				close(pending.fd);
				process_clipboard();
			}
		}
	}
}
