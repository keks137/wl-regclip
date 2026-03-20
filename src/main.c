#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-data-control-unstable-v1.h"

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *mgr;
static struct zwlr_data_control_device_v1 *dev;
static struct zwlr_data_control_source_v1 *source;
static char modified_buf[4096];
static char last_written[4096];

#define UNUSED(arg) (void)(arg)

void source_send(void *data, struct zwlr_data_control_source_v1 *src,
		 const char *mime_type, int32_t fd)
{
	UNUSED(data);
	UNUSED(src);
	UNUSED(mime_type);

	write(fd, modified_buf, strlen(modified_buf));
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

void data_offer(void *data, struct zwlr_data_control_device_v1 *dev,
		struct zwlr_data_control_offer_v1 *offer)
{
	UNUSED(dev);
	UNUSED(data);
	UNUSED(offer);
}

void sel(void *data, struct zwlr_data_control_device_v1 *dev,
	 struct zwlr_data_control_offer_v1 *offer)
{
	UNUSED(data);
	if (!offer)
		return;

	int p[2];
	pipe(p);
	zwlr_data_control_offer_v1_receive(offer, "text/plain", p[1]);
	close(p[1]);

	wl_display_roundtrip(dpy);

	char buf[4096];
	int n = read(p[0], buf, 4095);
	close(p[0]);

	if (n > 0) {
		buf[n] = 0;

		// Check if this is our own write coming back
		if (strcmp(buf, last_written) == 0) {
			zwlr_data_control_offer_v1_destroy(offer);
			return;
		}

		// printf("CLIP: %s\n", buf);

		strcat(buf, "69");
		strcpy(modified_buf, buf);

		// Store what we're about to write
		strcpy(last_written, modified_buf);

		// Create a new source with modified content
		if (source)
			zwlr_data_control_source_v1_destroy(source);

		source = zwlr_data_control_manager_v1_create_data_source(mgr);
		zwlr_data_control_source_v1_add_listener(source, &source_listener, NULL);

		// Offer multiple MIME types
		zwlr_data_control_source_v1_offer(source, "text/plain");
		zwlr_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
		zwlr_data_control_source_v1_offer(source, "UTF8_STRING");
		zwlr_data_control_source_v1_offer(source, "STRING");
		zwlr_data_control_source_v1_offer(source, "TEXT");

		// Set the selection to our modified content
		zwlr_data_control_device_v1_set_selection(dev, source);
	}

	zwlr_data_control_offer_v1_destroy(offer);
}

void finished(void *data, struct zwlr_data_control_device_v1 *dev)
{
	UNUSED(data);
	UNUSED(dev);
}

static const struct zwlr_data_control_device_v1_listener l = {
	.data_offer = data_offer,
	.selection = sel,
	.finished = finished
};

void setup()
{
	if (!seat || !mgr || dev)
		return;
	dev = zwlr_data_control_manager_v1_get_data_device(mgr, seat);
	zwlr_data_control_device_v1_add_listener(dev, &l, NULL);
	printf("Watching clipboard...\n");
}

void reg(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
	UNUSED(d);
	UNUSED(ver);

	if (!strcmp(iface, "wl_seat")) {
		seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
		setup();
	}
	if (!strcmp(iface, "zwlr_data_control_manager_v1")) {
		mgr = wl_registry_bind(r, id, &zwlr_data_control_manager_v1_interface, 1);
		setup();
	}
}

static const struct wl_registry_listener rl = { .global = reg };

int main()
{
	dpy = wl_display_connect(NULL);
	struct wl_registry *r = wl_display_get_registry(dpy);
	wl_registry_add_listener(r, &rl, NULL);
	wl_display_roundtrip(dpy);

	if (!dev) {
		fprintf(stderr, "Failed to create data device\n");
		return 1;
	}

	while (wl_display_dispatch(dpy) >= 0)
		;
}
