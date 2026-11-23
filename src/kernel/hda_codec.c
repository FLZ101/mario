#include <hda.h>
#include <lib/string.h>
#include <misc.h>

#define GET_PARAMETER 0xf0000
#define GET_CONNECTIONS 0xf0200
#define GET_PIN_DEFAULT 0xf1c00
#define GET_EAPD 0xf0c00
#define SET_SELECTOR 0x70100
#define SET_POWER 0x70500
#define SET_CHANNEL 0x70600
#define SET_PIN_CONTROL 0x70700
#define SET_EAPD 0x70c00
#define SET_FORMAT 0x20000
#define SET_AMP 0x30000

#define PAR_VENDOR 0x00
#define PAR_REVISION 0x02
#define PAR_NODES 0x04
#define PAR_FUNCTION 0x05
#define PAR_WIDGET 0x09
#define PAR_PCM 0x0a
#define PAR_FORMATS 0x0b
#define PAR_PIN 0x0c
#define PAR_INPUT_AMP 0x0d
#define PAR_CONNECTIONS 0x0e
#define PAR_POWER 0x0f
#define PAR_OUTPUT_AMP 0x12

#define WCAP_STEREO (1 << 0)
#define WCAP_INPUT_AMP (1 << 1)
#define WCAP_OUTPUT_AMP (1 << 2)
#define WCAP_AMP_OVERRIDE (1 << 3)
#define WCAP_FORMAT_OVERRIDE (1 << 4)
#define WCAP_CONNECTIONS (1 << 8)
#define WCAP_DIGITAL (1 << 9)
#define WCAP_POWER (1 << 10)
#define WIDGET_TYPE(caps) (((caps) >> 20) & 0x0f)
#define WIDGET_DAC 0
#define WIDGET_MIXER 2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN 4

#define PIN_HEADPHONE (1 << 3)
#define PIN_OUTPUT (1 << 4)
#define PIN_HDMI (1 << 7)
#define PIN_EAPD (1 << 16)
#define PIN_DISPLAYPORT (1 << 24)
#define PIN_CTL_OUTPUT (1 << 6)
#define PIN_CTL_HEADPHONE (1 << 7)
#define PCM_48K_S16 ((1 << 6) | (1 << 17))
#define AMP_BOTH_CHANNELS (3 << 12)
#define AMP_INPUT (1 << 14)
#define AMP_OUTPUT (1 << 15)
#define AMP_MUTE (1 << 7)
#define AMP_CAN_MUTE (1U << 31)
#define MAX_NODES 256
#define MAX_PATH 8

static struct hda_graph {
	unsigned int codec;
	unsigned int group;
	unsigned int first;
	unsigned int end;
	uint32_t caps[MAX_NODES];
	uint8_t seen[MAX_NODES];
	struct {
		uint8_t node;
		uint8_t input;
		uint16_t connections;
	} path[MAX_PATH];
	unsigned int length;
} graph;

static uint32_t codec_read(unsigned int node, uint32_t verb)
{
	uint32_t response;
	return hda_verb(graph.codec, node, verb, &response) ? ~0U : response;
}

static int codec_write(unsigned int node, uint32_t verb)
{
	uint32_t response;
	return hda_verb(graph.codec, node, verb, &response);
}

static uint32_t parameter(unsigned int node, unsigned int index)
{
	return codec_read(node, GET_PARAMETER | index);
}

static unsigned int get_connections(unsigned int node, uint8_t *connections)
{
	uint32_t info = parameter(node, PAR_CONNECTIONS);
	unsigned int entries = info & 0x7f;
	unsigned int width = info & 0x80 ? 16 : 8;
	unsigned int range_bit = 1U << (width - 1);
	unsigned int previous = 0, count = 0;
	uint32_t response = 0;

	for (unsigned int index = 0; index < entries; index++) {
		unsigned int current, first;
		if (!(index % (32 / width))) {
			response = codec_read(node, GET_CONNECTIONS | index);
			if (response == ~0U)
				return 0;
		}
		current = response & (range_bit - 1);
		first = (response & range_bit) ? previous + 1 : current;
		if (!current || current >= MAX_NODES || first > current ||
		    count + current - first + 1 > MAX_NODES)
			return 0;
		for (; first <= current; first++)
			connections[count++] = first;
		previous = current;
		response >>= width;
	}
	return count;
}

static int find_dac(unsigned int node, unsigned int depth)
{
	uint32_t caps;
	unsigned int type, count;
	uint8_t connections[MAX_NODES];

	if (depth == MAX_PATH || node < graph.first || node >= graph.end ||
	    graph.seen[node])
		return 0;
	graph.seen[node] = 1;
	caps = graph.caps[node];
	type = WIDGET_TYPE(caps);
	if (caps == ~0U || caps & WCAP_DIGITAL)
		return 0;
	graph.path[depth].node = node;
	graph.path[depth].connections = 0;
	if (type == WIDGET_DAC) {
		unsigned int source = caps & WCAP_FORMAT_OVERRIDE ? node : graph.group;
		uint32_t pcm = parameter(source, PAR_PCM);
		uint32_t formats = parameter(source, PAR_FORMATS);
		if (!(caps & WCAP_STEREO) || pcm == ~0U || formats == ~0U ||
		    (pcm & PCM_48K_S16) != PCM_48K_S16 || !(formats & 1))
			return 0;
		graph.length = depth + 1;
		return 1;
	}
	if (!(caps & WCAP_CONNECTIONS) ||
	    (type != WIDGET_MIXER && type != WIDGET_SELECTOR &&
	     (type != WIDGET_PIN || depth)))
		return 0;
	count = get_connections(node, connections);
	for (unsigned int index = 0; index < count; index++) {
		if (index >= 16 && (caps & WCAP_INPUT_AMP))
			break;
		if (find_dac(connections[index], depth + 1)) {
			graph.path[depth].input = index;
			graph.path[depth].connections = count;
			return 1;
		}
	}
	return 0;
}

static int setup_amps(unsigned int node, unsigned int input, unsigned int count)
{
	uint32_t caps = graph.caps[node];
	unsigned int source = caps & WCAP_AMP_OVERRIDE ? node : graph.group;
	uint32_t amp;

	if (caps & WCAP_OUTPUT_AMP) {
		amp = parameter(source, PAR_OUTPUT_AMP);
		if (amp == ~0U ||
		    codec_write(node, SET_AMP | AMP_BOTH_CHANNELS | AMP_OUTPUT |
				(amp & 0x7f)))
			return -1;
	}
	if ((caps & WCAP_INPUT_AMP) && count) {
		amp = parameter(source, PAR_INPUT_AMP);
		if (amp == ~0U)
			return -1;
		for (unsigned int index = 0; index < count && index < 16; index++) {
			if (index != input && (WIDGET_TYPE(caps) != WIDGET_MIXER ||
					      !(amp & AMP_CAN_MUTE)))
				continue;
			if (codec_write(node, SET_AMP | AMP_BOTH_CHANNELS | AMP_INPUT |
					(index << 8) | (index == input ? amp & 0x7f : AMP_MUTE)))
				return -1;
		}
	}
	return 0;
}

static int setup_path(uint32_t pin_caps, int headphones)
{
	unsigned int pin = graph.path[0].node;
	unsigned int dac = graph.path[graph.length - 1].node;
	uint32_t pin_ctl = PIN_CTL_OUTPUT;

	if ((parameter(graph.group, PAR_POWER) & 1) && codec_write(graph.group, SET_POWER))
		return -1;
	for (unsigned int index = graph.length; index; index--) {
		unsigned int node = graph.path[index - 1].node;
		unsigned int input = graph.path[index - 1].input;
		unsigned int count = graph.path[index - 1].connections;
		uint32_t caps = graph.caps[node];

		if ((caps & WCAP_POWER) && (parameter(node, PAR_POWER) & 1) &&
		    codec_write(node, SET_POWER))
			return -1;
		if (count > 1 && WIDGET_TYPE(caps) != WIDGET_MIXER &&
		    codec_write(node, SET_SELECTOR | input))
			return -1;
		if (setup_amps(node, input, count))
			return -1;
	}
	if (codec_write(dac, SET_FORMAT | HDA_PCM_FORMAT) ||
	    codec_write(dac, SET_CHANNEL | (HDA_STREAM_TAG << 4)))
		return -1;
	if (headphones && (pin_caps & PIN_HEADPHONE))
		pin_ctl |= PIN_CTL_HEADPHONE;
	if (codec_write(pin, SET_PIN_CONTROL | pin_ctl))
		return -1;
	if (pin_caps & PIN_EAPD) {
		uint32_t eapd = codec_read(pin, GET_EAPD);
		if (eapd == ~0U || codec_write(pin, SET_EAPD | (eapd & 7) | 2))
			return -1;
	}
	return 0;
}

static int find_pin(unsigned int device)
{
	for (unsigned int pin = graph.first; pin < graph.end; pin++) {
		uint32_t config, pin_caps;
		unsigned int port;
		if (WIDGET_TYPE(graph.caps[pin]) != WIDGET_PIN ||
		    graph.caps[pin] & WCAP_DIGITAL)
			continue;
		config = codec_read(pin, GET_PIN_DEFAULT);
		port = config >> 30;
		if ((port != 0 && port != 3) || ((config >> 20) & 0x0f) != device)
			continue;
		pin_caps = parameter(pin, PAR_PIN);
		if (!(pin_caps & PIN_OUTPUT) || pin_caps & (PIN_HDMI | PIN_DISPLAYPORT))
			continue;
		memset(graph.seen, 0, sizeof(graph.seen));
		if (find_dac(pin, 0) && !setup_path(pin_caps, device == 2))
			return 1;
	}
	return 0;
}

int hda_configure_codec(unsigned int codec)
{
	uint32_t vendor, nodes, revision;
	unsigned int first, end;

	graph.codec = codec;
	vendor = parameter(0, PAR_VENDOR);
	if (!vendor || vendor == ~0U)
		return -1;
	revision = parameter(0, PAR_REVISION);
	nodes = parameter(0, PAR_NODES);
	first = (nodes >> 16) & 0xff;
	end = first + (nodes & 0xff);
	if (!first || end > MAX_NODES)
		return -1;
	for (unsigned int group = first; group < end; group++) {
		if ((parameter(group, PAR_FUNCTION) & 0xff) != 1)
			continue;
		graph.group = group;
		nodes = parameter(group, PAR_NODES);
		graph.first = (nodes >> 16) & 0xff;
		graph.end = graph.first + (nodes & 0xff);
		if (!graph.first || graph.end > MAX_NODES)
			continue;
		for (unsigned int node = graph.first; node < graph.end; node++)
			graph.caps[node] = parameter(node, PAR_WIDGET);
		if (!find_pin(0) && !find_pin(2))
			continue;
		pr_info("HDA: codec %u vendor=%x revision=%x group=%u pin=%u DAC=%u\n",
			codec, vendor, revision, group, graph.path[0].node,
			graph.path[graph.length - 1].node);
		return 0;
	}
	pr_info("HDA: codec %u vendor=%x revision=%x has no usable analog path\n",
		codec, vendor, revision);
	return -1;
}
