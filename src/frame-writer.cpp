// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#include "frame-writer.hpp"
#include <vector>
#include <cstring>

#define FPS 60

using namespace std;

class FFmpegInitialize
{
public :

	FFmpegInitialize()
	{
		// Loads the whole database of available codecs and formats.
		av_register_all();
	}
};

static FFmpegInitialize ffmpegInitialize;

FrameWriter::FrameWriter(const string& filename, int width_, int height_, InputFormat format) :
    width(width_), height(height_)
{
    switch (format)
    {
        case INPUT_FORMAT_BGR0:
            swsCtx = sws_getContext(width, height,
                AV_PIX_FMT_BGR0, width, height, AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
            break;
        case INPUT_FORMAT_RGB0:
            swsCtx = sws_getContext(width, height,
                AV_PIX_FMT_RGB0, width, height, AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
            break;
    }

	// Preparing the data concerning the format and codec,
	// in order to write properly the header, frame data and end of file.
	fmt = av_guess_format(NULL, filename.c_str(), NULL);
	avformat_alloc_output_context2(&fc, NULL, NULL, filename.c_str());

	// Setting up the codec.
	AVCodec* codec = avcodec_find_encoder_by_name("libx264");
	AVDictionary* opt = NULL;
    av_dict_set(&opt, "tune", "zerolatency", 0);
	av_dict_set(&opt, "preset", "ultrafast", 0);
	av_dict_set(&opt, "crf", "20", 0);

	stream = avformat_new_stream(fc, codec);
	c = stream->codec;
	c->width = width;
	c->height = height;
	c->pix_fmt = AV_PIX_FMT_YUV420P;
	c->time_base = (AVRational){ 1, FPS };

	// Setting up the format, its stream(s),
	// linking with the codec(s) and write the header.
	if (fc->oformat->flags & AVFMT_GLOBALHEADER)
	{
		// Some formats require a global header.
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);

	// Once the codec is set up, we need to let the container know
	// which codec are the streams using, in this case the only (video) stream.
	stream->time_base = (AVRational){ 1, FPS };
	av_dump_format(fc, 0, filename.c_str(), 1);
	avio_open(&fc->pb, filename.c_str(), AVIO_FLAG_WRITE);
	int ret = avformat_write_header(fc, &opt);
	av_dict_free(&opt);

	// Allocating memory for each conversion output YUV frame.
	yuvpic = av_frame_alloc();
	yuvpic->format = AV_PIX_FMT_YUV420P;
	yuvpic->width = width;
	yuvpic->height = height;
	ret = av_frame_get_buffer(yuvpic, 1);
}

void FrameWriter::add_frame(const uint8_t* pixels, int msec, bool y_invert)
{
	// Not actually scaling anything, but just converting
	// the RGB data to YUV and store it in yuvpic.
    int stride[] = {int(4 * width)};
    const uint8_t *formatted_pixels = pixels;

    if (y_invert)
    {
        formatted_pixels += stride[0] * (height - 1);
        stride[0] *= -1;
    }

    sws_scale(swsCtx, &formatted_pixels, stride, 0,
    	height, yuvpic->data, yuvpic->linesize);

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	yuvpic->pts = msec;

	int got_output;
	avcodec_encode_video2(c, &pkt, yuvpic, &got_output);
	if (got_output)
		finish_frame();
}

void FrameWriter::finish_frame()
{
	static int iframe = 0;
	av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000 }, stream->time_base);

	pkt.stream_index = stream->index;

	av_interleaved_write_frame(fc, &pkt);
	av_packet_unref(&pkt);

	printf("Wrote frame %d\n", iframe++);
	fflush(stdout);
}

FrameWriter::~FrameWriter()
{
	// Writing the delayed frames:
	for (int got_output = 1; got_output; )
	{
		avcodec_encode_video2(c, &pkt, NULL, &got_output);
		if (got_output)
			finish_frame();
	}

	// Writing the end of the file.
	av_write_trailer(fc);

	// Closing the file.
	if (!(fmt->flags & AVFMT_NOFILE))
	    avio_closep(&fc->pb);
	avcodec_close(stream->codec);

	// Freeing all the allocated memory:
	sws_freeContext(swsCtx);
	av_frame_free(&yuvpic);
	avformat_free_context(fc);
}

