#include <assert.h>
#include <errno.h>
#include <linux/limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <wayland-client.h>
#include "wlr-data-control-unstable-v1.h"
#include "vassert.h"

#undef VINFO
#define VINFO(msg, ...) (void)0;

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
static Buf recv = { 0 };
#define NUM_REGS 4
static Buf last[NUM_REGS] = { 0 };

#define MAX_BUF_SIZE (1024 * 1024 * 64)

static bool ar_file_changed = true;
static int inotify_fd = -1;

uint32_t ar = 0;

#define UNUSED(arg) (void)(arg)

#define REGCLIP_INP_FILE "/tmp/clipboard_reg"
bool setup_inotify(void)
{
	int fd = open(REGCLIP_INP_FILE, O_CREAT | O_RDONLY, 0644);
	if (fd < 0) {
		VERROR("Couldn't create/open " REGCLIP_INP_FILE);
		return false;
	}
	close(fd);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd < 0) {
		VERROR("inotify_init1");
		return false;
	}

	int wd = inotify_add_watch(inotify_fd, "/tmp/clipboard_reg",
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

void notify(const char *title, const char *body)
{
	pid_t pid = fork();
	if (pid == 0) {
		execlp("notify-send", "notify-send", title, body, NULL);
		_exit(1);
	} else if (pid > 0) {
		signal(SIGCHLD, SIG_IGN);
	}
}
void get_ar()
{
	if (!ar_file_changed)
		return;
	FILE *f = fopen("/tmp/clipboard_reg", "r");
	if (!f)
		return;

	if (fscanf(f, "%d", &ar) != 1) {
		ar = 0;

		goto cleanup;
	}
cleanup:
	fclose(f);
	ar_file_changed = false;
}

void source_send(void *data, struct zwlr_data_control_source_v1 *src,
		 const char *mime_type, int32_t fd)
{
	UNUSED(data);
	UNUSED(src);
	UNUSED(mime_type);

	if (!last[ar].data || last[ar].lvl == 0) {
		goto cleanup;
	}

	size_t written = 0;
	while (written < last[ar].lvl) {
		ssize_t n = write(fd, last[ar].data + written, last[ar].lvl - written);
		if (n < 0) {
			if (errno == EINTR) {
				VINFO("hi");
				continue;
			}
			break;
		}
		written += n;
	}
cleanup:
	close(fd);
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

void process_inotify(void)
{
	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	ssize_t n;

	bool changed = false;
	while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
		changed = true;
	}
	if (changed) {
		ar_file_changed = true;
		// uint32_t old_ar = ar;
		get_ar();
		//if (ar != old_ar && ar < NUM_REGS) {
		if (ar < NUM_REGS) {
			publish_register(ar);
			char regstr[16];
			snprintf(regstr, sizeof(regstr), "Reg: %u", ar);
			char datastr[PREVIEW_SIZE];
			snprintf(datastr, sizeof(datastr), "%.*s", (int)MIN(last[ar].lvl, sizeof(datastr)), last[ar].data);
			notify(regstr, datastr);
		} else if (ar >= NUM_REGS) {
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
			notify("Regs:", datastr);
		}
	}
}
void sel(void *data, struct zwlr_data_control_device_v1 *dev,
	 struct zwlr_data_control_offer_v1 *offer)
{
	VINFO("sel start");
	UNUSED(data);
	UNUSED(dev);
	if (!offer) {
		VINFO("ret");
		return;
	}
	int p[2];
	if (pipe(p) < 0) {
		VINFO("ret");

		return;
	}
	// fcntl(p[0], F_SETFL, O_NONBLOCK);

	zwlr_data_control_offer_v1_receive(offer, "text/plain", p[1]);
	close(p[1]);
	wl_display_roundtrip(dpy);

	// wl_display_flush(dpy);

	recv.lvl = 0;
	if (!buf_ensure(&recv, 4096)) {
		close(p[0]);

		VINFO("ret");
		return;
	}

	ssize_t n;
	while ((n = read(p[0], recv.data + recv.lvl, recv.cap - recv.lvl - 1)) > 0) {
		recv.lvl += n;
		if (recv.lvl >= recv.cap - 8) {
			if (!buf_ensure(&recv, recv.cap * 2)) {
				close(p[0]);
				VINFO("ret");
				return;
			}
		}
		if (recv.lvl >= MAX_BUF_SIZE)
			break;
	}
	close(p[0]);

	if (n < 0 || recv.lvl == 0) {
		zwlr_data_control_offer_v1_destroy(offer);
		VINFO("ret");
		return;
	}

	recv.data[recv.lvl] = '\0';

	if (recv.lvl == last[ar].lvl && last[ar].data &&
	    // memcmp(recv.data, last[ar].data, MIN(recv.lvl, 1024)) == 0) {
	    memcmp(recv.data, last[ar].data, recv.lvl) == 0) {
		zwlr_data_control_offer_v1_destroy(offer);
		VINFO("ret");
		return;
	}

	size_t need = recv.lvl;

	// Save to last
	if (!buf_ensure(&last[ar], need)) {
		zwlr_data_control_offer_v1_destroy(offer);
		VINFO("ret");
		return;
	}
	memcpy(last[ar].data, recv.data, recv.lvl);
	last[ar].data[recv.lvl] = '\0';
	last[ar].lvl = recv.lvl;

	VINFO("lvl: %zu", last[ar].lvl);
	// if (last[ar].lvl > 0)
	publish_register(ar);
	zwlr_data_control_offer_v1_destroy(offer);
	VINFO("sel end");
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
	// printf("Watching clipboard...");
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

int main()
{
	signal(SIGPIPE, SIG_IGN);
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

	struct pollfd fds[2];
	fds[0].fd = wl_display_get_fd(dpy);
	fds[1].fd = inotify_fd;
	fds[0].events = fds[1].events = POLLIN;
	while (1) {
		VINFO("frame");
		wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		if (wl_display_prepare_read(dpy) < 0) {
			continue;
		}

		if (poll(fds, 2, -1) < 0) {
			wl_display_cancel_read(dpy);
			perror("poll");
			break;
		}

		if (fds[0].revents & POLLIN) {
			wl_display_read_events(dpy);
		} else {
			wl_display_cancel_read(dpy);
		}

		if (fds[1].revents & POLLIN) {
			VINFO("inotify");
			process_inotify();
		}
	}
}
