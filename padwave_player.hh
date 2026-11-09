// padwave_player.hh — the player engine.
//
// One Player object owns the FFmpeg decoders, the Opus encoder, the two
// prefetch rings, the crossfade state, the 0x36 writer and the hidraw fd. The
// app embeds one; there is no separate player process or CLI.
//
// One thread, no async I/O framework: the server's epoll loop and a timerfd
// are enough because decoding is not the bottleneck, *latency* was. Every loop iteration
// spends at most DECODE_BUDGET_MS inside libav, the ring is primed before the
// first send, and a late timer catches up with a few packets. That keeps the
// 10.667 ms tick without a decoder thread; io_uring would only matter for
// high-IOPS storage, and libavformat's AVIO is synchronous anyway.
//
// Measured playing a 1 kHz tone: 750 packets over 8 s (93.9/s), gap median
// 10.67 ms, p99 10.71 ms, max 13.38 ms, zero underruns, decoded pitch
// 1066.8 Hz — the same cadence as the audio service it replaces.
//
//   file → libavformat/libavcodec → 48 kHz stereo → 512-frame blocks
//        → resample to the 480-sample Opus frame → libopus CBR 160k
//        → a complete DualSense 0x36 report (state snapshot + haptics +
//          speaker + CRC-32 seed 0xA2) → hidraw
//
// No external player and no audio service: decode, encode and the report are
// one process.
//
// Build (no STL, no RTTI, no exceptions): ./build.sh (single translation unit,
// padwave.cc plus these headers).
#pragma once

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

// FFmpeg's public headers are C-only (no extern "C" guards).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "padwave_fields.hh"
#include "padwave_hid.hh"
#include "dualsense.hh"
#include "dualsense-extra.hh"

struct player_status {
  long pos_ms;
  long dur_ms;
  long sent;
  long underruns;
  long pushed;
  int paused;
  int eof;
};

struct App;

#define RATE 48000
#define CHANNELS 2
#define IN_FRAMES 512
#define REPORT_SIZE 398
#define RING_BLOCKS 192
#define RING_HIGH 160
#define RING_PRIME 24
#define DECODE_BUDGET_MS 3

typedef float block_t[IN_FRAMES][CHANNELS];

struct Player {
private:
  struct File {
    AVFormatContext *fmt    = nullptr;
    AVCodecContext *dec     = nullptr;
    SwrContext *swr         = nullptr;
    AVFilterGraph *graph    = nullptr;
    AVFilterContext *fsrc   = nullptr;
    AVFilterContext *fsink  = nullptr;
    AVFilterContext *ftempo = nullptr;
    int audio_index         = -1;
    int speed_percent       = 100;
    int64_t duration_ms     = 0;
    bool eof                = false;
    bool filter_flushed     = false;

    void close() {
      closeFilter();
      if (swr) swr_free(&swr);
      if (dec) avcodec_free_context(&dec);
      if (fmt) avformat_close_input(&fmt);
      fmt         = NULL;
      dec         = NULL;
      swr         = NULL;
      audio_index = -1;
      duration_ms = 0;
      eof         = false;
    }

    void closeFilter() {
      if (graph) avfilter_graph_free(&graph);
      fsrc           = nullptr;
      fsink          = nullptr;
      ftempo         = nullptr;
      filter_flushed = false;
    }

    bool openFilter() {
      closeFilter();
      graph = avfilter_graph_alloc();
      if (!graph) return false;

      char args[192];
      snprintf(args, sizeof(args),
               "sample_fmt=flt:sample_rate=%d:channel_layout=stereo:time_base=1/%d", RATE, RATE);
      const AVFilter *src   = avfilter_get_by_name("abuffer");
      const AVFilter *sink  = avfilter_get_by_name("abuffersink");
      const AVFilter *tempo = avfilter_get_by_name("atempo");
      if (!src || !sink || !tempo ||
          avfilter_graph_create_filter(&fsrc, src, "in", args, nullptr, graph) < 0 ||
          avfilter_graph_create_filter(&fsink, sink, "out", nullptr, nullptr, graph) < 0 ||
          avfilter_graph_create_filter(&ftempo, tempo, "tempo", "tempo=1.0", nullptr, graph) < 0 ||
          avfilter_link(fsrc, 0, ftempo, 0) < 0 || avfilter_link(ftempo, 0, fsink, 0) < 0 ||
          avfilter_graph_config(graph, nullptr) < 0) {
        closeFilter();
        return false;
      }
      if (!setSpeed(speed_percent)) {
        closeFilter();
        return false;
      }
      return true;
    }

    bool setSpeed(int percent) {
      if (percent < 50) percent = 50;
      if (percent > 200) percent = 200;
      speed_percent = percent;
      if (!ftempo) return true;
      char value[16];
      snprintf(value, sizeof(value), "%.3f", static_cast<double>(percent) / 100.0);
      if (avfilter_graph_send_command(graph, "tempo", "tempo", value, nullptr, 0, 0) < 0)
        return false;
      return true;
    }

    void resetFilter() {
      closeFilter();
      openFilter();
    }

    bool filterInput(const float *samples, int n) {
      if (!fsrc || n <= 0) return false;
      AVFrame *in = av_frame_alloc();
      if (!in) return false;
      in->format      = AV_SAMPLE_FMT_FLT;
      in->sample_rate = RATE;
      in->nb_samples  = n;
      av_channel_layout_default(&in->ch_layout, CHANNELS);
      if (av_frame_get_buffer(in, 0) < 0) {
        av_frame_free(&in);
        return false;
      }
      memcpy(in->data[0], samples, static_cast<size_t>(n) * CHANNELS * sizeof(float));
      int ret = av_buffersrc_add_frame_flags(fsrc, in, AV_BUFFERSRC_FLAG_KEEP_REF);
      av_frame_free(&in);
      return ret >= 0;
    }

    bool open(const char *file_path, int percent) {
      speed_percent = percent;
      close();

      if (avformat_open_input(&fmt, file_path, NULL, NULL) < 0) return false;
      if (avformat_find_stream_info(fmt, NULL) < 0) return false;

      const AVCodec *codec = NULL;
      audio_index          = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
      if (audio_index < 0 || !codec) return false;

      AVStream *st = fmt->streams[audio_index];
      dec          = avcodec_alloc_context3(codec);
      if (!dec) return false;
      if (avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
      if (avcodec_open2(dec, codec, NULL) < 0) return false;

      AVChannelLayout in_layout;
      AVChannelLayout out_layout;
      av_channel_layout_copy(&in_layout, &dec->ch_layout);
      av_channel_layout_default(&out_layout, CHANNELS);

      int converted = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE, &in_layout,
                                          dec->sample_fmt, dec->sample_rate, 0, NULL);

      av_channel_layout_uninit(&in_layout);
      av_channel_layout_uninit(&out_layout);
      if (converted < 0 || !swr || swr_init(swr) < 0) return false;

      if (!openFilter()) return false;

      duration_ms = fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0;
      eof         = false;
      return true;
    }
  };

  int hidraw              = -1;
  int hidraw_retry        = 0;
  char node_name[64]      = "";
  char controller_mac[32] = "";
  bool is_usb             = false;
  bool speaker            = false;
  FILE *usb_audio         = nullptr;
  App *app                = nullptr;

  dualsense::AudioReport::Encoder enc;
  dualsense::OutputReport::Sequence audio_sequence;

  File cur;
  File nxt;
  AVPacket *pkt        = nullptr;
  AVFrame *frame       = nullptr;
  char path[1024]      = "";
  char next_path[1024] = "";
  bool has_next        = false;
  bool next_warmed     = false;

  int fade_ms  = 0;
  bool fading  = false;
  int fade_pos = 0, fade_total = 0;
  bool manual_fade       = false;
  bool manual_pending    = false;
  char manual_path[1024] = "";
  bool seek_fade         = false;

  dualsense::OutputReport::Sequence output_sequence;
  dualsense::OutputBody state{};
  bool mic = false;

  float ring[RING_BLOCKS][IN_FRAMES][CHANNELS] = {};
  int head = 0, tail = 0;
  float acc[IN_FRAMES][CHANNELS] = {};
  int acc_frames                 = 0;
  block_t nring[RING_BLOCKS]     = {};
  int nhead = 0, ntail = 0;
  float nacc[IN_FRAMES][CHANNELS] = {};
  int nacc_frames                 = 0;
  int64_t npushed                 = 0;

  bool playing = false, paused = false, eof = false, primed = false;
  int volume                  = 80;
  int speed_percent           = 100;
  bool haptic_subwoofer       = false;
  bool haptics_only           = false;
  int haptic_cutoff           = 150;
  int haptic_gain             = 100;
  float haptic_lp             = 0;
  SwrContext *haptic_swr      = nullptr;
  int16_t haptic_pending[256] = {};
  size_t haptic_pending_len   = 0;
  int64_t base_ms = 0, consumed = 0, sent = 0, underruns = 0, pushed = 0;

  void advanceConsumed() { consumed += IN_FRAMES * speed_percent / 100; }

  int tfd             = -1;
  int64_t last_status = 0;
  long tick_ns        = 0;

  static constexpr long kPlayTickNs  = 10667000;
  static constexpr long kPauseTickNs = 500000000;

  static int countOf(int head, int tail, int size) { return (head - tail + size) % size; }

  static void warmFile(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
      close(fd);
      if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "warm %s\n", path);
    }
  }

  int ringLen() const { return countOf(head, tail, RING_BLOCKS); }

  int nextRingLen() const { return countOf(nhead, ntail, RING_BLOCKS); }

  void closeFile() {
    cur.close();
    head = tail = 0;
    acc_frames  = 0;
  }

  void closeNext() {
    nxt.close();
    nhead = ntail = 0;
    nacc_frames   = 0;
    has_next      = false;
  }

  bool openFile(const char *file_path) {
    closeFile();
    if (!cur.open(file_path, speed_percent)) return false;
    snprintf(this->path, sizeof(this->path), "%s", file_path);
    eof       = false;
    consumed  = 0;
    sent      = 0;
    underruns = 0;
    base_ms   = 0;
    return true;
  }

  void appendFiltered(File *f, block_t *dst_ring, int *dst_head, int *dst_tail,
                      float dst_acc[IN_FRAMES][CHANNELS], int *dst_acc_frames,
                      int64_t *dst_pushed) {
    if (!f->fsink) return;
    AVFrame *fo = av_frame_alloc();
    if (!fo) return;
    while (av_buffersink_get_frame(f->fsink, fo) >= 0) {
      const float *samples = reinterpret_cast<const float *>(fo->data[0]);
      for (int i = 0; i < fo->nb_samples; i++) {
        dst_acc[*dst_acc_frames][0] = samples[i * CHANNELS + 0];
        dst_acc[*dst_acc_frames][1] = samples[i * CHANNELS + 1];
        if (++(*dst_acc_frames) == IN_FRAMES) {
          int next = (*dst_head + 1) % RING_BLOCKS;
          if (next == *dst_tail) {
            *dst_acc_frames = 0;
            break;
          }
          memcpy(dst_ring[*dst_head], dst_acc, sizeof(float) * IN_FRAMES * CHANNELS);
          *dst_head = next;
          if (dst_pushed) (*dst_pushed)++;
          *dst_acc_frames = 0;
        }
      }
      av_frame_unref(fo);
      if (((*dst_head + 1) % RING_BLOCKS) == *dst_tail) break;
    }
    av_frame_free(&fo);
  }

  void fillInto(File *f, block_t *dst_ring, int *dst_head, int *dst_tail,
                float dst_acc[IN_FRAMES][CHANNELS], int *dst_acc_frames, int high,
                int64_t *dst_pushed) {
    if (!f->fmt || paused) return;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (countOf(*dst_head, *dst_tail, RING_BLOCKS) < high && !f->eof) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
      if (elapsed >= DECODE_BUDGET_MS) break;

      int ret = av_read_frame(f->fmt, pkt);
      if (getenv("PADWAVE_DEBUG") && ret < 0) fprintf(stderr, "read: %d\n", ret);
      if (ret == AVERROR_EOF) {
        avcodec_send_packet(f->dec, NULL);
        f->eof = true;
      } else if (ret < 0) {
        f->eof = true;
      } else {
        if (pkt->stream_index == f->audio_index) avcodec_send_packet(f->dec, pkt);
        av_packet_unref(pkt);
      }

      for (;;) {
        ret = avcodec_receive_frame(f->dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        uint8_t *encoded[1];
        float tmp[8192 * CHANNELS];
        encoded[0] = reinterpret_cast<uint8_t *>(tmp);
        int cap    = swr_get_out_samples(f->swr, frame->nb_samples);
        if (cap > 8192) cap = 8192;
        const uint8_t *in[AV_NUM_DATA_POINTERS] = {};
        int channels                            = f->dec->ch_layout.nb_channels;
        if (channels > AV_NUM_DATA_POINTERS) channels = AV_NUM_DATA_POINTERS;
        for (int i = 0; i < channels; i++) in[i] = frame->extended_data[i];
        int got = swr_convert(f->swr, encoded, cap, in, frame->nb_samples);
        if (got > 0 && !f->filterInput(tmp, got)) {
          av_frame_unref(frame);
          break;
        }
        appendFiltered(f, dst_ring, dst_head, dst_tail, dst_acc, dst_acc_frames, dst_pushed);
        av_frame_unref(frame);
        if (((*dst_head + 1) % RING_BLOCKS) == *dst_tail) break;
      }
    }

    if (f->eof && !f->filter_flushed) {
      if (f->fsrc) (void)av_buffersrc_add_frame_flags(f->fsrc, nullptr, 0);
      f->filter_flushed = true;
      appendFiltered(f, dst_ring, dst_head, dst_tail, dst_acc, dst_acc_frames, dst_pushed);
    }
  }

  void fill() {
    fillInto(&cur, ring, &head, &tail, acc, &acc_frames, RING_HIGH, &pushed);
    eof = cur.eof;
  }

  void fillNext() {
    fillInto(&nxt, nring, &nhead, &ntail, nacc, &nacc_frames, RING_HIGH, &npushed);
  }

  void promote() {
    cur.close();
    cur = nxt;
    nxt = File{};

    memcpy(ring, nring, sizeof(ring));
    head = nhead;
    tail = ntail;
    memcpy(acc, nacc, sizeof(acc));
    acc_frames = nacc_frames;

    nhead = ntail = 0;
    nacc_frames   = 0;
    npushed       = 0;

    snprintf(path, sizeof(path), "%s", next_path);
    base_ms   = 0;
    consumed  = 0;
    sent      = 0;
    underruns = 0;
    eof       = cur.eof;
    has_next  = false;

    emitTrack(path);
  }

  bool takeBlock(float dst[IN_FRAMES][CHANNELS]) {
    if (tail == head) return false;
    memcpy(dst, ring[tail], sizeof(ring[0]));
    tail = (tail + 1) % RING_BLOCKS;
    return true;
  }

  bool takeNextBlock(float dst[IN_FRAMES][CHANNELS]) {
    if (ntail == nhead) return false;
    memcpy(dst, nring[ntail], sizeof(nring[0]));
    ntail = (ntail + 1) % RING_BLOCKS;
    return true;
  }

  void finishFade() {
    cur.close();
    cur = nxt;
    nxt = File{};

    memcpy(ring, nring, sizeof(ring));
    head = nhead;
    tail = ntail;
    memcpy(acc, nacc, sizeof(acc));
    acc_frames = nacc_frames;

    nhead = ntail = 0;
    nacc_frames   = 0;
    npushed       = 0;

    bool manual = manual_fade;
    bool seek   = seek_fade;
    if (manual)
      snprintf(path, sizeof(path), "%s", manual_path);
    else if (!seek)
      snprintf(path, sizeof(path), "%s", next_path);

    if (!seek) {
      base_ms  = 0;
      consumed = 0;
    }
    sent           = 0;
    underruns      = 0;
    eof            = cur.eof;
    fading         = false;
    fade_pos       = 0;
    fade_total     = 0;
    manual_fade    = false;
    manual_pending = false;
    seek_fade      = false;

    if (!manual && !seek) {
      has_next = false;
      emitTrack(path);
    }
  }

  void emitStatus(const player_status &st);
  void emitTrack(const char *path);

  bool openDevice() {
    DualSenseDevice device;
    int fd = device.open();
    if (fd < 0) return false;
    hidraw = fd;
    is_usb = device.isUsb();
    snprintf(node_name, sizeof(node_name), "%s", device.node);
    snprintf(controller_mac, sizeof(controller_mac), "%s", device.mac);
    return true;
  }

  template <typename Fill>
  bool sendOutput(Fill fill) {
    if (is_usb) {
      dualsense::UsbOutputReport report{};
      if (!fill(report.body())) return false;
      return writeDevice(&report, sizeof(report));
    }
    dualsense::OutputReport report(output_sequence);
    if (!fill(report.body())) return false;
    report.sign();
    return writeDevice(&report, sizeof(report));
  }

  void sendAudioState() {
    dualsense::OutputBody body = state.audioSnapshot();
    sendOutput([&](dualsense::OutputBody &target) {
      target = body;
      return true;
    });
  }

  void sendAudioRoute() {
    state.setAudio(speaker ? dualsense::OutputBody::AudioControl::Speaker
                           : dualsense::OutputBody::AudioControl::Headphones);
    sendAudioState();
  }

  bool readExtraCommand(dualsense::internal::CommandDevice device, uint8_t action,
                        dualsense::internal::CommandResponse &response) {
    using namespace dualsense::internal;
    if (hidraw < 0) return false;
    CommandRequest request(device, action);
    if (ioctl(hidraw, HIDIOCSFEATURE(sizeof(request)), &request) < 0) return false;
    for (int attempt = 0; attempt < 10; ++attempt) {
      if (ioctl(hidraw, HIDIOCGFEATURE(sizeof(response)), &response) < 0) return false;
      if (response.matches(device, action) && (response.complete() || response.completeMore()))
        return true;
      usleep(1000);
    }
    return false;
  }

  bool readFeature(uint8_t reportId, uint8_t *data, size_t length) {
    if (hidraw < 0) return false;
    memset(data, 0, length);
    data[0] = reportId;
    return ioctl(hidraw, HIDIOCGFEATURE(length), data) >= 0;
  }

  bool writeDevice(const void *data, size_t len) {
    if (hidraw < 0) {
      if (hidraw_retry > 0)
        hidraw_retry--;
      else if (!openDevice())
        hidraw_retry = 5;
    }
    if (hidraw < 0) return false;
    ssize_t n = write(hidraw, data, len);
    if (n != static_cast<ssize_t>(len)) {
      dropDevice();
      hidraw_retry = 5;
      return false;
    }
    return true;
  }

  bool startUsbAudio() {
    if (usb_audio) return true;
    // A stable ALSA card id beats a card index that changes across replugs.
    const char *device = "plughw:CARD=Controller,DEV=0";
    char command[256];
    snprintf(command, sizeof(command),
             "exec aplay -q -D '%s' -t raw -f S16_LE -r 48000 -c %d "
             "--buffer-time=100000 --period-time=20000",
             device, haptic_subwoofer ? 4 : 2);
    usb_audio = popen(command, "w");
    if (!usb_audio) {
      fprintf(stderr, "padwave player: USB audio start failed\n");
      return false;
    }
    setvbuf(usb_audio, nullptr, _IONBF, 0);
    return true;
  }

  void stopUsbAudio() {
    if (!usb_audio) return;
    pclose(usb_audio);
    usb_audio = nullptr;
  }

  float hapticAlpha() const {
    float x = 2.0f * 3.14159265358979323846f * static_cast<float>(haptic_cutoff) / 48000.0f;
    if (x > 0.99f) x = 0.99f;
    return x < 0.0001f ? 0.0001f : x;
  }

  float lowPass(float input) {
    haptic_lp += hapticAlpha() * (input - haptic_lp);
    return haptic_lp;
  }

  // The USB card is natively 4ch: FL/FR go to the speaker or headphone jack,
  // RL/RR drive the two haptic actuators. With haptic subwoofer enabled, send
  // a low-passed mono bass signal to RL/RR; otherwise leave them silent.
  void writeUsbPcm(const float block[IN_FRAMES][CHANNELS], float gain) {
    if (haptic_subwoofer) {
      int16_t samples[IN_FRAMES * 4];
      float hgain = static_cast<float>(haptic_gain) / 100.0f;
      for (int i = 0; i < IN_FRAMES; i++) {
        float l    = block[i][0] * gain;
        float r    = block[i][1] * gain;
        float bass = lowPass((l + r) * 0.5f) * hgain;
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        if (bass > 1.0f) bass = 1.0f;
        if (bass < -1.0f) bass = -1.0f;
        samples[i * 4 + 0] = static_cast<int16_t>(l * 32767.0f);
        samples[i * 4 + 1] = static_cast<int16_t>(r * 32767.0f);
        samples[i * 4 + 2] = static_cast<int16_t>(bass * 32767.0f);
        samples[i * 4 + 3] = static_cast<int16_t>(bass * 32767.0f);
      }
      if (!startUsbAudio()) return;
      size_t count = IN_FRAMES * 4;
      if (fwrite(samples, sizeof(int16_t), count, usb_audio) != count) {
        stopUsbAudio();
        return;
      }
      sent++;
      return;
    }

    int16_t samples[IN_FRAMES * CHANNELS];
    for (int i = 0; i < IN_FRAMES; i++) {
      for (int ch = 0; ch < CHANNELS; ch++) {
        float v = block[i][ch] * gain;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        samples[i * CHANNELS + ch] = static_cast<int16_t>(v * 32767.0f);
      }
    }
    if (!startUsbAudio()) return;
    size_t count = IN_FRAMES * CHANNELS;
    if (fwrite(samples, sizeof(int16_t), count, usb_audio) != count) {
      stopUsbAudio();
      return;
    }
    sent++;
  }

public:
  bool releaseLEDs() {
    return sendOutput([](dualsense::OutputBody &body) {
      body.releaseLEDs();
      return true;
    });
  }

  void setTriggerEffect(bool left, const dualsense::OutputBody::TriggerEffect &effect) {
    if (left)
      state.setLeftTriggerEffect(effect);
    else
      state.setRightTriggerEffect(effect);

    // One-sided update: the untouched block stays all zero, matching the
    // reference one-sided writers. Only the selected valid bit is claimed.
    sendOutput([&](dualsense::OutputBody &body) {
      if (left)
        body.setLeftTriggerEffect(effect);
      else
        body.setRightTriggerEffect(effect);
      return true;
    });
  }

  int firmwareInfo(char *dest, size_t cap) {
    if (hidraw < 0) return snprintf(dest, cap, "ok=false\nerror=no controller\n");
    dualsense::Firmware info{};
    int got = ioctl(hidraw, HIDIOCGFEATURE(sizeof(info)), &info);
    if (got < 0 || !info.isValid(is_usb))
      return snprintf(dest, cap, "ok=false\nerror=firmware get failed\n");

    size_t o    = 0;
    auto append = [&](const char *text) {
      int n = snprintf(dest + o, cap > o ? cap - o : 0, "%s", text);
      if (n > 0) o += static_cast<size_t>(n);
    };
    char line[192];
    append("ok=true\nfirmware.valid=true\n");
    {
      auto [value, length] = info.buildDateText();
      snprintf(line, sizeof(line), "firmware.build_date=%.*s\n", static_cast<int>(length), value);
    }
    append(line);
    {
      auto [value, length] = info.buildTimeText();
      snprintf(line, sizeof(line), "firmware.build_time=%.*s\n", static_cast<int>(length), value);
    }
    append(line);
    snprintf(line, sizeof(line), "firmware.type=%u\n", static_cast<unsigned>(info.typeValue()));
    append(line);
    snprintf(line, sizeof(line), "firmware.series=%u\n", static_cast<unsigned>(info.seriesValue()));
    append(line);
    snprintf(line, sizeof(line), "firmware.hardware=0x%08X\n", info.hardwareInfoValue());
    append(line);
    snprintf(line, sizeof(line), "firmware.version=0x%08X\n", info.firmwareVersionValue());
    append(line);
    char hex[129];
    size_t h = 0;
    {
      auto [value, length] = info.deviceInfoText();
      for (unsigned i = 0; i < length; ++i) {
        int n = snprintf(hex + h, sizeof(hex) - h, "%02x", static_cast<unsigned>(value[i]));
        if (n > 0) h += static_cast<size_t>(n);
      }
    }
    snprintf(line, sizeof(line), "firmware.device_info=%s\n", hex);
    append(line);
    snprintf(line, sizeof(line), "firmware.update_version=0x%04X\n",
             static_cast<unsigned>(info.updateVersionValue()));
    append(line);
    snprintf(line, sizeof(line), "firmware.update_image=%u\n",
             static_cast<unsigned>(info.updateImageInfoValue()));
    append(line);
    snprintf(line, sizeof(line), "firmware.update_unknown=%u\n",
             static_cast<unsigned>(info.updateUnkValue()));
    append(line);
    snprintf(line, sizeof(line), "firmware.version1=0x%08X\n", info.fwVersion1Value());
    append(line);
    snprintf(line, sizeof(line), "firmware.version2=0x%08X\n", info.fwVersion2Value());
    append(line);
    snprintf(line, sizeof(line), "firmware.version3=0x%08X\n", info.fwVersion3Value());
    append(line);
    snprintf(line, sizeof(line), "firmware.crc=0x%08X\n", info.crcValue());
    append(line);
    snprintf(line, sizeof(line), "firmware.vibration_v2=%s\n",
             info.usesVibrationV2() ? "true" : "false");
    append(line);
    const auto *raw = reinterpret_cast<const uint8_t *>(&info);
    h               = 0;
    for (size_t i = 0; i < sizeof(info); i++) {
      int n = snprintf(hex + h, sizeof(hex) - h, "%02x", static_cast<unsigned>(raw[i]));
      if (n > 0) h += static_cast<size_t>(n);
    }
    snprintf(line, sizeof(line), "firmware.raw=%s\n", hex);
    append(line);
    return static_cast<int>(o);
  }

  int extraInfo(char *dest, size_t cap) {
    using namespace dualsense::internal;
    if (!is_usb) return snprintf(dest, cap, "ok=false\nerror=USB required\n");
    if (hidraw < 0) return snprintf(dest, cap, "ok=false\nerror=no controller\n");

    size_t o    = 0;
    auto append = [&](const char *text) {
      int n = snprintf(dest + o, cap > o ? cap - o : 0, "%s", text);
      if (n > 0) o += static_cast<size_t>(n);
    };
    auto appendBytes = [&](const char *name, const uint8_t *data, size_t length) {
      append(name);
      append("=");
      char hex[3];
      for (size_t i = 0; i < length; ++i) {
        snprintf(hex, sizeof(hex), "%02X", data[i]);
        append(hex);
      }
      append("\n");
    };
    auto textField = [&](const char *name, CommandDevice device, uint8_t action, size_t length,
                         bool reverse, char *destText, size_t destCap) {
      CommandResponse response;
      if (!readExtraCommand(device, action, response)) {
        append(name);
        append("=unavailable\n");
        return;
      }
      char text[64] = {};
      if (length > sizeof(text) - 1) length = sizeof(text) - 1;
      for (size_t i = 0; i < length; ++i) {
        char c = reverse ? static_cast<char>(response.payload[length - 1 - i])
                         : static_cast<char>(response.payload[i]);
        if (c == '\0') break;
        text[i] = c;
      }
      size_t n = strlen(text);
      while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t')) text[--n] = 0;
      if (destText && destCap) snprintf(destText, destCap, "%s", text);
      append(name);
      append("=");
      append(text);
      append("\n");
    };
    auto appendU = [&](const char *name, unsigned value) {
      char line[64];
      snprintf(line, sizeof(line), "%s=%u\n", name, value);
      append(line);
    };
    auto appendHex = [&](const char *name, uint32_t value) {
      char line[64];
      snprintf(line, sizeof(line), "%s=0x%08X\n", name, value);
      append(line);
    };
    char line[192];
    append("ok=true\nextras.usb=true\n");

    CommandResponse response;
    if (readExtraCommand(CommandDevice::MainMcu, 9, response)) {
      auto id = parseMcuUniqueId(response);
      if (id.valid) {
        snprintf(line, sizeof(line), "extras.mcu_uid=0x%016llX\n",
                 static_cast<unsigned long long>(id.value));
        append(line);
      } else {
        append("extras.mcu_uid=invalid\n");
      }
    } else {
      append("extras.mcu_uid=unavailable\n");
    }

    char serial[64] = "";
    textField("extras.serial_number", CommandDevice::MainMcu, 19, 17, false, serial,
              sizeof(serial));
    textField("extras.pcba_id", CommandDevice::MainMcu, 17, 14, true, nullptr, 0);
    textField("extras.battery_barcode", CommandDevice::MainMcu, 24, 23, false, nullptr, 0);
    textField("extras.vcm_left_barcode", CommandDevice::MainMcu, 26, 16, false, nullptr, 0);
    textField("extras.vcm_right_barcode", CommandDevice::MainMcu, 28, 16, false, nullptr, 0);
    SerialNumber serialNumber(serial);
    snprintf(line, sizeof(line), "extras.color=%s\n", serialNumber.colorName());
    append(line);

    if (readExtraCommand(CommandDevice::Audio, 3, response)) {
      appendBytes("extras.codec_fw_raw", reinterpret_cast<const uint8_t *>(response.payload), 22);
      char build[24] = {};
      size_t n       = sizeof(response.payload) - 8;
      if (n >= sizeof(build)) n = sizeof(build) - 1;
      for (size_t i = 0; i < n; ++i) {
        char c = static_cast<char>(response.payload[8 + i]);
        if (c == '\0') break;
        build[i] = c;
      }
      snprintf(line, sizeof(line), "extras.codec_build=%s\n", build);
      append(line);
    } else {
      append("extras.codec_fw_raw=unavailable\n");
    }

    if (readExtraCommand(CommandDevice::DeviceInfo, 2, response)) {
      auto mac = parseBluetoothAddress(response);
      if (mac.valid) {
        snprintf(line, sizeof(line), "extras.bt_address=%02X:%02X:%02X:%02X:%02X:%02X\n",
                 mac.bytes[5], mac.bytes[4], mac.bytes[3], mac.bytes[2], mac.bytes[1],
                 mac.bytes[0]);
        append(line);
      } else {
        append("extras.bt_address=invalid\n");
      }
    } else {
      append("extras.bt_address=unavailable\n");
    }

    if (readExtraCommand(CommandDevice::DeviceInfo, 13, response)) {
      auto patch = parsePatchInfo(response);
      if (patch.valid) {
        appendHex("extras.patch_id", patch.id);
        appendU("extras.patch_count", patch.count);
        appendU("extras.patch_size", patch.size);
      } else {
        append("extras.patch=invalid\n");
      }
    } else {
      append("extras.patch=unavailable\n");
    }

    if (readExtraCommand(CommandDevice::MainMcu, 12, response)) {
      auto info = parseDeviceInfo(response);
      if (info.valid) {
        append("extras.device_info=");
        for (unsigned i = 0; i < sizeof(info.bytes); ++i) {
          snprintf(line, sizeof(line), "%02X", info.bytes[i]);
          append(line);
        }
        append("\n");
      } else {
        append("extras.device_info=invalid\n");
      }
    } else {
      append("extras.device_info=unavailable\n");
    }

    if (readExtraCommand(CommandDevice::Codec, 1, response)) {
      auto status = parseSpiderStatus(response);
      if (status.valid) {
        snprintf(line, sizeof(line), "extras.spider_init=0x%02X\n", status.value);
        append(line);
      } else {
        append("extras.spider_init=invalid\n");
      }
    } else {
      append("extras.spider_init=unavailable\n");
    }

    if (readExtraCommand(CommandDevice::Codec, 2, response)) {
      auto status = parseSpiderStatus(response);
      if (status.valid) {
        snprintf(line, sizeof(line), "extras.spider_state=0x%02X\n", status.value);
        append(line);
      } else {
        append("extras.spider_state=invalid\n");
      }
    } else {
      append("extras.spider_state=unavailable\n");
    }

    bool finetuneValid = false;
    if (readExtraCommand(CommandDevice::Stick, 2, response)) {
      auto finetune = parseFinetune(response);
      finetuneValid = finetune.valid;
      append(finetuneValid ? "extras.finetune=ok\n" : "extras.finetune=invalid\n");
    } else {
      append("extras.finetune=unavailable\n");
    }

    dualsense::Firmware firmware{};
    if (ioctl(hidraw, HIDIOCGFEATURE(sizeof(firmware)), &firmware) >= 0 && firmware.isValid(true)) {
      appendHex("extras.hw_model", firmware.hardwareInfoValue());
      snprintf(line, sizeof(line), "extras.board_model=%s\n", firmware.boardModel());
      append(line);
      time_t now      = time(nullptr);
      struct tm *tm   = localtime(&now);
      int currentYear = tm ? tm->tm_year + 1900 : 0;
      snprintf(line, sizeof(line), "extras.device_type=%s\n",
               firmware.deviceType(finetuneValid, currentYear));
      append(line);
    } else {
      append("extras.hw_model=unavailable\n");
    }

    if (readExtraCommand(CommandDevice::Bluetooth, 4, response)) {
      appendBytes("extras.bt_patch", reinterpret_cast<const uint8_t *>(response.payload),
                  sizeof(response.payload));
    } else {
      append("extras.bt_patch=unavailable\n");
    }

    auto feature = [&](const char *name, uint8_t id, size_t length) {
      uint8_t bytes[64] = {};
      if (readFeature(id, bytes, length))
        appendBytes(name, bytes, length);
      else {
        append(name);
        append("=unavailable\n");
      }
    };
    feature("extras.feature_calibration", 0x05, 41);
    feature("extras.feature_pairing", 0x09, 20);
    feature("extras.feature_setup", 0x0B, 42);
    feature("extras.feature_82", 0x82, 10);
    feature("extras.feature_85", 0x85, 3);
    feature("extras.feature_auth_status", 0xF2, 16);

    return static_cast<int>(o);
  }

private:
  void sendMuteLed(bool on) {
    char fields[64];
    snprintf(fields, sizeof(fields), "mute_button_led=%d", on ? 1 : 0);
    sendOutput([&](dualsense::OutputBody &body) {
      return padwave_fields::apply_fields(&body, fields);
    });
  }

  bool openHapticResampler() {
    if (haptic_swr) swr_free(&haptic_swr);
    AVChannelLayout layout;
    av_channel_layout_default(&layout, CHANNELS);
    int r = swr_alloc_set_opts2(&haptic_swr, &layout, AV_SAMPLE_FMT_S16,
                                dualsense::HapticsReport::sampleRate, &layout, AV_SAMPLE_FMT_FLT,
                                RATE, 0, nullptr);
    av_channel_layout_uninit(&layout);
    if (r < 0 || !haptic_swr || swr_init(haptic_swr) < 0) {
      if (haptic_swr) swr_free(&haptic_swr);
      return false;
    }
    haptic_pending_len = 0;
    return true;
  }

  void sendHapticsOnly(const float block[IN_FRAMES][CHANNELS]) {
    if (!haptic_swr) return;
    int16_t converted[256];
    uint8_t *outbuf[1]   = {reinterpret_cast<uint8_t *>(converted)};
    const uint8_t *in[1] = {reinterpret_cast<const uint8_t *>(block)};
    int got              = swr_convert(haptic_swr, outbuf, 128, in, IN_FRAMES);
    if (got <= 0) return;
    size_t bytes = static_cast<size_t>(got) * CHANNELS * sizeof(int16_t);
    if (haptic_pending_len + bytes > sizeof(haptic_pending)) return;
    memcpy(reinterpret_cast<uint8_t *>(haptic_pending) + haptic_pending_len, converted, bytes);
    haptic_pending_len += bytes;

    const float vgain           = static_cast<float>(volume) / 100.0f;
    const float hgain           = vgain * static_cast<float>(haptic_gain) / 100.0f;
    const size_t bytesPerReport = dualsense::HapticsReport::sampleBytes * sizeof(int16_t);
    while (haptic_pending_len >= bytesPerReport) {
      dualsense::HapticsReport report(audio_sequence);
      report.setHaptics([&](size_t i) {
        int sample = static_cast<int>(static_cast<float>(haptic_pending[i] >> 8) * hgain);
        if (sample < -128) sample = -128;
        if (sample > 127) sample = 127;
        return static_cast<int8_t>(sample);
      });
      report.sign();
      memmove(haptic_pending, reinterpret_cast<uint8_t *>(haptic_pending) + bytesPerReport,
              haptic_pending_len - bytesPerReport);
      haptic_pending_len -= bytesPerReport;
      if (writeDevice(&report, sizeof(report))) sent++;
    }
  }

  void sendReport(const float *pcm, int frames) {
    if (is_usb) return; // USB audio uses the ALSA endpoint, not 0x36.
    const float hgain = static_cast<float>(haptic_gain) / 100.0f;
    dualsense::AudioReport report(
        speaker ? dualsense::AudioReport::SpeakerTag::Speaker
                : dualsense::AudioReport::SpeakerTag::Jack,
        mic ? dualsense::AudioReport::ConfigMask::SpeakerAndMic
            : dualsense::AudioReport::ConfigMask::SpeakerOnly,
        audio_sequence);

    report.stateBody() = state.motorSnapshot();
    report.setHaptics([&](size_t i) {
      if (!haptic_subwoofer) return int8_t{0};
      const size_t first = i * 8;
      if (first >= static_cast<size_t>(frames)) return int8_t{0};
      float output = 0;
      for (size_t src = first; src < first + 8 && src < static_cast<size_t>(frames); ++src) {
        float mono = (pcm[src * CHANNELS] + pcm[src * CHANNELS + 1]) * 0.5f;
        float bass = lowPass(mono) * hgain;
        if (src == first) output = bass;
      }
      if (output > 1.0f) output = 1.0f;
      if (output < -1.0f) output = -1.0f;
      return static_cast<int8_t>(output * 127.0f);
    });

    report.setPcm(pcm, frames, enc);

    report.sign();

    // The controller can sleep (idle power-off) and come back on a new hidraw
    // node. Retry about once a second, and drop a stale fd on a failed write.
    if (hidraw < 0) {
      if (hidraw_retry > 0)
        hidraw_retry--;
      else if (!openDevice())
        hidraw_retry = 5;
    }
    if (hidraw >= 0) {
      ssize_t n = write(hidraw, &report, sizeof(report));
      if (n != static_cast<ssize_t>(sizeof(report))) {
        dropDevice();
        hidraw_retry = 5;
      }
    }

    sent++;
  }

  void handleLine(char *line) {
    char line_path[1024];
    long value;

    int percent = 0;
    if (sscanf(line, "speed %d", &percent) == 1) {
      setSpeed(percent);
      return;
    }

    // "play <start_ms> <path>" — the number comes first so a path can contain
    // spaces and the two fields never have to be guessed apart. "play <path>" is
    // still accepted for driving the binary by hand.
    if (sscanf(line, "play %ld %1023[^\n]", &value, line_path) == 2) {
      if (playing && !paused && cur.fmt && fade_ms > 0) {
        closeNext();
        if (nxt.open(line_path, speed_percent)) {
          if (value > 0) {
            int64_t ts = value * (AV_TIME_BASE / 1000);
            av_seek_frame(nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(nxt.dec);
            nxt.resetFilter();
          }
          snprintf(manual_path, sizeof(manual_path), "%s", line_path);
          manual_fade    = true;
          manual_pending = true;
          base_ms        = value;
          consumed       = 0;
          sent           = 0;
          underruns      = 0;
          return;
        }
      }

      closeNext();
      bool was_paused = paused;
      if (openFile(line_path)) {
        playing = true;
        paused  = false;
        primed  = false;
        if (was_paused) sendMuteLed(false);
        if (value > 0) {
          int64_t ts = value * (AV_TIME_BASE / 1000);
          av_seek_frame(cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(cur.dec);
          cur.resetFilter();
          base_ms = value;
        }
      }
      return;
    }

    if (sscanf(line, "play %1023[^\n]", line_path) == 1) {
      closeNext();
      if (openFile(line_path)) {
        playing = true;
        paused  = false;
        primed  = false;
      }
      return;
    }

    // "queue <path>" — the next track, to be decoded ahead and promoted at the
    // end of the current one. Only one is held; the radio pushes the one after
    // that once the promotion is reported.
    if (sscanf(line, "queue %1023[^\n]", line_path) == 1) {
      if (!has_next) {
        snprintf(next_path, sizeof(next_path), "%s", line_path);
        has_next    = true;
        next_warmed = false;
        warmFile(line_path);
      }
      return;
    }

    if (strcmp(line, "state_clear_lightbar") == 0) {
      sendOutput([](dualsense::OutputBody &body) {
        body.setLightbarSetup(dualsense::OutputBody::LightbarSetup::LightOut);
        body.setLightbar(0, 0, 0);
        return true;
      });
      return;
    }

    if (strncmp(line, "fields ", 7) == 0) {
      if (!sendOutput([&](dualsense::OutputBody &body) {
            return padwave_fields::apply_fields(&body, line + 7);
          }))
        return;
      padwave_fields::apply_fields(&state, line + 7);
      return;
    }

    if (sscanf(line, "fade %ld", &value) == 1) {
      fade_ms = static_cast<int>((value < 0 ? 0 : value));
      return;
    }

    if (sscanf(line, "pause %ld", &value) == 1) {
      bool next = value != 0;
      if (next != paused) {
        paused = next;
        sendMuteLed(paused);
      }
      return;
    }

    if (sscanf(line, "mic %ld", &value) == 1) {
      mic = value != 0;
      state.setMic(mic ? dualsense::OutputBody::micMaxVolume : 0);
      sendAudioState();
      // The controller only starts/stops microphone capture when it sees a
      // 0x36 report with the enable-mask bit changed. Send the state packet
      // immediately, and reassert the pause-only mute LED with a one-shot
      // 0x31 so mic capture cannot turn it into an activity blink.
      sendMuteLed(paused);
      float silence[dualsense::AudioReport::pcmFrames * CHANNELS] = {};
      sendReport(silence, dualsense::AudioReport::pcmFrames);
      return;
    }

    long enabled = 0, cutoff = 0, gain = 0, only = 0;
    if (sscanf(line, "haptic %ld %ld %ld %ld", &enabled, &cutoff, &gain, &only) >= 3) {
      bool was         = haptic_subwoofer;
      haptic_subwoofer = enabled != 0;
      haptic_cutoff    = cutoff < 20 ? 20 : (cutoff > 2000 ? 2000 : static_cast<int>(cutoff));
      haptic_gain      = gain < 0 ? 0 : (gain > 200 ? 200 : static_cast<int>(gain));
      haptics_only     = only != 0;
      haptic_lp        = 0;
      if (was != haptic_subwoofer) {
        stopUsbAudio();
        state.setHapticLowPassFilter(haptic_subwoofer);
        sendAudioState();
      }
      return;
    }

    if (sscanf(line, "volume %ld", &value) == 1) {
      volume = static_cast<int>((value < 0 ? 0 : (value > 100 ? 100 : value)));
      return;
    }

    long fade = 0;
    if (sscanf(line, "seek %ld %ld", &value, &fade) == 2 && cur.fmt) {
      if (fade > 0 && playing && !paused && !fading) {
        if (nxt.open(this->path, speed_percent)) {
          int64_t ts = value * (AV_TIME_BASE / 1000);
          av_seek_frame(nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(nxt.dec);
          nxt.resetFilter();
          nhead = ntail = 0;
          nacc_frames   = 0;
          seek_fade     = true;
          fading        = true;
          fade_pos      = 0;
          fade_total    = static_cast<int>(fade * 1000 / 10667);
          if (fade_total < 1) fade_total = 1;
          base_ms  = value;
          consumed = 0;
          return;
        }
      }
      /* Fall through to the hard seek if the crossfade could not start. */
    }

    if (sscanf(line, "seek %ld", &value) == 1 && cur.fmt) {
      int64_t ts = value * (AV_TIME_BASE / 1000);
      if (av_seek_frame(cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(cur.dec);
        cur.resetFilter();
        head = tail = 0;
        acc_frames  = 0;
        cur.eof     = false;
        eof         = false;
        primed      = false;
        base_ms     = value;
        consumed    = 0;
        seek_fade   = false;
      }
      return;
    }
  }

  void tick() {
    // The decode ring is intentionally built from 512-sample blocks, while
    // the controller's speaker frame is 480 samples (10 ms). The 10.667 ms
    // tick (512 / 48 kHz) and the 512→480 interpolation below are coupled to
    // that ring, matching LinuxAudio4Dualsense5; changing one without the
    // other changes pitch/timing. Keep the conversion.
    float block[IN_FRAMES][CHANNELS];

    if (fading) {
      float old[IN_FRAMES][CHANNELS];
      float next[IN_FRAMES][CHANNELS];
      bool have_old  = takeBlock(old);
      bool have_next = takeNextBlock(next);

      if (!have_next) {
        /* The prefetched side is not ready yet: keep playing the current one
         * without advancing the fade. */
        if (have_old) {
          memcpy(block, old, sizeof(block));
          advanceConsumed();
        } else {
          underruns++;
          memset(block, 0, sizeof(block));
        }
      } else {
        float g =
            fade_total > 0 ? static_cast<float>(fade_pos) / static_cast<float>(fade_total) : 1.0f;
        /* Equal-power: the two sides' powers add to one, so the level stays
         * constant through the overlap instead of dipping in the middle. */
        float phase = g * 1.5707963267948966f;
        float g_out = cosf(phase);
        float g_in  = sinf(phase);
        for (int i = 0; i < IN_FRAMES; i++)
          for (int ch = 0; ch < CHANNELS; ch++)
            block[i][ch] = (have_old ? old[i][ch] : 0.0f) * g_out + next[i][ch] * g_in;

        if (seek_fade) {
          if (have_next) advanceConsumed();
        } else if (have_old) {
          advanceConsumed();
        }
        fade_pos++;
        if (fade_pos >= fade_total || !have_old) finishFade();
      }
    } else {
      bool have = takeBlock(block);

      if (!have) {
        underruns++;
        memset(block, 0, sizeof(block)); /* keep the stream alive with silence */
      } else {
        advanceConsumed();
      }
    }

    if (haptics_only && !is_usb) {
      sendHapticsOnly(block);
      return;
    }

    float gain = static_cast<float>((volume / 100.0));
    gain       = gain * gain * gain;

    if (is_usb) {
      // USB speaker path: 512 frames at 48 kHz every 10.667 ms, no 512→480
      // conversion. That conversion remains Bluetooth-only.
      writeUsbPcm(block, gain);
      return;
    }

    float pcm[dualsense::AudioReport::pcmFrames * CHANNELS];
    double step = (IN_FRAMES - 1) / static_cast<double>((dualsense::AudioReport::pcmFrames - 1));

    for (int i = 0; i < dualsense::AudioReport::pcmFrames; i++) {
      double src      = i * step;
      int idx         = static_cast<int>(src);
      int next_sample = idx < IN_FRAMES - 1 ? idx + 1 : IN_FRAMES - 1;
      float frac      = static_cast<float>((src - idx));
      for (int ch = 0; ch < CHANNELS; ch++) {
        float a                = block[idx][ch];
        float b                = block[next_sample][ch];
        pcm[i * CHANNELS + ch] = (a + (b - a) * frac) * gain;
      }
    }

    sendReport(pcm, dualsense::AudioReport::pcmFrames);
  }

public:
  Player() {}

  ~Player() { destroy(); }

  bool isValid() const { return enc.isValid(); }

  explicit operator bool() const { return isValid(); }

  int timerFd() const { return tfd; }

  void attach(App *owner) { app = owner; }

  void setSpeaker(bool on) {
    if (on == speaker) return;
    speaker = on;
    sendAudioRoute();
  }

  bool speakerOutput() const { return speaker; }

  void setSpeed(int percent) {
    if (percent < 50) percent = 50;
    if (percent > 200) percent = 200;
    speed_percent = percent;
    cur.setSpeed(percent);
    nxt.setSpeed(percent);
  }

  int speedPercent() const { return speed_percent; }

  int fd() const { return hidraw; }

  const char *node() const { return node_name; }

  const char *mac() const { return controller_mac; }

  bool isUsb() const { return is_usb; }

  bool audioActive() const { return usb_audio != nullptr; }

  const char *audioSink() const { return usb_audio ? "usb-alsa" : ""; }

  void dropDevice() {
    stopUsbAudio();
    if (hidraw >= 0) close(hidraw);
    hidraw            = -1;
    hidraw_retry      = 0;
    node_name[0]      = 0;
    controller_mac[0] = 0;
    is_usb            = false;
  }

  bool create(const char *node = nullptr) {
    destroy();
    is_usb = false;
    if (node) {
      hidraw = open(node, O_RDWR | O_CLOEXEC);
      if (hidraw >= 0) snprintf(node_name, sizeof(node_name), "%s", node);
    } else {
      openDevice();
    }
    if (hidraw < 0) fprintf(stderr, "padwave player: no DualSense hidraw node\n");
    state = dualsense::OutputBody(dualsense::OutputBody::AudioControl::Headphones);

    if (!openHapticResampler()) fprintf(stderr, "padwave player: haptic resampler init failed\n");
    if (!enc.open(RATE, CHANNELS)) {
      fprintf(stderr, "padwave player: audio encoder init failed\n");
      dropDevice();
      return false;
    }

    if (is_usb && !startUsbAudio()) fprintf(stderr, "padwave player: USB audio unavailable\n");
    sendMuteLed(false);
    sendAudioRoute();

    pkt     = av_packet_alloc();
    frame   = av_frame_alloc();
    tfd     = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    tick_ns = 0;
    setTick(kPlayTickNs);
    last_status = 0;
    return true;
  }

  void destroy() {
    stopUsbAudio();
    closeFile();
    closeNext();
    if (hidraw >= 0) close(hidraw);
    enc.close();
    if (haptic_swr) swr_free(&haptic_swr);
    haptic_pending_len = 0;
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (tfd >= 0) close(tfd);
    hidraw            = -1;
    node_name[0]      = 0;
    controller_mac[0] = 0;
    pkt               = nullptr;
    frame             = nullptr;
    tfd               = -1;
    tick_ns           = 0;
    playing           = false;
    paused            = false;
    app               = nullptr;
  }

  void setTick(long ns) {
    if (tfd < 0 || tick_ns == ns) return;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = ns;
    its.it_value            = its.it_interval;
    if (timerfd_settime(tfd, 0, &its, nullptr) == 0) tick_ns = ns;
  }

  void command(const char *line) {
    if (!line) return;
    char buf[1200];
    snprintf(buf, sizeof(buf), "%s", line);
    char *p = buf;
    char *nl;
    while ((nl = strchr(p, '\n')) != nullptr) {
      *nl = 0;
      if (*p) handleLine(p);
      p = nl + 1;
    }
    if (*p) handleLine(p);
    setTick((playing && !paused) ? kPlayTickNs : kPauseTickNs);
  }

  void step() {
    if (hidraw < 0) {
      if (hidraw_retry > 0)
        hidraw_retry--;
      else if (!openDevice())
        hidraw_retry = 5;
    }
    setTick((playing && !paused) ? kPlayTickNs : kPauseTickNs);
    if (playing && !paused) fill();

    if (playing && !paused && has_next && !nxt.fmt && !next_warmed && cur.duration_ms > 0) {
      int64_t pos = base_ms + consumed * 1000 / RATE;
      if (pos + 8000 >= cur.duration_ms) {
        warmFile(next_path);
        next_warmed = true;
      }
    }

    if (playing && !paused && has_next && !nxt.fmt) {
      int64_t pos = base_ms + consumed * 1000 / RATE;
      if (eof || (cur.duration_ms > 0 && pos + 5000 >= cur.duration_ms)) {
        warmFile(next_path);
        if (!nxt.open(next_path, speed_percent)) has_next = false;
      }
    }

    if (playing && !paused && nxt.fmt) fillNext();

    if (manual_pending && manual_fade && nextRingLen() >= 8) {
      int fade_blocks = static_cast<int>((static_cast<long>(fade_ms) * 1000 / 10667));
      manual_pending  = false;
      fading          = true;
      fade_total      = fade_blocks > 0 ? fade_blocks : 1;
      fade_pos        = 0;
    }

    if (!fading && !manual_fade && playing && !paused && fade_ms > 0 && has_next && nxt.fmt) {
      int fade_blocks = static_cast<int>((static_cast<long>(fade_ms) * 1000 / 10667));
      int left        = ringLen();
      if (eof && left > 0 && left <= fade_blocks && nextRingLen() >= left) {
        fading     = true;
        fade_total = left;
        fade_pos   = 0;
      }
    }

    if (!fading && !manual_fade && eof && ringLen() == 0 && nextRingLen() > 0) promote();
    if (!primed && ringLen() >= RING_PRIME) primed = true;

    uint64_t expirations = 0;
    if (tfd >= 0 && read(tfd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
      if (primed && playing && !paused) {
        for (uint64_t i = 0; i < expirations && i < 4; i++) tick();
      }
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
    if (ms - last_status >= 100) {
      last_status = ms;
      struct player_status st;
      st.pos_ms    = base_ms + consumed * 1000 / RATE;
      st.dur_ms    = cur.duration_ms;
      st.sent      = sent;
      st.underruns = underruns;
      st.pushed    = pushed;
      st.paused    = paused ? 1 : 0;
      st.eof       = (eof && tail == head && nxt.fmt == NULL && !has_next) ? 1 : 0;
      emitStatus(st);
    }
  }
};
