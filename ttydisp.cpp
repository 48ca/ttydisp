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
    } av;
  public:
    std::string filename;
    void display(void) {
        /*
        logger.log("Searching for video codec");
        av.codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        // av.codec = av_guess_format("mp4", NULL, NULL);
        if(!av.codec) {
            std::cerr << "Couldn't find MP4 codec" << std::endl;
            return;
        }
        logger.log("Allocating context");
        av.ctx = avcodec_alloc_context3(av.codec);
        AVFrame* picture = av_frame_alloc();
        if(avcodec_open2(av.ctx, av.codec, NULL) < 0) {
            logger.log("Error opening codec");
        }
        */
    }
    int read(void) {
        if(int err = avformat_open_input(&av.formatContext, filename.c_str(), NULL, NULL) != 0) {
            logger.log("Error reading input from file `" + filename);
            char error[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(err, error, AV_ERROR_MAX_STRING_SIZE);
            logger.log(error);
            return 1;
        }
        if(avformat_find_stream_info(av.formatContext, NULL) < 0) {
            logger.log("Error finding stream info");
            // return 1;
        }
        logger.log("Finished read");
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
    bool verbose;
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

    Stream stream{config.filename};
    logger.log("Reading");
    auto readError = stream.read();
    if(readError) logger.log("Read error");

    stream.display();

    return 0;
}
