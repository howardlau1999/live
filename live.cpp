#define SESSIONS 1
#define W 1920
#define H 1080
#define WSTR "1920"
#define HSTR "1080"
#define HW 1920
#define HH 1080
#define HWSTR "1920"
#define HHSTR "1080"

const int bitrate = 6000 * 1024;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct QueueItem {
  AVFrame* frame;
  int is_audio;
  int pts;
  bool operator<(QueueItem const& other) { return pts < other.pts; }
};

const int framerate = 60;
const int samplerate = 48000;
std::mutex mu;
std::deque<QueueItem> output_queue;
std::condition_variable cond;
int64_t start = av_gettime();
int64_t oframes = 0;

std::string outputURL;
std::string inputPrefix;
std::string texts[5] = {"设备测试", "参赛选手 1", "参赛选手 2", "参赛选手 3",
                        "参赛选手 4"};
int64_t lastFrame[4] = {-1, -1, -1, -1};
static int interrupt_cb(void* ctx) {
  int64_t last = *(int64_t*)(ctx);
  if (last > 0 && av_gettime() - last >= 3 * 1000000) {
    printf("timeout\n");
    return -1;
  }
  return 0;
}

AVFrame* null_frame;
std::mutex locks[SESSIONS];
AVFilterContext* inputs[SESSIONS];
AVFilterContext* scaled[SESSIONS];
AVFilterContext* compose_inputs[SESSIONS];
AVFilterContext* composed;

void compositor() {
  int ret = 0;
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterInOut* input_inouts[SESSIONS];
  AVFilterInOut* output_inout = avfilter_inout_alloc();
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  const AVFilter* buffer = avfilter_get_by_name("buffer");
  AVFilterContext* output_filter_ctx = nullptr;
  AVFilterGraph* filter_graph = avfilter_graph_alloc();
  ret = avfilter_graph_create_filter(&output_filter_ctx, buffersink, "out",
                                     NULL, NULL, filter_graph);
  if (ret) {
    printf("create out filter error %d\n", ret);
    return;
  }
  for (int i = 0; i < SESSIONS; ++i) {
    compose_inputs[i] = nullptr;
    std::string name = "in" + std::to_string(i);
    ret =
        avfilter_graph_create_filter(&compose_inputs[i], buffer, name.c_str(),
                                     "video_size=" HWSTR "x" HHSTR
                                     ":pix_fmt=0:time_base=1/60:pixel_aspect=1",
                                     NULL, filter_graph);
    if (ret) {
      printf("create in filter %d error %d\n", i, ret);
      return;
    }
    input_inouts[i] = avfilter_inout_alloc();
    input_inouts[i]->next = nullptr;
    input_inouts[i]->name = av_strdup(name.c_str());
    input_inouts[i]->filter_ctx = compose_inputs[i];
    input_inouts[i]->pad_idx = 0;
    if (i > 0) {
      input_inouts[i - 1]->next = input_inouts[i];
    }
  }

  output_inout->name = av_strdup("out");
  output_inout->filter_ctx = output_filter_ctx;
  output_inout->pad_idx = 0;
  output_inout->next = nullptr;

  AVFrame* output_frame = av_frame_alloc();
  enum AVPixelFormat pix_fmts[2] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
  av_opt_set_int_list(output_filter_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
                      AV_OPT_SEARCH_CHILDREN);
  std::string filter_desc =
      std::string() +
      "[in0]setpts=PTS-STARTPTS,pad=" WSTR ":" HSTR
      " [out]";
  ret = avfilter_graph_parse_ptr(filter_graph, filter_desc.c_str(),
                                 &output_inout, input_inouts, nullptr);
  if (ret < 0) {
    printf("compositor graph parse failed %d\n", ret);
    return;
  }
  ret = avfilter_graph_config(filter_graph, nullptr);
  if (ret < 0) {
    printf("compositor graph config failed %d\n", ret);
    return;
  }
  composed = output_filter_ctx;
}

void scaler(int i) {
  int ret = 0;
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterInOut* input_inout = avfilter_inout_alloc();
  AVFilterInOut* output_inout = avfilter_inout_alloc();
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  const AVFilter* buffer = avfilter_get_by_name("buffer");
  AVFilterContext* output_filter_ctx = nullptr;
  AVFilterContext* input_filter_ctx = nullptr;
  AVFilterGraph* filter_graph = avfilter_graph_alloc();
  ret = avfilter_graph_create_filter(&output_filter_ctx, buffersink, "out",
                                     NULL, NULL, filter_graph);
  if (ret) {
    printf("create scaler out filter %d error %d\n", i, ret);
    return;
  }
  ret = avfilter_graph_create_filter(&input_filter_ctx, buffer, "in",
                                     "video_size=" WSTR "x" HSTR
                                     ":pix_fmt=0:time_base=1/60:pixel_aspect=1",
                                     NULL, filter_graph);
  if (ret) {
    printf("create scaler in filter %d error %d\n", i, ret);
    return;
  }

  output_inout->name = av_strdup("out");
  output_inout->filter_ctx = output_filter_ctx;
  output_inout->pad_idx = 0;
  output_inout->next = nullptr;

  input_inout->name = av_strdup("in");
  input_inout->filter_ctx = input_filter_ctx;
  input_inout->pad_idx = 0;
  input_inout->next = nullptr;

  enum AVPixelFormat pix_fmts[2] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
  av_opt_set_int_list(output_filter_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
                      AV_OPT_SEARCH_CHILDREN);
  std::string filter_desc =
      "[in]setpts=PTS-STARTPTS,scale=w=" HWSTR ":h=" HHSTR "[out]";
  ret = avfilter_graph_parse(filter_graph, filter_desc.c_str(), output_inout,
                             input_inout, nullptr);
  if (ret < 0) {
    printf("graph parse failed %d\n", ret);
    return;
  }
  ret = avfilter_graph_config(filter_graph, nullptr);
  if (ret < 0) {
    printf("graph config failed %d\n", ret);
    return;
  }
  scaled[i] = output_filter_ctx;
  inputs[i] = input_filter_ctx;
}

void blank_screen_generator() {
  AVFormatContext* ctx = avformat_alloc_context();
  std::string url = "testsrc=size=" HWSTR "x" HHSTR ":rate=60";
  AVInputFormat* fmt = av_find_input_format("lavfi");
  int ret = avformat_open_input(&ctx, url.c_str(), fmt, NULL);
  if (ret) {
    printf("open null input %d\n", ret);
    return;
  }
  ret = avformat_find_stream_info(ctx, NULL);
  if (ret < 0) {
    printf("find null stream info %d\n", ret);
    return;
  }
  int vidx = -1;
  vidx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (vidx < 0) {
    printf("find_best_stream\n");
    return;
  }
  AVCodec* decode_codec =
      avcodec_find_decoder(ctx->streams[vidx]->codecpar->codec_id);
  AVCodecContext* decode_ctx = avcodec_alloc_context3(decode_codec);
  avcodec_parameters_to_context(decode_ctx, ctx->streams[vidx]->codecpar);
  avcodec_open2(decode_ctx, decode_codec, NULL);
  printf("------ null ------\n");
  av_dump_format(ctx, 0, url.c_str(), 0);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  while (true) {
    ret = av_read_frame(ctx, packet);
    if (ret) {
      printf("read null frame error %d\n", ret);
      return;
    }
    if (packet->stream_index == vidx) {
      int got = 0;
      ret = avcodec_send_packet(decode_ctx, packet);
      if (ret < 0) {
        printf("codec send packet error %d\n", ret);
        return;
      }
      av_packet_unref(packet);

      ret = avcodec_receive_frame(decode_ctx, frame);
      null_frame = av_frame_alloc();
      null_frame->format = AV_PIX_FMT_YUV420P;
      null_frame->width = frame->width;
      null_frame->height = frame->height;
      uint8_t* buf = (uint8_t*)av_malloc(
          av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frame->width,
                                   frame->height, 1) *
          sizeof(uint8_t));
      null_frame->data[0] = buf;
      av_image_fill_arrays(null_frame->data, null_frame->linesize, buf,
                           AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);
      struct SwsContext* conv_ctx =
          sws_getContext(frame->width, frame->height, AV_PIX_FMT_RGB24,
                         frame->width, frame->height, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
      ret = sws_scale(conv_ctx, (const uint8_t* const*)frame->data,
                      frame->linesize, 0, frame->height, null_frame->data,
                      null_frame->linesize);
      av_frame_free(&frame);
      break;
    }
  }
  avformat_close_input(&ctx);
}

void audio_input_handler() {
  std::string url = "rtmp://localhost/live/audio";
  int64_t audio_counter = 0;
  while (true) {
    AVFormatContext* ctx = avformat_alloc_context();
    int ret = avformat_open_input(&ctx, url.c_str(), NULL, NULL);
    if (ret) {
      printf("open_audio input %d\n", ret);
      break;
    }
    ret = avformat_find_stream_info(ctx, NULL);
    if (ret < 0) {
      printf("find_stream_info %d\n", ret);
      return;
    }
    int aidx = -1;
    aidx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (aidx < 0) {
      printf("find_best_stream audio\n");
      return;
    }
    AVCodec* decode_codec =
        avcodec_find_decoder(ctx->streams[aidx]->codecpar->codec_id);
    AVCodecContext* decode_ctx = avcodec_alloc_context3(decode_codec);
    avcodec_parameters_to_context(decode_ctx, ctx->streams[aidx]->codecpar);
    avcodec_open2(decode_ctx, decode_codec, nullptr);
    printf("------ audio ------\n");
    av_dump_format(ctx, 0, url.c_str(), 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t delta = 0;
    int missed = 0;
    int64_t first_frame = -1;
    while (true) {
      ret = av_read_frame(ctx, packet);
      if (ret) {
        printf("read audio frame error %d\n", ret);
        break;
      }
      if (packet->stream_index == aidx) {
        ret = avcodec_send_packet(decode_ctx, packet);
        if (ret < 0) {
          printf("codec send packet error %d\n", ret);
          break;
        }
        while (true) {
          ret = avcodec_receive_frame(decode_ctx, frame);
          if (ret != 0) {
            av_frame_unref(frame);
            break;
          }
          {
            std::lock_guard<std::mutex> lock(mu);
            if (output_queue.size() < 120)
              output_queue.push_back({av_frame_clone(frame), 1});
            cond.notify_all();
          }
          // printf("audio %d %d %d\n", frame->pts, frame->pkt_dts,
          // frame->nb_samples);
          if (ret < 0) {
            printf("audio add buffersrc failed %d\n", ret);
            break;
          }
        }
      }
      av_packet_unref(packet);
    }
    avcodec_close(decode_ctx);
    avformat_close_input(&ctx);
    av_packet_free(&packet);
    av_frame_free(&frame);
  }
}

void input_stream_handler(int i) {
  std::string url =
      "rtmp://localhost/live/" + inputPrefix + "-" + std::to_string(i + 1);
  while (true) {
    AVFormatContext* ctx = avformat_alloc_context();
    int frames = 0;
    ctx->interrupt_callback.opaque = &lastFrame[i];
    ctx->interrupt_callback.callback = interrupt_cb;
    int ret = avformat_open_input(&ctx, url.c_str(), NULL, NULL);
    if (ret) {
      printf("open_input %d %d\n", i, ret);
      return;
    }
    ret = avformat_find_stream_info(ctx, NULL);
    if (ret < 0) {
      printf("find_stream_info %d %d\n", i, ret);
      return;
    }
    int vidx = -1;
    vidx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) {
      printf("find_best_stream %d\n", i);
      return;
    }
    AVCodec* decode_codec =
        avcodec_find_decoder(ctx->streams[vidx]->codecpar->codec_id);
    AVCodecContext* decode_ctx = avcodec_alloc_context3(decode_codec);
    avcodec_parameters_to_context(decode_ctx, ctx->streams[vidx]->codecpar);
    avcodec_open2(decode_ctx, decode_codec, nullptr);
    printf("------ %d ------\n", i);
    av_dump_format(ctx, 0, url.c_str(), 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t delta = 0;
    int missed = 0;
    int64_t first_frame = -1;
    while (true) {
      ret = av_read_frame(ctx, packet);
      lastFrame[i] = av_gettime();
      if (ret) {
        printf("read frame error %d %d\n", i, ret);
        break;
      }
      if (packet->stream_index == vidx) {
        ret = avcodec_send_packet(decode_ctx, packet);
        if (ret < 0) {
          printf("codec send packet error %d %d\n", i, ret);
          break;
        }
        while (true) {
          ret = avcodec_receive_frame(decode_ctx, frame);
          if (ret != 0) {
            av_frame_unref(frame);
            break;
          }
          ++frames;
          if (first_frame < 0) {
            first_frame = av_gettime();
          } else if (av_gettime() > first_frame + frames * 66666) {
            printf("%d discard frame %d\n", i, frames);
            av_frame_unref(frame);
            av_packet_unref(packet);
            goto retry;
          }
          {
            std::lock_guard<std::mutex> guard(locks[i]);
            ret = av_buffersrc_add_frame(inputs[i], frame);
          }
          if (ret < 0) {
            printf("input %d add buffersrc failed %d\n", i, ret);
            break;
          }
        }
      }
      av_packet_unref(packet);
    }
  retry:
    lastFrame[i] = -1;
    first_frame = -1;
    av_buffersrc_write_frame(inputs[i], null_frame);
    avcodec_close(decode_ctx);
    avformat_close_input(&ctx);
    av_packet_free(&packet);
    av_frame_free(&frame);
  }
}

void output_thread() {
  int ret = 0;
  AVFrame* stream_frames[SESSIONS];
  AVFrame* output_frame = av_frame_alloc();
  for (int i = 0; i < SESSIONS; ++i) {
    stream_frames[i] = av_frame_clone(null_frame);
  }

  while (true) {
    int64_t last = av_gettime();
    int64_t pts = (oframes / framerate);
    AVFrame* pull_frame = av_frame_alloc();
    for (int i = 0; i < SESSIONS; ++i) {
      {
        std::lock_guard<std::mutex> guard(locks[i]);
        ret = av_buffersink_get_frame(scaled[i], pull_frame);
      }
      if (ret >= 0) {
        av_frame_free(&stream_frames[i]);
        stream_frames[i] = av_frame_clone(pull_frame);
        av_frame_unref(pull_frame);
      }
      stream_frames[i]->pts = pts;
      ret = av_buffersrc_write_frame(compose_inputs[i], stream_frames[i]);
      if (ret < 0) {
        printf("add buffersrc i %d ret %d\n", i, ret);
      }
    }
    ret = av_buffersink_get_frame(composed, output_frame);
    if (ret < 0) {
      av_frame_unref(output_frame);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      if (output_queue.size() < 120)
        output_queue.push_back({av_frame_clone(output_frame), 0});
      av_frame_unref(output_frame);
      cond.notify_all();
    }
    int sl = (16666 - av_gettime() + last);
    if (sl < 0) {
      printf("deadline missed\n");
      continue;
    }
    av_usleep(sl);
  }
}
int aframes = 0;

void make_dsi(unsigned int sampling_frequency_index,
              unsigned int channel_configuration, unsigned char* dsi) {
  unsigned int object_type = 2;  // AAC LC by default
  dsi[0] = (object_type << 3) | (sampling_frequency_index >> 1);
  dsi[1] = ((sampling_frequency_index & 1) << 7) | (channel_configuration << 3);
}

int get_sr_index(unsigned int sampling_frequency) {
  switch (sampling_frequency) {
    case 96000:
      return 0;
    case 88200:
      return 1;
    case 64000:
      return 2;
    case 48000:
      return 3;
    case 44100:
      return 4;
    case 32000:
      return 5;
    case 24000:
      return 6;
    case 22050:
      return 7;
    case 16000:
      return 8;
    case 12000:
      return 9;
    case 11025:
      return 10;
    case 8000:
      return 11;
    case 7350:
      return 12;
    default:
      return 0;
  }
}

void output_io_thread() {
  int ret = 0;
  std::string url = outputURL;
  AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) {
    printf("codec not found\n");
    return;
  }
  AVFormatContext* ctx = nullptr;
  AVFormatContext* fileCtx = nullptr;
  ret = avformat_alloc_output_context2(&ctx, NULL, "flv", url.c_str());
  if (ret) {
    printf("alloc_output %d\n", ret);
    return;
  }
  std::string fn = inputPrefix + "_" + std::to_string(av_gettime()) + ".flv";
  avformat_alloc_output_context2(&fileCtx, NULL, "flv", fn.c_str());
  ret = avio_open2(&ctx->pb, url.c_str(), AVIO_FLAG_WRITE,
                   &ctx->interrupt_callback, NULL);
  avio_open2(&fileCtx->pb, fn.c_str(), AVIO_FLAG_WRITE,
             &fileCtx->interrupt_callback, NULL);
  if (ret) {
    printf("avio_open %d\n", ret);
    return;
  }
  AVStream* stream = avformat_new_stream(ctx, nullptr);
  AVStream* audio = avformat_new_stream(ctx, nullptr);
  AVStream* fs = avformat_new_stream(fileCtx, nullptr);
  AVStream* afs = avformat_new_stream(fileCtx, nullptr);
  AVCodecContext* ocodec_ctx = avcodec_alloc_context3(codec);
  AVCodecContext* audio_ctx = avcodec_alloc_context3(audio_codec);
  if (ctx->oformat->flags & AVFMT_GLOBALHEADER) {
    ocodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    audio_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  audio_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_ctx->codec_id = AV_CODEC_ID_AAC;
  audio_ctx->sample_rate = samplerate;
  audio_ctx->bit_rate = 192 * 1024;
  audio_ctx->sample_fmt = audio_codec->sample_fmts[0];
  audio_ctx->channels = 2;
  audio_ctx->time_base = (AVRational){1, samplerate};
  audio_ctx->channel_layout = av_get_default_channel_layout(2);

  ocodec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  ocodec_ctx->codec_id = AV_CODEC_ID_H264;
  ocodec_ctx->width = W;
  ocodec_ctx->height = H;
  ocodec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ocodec_ctx->gop_size = 5;
  ocodec_ctx->time_base = AVRational{1, framerate};
  ocodec_ctx->bit_rate = bitrate;
  AVDictionary* opts = NULL;
  av_dict_set(&opts, "tune", "zerolatency", 0);
  av_dict_set(&opts, "preset", "ultrafast", 0);
  avcodec_open2(ocodec_ctx, codec, &opts);
  avcodec_open2(audio_ctx, audio_codec, nullptr);
  avcodec_parameters_from_context(stream->codecpar, ocodec_ctx);
  avcodec_parameters_from_context(audio->codecpar, audio_ctx);
  avcodec_parameters_from_context(fs->codecpar, ocodec_ctx);
  avcodec_parameters_from_context(afs->codecpar, audio_ctx);
  AVBSFContext* aacbsf = nullptr;
  av_bsf_alloc(av_bsf_get_by_name("aac_adtstoasc"), &aacbsf);
  avcodec_parameters_copy(aacbsf->par_in, audio->codecpar);
  av_bsf_init(aacbsf);
  avcodec_parameters_copy(audio->codecpar, aacbsf->par_out);
  avcodec_parameters_copy(afs->codecpar, aacbsf->par_out);
  printf("------- Output -------\n");
  av_dump_format(ctx, 0, url.c_str(), 1);
  ret = avformat_write_header(ctx, NULL);
  if (ret) {
    printf("write_header %d\n", ret);
    return;
  }
  avformat_write_header(fileCtx, NULL);
  AVPacket* packet = av_packet_alloc();
  AVFrame* output_frame = nullptr;
  int64_t video_pts = 0, audio_pts = 0, last_pts = 0;
  int is_audio = 0;
  std::set<QueueItem> reorder_buffer;

  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mu);
      cond.wait(lock, []() { return output_queue.size() > 0; });
      output_frame = output_queue.front().frame;
      is_audio = output_queue.front().is_audio;
      if (!is_audio) {
        video_pts = (oframes * 1.0 / framerate) / (av_q2d(stream->time_base));
        ++oframes;
      } else {
        audio_pts =
            (aframes * 1024.0 / samplerate) / (av_q2d(audio->time_base));
        ++aframes;
      }
      // printf("%d\n", oframes);
      output_queue.pop_front();
    }
    // printf("%d %d %d\n", video_pts, audio_pts, last_pts);
    if (!is_audio) {
      output_frame->pict_type = AV_PICTURE_TYPE_NONE;
      output_frame->pts = video_pts;
      ret = avcodec_send_frame(ocodec_ctx, output_frame);
      if (ret) {
        printf("encode send %d err %d\n", oframes, ret);
        av_frame_free(&output_frame);
        continue;
      }
      av_frame_free(&output_frame);
      while (true) {
        ret = avcodec_receive_packet(ocodec_ctx, packet);
        if (ret) {
          // printf("encode %d err %d\n", frames, ret);
          break;
        }
        packet->pts = packet->dts = video_pts;
        packet->duration =
            (double)(1.0 / framerate) / av_q2d(stream->time_base);
        packet->stream_index = 0;
        last_pts = packet->pts;
        av_interleaved_write_frame(fileCtx, av_packet_clone(packet));
        ret = av_interleaved_write_frame(ctx, packet);
        if (oframes % 600 == 0) {
          printf("sent %ld frames\n", oframes);
        }
        av_packet_unref(packet);
      }
    } else {
      output_frame->pts = audio_pts;
      // printf("audio\n");
      ret = avcodec_send_frame(audio_ctx, output_frame);
      if (ret) {
        printf("encode send %ld err %d\n", oframes, ret);
        av_frame_free(&output_frame);
        continue;
      }
      av_frame_free(&output_frame);
      while (true) {
        ret = avcodec_receive_packet(audio_ctx, packet);
        if (ret) {
          break;
        }
        ret = av_bsf_send_packet(aacbsf, packet);
        if (ret) {
          break;
        }
        while (true) {
          ret = av_bsf_receive_packet(aacbsf, packet);
          if (ret) {
            // printf("encode %d err %d\n", frames, ret);
            break;
          }
          packet->pts = packet->dts = audio_pts;
          last_pts = packet->pts;
          packet->duration =
              (double)(1024.0 / samplerate) / av_q2d(audio->time_base);
          packet->stream_index = 1;
          av_interleaved_write_frame(fileCtx, av_packet_clone(packet));
          ret = av_interleaved_write_frame(ctx, packet);
        }
        av_packet_unref(packet);
      }
    }
  }
}

int main(int argc, char* argv[]) {
  int ret = 0;
  outputURL = argv[1];
  inputPrefix = argv[2];
  for (int i = 3; i < argc; ++i) {
    texts[i - 3] = argv[i];
  }
  av_log_set_level(AV_LOG_INFO);
  avdevice_register_all();
  avformat_network_init();
  std::vector<std::thread> input_threads;
  blank_screen_generator();
  for (int i = 0; i < SESSIONS; ++i) {
    scaler(i);
  }
  compositor();
  for (int i = 0; i < SESSIONS; ++i) {
    input_threads.emplace_back(std::thread(input_stream_handler, i));
  }
  std::thread audio(audio_input_handler);
  std::thread output(output_thread);
  std::thread output_io(output_io_thread);
  for (auto&& thread : input_threads) {
    thread.join();
  }
  output.join();
  output_io.join();
  audio.join();
  return 0;
}
