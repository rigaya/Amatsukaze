﻿/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "AmatsukazeTestImpl.h"
#include "faad.h"
#include <thread>
#include <chrono>

/* static */ int test::PrintCRCTable(AMTContext& ctx, const ConfigWrapper& setting) {
    CRC32 crc;

    const uint32_t* table = crc.getTable();

    for (int i = 0; i < 256; ++i) {
        fprintf(stderr, "0x%08x%c", table[i], ((i + 1) % 8) ? ',' : '\n');
    }

    return 0;
}

/* static */ int test::CheckCRC(AMTContext& ctx, const ConfigWrapper& setting) {
    CRC32 crc;

    auto toBytesBE = [](uint8_t* bytes, uint32_t data) {
        bytes[3] = ((data >> 0) & 0xFF);
        bytes[2] = ((data >> 8) & 0xFF);
        bytes[1] = ((data >> 16) & 0xFF);
        bytes[0] = ((data >> 24) & 0xFF);
        };

    const uint8_t* data = (const uint8_t*)"ABCD";
    uint32_t result = crc.calc(data, 4, 123456);
    // fprintf(stderr, "RESULT: 0x%x\n");
    uint8_t buf[4]; toBytesBE(buf, result);
    uint32_t t = crc.calc(data, 4, 123456);
    result = crc.calc(buf, 4, t);

    if (result != 0) {
        fprintf(stderr, "[CheckCRC] Result does not match: 0x%x\n", result);
        return 1;
    }

    return 0;
}

/* static */ int test::ReadBits(AMTContext& ctx, const ConfigWrapper& setting) {
    uint8_t data[16];
    srand(0);
    for (int i = 0; i < sizeof(data); ++i) data[i] = rand();

    //uint16_t a = read16(data);
    //uint32_t b = read24(data);
    //uint32_t c = read32(data);
    //uint64_t d = read40(data);
    uint64_t e = read48(data);

    fprintf(stderr, "sum=%f\n", double(e));

    return 0;
}

/* static */ int test::CheckAutoBuffer(AMTContext& ctx, const ConfigWrapper& setting) {
    srand(0);

    std::unique_ptr<uint8_t[]> buf = std::unique_ptr<uint8_t[]>(new uint8_t[65536]);
    int addCnt = 0;
    int delCnt = 0;

    AutoBuffer ab;
    for (int i = 0; i < 10000; ++i) {
        int addNum = rand();
        int delNum = rand();

        for (int c = 0; c < addNum; ++c) {
            buf[c] = addCnt++;
        }
        //fprintf(stderr, "Add %d\n", addNum);
        ab.add(MemoryChunk(buf.get(), addNum));

        uint8_t *data = ab.ptr();
        for (int c = 0; c < (int)ab.size(); ++c) {
            if (data[c] != ((delCnt + c) & 0xFF)) {
                fprintf(stderr, "[CheckAutoBuffer] Result does not match\n");
                return 1;
            }
        }

        delNum = std::min<int>(delNum, (int)ab.size());
        //fprintf(stderr, "Del %d\n", delNum);
        ab.trimHead(delNum);
        delCnt += delNum;
    }

    return 0;
}

/* static */ int test::VerifyMpeg2Ps(AMTContext& ctx, const ConfigWrapper& setting) {
    enum {
        BUF_SIZE = 1400 * 1024 * 1024, // 1GB
    };
    auto buf = std::unique_ptr<uint8_t>(new uint8_t[BUF_SIZE]);
    FILE* fp = fsopenT(setting.getSrcFilePath().c_str(), _T("rb"), _SH_DENYNO);
    if (fp == nullptr) {
        return 1;
    }
    try {
        AMTContext ctx;
        PsStreamVerifier psVerifier(ctx);

        size_t readBytes = fread(buf.get(), 1, BUF_SIZE, fp);
        psVerifier.verify(MemoryChunk(buf.get(), readBytes));
    } catch (const Exception& e) {
        fprintf(stderr, "Verify MPEG2-PS Error: 例外がスローされました -> %s\n", e.message());
        return 1;
    }
    fclose(fp);
    fp = NULL;

    return 0;
}

/* static */ int test::ReadTS(AMTContext& ctx, const ConfigWrapper& setting) {
    try {
        auto splitter = std::unique_ptr<AMTSplitter>(new AMTSplitter(ctx, setting));
        if (setting.getServiceId() > 0) {
            splitter->setServiceId(setting.getServiceId());
        }
        StreamReformInfo reformInfo = splitter->split();
        reformInfo.serialize(setting.getStreamInfoPath());
    } catch (const Exception& e) {
        fprintf(stderr, "ReadTS Error: 例外がスローされました -> %s\n", e.message());
        return 1;
    }

    return 0;
}

/* static */ int test::AacDecode(AMTContext& ctx, const ConfigWrapper& setting) {
    tstring srcfile = setting.getSrcFilePath() + _T(".aac");
    tstring testfile = setting.getSrcFilePath() + _T(".wav");

    FILE* fp = fsopenT(srcfile.c_str(), _T("rb"), _SH_DENYNO);
    if (fp == nullptr) {
        return 1;
    }

    enum {
        BUF_SIZE = 1024 * 1024, // 1MB
    };
    auto buf = std::unique_ptr<uint8_t>(new uint8_t[BUF_SIZE]);
    size_t readBytes = fread(buf.get(), 1, BUF_SIZE, fp);

    AutoBuffer decoded;

    NeAACDecHandle hAacDec = NeAACDecOpen();
    /*
    NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(hAacDec);
    conf->outputFormat = FAAD_FMT_16BIT;
    NeAACDecSetConfiguration(hAacDec, conf);
    */

    unsigned long samplerate;
    unsigned char channels;
    if (NeAACDecInit(hAacDec, buf.get(), (unsigned long)readBytes, &samplerate, &channels)) {
        fprintf(stderr, "NeAACDecInit failed\n");
        return 1;
    }

    printf("samplerate=%d, channels=%d\n", samplerate, channels);

    for (int i = 0; i < (int)readBytes; ) {
        NeAACDecFrameInfo frameInfo;
        void* samples = NeAACDecDecode(hAacDec, &frameInfo, buf.get() + i, (unsigned long)readBytes - i);
        decoded.add(MemoryChunk((uint8_t*)samples, frameInfo.samples * 2));
        i += frameInfo.bytesconsumed;
    }

    // 正解データと比較
    FILE* testfp = fsopenT(testfile.c_str(), _T("rb"), _SH_DENYNO);
    if (testfp == nullptr) {
        return 1;
    }

    auto testbuf = std::unique_ptr<uint8_t>(new uint8_t[BUF_SIZE]);
    size_t testBytes = fread(testbuf.get(), 1, BUF_SIZE, testfp);
    // data chunkを探す
    for (int i = sizeof(RiffHeader); ; ) {
        if (!(i < (int)testBytes - 8)) {
            fprintf(stderr, "出力が小さすぎます\n");
            return 1;
        }
        if (read32(testbuf.get() + i) == 'data') {
            int testLength = (int)testBytes - i - 8;
            const uint16_t* pTest = (const uint16_t*)(testbuf.get() + i + 8);
            const uint16_t* pDec = (const uint16_t*)decoded.ptr();
            if (testLength != decoded.size()) {
                fprintf(stderr, "結果のサイズが合いません\n");
                return 1;
            }
            // AACのデコード結果は小数なので丸め誤差を考慮して
            for (int c = 0; c < testLength / 2; ++c) {
                if ((std::abs((int)pTest[c] - (int)pDec[c]) > 1)) {
                    fprintf(stderr, "デコード結果が合いません\n");
                    return 1;
                }
            }
            break;
        }
        i += *(uint32_t*)(testbuf.get() + i + 4) + 8;
    }

    NeAACDecClose(hAacDec);
    fclose(fp);
    fclose(testfp);

    return 0;
}

/* static */ int test::WaveWriteHeader(AMTContext& ctx, const ConfigWrapper& setting) {
    tstring dstfile = setting.getOutFilePath(EncodeFileKey(), EncodeFileKey(), setting.getFormat(), VS_H264);

    FILE* fp = fsopenT(dstfile.c_str(), _T("wb"), _SH_DENYNO);
    if (fp == nullptr) {
        fprintf(stderr, "failed to open file...\n");
        return 1;
    }

    int writeSeconds = 300;
    int sampleRate = 24000;
    int bitsPerSample = 8;
    int nChannels = 1;

    uint8_t* samples = (uint8_t*)malloc(writeSeconds * sampleRate * nChannels * (bitsPerSample / 2));
    for (int i = 0; i < writeSeconds * sampleRate; ++i) {
        for (int c = 0; c < nChannels; ++c) {
            samples[i * nChannels + c] = (i % sampleRate);
        }
    }

    writeWaveHeader(fp, nChannels, sampleRate, bitsPerSample, writeSeconds * sampleRate);
    if (fwrite(samples, writeSeconds * sampleRate * nChannels, 1, fp) != 1) {
        fprintf(stderr, "failed to write file...\n");
        return 1;
    }

    free(samples);
    fclose(fp);

    return 0;
}

/* static */ int test::ProcessTest(AMTContext& ctx, const ConfigWrapper& setting) {
    class ProcTest : public EventBaseSubProcess {
    public:
        ProcTest() : EventBaseSubProcess(_T("x264.exe --help")) {}
    protected:
        virtual void onOut(bool isErr, MemoryChunk mc) {
            fwrite(mc.data, mc.length, 1, stdout);
        }
    };

    ProcTest proc;
    proc.join();

    return 0;
}

/* static */ int test::FileStreamInfo(AMTContext& ctx, const ConfigWrapper& setting) {
    StreamReformInfo reformInfo = StreamReformInfo::deserialize(ctx, setting.getStreamInfoPath());
    reformInfo.prepare(false, false, false);
    auto audioDiffInfo = reformInfo.genAudio({ CMTYPE_BOTH });
    audioDiffInfo.printAudioPtsDiff(ctx);
    reformInfo.printOutputMapping([&](EncodeFileKey key) {
        const auto& file = reformInfo.getEncodeFile(key);
        return setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(key, reformInfo, setting), VS_H264);
        });
    return 0;
}

/* static */ int test::ParseArgs(AMTContext& ctx, const ConfigWrapper& setting) {
    const_cast<ConfigWrapper&>(setting).CreateTempDir();
    setting.dump();
    return 0;
}

/* static */ int test::LogoFrameTest(AMTContext& ctx, const ConfigWrapper& setting) {
    {
        auto env = make_unique_ptr(CreateScriptEnvironment2());
        PClip clip = env->Invoke("Import", tchar_to_string(setting.getFilterScriptPath()).c_str()).AsClip();

        logo::LogoFrame logof(ctx, setting.getLogoPath(), 0.1f);
        logof.scanFrames(clip, {}, 0, 1, env.get());
        logof.writeResult(setting.getTmpLogoFramePath(0));

        ctx.infoF("BestLogo: %s\n", setting.getLogoPath()[logof.getBestLogo()].c_str());
        ctx.infoF("LogoRatio: %f\n", logof.getLogoRatio());
    }

    return 0;
}
test::TestSplitDualMono::TestSplitDualMono(AMTContext& ctx, const std::vector<tstring>& outpaths)
    : DualMonoSplitter(ctx)
    , file0(new File(outpaths[0], _T("wb")))
    , file1(new File(outpaths[1], _T("wb"))) {}

/* virtual */ void test::TestSplitDualMono::OnOutFrame(int index, MemoryChunk mc) {
    ((index == 0) ? file0.get() : file1.get())->write(mc);
}

/* static */ int test::SplitDualMonoAAC(AMTContext& ctx, const ConfigWrapper& setting) {
    std::vector<tstring> outpaths;
    outpaths.push_back(setting.getIntAudioFilePath(EncodeFileKey(), 0, setting.getAudioEncoder()));
    outpaths.push_back(setting.getIntAudioFilePath(EncodeFileKey(), 1, setting.getAudioEncoder()));
    TestSplitDualMono splitter(ctx, outpaths);

    File src(setting.getSrcFilePath(), _T("rb"));
    int sz = (int)src.size();
    std::unique_ptr<uint8_t[]> buf = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
    src.read(MemoryChunk(buf.get(), sz));

    for (int offset = 0; offset + 7 <= sz; ) {
        AdtsHeader header;
        if (!header.parse(buf.get() + offset, 7)) {
            THROW(FormatException, "Failed to parse AAC frame ...");
        }
        if (offset + header.frame_length > sz) {
            THROW(FormatException, "frame_length too long ...");
        }
        splitter.inputPacket(MemoryChunk(buf.get() + offset, header.frame_length));
        offset += header.frame_length;
    }

    return 0;
}

/* static */ int test::AACDecodeTest(AMTContext& ctx, const ConfigWrapper& setting) {
    File src(setting.getSrcFilePath(), _T("rb"));
    int sz = (int)src.size();
    std::unique_ptr<uint8_t[]> buf = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
    src.read(MemoryChunk(buf.get(), sz));

    NeAACDecHandle hAacDec = NeAACDecOpen();
    NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(hAacDec);
    conf->outputFormat = FAAD_FMT_16BIT;
    NeAACDecSetConfiguration(hAacDec, conf);


    for (int offset = 0; offset + 7 <= sz; ) {
        AdtsHeader header;
        if (!header.parse(buf.get() + offset, 7)) {
            THROW(FormatException, "Failed to parse AAC frame ...");
        }
        if (offset + header.frame_length > sz) {
            THROW(FormatException, "frame_length too long ...");
        }
        if (offset == 0) {
            unsigned long samplerate;
            unsigned char channels;
            if (NeAACDecInit(hAacDec, buf.get() + offset, (int)header.frame_length, &samplerate, &channels)) {
                ctx.warn("NeAACDecInitに失敗");
                return 1;
            }
        }
        NeAACDecFrameInfo frameInfo;
        void* samples = NeAACDecDecode(hAacDec, &frameInfo, buf.get() + offset, header.frame_length);
        if (frameInfo.error != 0) {
            THROW(FormatException, "デコード失敗 ...");
        }
        offset += header.frame_length;
    }

    return 0;
}

/* static */ int test::CaptionASS(AMTContext& ctx, const ConfigWrapper& setting) {
    try {
        StreamReformInfo reformInfo = StreamReformInfo::deserialize(ctx, setting.getStreamInfoPath());

        reformInfo.prepare(false, false, false);
        auto audioDiffInfo = reformInfo.genAudio({ CMTYPE_BOTH });
        audioDiffInfo.printAudioPtsDiff(ctx);

        CaptionASSFormatter formatterASS(ctx);
        CaptionSRTFormatter formatterSRT(ctx);
        const auto& keys = reformInfo.getOutFileKeys();
        for (int i = 0; i < (int)keys.size(); ++i) {
            auto key = keys[i];
            auto& capList = reformInfo.getEncodeFile(key).captionList;
            for (int lang = 0; lang < capList.size(); ++lang) {
                WriteUTF8File(
                    setting.getTmpASSFilePath(key, lang),
                    formatterASS.generate(capList[lang]));
                WriteUTF8File(
                    setting.getTmpSRTFilePath(key, lang),
                    formatterSRT.generate(capList[lang]));
            }
        }
    } catch (const Exception& e) {
        fprintf(stderr, "CaptionASS Error: 例外がスローされました -> %s\n", e.message());
        return 1;
    }

    return 0;
}

/* static */ int test::EncoderOptionParse(AMTContext& ctx, const ConfigWrapper& setting) {
    auto info = ParseEncoderOption(ENCODER_NVENC, setting.getEncoderOptions());
    PrintEncoderInfo(ctx, info);
    return 0;
}

/* static */ int test::DecodePerformance(AMTContext& ctx, const ConfigWrapper& setting) {
    using namespace av;

    InputContext inputCtx(setting.getSrcFilePath());
    if (avformat_find_stream_info(inputCtx(), NULL) < 0) {
        THROW(FormatException, "avformat_find_stream_info failed");
    }
    AVStream *videoStream = av::GetVideoStream(inputCtx());
    if (videoStream == NULL) {
        THROW(FormatException, "Could not find video stream ...");
    }
    AVCodecID vcodecId = videoStream->codecpar->codec_id;
    const AVCodec *pCodec = avcodec_find_decoder(vcodecId);
    if (pCodec == NULL) {
        THROW(FormatException, "Could not find decoder ...");
    }
    CodecContext codecCtx(pCodec);
    if (avcodec_parameters_to_context(codecCtx(), videoStream->codecpar) != 0) {
        THROW(FormatException, "avcodec_parameters_to_context failed");
    }
    codecCtx()->thread_count = GetFFmpegThreads(GetProcessorCount() - 2, videoStream->codecpar->height);
    if (avcodec_open2(codecCtx(), pCodec, NULL) != 0) {
        THROW(FormatException, "avcodec_open2 failed");
    }

    Stopwatch sw;
    sw.start();

    int nframes = 0;
    Frame frame;
    AVPacket packet = AVPacket();
    while (av_read_frame(inputCtx(), &packet) == 0) {
        if (packet.stream_index == videoStream->index) {
            if (avcodec_send_packet(codecCtx(), &packet) != 0) {
                THROW(FormatException, "avcodec_send_packet failed");
            }
            while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
                ++nframes;
            }
        }
        av_packet_unref(&packet);
    }

    // flush decoder
    if (avcodec_send_packet(codecCtx(), NULL) != 0) {
        THROW(FormatException, "avcodec_send_packet failed");
    }
    while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
        ++nframes;
    }

    sw.stop();
    double sec = sw.getTotal();
    printf("%f sec for %d frames ... %f fps\n", sec, nframes, nframes / sec);

    return 0;
}

/* static */ int test::BitrateZones(AMTContext& ctx, const ConfigWrapper& setting) {
    std::vector<double> durations;
    double elapsed = 0;
    double tick = 1000.0 * 1001 / 60000;
    for (int i = 0; i < 30; ++i) {
        durations.push_back(elapsed); elapsed += tick * 2;
        durations.push_back(elapsed); elapsed += tick * 3;
    }
    for (int i = 0; i < 40; ++i) {
        durations.push_back(elapsed); elapsed += tick * 1;
    }
    for (int i = 0; i < 50; ++i) {
        durations.push_back(elapsed); elapsed += tick * 2;
    }
    std::vector<EncoderZone> cmzones;
    cmzones.push_back(EncoderZone{ 40, 80 });
    cmzones.push_back(EncoderZone{ 110, 130 });

    auto ret = MakeVFRBitrateZones(durations, cmzones, 0.6, 60000, 1001, 1.0, 0.15);

    if (ret.size() != 3) THROW(TestException, "");
    if (ret[0].startFrame != 0) THROW(TestException, "");
    if (ret[0].endFrame != 40) THROW(TestException, "");
    if (ret[0].bitrate != 2.5) THROW(TestException, "");
    if (ret[1].startFrame != 40) THROW(TestException, "");
    if (ret[1].endFrame != 128) THROW(TestException, "");
    if (std::abs(1.195 - ret[1].bitrate) > 0.01) THROW(TestException, "");
    if (ret[2].startFrame != 128) THROW(TestException, "");
    if (ret[2].endFrame != 150) THROW(TestException, "");
    if (ret[2].bitrate != 2.0) THROW(TestException, "");

    return 0;
}

/* static */ int test::BitrateZonesBug(AMTContext& ctx, const ConfigWrapper& setting) {
    File dump(setting.getSrcFilePath(), _T("rb"));
    auto timeCodes = dump.readArray<double>();
    auto cmzones = dump.readArray<EncoderZone>();
    auto bitrateCM = dump.readValue<double>();
    auto fpsNum = dump.readValue<int>();
    auto fpsDenom = dump.readValue<int>();
    auto timeFactor = dump.readValue<double>();
    auto costLimit = dump.readValue<double>();

    auto ret = MakeVFRBitrateZones(timeCodes, cmzones, bitrateCM, fpsNum, fpsDenom, timeFactor, costLimit);

    return 0;
}

/* static */ int test::PrintfBug(AMTContext& ctx, const ConfigWrapper& setting) {
    File txtf(setting.getSrcFilePath(), _T("rb"));
    std::vector<char> strv(txtf.size());
    txtf.read(MemoryChunk((uint8_t*)strv.data(), strv.size()));
    printf("txt len: %d\n", (int)strv.size());
    std::string str(strv.begin(), strv.end());
    ctx.info(str.c_str());
    return 0;
}

/* static */ int test::ResourceTest(AMTContext& ctx, const ConfigWrapper& setting) {
    srand((int)time(0));
    ResourceManger rm(ctx, setting.getInPipe(), setting.getOutPipe());
    for (int i = 0; i < 10000; ++i) {
        ctx.infoF("Test Loop: %d", i);
        rm.wait(HOST_CMD_TSAnalyze);
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 300));
        rm.wait(HOST_CMD_CMAnalyze);
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 300));
        rm.wait(HOST_CMD_Filter);
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 100));
        auto encodeRes = rm.request(HOST_CMD_Encode);
        if (encodeRes.IsFailed()) {
            // リソースが確保できていなかったら確保できるまで待つ
            encodeRes = rm.wait(HOST_CMD_Encode);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 1000));
        rm.wait(HOST_CMD_Mux);
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 300));
    }
    return 0;
}
