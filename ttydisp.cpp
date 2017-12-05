#include <iostream>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <string>
#include <vector>

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

#include "logger.hpp"
static Logger logger;

/* tty dimensions */

#define COLOR_TEXT_FORMAT "\x1B[48;05;%um\x1B[38;05;%um%c"
#define COLOR_FORMAT "\x1B[48;05;%um "
#define COLOR_RESET "\x1B[0m"

typedef struct {
    std::string filename;
    bool verbose = false;
    int height = -1;
    int width = -1;
} configType;

std::pair<unsigned/*width*/, unsigned/*height*/> getTTYDimensions(void) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return {w.ws_col, w.ws_row};
}

/* ffmpeg abstraction */
class Stream {
  private:
    struct av {
        AVCodec* codec = nullptr;
        AVCodecContext* codecContext = nullptr;
        AVFormatContext* formatContext = nullptr;
        AVDictionary* dict;
        struct SwsContext *swsContext = nullptr;
        int videoStreamIndex;
    } av;
  protected:
    unsigned char generateANSIColor(uint8_t r, uint8_t g, uint8_t b) {
        return 16 + (36 * lround(r*5.0/256)) + (6 * lround(g*5.0/256)) + lround(b*5.0/256);
    }
    void resetFrame(unsigned ttyHeight) {
        for(unsigned i = 0; i < ttyHeight - 1; ++i)
            printf("\x1B[F");
    }
    void render(AVFrame* frame, unsigned ttyWidth, unsigned ttyHeight) {
        unsigned i, j;
        for(j = 0; j < ttyHeight; ++j) {
            for(i = 0; i < ttyWidth; ++i) {
                int x = i * av.codecContext->width / ttyWidth;
                int y = j * av.codecContext->height / ttyHeight;
                uint8_t* p = frame->data[0] + y * frame->linesize[0] + x;
                uint8_t r = *p;
                uint8_t g = *(p+1);
                uint8_t b = *(p+2);

                auto ansiColor = generateANSIColor(r, g, b);
                printf(COLOR_FORMAT, ansiColor);
            }
            printf(COLOR_RESET);
            if(i < ttyHeight - 1) {
                printf("\n");
            } else {
                printf("\x1B[m");
                fflush(stdout);
            }
        }
    }
  public:
    std::string filename;
    int readFormat(bool verbose) {
        int err = avformat_open_input(&av.formatContext, filename.c_str(), NULL, NULL);
        if(err != 0) {
            logger.log("Error reading input from file `" + filename);
            char error[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(err, error, AV_ERROR_MAX_STRING_SIZE);
            logger.log(error);
            return 1;
        }
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
        av.codecContext = avcodec_alloc_context3(nullptr);
        if(!av.codecContext) {
            logger.log("Error allocating codecc ontext");
            return 1;
        }
        for(unsigned i = 0; i < av.formatContext->nb_streams; ++i) {
            if(av.formatContext->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO) {
                av.videoStreamIndex = i;
                break;
            }
        }
        if(av.videoStreamIndex == -1) {
            logger.log("Could not read any video stream");
            return 2;
        }
        auto stream = av.formatContext->streams[av.videoStreamIndex];
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
        /*
        logger.log("Expecting segmentation fault");
        // TODO: Fix this line
        if(avcodec_open2(av.codecContext, av.codec, &av.dict) < 0) {
            logger.log("Error opening codec");
            return 5;
        }
        logger.log("Passed expected segmentation fault");
        */
        return 0;
    }

    int display(configType const& config) {
        if(av.codec == nullptr) {
            logger.log("Attempted to display video without reading the codec first");
            return 1;
        }
        AVFrame* frame = av_frame_alloc();
        AVFrame* RGBframe = av_frame_alloc();
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, av.codecContext->width, av.codecContext->height, 16);
        uint8_t* buffer = (uint8_t*) av_malloc(numBytes*sizeof(uint8_t));

        logger.log("Getting context");
        av.swsContext =
            sws_getContext
            (
             av.codecContext->width,
             av.codecContext->height,
             av.codecContext->pix_fmt,
             av.codecContext->width,
             av.codecContext->height,
             AV_PIX_FMT_RGB24,
             SWS_BILINEAR,
             NULL,
             NULL,
             NULL
            );
        logger.log("Got context");

        AVPacket packet;
        unsigned frameNum = 0;
        av_init_packet(&packet);
        while(av_read_frame(av.formatContext, &packet) >= 0)
        {
            if(packet.stream_index != av.videoStreamIndex) continue;
            auto [ tty_width, tty_height ] = getTTYDimensions();
            if(frameNum) {
                resetFrame(tty_height); // move cursor back
            }

            avcodec_send_packet(av.codecContext, &packet);
            avcodec_receive_frame(av.codecContext, frame);

            av_image_fill_arrays(frame->data, frame->linesize, buffer, AV_PIX_FMT_RGB24, av.codecContext->width, av.codecContext->height, 1);

            // avpicture_fill((AVPicture*) RGBframe, buffer, AV_PIX_FMT_RGB24, av.codecContext->width, av.codecContext->height);

            if(config.verbose)
                logger.log("Scaling");
            sws_scale(av.swsContext, frame->data, frame->linesize, 0, av.codecContext->height, RGBframe->data, RGBframe->linesize);
            if(config.verbose)
                logger.log("Done");

            if(config.verbose)
                logger.log("Rendering");
            render(RGBframe, tty_width, tty_height);
            if(config.verbose)
                logger.log("Done");

            // logger.log("Read frame " + std::to_string(frameNum));
            frameNum++;
            av_packet_unref(&packet);
            // av_free_packet(&packet);
        }
        sws_freeContext(av.swsContext);
        logger.log("Finished displaying");
        return 0;
    }
    Stream(std::string f) {
        logger.log("Initializing stream");
        filename = f;
    }
    ~Stream(void) {
        logger.log("Destructing stream");
        if(av.codec != nullptr) {
            avcodec_close(av.codecContext);
            // av_free(av.codec);
        }
        logger.log("Stream destroyed");
    }
};

static std::vector<std::function<int(void)>> terminationHooks;
enum InterruptType { CONTINUE, HALT, ERROR };
static std::unordered_map<std::string, std::function<InterruptType(int&, int, char**, configType&)>> functionMap {
    /* lambdas return 0 if no error */
    {"-f", [](int& argumentIndex, int argc, char** arguments, configType& config)
        {
            if(argumentIndex+1 < argc) {
                config.filename = std::string{arguments[++argumentIndex]};
                return CONTINUE;
            }
            else {
                std::cerr << "No argument passed to `-f' flag" << std::endl;
                return ERROR;
            }
        }
    },
    {"-v", [](int&, int, char**, configType& config)
        {
            config.verbose = true;
            return CONTINUE;
        }
    },
    {"--help", [](int&, int, char**, configType&)
        {
            return HALT;
        }
    }/*,
    {"-w", [](int&, int, char**, configType& config)
        {
            terminationHooks.push_back([&config]()
                {
                    if(config.height == -1) {
                        std::cerr << "Custom width undefined with custom height" << std::endl;
                        return 1;
                    }
                    return 0;
                }
            );
            return CONTINUE;
        }
    },
    {"-h", [](int&, int, char**, configType&)
        {
            terminationHooks.push_back([](configType& config)
                {
                    if(config.width == -1) {
                        std::cerr << "Custom height undefined with custom width" << std::endl;
                        return 1;
                    }
                    return 0;
                }
            );
            return CONTINUE;
        }
    }*/
};
std::pair<bool, configType> parseArguments(int argc, char** argv) {
    configType config;
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
                    std::cerr << "Fatal error thrown in argument parser" << std::endl;
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

    for(auto hook : terminationHooks) {
        int err = hook();
        if(err) {
            std::cerr << "Error running termination hooks" << std::endl;
        }
    }

    return {true, config};
}

int main(int argc, char** argv) {
    auto [success, config] = parseArguments(argc, argv);
    if(!success) {
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
    std::cout << "Reading from file `" << config.filename << "'" << std::endl;

    logger.log("Reading FFMPEG codecs");
    avcodec_register_all();
    av_register_all();

    Stream stream{config.filename};
    logger.log("Starting reading");

    int err = stream.readFormat(/*verbose*/config.verbose);
    if(err != 0) return 1;
    logger.log("Finished reading format");
    err = stream.readVideoCodec();
    logger.log("Finished reading video codec");
    stream.display(config);

    return 0;
}
