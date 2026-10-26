// ragtime_ear_probe: the Ear without the window, for the bench and for
// checking a station by hand. It listens as the Ear does (alongside
// data2g-host, or to --source NAME) for --seconds, and prints what it
// finds in PipeWire's graph, the loudest part of each second's audio, and
// the filter edges the band's noise shows (ear::EdgeFinder, as the Ear
// window draws them), from 0 to --span HZ (4000 by default; up to 12000,
// for FM). With --average it ends with the whole run's spectrum, one
// "Hz dB" line a bin, to find a notch's dip. It only records; it never
// plays a sound. Not a test.
//
//   ragtime_ear_probe [--seconds N] [--source NODE_NAME] [--port 8300] [--span HZ] [--average]
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ragtime/audio/EarAudio.h"
#include "ragtime/ear/Edges.h"
#include "ragtime/ear/ProcNet.h"
#include "ragtime/ear/Spectrum.h"

using namespace ragtime;

int main(int argc, char** argv)
{
    int seconds = 5;
    int port = 8300;
    double span = ear::Spectrum::DEFAULT_TOP_HZ;
    std::string source;
    bool average = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--average")) average = true;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (!std::strcmp(argv[i], "--average"))
        {
            i--;
            continue;
        }
        if (!std::strcmp(argv[i], "--seconds")) seconds = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--source")) source = argv[i + 1];
        else if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--span")) span = std::atof(argv[i + 1]);
    }

    std::mutex mutex;
    // The Ear's normal speed, so the edges average as they do there.
    constexpr double ROWS_PER_SECOND = 10.0;
    ear::Spectrum spectrum(48000, ROWS_PER_SECOND);
    ear::EdgeFinder edges(ROWS_PER_SECOND);
    spectrum.setTopHz(span);
    edges.setTopHz(spectrum.topHz());
    std::vector<std::vector<float>> rows;
    std::vector<double> sum;
    long summed = 0;
    std::atomic<long> samples{0};
    auto ear = audio::makeEarAudio();
    ear->setHostPid(ear::processListeningOn(port));
    ear->start(
        [&](const float* x, std::size_t n, int rate) {
            std::lock_guard<std::mutex> lock(mutex);
            spectrum.setRate(rate);
            for (auto& r : spectrum.push(x, n)) rows.push_back(r);
            samples += (long)n;
        },
        nullptr);
    ear->choose(source);

    for (int s = 0; s < seconds; s++)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        audio::EarStatus st = ear->status();
        std::lock_guard<std::mutex> lock(mutex);
        std::string row = "no audio";
        if (!rows.empty())
        {
            const std::vector<float>& r = rows.back();
            int peak = 0;
            double mean = 0;
            for (int i = 0; i < (int)r.size(); i++)
            {
                if (r[i] > r[peak]) peak = i;
                mean += r[i];
            }
            char text[96];
            std::snprintf(text, sizeof text, "loudest %.0f Hz at %.1f dB, mean %.1f dB",
                          ear::Spectrum::binHz(peak, spectrum.topHz()), r[peak], mean / (double)r.size());
            row = text;
            for (const auto& each : rows)
            {
                edges.addRow(each, false);
                if (sum.size() != each.size())
                {
                    sum.assign(each.size(), 0.0);
                    summed = 0;
                }
                for (std::size_t i = 0; i < each.size(); i++) sum[i] += each[i];
                summed++;
            }
            rows.clear();
        }
        std::printf("%ds: %s\n", s + 1, !st.available ? st.problem.c_str() : "PipeWire connected");
        if (st.host.found)
            std::printf("  data2g capture: %s (pid %d) from %s%s\n", st.host.found->program.c_str(),
                        st.host.found->pid, st.host.found->sourceDescription.c_str(),
                        st.host.found->linked ? "" : " (its target; no link yet)");
        else
            std::printf("  data2g capture: none (%s)\n", st.host.why.c_str());
        std::printf("  listening to: %s, %s, %d Hz; %ld samples; %s\n",
                    st.listeningDescription.empty() ? "nothing" : st.listeningDescription.c_str(),
                    st.streaming ? "streaming" : "waiting", st.rate, (long)samples, row.c_str());
        const std::optional<ear::Edges> e = edges.edges();
        if (e)
            std::printf("  edges: low %s, high %s; passband %.1f dB, stopband %.1f dB, %.0f s of quiet\n",
                        e->lowHz ? std::to_string((int)*e->lowHz).c_str() : "none",
                        e->highHz ? std::to_string((int)*e->highHz).c_str() : "none", e->passbandDb, e->stopbandDb,
                        e->quietSeconds);
        else
            std::printf("  edges: none yet (%.0f s of quiet)\n", edges.quietSeconds());
    }
    ear->stop();
    if (average && summed > 0)
    {
        std::printf("average of %ld rows:\n", summed);
        for (std::size_t i = 0; i < sum.size(); i++)
            std::printf("%.0f %.1f\n", ear::Spectrum::binHz((int)i, spectrum.topHz()), sum[i] / (double)summed);
    }
    return 0;
}
