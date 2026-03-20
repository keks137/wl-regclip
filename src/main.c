#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <wayland-client.h>
#include "wlr-data-control-unstable-v1.h"

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
static Buf modified = { 0 };
static Buf last = { 0 };

#define NUM_MODES 6
static const char *mode_suffix[NUM_MODES] = { "69", "70", "71", "72", "73", "74" };
uint32_t active_mode = 0;

#define UNUSED(arg) (void)(arg)

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
void source_send(void *data, struct zwlr_data_control_source_v1 *src,
		 const char *mime_type, int32_t fd)
{
	UNUSED(data);
	UNUSED(src);
	UNUSED(mime_type);

	if (modified.data) {
		write(fd, modified.data, modified.lvl);
	}
	close(fd);
}
void get_mode()
{
	FILE *f = fopen("/tmp/clipboard_mode", "r");
	if (!f)
		return;

	int mode;
	if (fscanf(f, "%d", &mode) != 1) {
		fclose(f);
		return;
	}
	fclose(f);
}

const char *get_suffix()
{
	FILE *f = fopen("/tmp/clipboard_mode", "r");
	if (!f)
		return mode_suffix[0]; // default "69"

	int mode;
	if (fscanf(f, "%d", &mode) != 1) {
		fclose(f);
		return mode_suffix[0];
	}
	fclose(f);

	if (mode >= 0 && mode < NUM_MODES)
		return mode_suffix[mode];
	return mode_suffix[0];
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
void sel(void *data, struct zwlr_data_control_device_v1 *dev,
	 struct zwlr_data_control_offer_v1 *offer)
{
	UNUSED(data);
	if (!offer)
		return;

	int p[2];
	if (pipe(p) < 0)
		return;

	zwlr_data_control_offer_v1_receive(offer, "text/plain", p[1]);
	close(p[1]);
	wl_display_roundtrip(dpy);

	// Reset recv buffer
	recv.lvl = 0;
	if (!buf_ensure(&recv, 4096)) {
		close(p[0]);
		return;
	}

	// Read all data
	ssize_t n;
	while ((n = read(p[0], recv.data + recv.lvl, recv.cap - recv.lvl - 1)) > 0) {
		recv.lvl += n;
		if (recv.lvl >= recv.cap - 8) {
			if (!buf_ensure(&recv, recv.cap * 2)) {
				close(p[0]);
				return;
			}
		}
	}
	close(p[0]);

	if (n < 0 || recv.lvl == 0) {
		zwlr_data_control_offer_v1_destroy(offer);
		return;
	}

	recv.data[recv.lvl] = '\0';

	// Check if unchanged (compare lengths first, then data)
	if (recv.lvl == last.lvl && last.data &&
	    memcmp(recv.data, last.data, recv.lvl) == 0) {
		zwlr_data_control_offer_v1_destroy(offer);
		return;
	}

	// Build modified: data + suffix
	// get_mode();
	const char *suffix = get_suffix();
	size_t suffix_len = strlen(suffix);
	size_t need = recv.lvl + suffix_len + 1;

	if (!buf_ensure(&modified, need)) {
		zwlr_data_control_offer_v1_destroy(offer);
		return;
	}

	memcpy(modified.data, recv.data, recv.lvl);
	memcpy(modified.data + recv.lvl, suffix, suffix_len + 1); // includes \0
	modified.lvl = recv.lvl + suffix_len;

	// Save to last
	if (!buf_ensure(&last, need)) {
		zwlr_data_control_offer_v1_destroy(offer);
		return;
	}
	memcpy(last.data, modified.data, modified.lvl + 1);
	last.lvl = modified.lvl;

	// Set selection
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
	zwlr_data_control_offer_v1_destroy(offer);
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
	printf("Watching clipboard...\n");
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
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "Failed to connect to Wayland display\n");
		return 1;
	}

	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);

	if (!dev) {
		fprintf(stderr, "Failed to create data device (wlr-data-control unavailable?)\n");
		return 1;
	}

	while (wl_display_dispatch(dpy) >= 0)
		;

	return 0;
}
