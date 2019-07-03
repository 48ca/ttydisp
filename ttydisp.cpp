#include <iostream>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <csignal>

#include "colors.h"

#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

using clk = std::chrono::steady_clock;

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
}

#include "conf.h"

#include "logger.h"
std::ofstream of(LOG_FILENAME, std::ofstream::out);
static Logger logger(of);

static bool stop = false;

static const bool istty = isatty(fileno(stdout));

#define COLOR_TEXT_FORMAT "\x1B[48;05;%um\x1B[38;05;%um%c"
#define COLOR_FORMAT "\x1B[48;05;%um "
#define COLOR_RESET "\x1B[0m"

typedef struct {
    std::string filename;
    bool verbose = false;
    int height = -1;
    int width = -1;
    bool loop = false;
    uint8_t pad = 0;
    uint16_t fps = 0;
    bool accurate_colors = true;
} config_t;

static config_t config;

std::pair<unsigned/*width*/, unsigned/*height*/> getTTYDimensions(void) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return {w.ws_col, w.ws_row};
}

/* ffmpeg abstraction */
class Stream {
  private:
    struct av {
        AVCodec *codec = nullptr;
        AVCodecContext *codecContext = nullptr;
        AVFormatContext *formatContext = nullptr;
        AVDictionary *dict = nullptr;
        struct SwsContext *swsContext = nullptr;
        int videoStreamIndex = -1;
    } av;
    unsigned frameNum = 0;
    uint8_t pad = 0;
  protected:
    double wait_time() {
        if(!av.codecContext) return 0;
        if(config.fps != 0) {
            return 1.0/config.fps;
        }
        AVRational r = av.codecContext->time_base;
        return av_q2d(r) * std::max(av.codecContext->ticks_per_frame, 1);
    }
    unsigned char generateANSIColor(uint8_t r, uint8_t g, uint8_t b, uint8_t pad) {
        if(config.accurate_colors)
            return get_closest_color((r << 16) + (g << 8) + b);

        r = r >= pad ? r - pad : 0;
        g = g >= pad ? g - pad : 0;
        b = b >= pad ? b - pad : 0;
        float t = (r + g + b)/3.0; // 24 grayscale colors
        uint8_t g_rawlum = lround(t * 23.0/255);
        uint8_t r_rawval = lround(r * 5.0/255);
        uint8_t g_rawval = lround(g * 5.0/255);
        uint8_t b_rawval = lround(b * 5.0/255);

        float g_lum = g_rawlum * 255/23.0;
        float r_val = r_rawval * 255/5.0;
        float g_val = g_rawval * 255/5.0;
        float b_val = b_rawval * 255/5.0;

        float g_score = fabs(r - g_lum) + fabs(g - g_lum) + fabs(b - g_lum);
        float c_score = fabs(r - r_val) + fabs(g - g_val) + fabs(b - b_val);

        if(c_score < g_score - COLOR_BIAS || g_rawlum == 1) {
            return 16 + (36 * r_rawval) + (6 * g_rawval) + b_rawval;
        } else {
            return 232 + g_rawlum;
        }
    }
    void resetFrame(unsigned height) {
        for(unsigned i = 0; i < height - 1; ++i)
            printf("\x1B[F");
    }
    AVFrame* convert(AVFrame* frame, unsigned width, unsigned height) {
        // We don't use YUV because it introduces artefacts in the final image
        // av.swsContext = sws_getCachedContext(av.swsContext, frame->width, frame->height, (AVPixelFormat)frame->format, width, height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
        // av.swsContext = sws_getCachedContext(av.swsContext, frame->width, frame->height, (AVPixelFormat)frame->format, width, height, AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
        av.swsContext = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, width, height, AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
        logger.log("DONE");
        logger.log("Scaling to dims " + std::to_string(width) + ", " + std::to_string(height));
        AVFrame *nframe = av_frame_alloc();
        nframe->width = width;
        nframe->height = height;
        // unsigned nb = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1);
        unsigned nb = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        auto buffer = (uint8_t*)av_malloc(nb * sizeof(uint8_t));
        // av_image_fill_arrays(nframe->data, nframe->linesize, buffer, AV_PIX_FMT_YUV420P, width, height, 1);
        av_image_fill_arrays(nframe->data, nframe->linesize, buffer, AV_PIX_FMT_RGB24, width, height, 1);
        sws_scale(av.swsContext, (uint8_t**)frame->data, frame->linesize, 0, frame->height, (uint8_t**)nframe->data, nframe->linesize);
        return nframe;
    }
    void render(AVFrame* frame) {
        unsigned x, y;
        unsigned height = frame->height;
        unsigned width = frame->width;
        // std::cout << width <<' ' << height << std::endl;;
        // return;
        for(y = 0; y < height; ++y) {
            for(x = 0; x < width; ++x) {
                /*
                // YUV
                uint8_t Y = frame->data[0][y * frame->linesize[0] + x];
                uint8_t U = frame->data[1][y/2 * frame->linesize[1] + x/2];
                uint8_t V = frame->data[2][y/2 * frame->linesize[2] + x/2];

                // RGB conversion
                const uint8_t r = Y + 1.402*(V-128);
                const uint8_t g = Y - 0.344*(U-128) - 0.714*(V-128);
                const uint8_t b = Y + 1.772*(U-128);
                */
                const uint8_t r = frame->data[0][0 + 3 * (x + y * width)];
                const uint8_t g = frame->data[0][1 + 3 * (x + y * width)];
                const uint8_t b = frame->data[0][2 + 3 * (x + y * width)];

                auto ansiColor = generateANSIColor(r, g, b, pad);

                printf(COLOR_FORMAT, ansiColor);
                // printf(COLOR_TEXT_FORMAT, ansiColor, 0, ansiColor >= 232 ? 'g' : 'c');
                // printf("%d %d %d (%d %d %d)\n", x, y, ansiColor, r, g, b);
            }
            printf(COLOR_RESET);

            if(y < height - 1) {
                printf("\n");
            } else {
                printf("\x1B[m");
                fflush(stdout);
            }
        }
    }
  public:
    std::string filename;
    AVFormatContext* getFormatContext() const {
        return av.formatContext;
    }
    int readFormat(bool verbose) {
        int err = avformat_open_input(&av.formatContext, filename.c_str(), NULL, NULL);
        if(err != 0) {
            logger.log("Error reading input from file `" + filename + "'");
            char error[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(err, error, AV_ERROR_MAX_STRING_SIZE);
            logger.log(error);
            return 1;
        }
        // avformat_find_stream_info(av.formatContext, NULL);
        if(avformat_find_stream_info(av.formatContext, NULL) < 0) {
            logger.log("Error finding stream info");
            return 1;
        }
        if(verbose)
            av_dump_format(av.formatContext, 0, filename.c_str(), 0);
        return 0;
    }
    int readVideoCodec(void) {
        if(av.formatContext == nullptr) {
            std::cerr << "Tried to read video without first reading format" << std::endl;
            return 1;
        }
        av.codecContext = avcodec_alloc_context3(av.codec);
        if(!av.codecContext) {
            logger.log("Error allocating codec context");
            return 1;
        }
        for(unsigned i = 0; i < av.formatContext->nb_streams; ++i) {
            if(av.formatContext->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO) {
                av.videoStreamIndex = i;
                break;
            }
        }
        if(av.videoStreamIndex < 0) {
            logger.log("Could not read any video stream");
            return 2;
        }
        auto stream = av.formatContext->streams[av.videoStreamIndex];
        if(!stream) {
            logger.log("Error reading video stream");
            return 6;
        }
        int ret = avcodec_parameters_to_context(av.codecContext, stream->codecpar);
        if(ret < 0) {
            logger.log("Error reading codec context");
            return 3;
        }
        av.codec = avcodec_find_decoder(av.codecContext->codec_id);
        if(av.codec == nullptr) {
            logger.log("Unsupported codec");
            return 4;
        }
        if(avcodec_open2(av.codecContext, av.codec, &av.dict) < 0) {
            logger.log("Error opening codec");
            return 5;
        }
        return 0;
    }

    int display() {
        if(av.codec == nullptr) {
            logger.log("Attempted to display video without reading the codec first");
            return 1;
        }
        AVFrame* frame = av_frame_alloc();

        AVPacket packet;
        av_init_packet(&packet);
        packet.data = NULL;
        packet.size = 0;

        while(av_read_frame(av.formatContext, &packet) >= 0)
        {
            if(packet.stream_index != av.videoStreamIndex) continue;

            auto start = clk::now();
            std::chrono::nanoseconds dur((int)(1E9 * wait_time() - SPINLOCK_NS));
            auto stopt  = start + dur;

            auto avs = clk::now();
            int ret = 0;
            // AVERROR(EAGAIN) means that we need to feed more
            do {
                do {
                    ret = avcodec_send_packet(av.codecContext, &packet);
                } while(ret == AVERROR(EAGAIN));

                if(ret == AVERROR_EOF || ret == AVERROR(EINVAL)) {
                    fprintf(stderr, "AVERROR(EAGAIN): %d, AVERROR_EOF: %d, AVERROR(EINVAL): %d\n", AVERROR(EAGAIN), AVERROR_EOF, AVERROR(EINVAL));
                    fprintf(stderr, "fe_read_frame: Frame getting error (%d)!\n", ret);
                    return 1;
                }

                ret = avcodec_receive_frame(av.codecContext, frame);
            } while(ret == AVERROR(EAGAIN));

            auto [ tty_width, tty_height ] = getTTYDimensions();
            if(config.verbose)
                tty_height -= 1;

            auto height = config.height < 0 ? tty_height : config.height;
            auto width  = config.width  < 0 ? tty_width  : config.width;
            float aspect = (float)(frame->height)/frame->width * PIXEL_ASPECT_RATIO;
            if(config.height < 0 && config.width < 0) {
                if((unsigned)round(aspect * width) > height) {
                    // width is too great
                    width = (unsigned)(height/aspect);
                } else {
                    // height is too great
                    height = (unsigned)(aspect * width);
                }
            } else {
                if(config.height >= 0)
                    if(config.width < 0)
                        width = (unsigned)(height/aspect);
                if(config.height < 0)
                    if(config.width >= 0)
                        height = (unsigned)(width*aspect);
            }

            if(height <= 0 || width <= 0) {
                std::cerr << "Got invalid dimensions" << std::endl;
                return 1;
            }

            if(frameNum) {
                resetFrame(height + (config.verbose ? 1 : 0)); // move cursor back
            }

            auto ave = clk::now();
            logger.log("Took " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(ave-avs).count()) + " ms to decode");

            if(config.verbose)
                logger.log("Rendering frame " + std::to_string(frameNum));

            auto nf = convert(frame, width, height);
            render(nf);
            av_frame_unref(nf);
            if(config.verbose)
                logger.log("Rendered frame " + std::to_string(frameNum));

            frameNum++;
            av_packet_unref(&packet);
            auto n = clk::now();
            if(stopt < n) {
                logger.log("Missed frame by " + std::to_string(
                            std::chrono::duration_cast<std::chrono::milliseconds>(n - stopt).count()
                            ) + "ms");
            }
            if(istty) {
                std::this_thread::sleep_until(stopt);
                while(stopt + std::chrono::nanoseconds((int)SPINLOCK_NS) > clk::now()); // accurate waiting
            }
            if(config.verbose) {
                n = clk::now();
                std::cout << "\n file: " + config.filename + " | fps (des): " + std::to_string(1.0/wait_time())
                    + " | fps (act): " + std::to_string(1.0E9/std::chrono::duration_cast<std::chrono::nanoseconds>(n - start).count())
                    + " | height: " + std::to_string(height) + " | width: " + std::to_string(width) + "   ";
            }
            if(stop) // SIGINT
                goto done;

        }
done:
        puts("");
        logger.log("Finished displaying");
        av_packet_unref(&packet);
        av_frame_unref(frame);
        return 0;
    }
    Stream(config_t const& c) : pad(c.pad), filename(c.filename) {
        logger.log("Initializing stream");
    }
    ~Stream(void) {
        logger.log("Destructing stream");
        if(av.formatContext) {
            avformat_close_input(&av.formatContext);
        }
        if(av.codec != nullptr) {
            avcodec_close(av.codecContext);
            // av_free(av.codec);
        }
        if(av.swsContext != nullptr) {
            sws_freeContext(av.swsContext);
        }
        logger.log("Stream destroyed");
    }
};

static std::vector<std::function<int(void)>> termination_hooks;
enum Interrupt_t { CONTINUE, HALT, ERROR };
static std::unordered_map<std::string, std::function<Interrupt_t(int&, int, char**, config_t&)>> functionMap {
    {"-v", [](int&, int, char**, config_t& config)
        {
            config.verbose = true;
            return CONTINUE;
        }
    },
    {"-f", [](int& i, int argc, char** argv, config_t& config)
        {
            if(++i < argc) {
                config.fps = atoi(argv[i]);
                if(std::to_string(config.fps) != argv[i])
                    return ERROR;
            } else
                return ERROR;
            return CONTINUE;
        }
    },
    {"-l", [](int&, int, char**, config_t& config)
        {
            config.loop = true;
            return CONTINUE;
        }
    },
    {"-fc", [](int&, int, char**, config_t& config)
        {
            config.accurate_colors = false;
            return CONTINUE;
        }
    },
    {"-p", [](int& i, int argc, char** argv, config_t& config)
        {
            if(++i < argc) {
                int p = atoi(argv[i]);
                if(p < 0) {
                    return ERROR;
                }
                config.pad = (uint8_t)p;
                if(std::to_string(config.pad) != argv[i])
                    return ERROR;
            } else
                return ERROR;
            return CONTINUE;
        }
    },
    {"--help", [](int&, int, char**, config_t&)
        {
            std::cout << "usage: ttydisp [options] <filename>\n"
                << "    --help:\n"
                << "        Show this help message\n"
                << "    -l:\n"
                << "        Enable looping\n"
                << "    -fc:\n"
                << "        Disable accurate colors (might be faster)\n"
                << "    -p:\n"
                << "        Set brightness padding\n"
                << "    -v:\n"
                << "        Enable verbose logging\n"
                << "    -w:\n"
                << "        Set output width\n"
                << "    -h:\n"
                << "        Set output height\n";
            return HALT;
        }
    },
    {"-w", [](int& i, int argc, char** argv, config_t& config)
        {
            if(config.width >= 0)
                return ERROR;

            if(++i < argc) {
                config.width = atoi(argv[i]);
                if(std::to_string(config.width) != argv[i] || config.width <= 0)
                    return ERROR;
            } else
                return ERROR;
            termination_hooks.emplace_back([&config]()
                {
                    if(config.width < 0) {
                        std::cerr << "Custom width undefined with custom height" << std::endl;
                        return 1;
                    }
                    return 0;
                }
            );
            return CONTINUE;
        }
    },
    {"-h", [](int& i, int argc, char** argv, config_t& config)
        {
            if(config.height >= 0)
                return ERROR;

            if(++i < argc) {
                config.height = atoi(argv[i]);
                if(std::to_string(config.height) != argv[i] || config.height <= 0)
                    return ERROR;
            } else
                return ERROR;
            termination_hooks.emplace_back([&config]()
                {
                    if(config.height < 0) {
                        std::cerr << "Custom height undefined with custom width" << std::endl;
                        return 1;
                    }
                    return 0;
                }
            );
            return CONTINUE;
        }
    }
};
std::pair<bool, config_t> parseArguments(int argc, char** argv) {
    config_t config;
    int i;
    // std::string executionName{argv[0]};
    for(i = 1; i < argc; ++i)
    {
        std::string arg{argv[i]};
        if(arg[0] == '-') {
            auto func = functionMap.find(arg);
            if(func == functionMap.end()) {
                std::cerr << "Unknown switch `" << arg << "'" << std::endl;
                break;
            }
            switch(func->second(i, argc, argv, config)) {
                // check interrupt type
                case ERROR:
                    std::cerr << "Fatal error thrown while parsing argument " << func->first << std::endl;
                case HALT:
                    return {false, config};
                default:
                case CONTINUE:
                    break;
            }
        } else {
            if(config.filename.empty())
                config.filename = std::string{arg};
            else {
                std::cerr << "Unrecognized argument `" << arg << "'" << std::endl;
                break;
            }
        }
    }

    if(i < argc) {
        // Argument parser failure
        return {false, config};
    }

    for(auto hook : termination_hooks) {
        if(hook()) {
            std::cerr << "Error running termination hooks" << std::endl;
            return {false, config};
        }
    }

    if(!istty) {
        if(config.height < 0 && config.width < 0) {
            logger.log("Output is not a terminal, so custom dimensions must be set");
            return {false, config};
        }
    }

    return {true, config};
}

void interrupt_handler(int) {
    stop = true;
    logger.log("Got SIGINT. Exiting...");
}

void log(void*, int level, const char *fmt, va_list vargs) {
    if(level <= 24) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        // fprintf(stdout, fmt, vargs);
        sprintf(message, fmt, vargs);
        logger.log(message);
    }
}

int main(int argc, char** argv) {
    av_log_set_callback(log);
    std::pair res = parseArguments(argc, argv);
    int success = res.first;
    config = res.second;

    if(!success) {
        logger.dump(std::cerr);
        return 1;
    }
    if(config.verbose) {
        av_log_set_level(AV_LOG_DEBUG);
        logger.verbose = true;
    }

    if(config.filename.empty()) {
        std::cout << "No file specified" << std::endl;
        return 1;
    }
    logger.log("Reading from file `" + config.filename + "'");

    Stream stream{config};
    logger.log("Starting reading");

    int err = stream.readFormat(config.verbose);
    if(err) {
        std::cerr << "Error reading video format" << '\n';
        logger.dump(std::cerr);
        return 1;
    }
    logger.log("Finished reading format");
    err = stream.readVideoCodec();
    if(err) {
        std::cerr << "Error reading video codec" << '\n';
        logger.dump(std::cerr);
        return 1;
    }
    logger.log("Finished reading video codec");

    // Capture SIGINT, finish the frame
    signal(SIGINT, interrupt_handler);

    int ret;
    do {
        ret = stream.display();
        av_seek_frame(stream.getFormatContext(), -1, 0, AVSEEK_FLAG_FRAME);
    } while(config.loop && !ret && !stop);

    return 0;
}
