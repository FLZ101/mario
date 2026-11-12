#include <assert.h>
#include <gui/surface.h>

static gui_flush_result scanout_result;
static unsigned int scanout_calls;

gui_flush_result gui_surface_flush_rect_fd(const gui_surface *surface,
		gui_rect rectangle, size_t stride, int descriptor)
{
	assert(surface != NULL && rectangle.w == 8 && rectangle.h == 8);
	assert(stride == 32 && descriptor == 123);
	scanout_calls++;
	return scanout_result;
}

#define main wm_program_main
#include "../../gui/desktop/mario-wm/main.c"
#undef main

static void test_commit(gui_flush_result result, int graphics)
{
	static wm_server server;
	wm_client *client = &server.clients[0];
	gui_surface *previous = gui_surface_create(8, 8);
	gui_surface *next = gui_surface_create(8, 8);
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	assert(previous != NULL && next != NULL);
	memset(&server, 0, sizeof(server));
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		server.clients[index].fd = -1;
	server.state = (wm_state) { .framebuffer = 123, .graphics = graphics,
		.width = 8, .height = 8, .scanout_stride = 32,
		.surface_bytes = 512, .frame_number = 7,
		.composite = gui_surface_create(8, 8) };
	assert(server.state.composite != NULL);
	*client = (wm_client) { .fd = 124, .hello = 1,
		.role = GUI_PROTOCOL_ENDPOINT_ORDINARY, .window_id = 1,
		.configure_serial = 1, .width = 8, .height = 8,
		.committed = previous, .committed_bytes = 256,
		.staging = next, .staging_bytes = 256, .staging_pixels = 64,
		.staging_frame_serial = 2, .server = &server,
		.output = gui_protocol_output_create(GUI_PROTOCOL_OUTPUT_LIMIT) };
	assert(client->output != NULL);
	put_u32(commit, 1);
	put_u64(commit + 4, 1);
	put_u64(commit + 12, 2);
	scanout_result = result;
	scanout_calls = 0;
	assert(client_commit_surface(client, commit) == 0);
	assert(scanout_calls == (unsigned int)graphics);
	assert(server.state.frame_number == 7);
	if (result == GUI_FLUSH_ERROR && graphics) {
		assert(client->committed == previous && client->staging == next);
		assert(server.state.surface_bytes == 512);
		assert(gui_protocol_output_pending(client->output));
	} else {
		assert(client->committed == next && client->staging == NULL);
		assert(client->staging_frame_serial == 0 && client->staging_pixels == 0);
		assert(server.state.surface_bytes == 256);
		assert(!gui_protocol_output_pending(client->output));
	}
	client_discard_staging(client);
	client_discard_committed(client);
	assert(server.state.surface_bytes == 0);
	gui_protocol_output_destroy(client->output);
	gui_surface_destroy(server.state.composite);
}

int main(void)
{
	test_commit(GUI_FLUSH_BUSY, 1);
	test_commit(GUI_FLUSH_ERROR, 1);
	test_commit(GUI_FLUSH_COMPLETE, 0);
	puts("gui-wm-busy: valid commits survive suspended scanout");
	return 0;
}
