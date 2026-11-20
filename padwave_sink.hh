// padwave_sink.hh — optional PipeWire sink input.
//
// This is deliberately not a second audio engine. The PipeWire process
// callback only hands 48 kHz stereo float frames to the player's existing
// 512-frame ring; the normal timerfd path still does Opus and the 0x36 write.
// There are no pthreads here: the padwave event loop owns the PipeWire loop fd
// and calls dispatch() when it becomes readable.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

struct PipeWireSink {
  using Push = void (*)(void *owner, const float *samples, size_t frames);

  PipeWireSink() = default;
  ~PipeWireSink() { stop(); }

  PipeWireSink(const PipeWireSink &)            = delete;
  PipeWireSink &operator=(const PipeWireSink &) = delete;

  bool active() const { return stream_ != nullptr; }

  int fd() const {
    if (!loop_) return -1;
    return pw_loop_get_fd(pw_main_loop_get_loop(loop_));
  }

  bool start(Push push, void *owner) {
    stop();
    if (!push) return false;

    pw_init(nullptr, nullptr);
    initialized_ = true;
    loop_        = pw_main_loop_new(nullptr);
    if (!loop_) {
      stop();
      return false;
    }

    pw_loop *loop = pw_main_loop_get_loop(loop_);
    pw_loop_enter(loop);
    entered_ = true;

    events_               = pw_stream_events{};
    events_.version       = PW_VERSION_STREAM_EVENTS;
    events_.state_changed = &PipeWireSink::onState;
    events_.process       = &PipeWireSink::onProcess;

    pw_properties *context_props = pw_properties_new("module.rt", "false", nullptr);
    context_                     = pw_context_new(loop, context_props, 0);
    if (!context_) {
      stop();
      return false;
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
      stop();
      return false;
    }

    pw_properties *stream_props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_NAME, "padwave",
                          PW_KEY_NODE_DESCRIPTION, "padwave DualSense", PW_KEY_NODE_NICK, "padwave",
                          PW_KEY_NODE_LATENCY, "512/48000", PW_KEY_MEDIA_ROLE, "Music", nullptr);
    stream_ = pw_stream_new(core_, "padwave", stream_props);
    if (!stream_) {
      stop();
      return false;
    }
    pw_stream_add_listener(stream_, &stream_listener_, &events_, this);

    uint8_t pod_buffer[1024];
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
    spa_audio_info_raw info{};
    info.format      = SPA_AUDIO_FORMAT_F32;
    info.rate        = 48000;
    info.channels    = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    enum pw_stream_flags flags =
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
      stop();
      return false;
    }

    push_  = push;
    owner_ = owner;
    fprintf(stderr, "padwave: PipeWire sink enabled\n");
    return true;
  }

  void stop() {
    if (stream_) {
      pw_stream_destroy(stream_);
      stream_ = nullptr;
    }
    if (core_) {
      pw_core_disconnect(core_);
      core_ = nullptr;
    }
    if (context_) {
      pw_context_destroy(context_);
      context_ = nullptr;
    }
    if (loop_) {
      if (entered_) pw_loop_leave(pw_main_loop_get_loop(loop_));
      pw_main_loop_destroy(loop_);
      loop_ = nullptr;
    }
    entered_ = false;
    push_    = nullptr;
    owner_   = nullptr;
    if (initialized_) {
      pw_deinit();
      initialized_ = false;
    }
  }

  void dispatch() {
    if (loop_) (void)pw_loop_iterate(pw_main_loop_get_loop(loop_), 0);
  }

private:
  static void onState(void *data, pw_stream_state old, pw_stream_state state, const char *error) {
    (void)data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR && error)
      fprintf(stderr, "padwave: PipeWire sink error: %s\n", error);
  }

  static void onProcess(void *data) {
    PipeWireSink *self = static_cast<PipeWireSink *>(data);
    if (!self || !self->stream_ || !self->push_) return;

    pw_buffer *buffer = pw_stream_dequeue_buffer(self->stream_);
    if (!buffer) return;

    spa_buffer *spa = buffer->buffer;
    if (spa && spa->datas && spa->datas[0].data && spa->datas[0].chunk) {
      spa_data *d     = &spa->datas[0];
      uint32_t offset = d->chunk->offset;
      uint32_t size   = d->chunk->size;
      if (offset < d->maxsize) {
        if (size > d->maxsize - offset) size = d->maxsize - offset;
        size_t frames = size / (sizeof(float) * 2);
        if (frames > 0)
          self->push_(self->owner_, static_cast<const float *>(d->data) + offset / sizeof(float),
                      frames);
      }
    }

    pw_stream_queue_buffer(self->stream_, buffer);
  }

  pw_main_loop *loop_  = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_       = nullptr;
  pw_stream *stream_   = nullptr;
  pw_stream_events events_{};
  spa_hook stream_listener_{};
  Push push_        = nullptr;
  void *owner_      = nullptr;
  bool initialized_ = false;
  bool entered_     = false;
};
