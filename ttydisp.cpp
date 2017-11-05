#include <iostream>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <string>
#include <vector>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
}

#include "logger.hpp"
static Logger logger;

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
    int display(void) {
        if(av.codec == nullptr) {
            logger.log("Attempted to display video without reading the codec first");
            return 1;
        }
        AVFrame* frame = av_frame_alloc();
        AVFrame* RGBframe = av_frame_alloc();
        int numBytes = avpicture_get_size(AV_PIX_FMT_RGB24, av.codecContext->width, av.codecContext->height);
        uint8_t* buffer = (uint8_t*) av_malloc(numBytes*sizeof(uint8_t));

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

        avpicture_fill((AVPicture*) RGBframe, buffer, AV_PIX_FMT_RGB24, av.codecContext->width, av.codecContext->height);

        AVPacket packet;
        int frameFinished = 0;
        unsigned frameNum = 0;
        while(av_read_frame(av.formatContext, &packet) >= 0) {
            if(packet.stream_index != av.videoStreamIndex) continue;

            logger.log("Expecting segfault");
            // TODO: Fix this line
            avcodec_decode_video2(av.codecContext, frame, &frameFinished, &packet);
            logger.log("Passed segfault");

            if(!frameFinished) { logger.log("Got unfinished frame"); continue; }
            sws_scale(av.swsContext, (uint8_t const* const*) frame->data, frame->linesize, 0, av.codecContext->height, RGBframe->data, RGBframe->linesize);
            std::cout << "Read frame " << frameNum << '\n';
            frameNum++;
        }
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

typedef struct {
    std::string filename;
    bool verbose = false;
} configType;

enum InterruptType { CONTINUE, HALT, ERROR };
static std::unordered_map<std::string, std::function<InterruptType(int&, int, char**, configType&)>> functionMap {
    /* lambdas return 0 if no error */
    {"-f", [](int& number, int argc, char** arguments, configType& config)
        {
            if(number+1 < argc) {
                config.filename = std::string{arguments[++number]};
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
    {"-h", [](int&, int, char**, configType&)
        {
            return HALT;
        }
    }
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

    return {true, config};
}

int main(int argc, char** argv) {
    auto [success, config] = parseArguments(argc, argv);
    if(!success) {
        return 1;
    }
    if(config.verbose) {
        // av_log_set_level(AV_LOG_DEBUG);
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
    stream.display();

    return 0;
}
