#include <errno.h>
#include <fcntl.h>
#include <mario/pcm.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "audio/format.h"
#include "common/common.h"
#include "common/msg.h"
#include "internal.h"

struct priv {
    int fd;
    unsigned char *staging;
    int staged_frames;
    bool started;
    bool paused;
    bool failed;
};

static bool fail(struct ao *ao, const char *action, int error)
{
    struct priv *priv = ao->priv;
    MP_ERR(ao, "%s /dev/pcm0: %s\n", action, mp_strerror(error));
    priv->failed = true;
    return false;
}

static bool read_status(struct ao *ao, struct mario_pcm_status *status)
{
    struct priv *priv = ao->priv;
    if (priv->failed)
        return false;
    if (ioctl(priv->fd, MARIO_PCM_STATUS, status) < 0)
        return fail(ao, "Querying", errno);
    if (status->state == MARIO_PCM_FAILED)
        return fail(ao, "Playing through", EIO);
    return true;
}

static int init(struct ao *ao)
{
    struct priv *priv = ao->priv;
    priv->fd = open("/dev/pcm0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (priv->fd < 0) {
        fail(ao, "Opening", errno);
        return -1;
    }

    struct mario_pcm_status status;
    if (!read_status(ao, &status)) {
        close(priv->fd);
        return -1;
    }
    if (status.version != MARIO_PCM_ABI_VERSION || !status.capacity_frames) {
        MP_ERR(ao, "Unsupported /dev/pcm0 ABI\n");
        close(priv->fd);
        return -1;
    }

    ao->samplerate = MARIO_PCM_RATE;
    ao->format = AF_FORMAT_S16;
    ao->channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;
    ao->device_buffer = status.capacity_frames;
    priv->staging = talloc_array(priv, unsigned char,
                                 ao->device_buffer * MARIO_PCM_FRAME_BYTES);
    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *priv = ao->priv;
    close(priv->fd);
}

static void reset(struct ao *ao)
{
    struct priv *priv = ao->priv;
    priv->staged_frames = 0;
    priv->started = false;
    priv->paused = false;
    if (ioctl(priv->fd, MARIO_PCM_RESET, NULL) < 0 && !priv->failed)
        fail(ao, "Resetting", errno);
}

static bool update(struct ao *ao, struct mario_pcm_status *status)
{
    struct priv *priv = ao->priv;
    if (!read_status(ao, status))
        return false;
    if (priv->paused || status->state == MARIO_PCM_PAUSING ||
        status->state == MARIO_PCM_DRAINING)
        return true;

    int frames = MPMIN(priv->staged_frames, (int)status->writable_frames);
    if (frames) {
        ssize_t bytes = write(priv->fd, priv->staging,
                              frames * MARIO_PCM_FRAME_BYTES);
        if (bytes < 0 && errno != EAGAIN && errno != EBUSY && errno != EINTR)
            return fail(ao, "Writing to", errno);
        if (bytes > 0) {
            priv->staged_frames -= bytes / MARIO_PCM_FRAME_BYTES;
            memmove(priv->staging, priv->staging + bytes,
                    priv->staged_frames * MARIO_PCM_FRAME_BYTES);
        }
        if (!read_status(ao, status))
            return false;
    }

    if (priv->started && status->pending_frames &&
        (status->state == MARIO_PCM_STOPPED || status->state == MARIO_PCM_UNDERRUN ||
         status->state == MARIO_PCM_PAUSED)) {
        int command = status->state == MARIO_PCM_PAUSED ? MARIO_PCM_RESUME : MARIO_PCM_START;
        if (ioctl(priv->fd, command, NULL) < 0)
            return fail(ao, "Starting", errno);
        return read_status(ao, status);
    }
    return true;
}

static bool queue_samples(struct ao *ao, void **data, int samples)
{
    struct priv *priv = ao->priv;
    if (priv->failed || samples > ao->device_buffer - priv->staged_frames)
        return false;
    memcpy(priv->staging + priv->staged_frames * MARIO_PCM_FRAME_BYTES,
           data[0], samples * MARIO_PCM_FRAME_BYTES);
    priv->staged_frames += samples;
    return true;
}

static void start(struct ao *ao)
{
    struct priv *priv = ao->priv;
    struct mario_pcm_status status;
    priv->started = true;
    update(ao, &status);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *priv = ao->priv;
    struct mario_pcm_status status;
    if (paused) {
        if (!read_status(ao, &status))
            return false;
        if (status.state == MARIO_PCM_RUNNING &&
            ioctl(priv->fd, MARIO_PCM_PAUSE, NULL) < 0 && errno != EINVAL)
            return fail(ao, "Pausing", errno);
        priv->paused = true;
        return true;
    }

    priv->paused = false;
    return update(ao, &status);
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *priv = ao->priv;
    struct mario_pcm_status status;
    *state = (struct mp_pcm_state){0};
    if (!update(ao, &status))
        return;

    state->free_samples = ao->device_buffer - priv->staged_frames;
    state->queued_samples = status.pending_frames + priv->staged_frames;
    state->delay = state->queued_samples / (double)ao->samplerate;
    state->playing = priv->started &&
        (state->queued_samples || status.state == MARIO_PCM_RUNNING ||
         status.state == MARIO_PCM_PAUSING || status.state == MARIO_PCM_DRAINING);
}

const struct ao_driver audio_out_mario = {
    .name = "mario",
    .description = "Mario PCM playback",
    .init = init,
    .uninit = uninit,
    .reset = reset,
    .start = start,
    .set_pause = set_pause,
    .write = queue_samples,
    .get_state = get_state,
    .priv_size = sizeof(struct priv),
};
