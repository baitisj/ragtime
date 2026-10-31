// ragtime_voice_probe: voice's microphone and speaker without the window,
// for the bench and for checking a station by hand. It lists the
// microphones and speakers voice may use and the radio's cards it leaves
// out, then (with --record N) records N seconds from the microphone and
// says what Settings > Voice's Test would (voice::checkMicrophone), and
// (with --play) plays the recording back, or a short chime if nothing was
// recorded. --stream plays the chime as a clip that comes in pieces, as
// voice arriving in a ragchew does. Not a test.
//
//   ragtime_voice_probe [--microphone NAME] [--speaker NAME] [--record N] [--play] [--stream]
//                       [--remember NAME;NAME]
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ragtime/audio/VoiceAudio.h"
#include "ragtime/voice/MicCheck.h"

using namespace ragtime;

namespace
{

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

const char* why(voice::Pick p)
{
    switch (p)
    {
    case voice::Pick::None: return "none";
    case voice::Pick::Chosen: return "chosen";
    case voice::Pick::Default: return "default";
    case voice::Pick::InsteadOfRadio: return "instead of the radio's default";
    case voice::Pick::Missing: return "missing";
    case voice::Pick::Refused: return "refused, the radio's";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv)
{
    std::string microphone, speaker, remember;
    int record = 0;
    bool play = false, stream = false;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--microphone" && more) microphone = argv[++i];
        else if (a == "--speaker" && more) speaker = argv[++i];
        else if (a == "--record" && more) record = std::atoi(argv[++i]);
        else if (a == "--remember" && more) remember = argv[++i];
        else if (a == "--play") play = true;
        else if (a == "--stream") stream = true;
    }

    auto audio = audio::makeVoiceAudio();
    audio->start(nullptr, voice::splitNames(remember));
    audio->chooseMicrophone(microphone);
    audio->chooseSpeaker(speaker);
    sleepMs(1500);

    audio::VoiceStatus s = audio->status();
    if (!s.available)
    {
        std::printf("not available: %s\n", s.problem.c_str());
        return 1;
    }
    std::printf("microphones:\n");
    for (const auto& n : s.devices.microphones) std::printf("  %s  [%s]\n", voice::label(n).c_str(), n.name.c_str());
    std::printf("speakers:\n");
    for (const auto& n : s.devices.speakers) std::printf("  %s  [%s]\n", voice::label(n).c_str(), n.name.c_str());
    std::printf("left out as the radio's:\n");
    for (const auto& r : s.devices.radio)
        std::printf("  %s %s  [%s]%s%s\n", r.input ? "in " : "out", voice::label(r.node).c_str(), r.node.name.c_str(),
                    r.inUse ? " in use by data2g" : "", r.remembered ? " remembered" : "");
    std::printf("microphone: %s (%s)\n", voice::describe(s.microphone, true).c_str(), why(s.microphone.why));
    std::printf("speaker: %s (%s)\n", voice::describe(s.speaker, false).c_str(), why(s.speaker.why));
    std::printf("remember: %s\n", voice::joinNames(voice::rememberRadio(voice::splitNames(remember), s.devices.radioNow)).c_str());

    std::mutex mutex;
    std::vector<float> recorded;
    int rate = 0;
    if (record > 0)
    {
        audio->listen([&](const float* x, std::size_t n, int r) {
            std::lock_guard<std::mutex> lock(mutex);
            recorded.insert(recorded.end(), x, x + n);
            rate = r;
        });
        sleepMs(record * 1000);
        s = audio->status();
        audio->stopListening();
        std::lock_guard<std::mutex> lock(mutex);
        std::printf("recorded %zu samples at %d Hz%s%s\n", recorded.size(), rate,
                    s.microphoneProblem.empty() ? "" : ": ", s.microphoneProblem.c_str());
        if (rate > 0)
            std::printf("test says: %s\n", voice::checkMicrophone(recorded.data(), recorded.size(), rate).summary().c_str());
    }

    if (play || stream)
    {
        std::vector<float> clip = recorded;
        int clipRate = rate;
        if (clip.empty() || clipRate <= 0)
        {
            clipRate = 8000;
            clip = voice::testChime(clipRate);
        }
        std::atomic<int> done{-1};
        auto finished = [&](bool played) { done = played ? 1 : 0; };
        const auto t0 = std::chrono::steady_clock::now();
        if (stream)
        {
            audio->playStream(clipRate, finished);
            // In pieces of 200 ms, every 150 ms, with one stall of a second.
            const std::size_t piece = (std::size_t)clipRate / 5;
            for (std::size_t at = 0; at < clip.size(); at += piece)
            {
                audio->append(clip.data() + at, std::min(piece, clip.size() - at));
                sleepMs(at == piece * 2 ? 1000 : 150);
            }
            audio->endStream();
        }
        else
            audio->play(clip, clipRate, finished);
        for (int i = 0; i < 600 && done < 0; i++)
        {
            if (i % 10 == 0) std::printf("  at %.2f s\n", audio->playPosition());
            sleepMs(50);
        }
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        s = audio->status();
        std::printf("played %.2f s of audio in %.2f s: %s%s%s\n", (double)clip.size() / clipRate, took,
                    done == 1 ? "heard to the end" : done == 0 ? "stopped" : "no end",
                    s.speakerProblem.empty() ? "" : ": ", s.speakerProblem.c_str());
    }
    audio->stop();
    return 0;
}
