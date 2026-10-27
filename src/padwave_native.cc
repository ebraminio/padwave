// padwave_native — the whole playback path in one C program.
//
// One thread, no async I/O framework: poll() on stdin and a timerfd is enough
// because decoding is not the bottleneck, *latency* was. Every loop iteration
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
// Usage:  padwave native [hidraw-node]
//         (without a node it scans /sys/class/hidraw for a DualSense)
//
// stdin commands, one per line:
//   play <path> [start_ms]   pause <0|1>   volume <0..100>   seek <ms>   quit
// stdout, ten times a second:
//   pos <ms>   dur <ms>   sent <n>   underruns <n>   eof
//
// Build (no STL, no RTTI, no exceptions):
//   cc -std=c++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2
//      -o padwave padwave.cc padwave_native.cc padwave_opusdec.cc
//      $(pkg-config --cflags --libs libavformat libavcodec libswresample libavutil)
//      -lopus -lz -lm
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

// FFmpeg's public headers are C-only (no extern "C" guards), so a C++ caller
// has to provide the linkage wrapper itself.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <opus/opus.h>

#include "padwave_fields.hh"
#include "padwave_status.hh"
#include "padwave_schemas.hh"
#include "padwave_common.hh"

#define RATE 48000
#define CHANNELS 2
#define IN_FRAMES 512   /* 512 samples per input block, 93.75 blocks/s */
#define OPUS_FRAMES 480 /* what the controller's decoder consumes */
#define OPUS_BYTES 200  /* CBR 160 kbps at 10 ms */
#define REPORT_SIZE 398
#define RING_BLOCKS 192 /* about two seconds: enough for a 1.5 s crossfade */
#define RING_HIGH 160   /* stop decoding above this many buffered blocks */
#define RING_PRIME 24  /* start sending once this many are buffered */
#define DECODE_BUDGET_MS 3 /* but never spend longer than this per iteration */

struct file {
  AVFormatContext *fmt;
  AVCodecContext *dec;
  struct SwrContext *swr;
  int audio_index;
  int64_t duration_ms;
  bool eof;
};

typedef float block_t[IN_FRAMES][CHANNELS];

struct player {
  int hidraw;
  bool speaker; /* 0x13 tag instead of 0x16 */

  OpusEncoder *enc;
  uint8_t seq, counter;

  struct file cur; /* the station playing now */
  struct file nxt; /* the prefetched one, if any */
  AVPacket *pkt;
  AVFrame *frame;
  char path[1024];
  char next_path[1024];
  bool has_next;
  bool next_warmed;

  /* Crossfade at a station change: the last fade_total blocks of the current
   * station are mixed with the first fade_total blocks of the next one. */
  int fade_ms;
  bool fading;
  int fade_pos, fade_total;

  /* A manual station change (next/prev) fades too: the new station is opened
   * into the prefetch slot and the tick blends it in, without reporting a
   * track (the radio already made that one current). */
  bool manual_fade;
  bool manual_pending;
  char manual_path[1024];

  /* A seek inside the file can fade too: a second decoder opens the target
   * position and the tick blends into it, same as a station change. */
  bool seek_fade;

  /* The 0x31 control writer shares this process (and this hidraw fd) with the
   * 0x36 stream: one server for everything. */
  padwave::OutputReport out;
  uint8_t out_seq;

  /* The 0x31 features the writer has claimed, kept so the 0x36 state snapshot
   * carries them. The controller applies that snapshot 93 times a second; if it
   * only said "audio", the rumble and trigger values would be zeroed by the
   * stream. */
  padwave::OutputBody state;

  /* Experimental: the microphone is off in the 0x36 enable mask unless the UI
   * asks for the input level. The controller only sends 0xD4 mic frames while
   * this is on. */
  bool mic;

  float ring[RING_BLOCKS][IN_FRAMES][CHANNELS];
  int head, tail; /* head = write, tail = read */
  float acc[IN_FRAMES][CHANNELS];
  int acc_frames;

  /* Back-to-back playback: the next station is decoded into its own ring, then
   * swapped in when the current one drains. */
  block_t nring[RING_BLOCKS];
  int nhead, ntail;
  float nacc[IN_FRAMES][CHANNELS];
  int nacc_frames;
  int64_t npushed;

  bool playing, paused, eof, primed;
  int volume;
  int64_t base_ms;   /* source time at the start of the ring */
  int64_t consumed;  /* input frames taken out of the ring */
  int64_t sent, underruns, pushed;
};

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) {
  (void)sig;
  running = 0;
}

/* ---- 0x36 report -------------------------------------------------------- */

static void send_report(struct player *pl, const uint8_t *opus, int len) {
  padwave::AudioReport report;

  report.init(pl->speaker ? padwave::AudioReport::Speaker : padwave::AudioReport::Jack,
              pl->mic ? padwave::AudioReport::SpeakerAndMic : padwave::AudioReport::SpeakerOnly);

  report.state.body = pl->state;
  report.state.body.valid_flag0 |=
      padwave::OutputBody::HeadphoneVolume | padwave::OutputBody::SpeakerVolume |
      padwave::OutputBody::MicVolume | padwave::OutputBody::AudioRouting;
  report.state.body.valid_flag1 |= padwave::OutputBody::AudioControl2;
  report.state.body.headphone_volume = 0x7F;
  report.state.body.speaker_volume = 0x64;
  report.state.body.audio_control =
      pl->speaker ? padwave::OutputBody::Speaker : padwave::OutputBody::Headphones;
  report.state.body.audio_control2 = 0x0A;

  report.seq_tag = (uint8_t)((pl->seq & 0x0F) << 4);
  report.counter = pl->counter;

  size_t copy = len > 0 ? (size_t)len : 0;
  if (copy > padwave::AudioReport::OpusSize) copy = padwave::AudioReport::OpusSize;
  if (copy) memcpy(report.opus, opus, copy);

  report.sign();

  if (pl->hidraw >= 0) {
    ssize_t n = write(pl->hidraw, &report, sizeof(report));
    (void)n;
  }

  pl->seq = (uint8_t)((pl->seq + 1) & 0x0F);
  pl->counter++;
  pl->sent++;
}

/* ---- decode ------------------------------------------------------------- */

static int count_of(int head, int tail, int size) { return (head - tail + size) % size; }

/* Ask the kernel to read a file ahead of time. Non-blocking: the read happens
 * on the kernel's schedule, so the later open() finds the pages in cache. */
static void warm_file(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
    close(fd);
    if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "warm %s\n", path);
  }
}

static void file_close(struct file *f) {
  if (f->swr) swr_free(&f->swr);
  if (f->dec) avcodec_free_context(&f->dec);
  if (f->fmt) avformat_close_input(&f->fmt);
  f->fmt = NULL;
  f->dec = NULL;
  f->swr = NULL;
  f->audio_index = -1;
  f->duration_ms = 0;
  f->eof = false;
}

static bool file_open(struct file *f, const char *path) {
  file_close(f);

  if (avformat_open_input(&f->fmt, path, NULL, NULL) < 0) return false;
  if (avformat_find_stream_info(f->fmt, NULL) < 0) return false;

  const AVCodec *codec = NULL;
  f->audio_index = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (f->audio_index < 0 || !codec) return false;

  AVStream *st = f->fmt->streams[f->audio_index];
  f->dec = avcodec_alloc_context3(codec);
  if (!f->dec) return false;
  if (avcodec_parameters_to_context(f->dec, st->codecpar) < 0) return false;
  if (avcodec_open2(f->dec, codec, NULL) < 0) return false;

  AVChannelLayout in_layout;
  AVChannelLayout out_layout;
  av_channel_layout_copy(&in_layout, &f->dec->ch_layout);
  av_channel_layout_default(&out_layout, CHANNELS);

  int converted = swr_alloc_set_opts2(&f->swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE, &in_layout,
                                      f->dec->sample_fmt, f->dec->sample_rate, 0, NULL);

  av_channel_layout_uninit(&in_layout);
  av_channel_layout_uninit(&out_layout);
  if (converted < 0 || !f->swr || swr_init(f->swr) < 0) return false;

  f->duration_ms = f->fmt->duration > 0 ? f->fmt->duration / (AV_TIME_BASE / 1000) : 0;
  f->eof = false;
  return true;
}

static int ring_len(struct player *pl) { return count_of(pl->head, pl->tail, RING_BLOCKS); }
static int next_ring_len(struct player *pl) { return count_of(pl->nhead, pl->ntail, RING_BLOCKS); }

static void close_file(struct player *pl) {
  file_close(&pl->cur);
  pl->head = pl->tail = 0;
  pl->acc_frames = 0;
}

static void close_next(struct player *pl) {
  file_close(&pl->nxt);
  pl->nhead = pl->ntail = 0;
  pl->nacc_frames = 0;
  pl->has_next = false;
}

static bool open_file(struct player *pl, const char *path) {
  close_file(pl);
  if (!file_open(&pl->cur, path)) return false;
  snprintf(pl->path, sizeof(pl->path), "%s", path);
  pl->eof = false;
  pl->consumed = 0;
  pl->sent = 0;
  pl->underruns = 0;
  pl->base_ms = 0;
  return true;
}

/* Decode at most DECODE_BUDGET_MS worth of work into `ring`. This is the whole
 * trick: the loop keeps servicing the 10.667 ms timer because no single
 * iteration can sit inside libav for long. It serves the current station and
 * the prefetched one equally. */
static void fill_into(struct player *pl, struct file *f, block_t *ring, int *head, int *tail,
                      float acc[IN_FRAMES][CHANNELS], int *acc_frames, int high,
                      int64_t *pushed) {
  if (!f->fmt || pl->paused) return;

  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (count_of(*head, *tail, RING_BLOCKS) < high && !f->eof) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    if (elapsed >= DECODE_BUDGET_MS) break;

    int ret = av_read_frame(f->fmt, pl->pkt);
    if (getenv("PADWAVE_DEBUG") && ret < 0) fprintf(stderr, "read: %d\n", ret);
    if (ret == AVERROR_EOF) {
      avcodec_send_packet(f->dec, NULL);
      f->eof = true;
    } else if (ret < 0) {
      f->eof = true;
    } else {
      if (pl->pkt->stream_index == f->audio_index) avcodec_send_packet(f->dec, pl->pkt);
      av_packet_unref(pl->pkt);
    }

    for (;;) {
      ret = avcodec_receive_frame(f->dec, pl->frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
      if (ret < 0) break;

      uint8_t *out[1];
      float tmp[8192 * CHANNELS];
      out[0] = (uint8_t *)tmp;
      int cap = swr_get_out_samples(f->swr, pl->frame->nb_samples);
      if (cap > 8192) cap = 8192;
      int got = swr_convert(f->swr, out, cap, (const uint8_t **)pl->frame->extended_data,
                            pl->frame->nb_samples);
      for (int i = 0; i < got; i++) {
        acc[*acc_frames][0] = tmp[i * CHANNELS + 0];
        acc[*acc_frames][1] = tmp[i * CHANNELS + 1];
        if (++(*acc_frames) == IN_FRAMES) {
          int next = (*head + 1) % RING_BLOCKS;
          if (next == *tail) { /* ring full: keep the partial block */
            *acc_frames = 0;
            break;
          }
          memcpy(ring[*head], acc, sizeof(float) * IN_FRAMES * CHANNELS);
          *head = next;
          if (pushed) (*pushed)++;
          *acc_frames = 0;
        }
      }
      av_frame_unref(pl->frame);
      if (((*head + 1) % RING_BLOCKS) == *tail) break;
    }
  }
}

static void fill(struct player *pl) {
  fill_into(pl, &pl->cur, pl->ring, &pl->head, &pl->tail, pl->acc, &pl->acc_frames, RING_HIGH,
            &pl->pushed);
  pl->eof = pl->cur.eof;
}

static void fill_next(struct player *pl) {
  fill_into(pl, &pl->nxt, pl->nring, &pl->nhead, &pl->ntail, pl->nacc, &pl->nacc_frames,
            RING_HIGH, &pl->npushed);
}

/* The current station is exhausted; the prefetched one becomes current without
 * touching the encoder or the timer, which is what makes it gapless. */
static void promote(struct player *pl) {
  file_close(&pl->cur);
  pl->cur = pl->nxt;
  memset(&pl->nxt, 0, sizeof(pl->nxt));
  pl->nxt.audio_index = -1;

  memcpy(pl->ring, pl->nring, sizeof(pl->ring));
  pl->head = pl->nhead;
  pl->tail = pl->ntail;
  memcpy(pl->acc, pl->nacc, sizeof(pl->acc));
  pl->acc_frames = pl->nacc_frames;

  pl->nhead = pl->ntail = 0;
  pl->nacc_frames = 0;
  pl->npushed = 0;

  snprintf(pl->path, sizeof(pl->path), "%s", pl->next_path);
  pl->base_ms = 0;
  pl->consumed = 0;
  pl->sent = 0;
  pl->underruns = 0;
  pl->eof = pl->cur.eof;
  pl->has_next = false;

  printf("track %s\n", pl->path);
  fflush(stdout);
}

/* Take one 512-frame block out of the ring; returns false on underrun. */
static bool take_block(struct player *pl, float out[IN_FRAMES][CHANNELS]) {
  if (pl->tail == pl->head) return false;
  memcpy(out, pl->ring[pl->tail], sizeof(pl->ring[0]));
  pl->tail = (pl->tail + 1) % RING_BLOCKS;
  return true;
}

static bool take_next_block(struct player *pl, float out[IN_FRAMES][CHANNELS]) {
  if (pl->ntail == pl->nhead) return false;
  memcpy(out, pl->nring[pl->ntail], sizeof(pl->nring[0]));
  pl->ntail = (pl->ntail + 1) % RING_BLOCKS;
  return true;
}

/* The fade is over: the prefetched station becomes the current one. The blocks
 * already mixed are gone from nring, so the new ring continues exactly after
 * the faded-in part. */
static void finish_fade(struct player *pl) {
  file_close(&pl->cur);
  pl->cur = pl->nxt;
  memset(&pl->nxt, 0, sizeof(pl->nxt));
  pl->nxt.audio_index = -1;

  memcpy(pl->ring, pl->nring, sizeof(pl->ring));
  pl->head = pl->nhead;
  pl->tail = pl->ntail;
  memcpy(pl->acc, pl->nacc, sizeof(pl->acc));
  pl->acc_frames = pl->nacc_frames;

  pl->nhead = pl->ntail = 0;
  pl->nacc_frames = 0;
  pl->npushed = 0;

  bool manual = pl->manual_fade;
  bool seek = pl->seek_fade;
  if (manual)
    snprintf(pl->path, sizeof(pl->path), "%s", pl->manual_path);
  else if (!seek)
    snprintf(pl->path, sizeof(pl->path), "%s", pl->next_path);

  if (!seek) {
    pl->base_ms = 0;
    pl->consumed = 0;
  }
  pl->sent = 0;
  pl->underruns = 0;
  pl->eof = pl->cur.eof;
  pl->fading = false;
  pl->fade_pos = 0;
  pl->fade_total = 0;
  pl->manual_fade = false;
  pl->manual_pending = false;
  pl->seek_fade = false;

  if (!manual && !seek) {
    pl->has_next = false;
    printf("track %s\n", pl->path);
    fflush(stdout);
  }
}

/* ---- the tick ----------------------------------------------------------- */

static void tick(struct player *pl) {
  float block[IN_FRAMES][CHANNELS];

  if (pl->fading) {
    float old[IN_FRAMES][CHANNELS];
    float next[IN_FRAMES][CHANNELS];
    bool have_old = take_block(pl, old);
    bool have_next = take_next_block(pl, next);

    if (!have_next) {
      /* The prefetched side is not ready yet: keep playing the current one
       * without advancing the fade. */
      if (have_old) {
        memcpy(block, old, sizeof(block));
        pl->consumed += IN_FRAMES;
      } else {
        pl->underruns++;
        memset(block, 0, sizeof(block));
      }
    } else {
      float g = pl->fade_total > 0 ? (float)pl->fade_pos / (float)pl->fade_total : 1.0f;
      /* Equal-power: the two sides' powers add to one, so the level stays
       * constant through the overlap instead of dipping in the middle. */
      float phase = g * 1.5707963267948966f;
      float g_out = cosf(phase);
      float g_in = sinf(phase);
      for (int i = 0; i < IN_FRAMES; i++)
        for (int ch = 0; ch < CHANNELS; ch++)
          block[i][ch] = (have_old ? old[i][ch] : 0.0f) * g_out + next[i][ch] * g_in;

      if (pl->seek_fade) {
        if (have_next) pl->consumed += IN_FRAMES;
      } else if (have_old) {
        pl->consumed += IN_FRAMES;
      }
      pl->fade_pos++;
      if (pl->fade_pos >= pl->fade_total || !have_old) finish_fade(pl);
    }
  } else {
    bool have = take_block(pl, block);

    if (!have) {
      pl->underruns++;
      memset(block, 0, sizeof(block)); /* keep the stream alive with silence */
    } else {
      pl->consumed += IN_FRAMES;
    }
  }

  float pcm[OPUS_FRAMES * CHANNELS];
  double step = (IN_FRAMES - 1) / (double)(OPUS_FRAMES - 1);
  float gain = (float)(pl->volume / 100.0);
  gain = gain * gain * gain;

  for (int i = 0; i < OPUS_FRAMES; i++) {
    double src = i * step;
    int idx = (int)src;
    int nxt = idx < IN_FRAMES - 1 ? idx + 1 : IN_FRAMES - 1;
    float frac = (float)(src - idx);
    for (int ch = 0; ch < CHANNELS; ch++) {
      float a = block[idx][ch];
      float b = block[nxt][ch];
      pcm[i * CHANNELS + ch] = (a + (b - a) * frac) * gain;
    }
  }

  uint8_t opus[OPUS_BYTES];
  int n = opus_encode_float(pl->enc, pcm, OPUS_FRAMES, opus, sizeof(opus));
  if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "enc %d\n", n);
  if (n < 0) n = 0;
  send_report(pl, opus, n);
}

/* ---- commands ----------------------------------------------------------- */

static void handle_line(struct player *pl, char *line) {
  char path[1024];
  long value;

  // "play <start_ms> <path>" — the number comes first so a path can contain
  // spaces and the two fields never have to be guessed apart. "play <path>" is
  // still accepted for driving the binary by hand.
  if (sscanf(line, "play %ld %1023[^\n]", &value, path) == 2) {
    if (pl->playing && !pl->paused && pl->cur.fmt && pl->fade_ms > 0) {
      close_next(pl);
      if (file_open(&pl->nxt, path)) {
        if (value > 0) {
          int64_t ts = value * (AV_TIME_BASE / 1000);
          av_seek_frame(pl->nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(pl->nxt.dec);
        }
        snprintf(pl->manual_path, sizeof(pl->manual_path), "%s", path);
        pl->manual_fade = true;
        pl->manual_pending = true;
        pl->base_ms = value;
        pl->consumed = 0;
        pl->sent = 0;
        pl->underruns = 0;
        return;
      }
    }

    close_next(pl);
    if (open_file(pl, path)) {
      pl->playing = true;
      pl->paused = false;
      pl->primed = false;
      if (value > 0) {
        int64_t ts = value * (AV_TIME_BASE / 1000);
        av_seek_frame(pl->cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(pl->cur.dec);
        pl->base_ms = value;
      }
    }
    return;
  }

  if (sscanf(line, "play %1023[^\n]", path) == 1) {
    close_next(pl);
    if (open_file(pl, path)) {
      pl->playing = true;
      pl->paused = false;
      pl->primed = false;
    }
    return;
  }

  // "queue <path>" — the next station, to be decoded ahead and promoted at the
  // end of the current one. Only one is held; the radio pushes the one after
  // that once the promotion is reported.
  if (sscanf(line, "queue %1023[^\n]", path) == 1) {
    if (!pl->has_next) {
      snprintf(pl->next_path, sizeof(pl->next_path), "%s", path);
      pl->has_next = true;
      pl->next_warmed = false;
      warm_file(path);
    }
    return;
  }

  if (strncmp(line, "fields ", 7) == 0) {
    memset(&pl->out, 0, sizeof(pl->out));
    pl->out.report_id = padwave::OutputReport::ReportId;
    pl->out.tag = 0x10;
    pl->out.seq_tag = (uint8_t)((pl->out_seq & 0x0F) << 4);
    if (!padwave_fields::apply_fields(&pl->out.body, line + 7)) {
      printf("err field\n");
      fflush(stdout);
      return;
    }
    pl->out.sign();
    if (pl->hidraw < 0) pl->hidraw = find_dualsense();
    bool ok = pl->hidraw >= 0 && write_all(pl->hidraw, &pl->out, sizeof(pl->out));
    if (ok) pl->out_seq = (uint8_t)((pl->out_seq + 1) & 0x0F);
    if (ok) padwave_fields::apply_fields(&pl->state, line + 7, true);
    printf(ok ? "ok 1\n" : "err write\n");
    fflush(stdout);
    return;
  }

  if (sscanf(line, "fade %ld", &value) == 1) {
    pl->fade_ms = (int)(value < 0 ? 0 : value);
    return;
  }

  if (sscanf(line, "pause %ld", &value) == 1) {
    pl->paused = value != 0;
    return;
  }

  if (sscanf(line, "volume %ld", &value) == 1) {
    pl->volume = (int)(value < 0 ? 0 : (value > 100 ? 100 : value));
    return;
  }

  long fade = 0;
  if (sscanf(line, "seek %ld %ld", &value, &fade) == 2 && pl->cur.fmt) {
    if (fade > 0 && pl->playing && !pl->paused && !pl->fading) {
      if (file_open(&pl->nxt, pl->path)) {
        int64_t ts = value * (AV_TIME_BASE / 1000);
        av_seek_frame(pl->nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(pl->nxt.dec);
        pl->nhead = pl->ntail = 0;
        pl->nacc_frames = 0;
        pl->seek_fade = true;
        pl->fading = true;
        pl->fade_pos = 0;
        pl->fade_total = (int)((long)fade * 1000 / 10667);
        if (pl->fade_total < 1) pl->fade_total = 1;
        pl->base_ms = value;
        pl->consumed = 0;
        return;
      }
    }
    /* Fall through to the hard seek if the crossfade could not start. */
  }

  if (sscanf(line, "seek %ld", &value) == 1 && pl->cur.fmt) {
    int64_t ts = value * (AV_TIME_BASE / 1000);
    if (av_seek_frame(pl->cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(pl->cur.dec);
      pl->head = pl->tail = 0;
      pl->acc_frames = 0;
      pl->cur.eof = false;
      pl->eof = false;
      pl->primed = false;
      pl->base_ms = value;
      pl->consumed = 0;
      pl->seek_fade = false;
    }
    return;
  }

  if (strncmp(line, "quit", 4) == 0) running = 0;
}

/* ---- main --------------------------------------------------------------- */

int native_main(int argc, char **argv) {
  struct player pl;
  memset(&pl, 0, sizeof(pl));
  pl.volume = 80;

  pl.hidraw = argc > 1 ? open(argv[1], O_RDWR | O_CLOEXEC) : find_dualsense();
  if (pl.hidraw < 0) fprintf(stderr, "padwave_native: no DualSense hidraw node\n");
  pl.speaker = argc > 2 && strcmp(argv[2], "speaker") == 0;
  pl.state.init(pl.speaker ? padwave::OutputBody::Speaker : padwave::OutputBody::Headphones);

  int err = 0;
  pl.enc = opus_encoder_create(RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
  if (!pl.enc || err != OPUS_OK) {
    fprintf(stderr, "padwave_native: opus encoder: %s\n", opus_strerror(err));
    return 1;
  }
  opus_encoder_ctl(pl.enc, OPUS_SET_BITRATE(160000));
  opus_encoder_ctl(pl.enc, OPUS_SET_VBR(0));
  opus_encoder_ctl(pl.enc, OPUS_SET_COMPLEXITY(0));

  pl.pkt = av_packet_alloc();
  pl.frame = av_frame_alloc();

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  struct itimerspec its;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 10667000; /* 10.667 ms: 93.75 blocks/s */
  its.it_value = its.it_interval;
  timerfd_settime(tfd, 0, &its, NULL);

  int status_fd = padwave_status::sender_open();
  int64_t last_status = 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  // A local control bus with the same line protocol as stdin. stdin stays the
  // app's path when it owns the player; this is for the web front and tests.
  int cmd_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (cmd_fd >= 0) {
    struct sockaddr_un cmd_addr;
    memset(&cmd_addr, 0, sizeof(cmd_addr));
    cmd_addr.sun_family = AF_UNIX;
    snprintf(cmd_addr.sun_path, sizeof(cmd_addr.sun_path), "%s", padwave_status::kPlayerPath);
    unlink(padwave_status::kPlayerPath);
    if (bind(cmd_fd, (struct sockaddr *)&cmd_addr, sizeof(cmd_addr)) != 0) {
      close(cmd_fd);
      cmd_fd = -1;
    }
  }

  char line[1200];
  int line_len = 0;

  while (running) {
    int timeout = (pl.playing && !pl.paused && ring_len(&pl) < RING_HIGH) ? 1 : 20;
    struct pollfd fds[3];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = tfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    fds[2].fd = cmd_fd;
    fds[2].events = POLLIN;
    fds[2].revents = 0;
    int ready = poll(fds, 3, timeout);
    if (ready < 0 && errno != EINTR) break;

    if (fds[0].revents & POLLIN) {
      char buf[512];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
          line[line_len] = 0;
          handle_line(&pl, line);
          line_len = 0;
        } else if (line_len < (int)sizeof(line) - 1) {
          line[line_len++] = buf[i];
        }
      }
    }

    if (fds[2].revents & POLLIN) {
      char cmd[1200];
      ssize_t n = recv(cmd_fd, cmd, sizeof(cmd) - 1, 0);
      if (n > 0) {
        cmd[n] = 0;
        char *nl = strchr(cmd, '\n');
        if (nl) *nl = 0;
        handle_line(&pl, cmd);
      }
    }

    if (pl.playing && !pl.paused) fill(&pl);

    // Warm the next file again shortly before it is needed: the queue-time
    // readahead can be stale by the end of a long track, and the open() at the
    // prefetch point must not read cold pages while the stream is running.
    if (pl.playing && !pl.paused && pl.has_next && !pl.nxt.fmt && !pl.next_warmed &&
        pl.cur.duration_ms > 0) {
      int64_t pos = pl.base_ms + pl.consumed * 1000 / RATE;
      if (pos + 8000 >= pl.cur.duration_ms) {
        warm_file(pl.next_path);
        pl.next_warmed = true;
      }
    }

    // Gapless: start the next station three seconds before this one ends, keep
    // its frames in their own ring, and swap when the current one is drained.
    // Opening here blocks the loop for a few tens of milliseconds (libav reads
    // from disk); it lands before the junction, never on it.
    if (pl.playing && !pl.paused && pl.has_next && !pl.nxt.fmt) {
      int64_t pos = pl.base_ms + pl.consumed * 1000 / RATE;
      if (pl.eof || (pl.cur.duration_ms > 0 && pos + 5000 >= pl.cur.duration_ms)) {
        warm_file(pl.next_path); /* last call before the blocking open */
        if (!file_open(&pl.nxt, pl.next_path)) pl.has_next = false;
      }
    }

    if (pl.playing && !pl.paused && pl.nxt.fmt) fill_next(&pl);

    if (pl.manual_pending && pl.manual_fade && next_ring_len(&pl) >= 8) {
      int fade_blocks = (int)((long)pl.fade_ms * 1000 / 10667);
      pl.manual_pending = false;
      pl.fading = true;
      pl.fade_total = fade_blocks > 0 ? fade_blocks : 1;
      pl.fade_pos = 0;
    }

    // Crossfade the last fade_ms of the current station into the next one, once
    // the current end is in sight and the prefetched ring can cover it.
    if (!pl.fading && !pl.manual_fade && pl.playing && !pl.paused && pl.fade_ms > 0 && pl.has_next &&
        pl.nxt.fmt) {
      int fade_blocks = (int)((long)pl.fade_ms * 1000 / 10667); /* 10.667 ms blocks */
      int left = ring_len(&pl);
      if (pl.eof && left > 0 && left <= fade_blocks && next_ring_len(&pl) >= left) {
        pl.fading = true;
        pl.fade_total = left;
        pl.fade_pos = 0;
      }
    }

    if (!pl.fading && !pl.manual_fade && pl.eof && ring_len(&pl) == 0 && next_ring_len(&pl) > 0)
      promote(&pl);

    if (!pl.primed && ring_len(&pl) >= RING_PRIME) pl.primed = true;

    if (fds[1].revents & POLLIN) {
      uint64_t expirations = 0;
      if (read(tfd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
        /* A late loop may have missed a tick or two; send at most a few in a
         * row so a stall cannot turn into a burst of a hundred packets. */
        if (pl.primed && pl.playing && !pl.paused) {
          for (uint64_t i = 0; i < expirations && i < 4; i++) tick(&pl);
        }
      }
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
    if (ms - last_status >= 100) {
      last_status = ms;
      int64_t pos = pl.base_ms + pl.consumed * 1000 / RATE;
      printf("pos %ld\ndur %ld\nsent %ld\nunderruns %ld\npushed %ld\n", (long)pos,
             (long)pl.cur.duration_ms, (long)pl.sent, (long)pl.underruns, (long)pl.pushed);

      char status_line[160];
      snprintf(status_line, sizeof(status_line), "player %ld %ld %ld %ld %d", (long)pos,
               (long)pl.cur.duration_ms, (long)pl.sent, (long)pl.underruns,
               pl.paused ? 1 : 0);
      padwave_status::send_line(status_fd, status_line);
      if (pl.eof && pl.tail == pl.head && pl.nxt.fmt == NULL && !pl.has_next) printf("eof\n");
      fflush(stdout);
    }
  }

  close_file(&pl);
  close_next(&pl);
  if (cmd_fd >= 0) {
    close(cmd_fd);
    unlink(padwave_status::kPlayerPath);
  }
  if (pl.hidraw >= 0) close(pl.hidraw);
  opus_encoder_destroy(pl.enc);
  av_packet_free(&pl.pkt);
  av_frame_free(&pl.frame);
  close(tfd);
  return 0;
}
