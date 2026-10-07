#include "neaacdec.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    for (int channels = 1; channels <= 6; channels++) {
        std::ifstream file(std::string(argv[1]) + "/silent" + std::to_string(channels) + ".aac", std::ios::binary);
        const std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.empty()) return 1;
        auto decoder = NeAACDecOpen();
        if (!decoder) return 1;
        auto config = NeAACDecGetCurrentConfiguration(decoder);
        config->outputFormat = FAAD_FMT_16BIT;
        config->downMatrix = 0;
        if (!NeAACDecSetConfiguration(decoder, config)) return 1;
        unsigned long rate = 0;
        unsigned char decodedChannels = 0;
        if (NeAACDecInit(decoder, const_cast<unsigned char*>(data.data()), data.size(), &rate, &decodedChannels) < 0) return 1;
        size_t offset = 0;
        unsigned long samples = 0;
        int maximum = 0;
        int frames = 0;
        // faadはモノラルを暗黙PS判定用に2chへ展開する。
        const int outputChannels = channels == 1 ? 2 : channels;
        while (offset < data.size()) {
            if (offset + 7 > data.size()) return 1;
            const size_t length = ((data[offset + 3] & 3) << 11) | (data[offset + 4] << 3) | (data[offset + 5] >> 5);
            if (length < 7 || offset + length > data.size()) return 1;
            NeAACDecFrameInfo info = {};
            const auto decoded = static_cast<const int16_t*>(NeAACDecDecode(decoder, &info,
                const_cast<unsigned char*>(data.data()) + offset, length));
            if (info.error || info.samplerate != 48000 || info.channels != outputChannels || (!decoded && info.samples)) {
                std::cerr << channels << "ch無音AACのfaadデコードに失敗しました エラー=" << static_cast<int>(info.error) << " 周波数=" << info.samplerate << " ch=" << static_cast<int>(info.channels) << " samples=" << info.samples << " frames=" << frames << "\n";
                return 1;
            }
            for (unsigned long i = 0; i < info.samples; i++) maximum = std::max(maximum, std::abs(static_cast<int>(decoded[i])));
            samples += info.samples;
            offset += length;
            frames++;
        }
        NeAACDecClose(decoder);
        if (frames != 200 || samples != static_cast<unsigned long>(199 * 1024 * outputChannels) || maximum != 0) return 1;
        std::cout << channels << "ch faad成功 フレーム=" << frames << " サンプル=" << samples << " 最大絶対値=" << maximum << '\n';
    }
    return 0;
}
