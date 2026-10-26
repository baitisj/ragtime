// The Ear's core: the spectrum (a tone lands in its bin at 0 dB, noise
// spreads, rows come at the speed asked), finding data2g-host's capture in
// a PipeWire graph (by link, by target, by the port's process), and the
// port's process from a pretend /proc, and the filter's edges measured
// from the band's noise (bursts, carriers and our own transmissions left
// out).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/ear/Edges.h"
#include "ragtime/ear/Graph.h"
#include "ragtime/ear/Passbands.h"
#include "ragtime/ear/ProcNet.h"
#include "ragtime/ear/Spectrum.h"

using namespace ragtime::ear;

namespace
{

constexpr double PI = 3.14159265358979323846;

std::vector<float> tone(int rate, double hz, double amplitude, double seconds)
{
    std::vector<float> x((std::size_t)(rate * seconds));
    for (std::size_t i = 0; i < x.size(); i++) x[i] = (float)(amplitude * std::sin(2 * PI * hz * (double)i / rate));
    return x;
}

int peakBin(const std::vector<float>& row)
{
    int best = 0;
    for (int i = 1; i < (int)row.size(); i++)
        if (row[i] > row[best]) best = i;
    return best;
}

void fftOfImpulse()
{
    std::vector<std::complex<double>> x(8, 0.0);
    x[0] = 1.0;
    fft(x);
    for (const auto& v : x) CHECK(std::abs(v - std::complex<double>(1.0, 0.0)) < 1e-12);

    // A cosine at bin 2 of 16 lands in bins 2 and 14, half each.
    std::vector<std::complex<double>> c(16);
    for (int i = 0; i < 16; i++) c[i] = std::cos(2 * PI * 2 * i / 16.0);
    fft(c);
    CHECK(std::abs(std::abs(c[2]) - 8.0) < 1e-9);
    CHECK(std::abs(std::abs(c[14]) - 8.0) < 1e-9);
    CHECK(std::abs(c[3]) < 1e-9);
}

void toneAtZeroDb()
{
    for (int rate : {48000, 44100, 8000})
    {
        Spectrum s(rate, 10.0);
        CHECK(s.fftLength() >= (std::size_t)(rate / Spectrum::FFT_HZ));
        std::vector<float> x = tone(rate, 1000.0, 1.0, 2.0);
        auto rows = s.push(x.data(), x.size());
        CHECK(!rows.empty());
        const std::vector<float>& last = rows.back();
        CHECK_EQ((int)last.size(), Spectrum::BINS);
        CHECK_EQ(peakBin(last), Spectrum::binOf(1000.0));
        // A full-scale sine reads about 0 dB in its step (the step averages
        // the bins under it, so a little under).
        CHECK(last[Spectrum::binOf(1000.0)] > -6.0f);
        CHECK(last[Spectrum::binOf(1000.0)] < 0.5f);
        // Far from the tone, the Hann window's skirt is way down.
        CHECK(last[Spectrum::binOf(3000.0)] < -80.0f);
    }

    // A tone at -20 dBFS reads about -20 dB.
    Spectrum s(48000, 10.0);
    std::vector<float> x = tone(48000, 1500.0, 0.1, 1.5);
    auto rows = s.push(x.data(), x.size());
    CHECK(!rows.empty());
    CHECK(std::abs(rows.back()[Spectrum::binOf(1500.0)] + 20.0f) < 4.0f);
}

void rowsAtTheSpeedAsked()
{
    Spectrum s(48000, 10.0);
    std::vector<float> silence(48000 * 3, 0.0f);
    // The first row waits for a whole FFT; then one each hop.
    auto rows = s.push(silence.data(), silence.size());
    CHECK(rows.size() >= 28 && rows.size() <= 30);
    for (float v : rows.front()) CHECK_EQ(v, Spectrum::FLOOR_DB);

    s.setRowsPerSecond(5.0);
    rows = s.push(silence.data(), silence.size());
    CHECK(rows.size() >= 13 && rows.size() <= 15);

    // Fed in small pieces, as PipeWire does, it gives the same rows.
    Spectrum whole(48000, 10.0), pieces(48000, 10.0);
    std::vector<float> x = tone(48000, 700.0, 0.5, 1.0);
    auto a = whole.push(x.data(), x.size());
    std::vector<std::vector<float>> b;
    for (std::size_t i = 0; i < x.size(); i += 1024)
        for (auto& r : pieces.push(x.data() + i, std::min<std::size_t>(1024, x.size() - i))) b.push_back(r);
    CHECK_EQ(a.size(), b.size());
    if (!a.empty() && a.size() == b.size()) CHECK(a.back() == b.back());
}

void noiseIsFlat()
{
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.0f, 0.1f);
    std::vector<float> x(48000 * 2);
    for (float& v : x) v = gauss(rng);
    Spectrum s(48000, 10.0);
    auto rows = s.push(x.data(), x.size());
    CHECK(!rows.empty());
    double low = 0, high = 0;
    for (int i = Spectrum::binOf(500); i < Spectrum::binOf(1000); i++) low += rows.back()[i];
    for (int i = Spectrum::binOf(2500); i < Spectrum::binOf(3000); i++) high += rows.back()[i];
    int n = Spectrum::binOf(1000) - Spectrum::binOf(500);
    CHECK(std::abs(low / n - high / n) < 2.0);
}

void passbands()
{
    CHECK_EQ(data2gPassband(false).lowHz, 350.0);
    CHECK_EQ(data2gPassband(false).highHz, 2650.0);
    CHECK_EQ(data2gPassband(true).lowHz, 1250.0);
    CHECK_EQ(data2gPassband(true).highHz, 1750.0);
}

// Hamlet's graph on 2026-10-26: data2g-gui's capture on node 55, its
// playback on 54, Firefox playing, and a session manager.
Graph hamlet()
{
    Graph g;
    g.clients = {{33, "WirePlumber", 2000}, {851, "data2g-gui", 205757}, {867, "Firefox", 3000}};
    g.nodes = {
        {55, 30, "alsa_input.usb-Burr-Brown_USB_Audio_CODEC-00.analog-stereo-input", "USB Audio CODEC Analog Stereo",
         "Audio/Source", "", ""},
        {54, 30, "alsa_output.usb-Burr-Brown_USB_Audio_CODEC-00.analog-stereo-output", "USB Audio CODEC Analog Stereo",
         "Audio/Sink", "", ""},
        {60, 30, "alsa_input.pci-0000_00_1f.3.analog-stereo", "Built-in Audio Analog Stereo", "Audio/Source", "", ""},
        {129, 851, "data2g-gui", "", "Stream/Input/Audio", "data2g-gui", "55"},
        {130, 851, "data2g-gui", "", "Stream/Output/Audio", "data2g-gui", "54"},
        {140, 867, "Firefox", "", "Stream/Output/Audio", "Firefox", ""},
    };
    g.links = {{55, 129}, {130, 54}, {140, 54}};
    return g;
}

void findsData2GGui()
{
    Graph g = hamlet();
    HostSearch s = findHostCapture(g, std::nullopt);
    CHECK(s.found.has_value());
    CHECK_EQ(s.candidates, 1);
    if (!s.found) return;
    CHECK_EQ(s.found->stream, 129u);
    CHECK_EQ(s.found->program, std::string("data2g-gui"));
    CHECK_EQ(s.found->pid, 205757);
    CHECK_EQ(s.found->source, 55u);
    CHECK(s.found->linked);
    CHECK_EQ(s.found->sourceDescription, std::string("USB Audio CODEC Analog Stereo"));
    CHECK(s.why.empty());

    // The dropdown offers the two sources, never sinks or streams.
    auto list = sources(g);
    CHECK_EQ(list.size(), 2u);
    if (list.size() == 2)
    {
        CHECK_EQ(list[0].id, 60u);
        CHECK_EQ(list[1].id, 55u);
    }
}

void linkBeatsTarget()
{
    // Moved in pavucontrol: the target still says 55, the link says 60.
    Graph g = hamlet();
    g.links = {{60, 129}};
    HostSearch s = findHostCapture(g, std::nullopt);
    CHECK(s.found && s.found->source == 60u && s.found->linked);

    // No link yet: the target stands in.
    g.links.clear();
    s = findHostCapture(g, std::nullopt);
    CHECK(s.found && s.found->source == 55u && !s.found->linked);

    // A target by name works too; one naming no source leaves it unknown.
    g.nodes[3].target = "alsa_input.pci-0000_00_1f.3.analog-stereo";
    s = findHostCapture(g, std::nullopt);
    CHECK(s.found && s.found->source == 60u);
    g.nodes[3].target = "nothing";
    s = findHostCapture(g, std::nullopt);
    CHECK(s.found && s.found->source == 0u);
    CHECK(!s.why.empty());
}

void twoHostsByPort()
{
    Graph g;
    g.clients = {{10, "data2g-host", 111}, {11, "data2g-host", 222}};
    g.nodes = {
        {1, 0, "radio-a", "Radio A", "Audio/Source/Virtual", "", ""},
        {2, 0, "radio-b", "Radio B", "Audio/Source/Virtual", "", ""},
        {3, 10, "data2g-host", "", "Stream/Input/Audio", "", ""},
        {4, 11, "data2g-host", "", "Stream/Input/Audio", "", ""},
    };
    g.links = {{1, 3}, {2, 4}};
    HostSearch s = findHostCapture(g, 222);
    CHECK(s.found && s.found->source == 2u);
    s = findHostCapture(g, 111);
    CHECK(s.found && s.found->source == 1u);
    s = findHostCapture(g, std::nullopt);
    CHECK(!s.found);
    CHECK_EQ(s.candidates, 2);
    CHECK(!s.why.empty());
    s = findHostCapture(g, 333);
    CHECK(!s.found);

    // No data2g at all; a recorder of another name doesn't count.
    Graph other;
    other.clients = {{5, "pw-record", 9}};
    other.nodes = {{1, 0, "radio", "", "Audio/Source", "", ""}, {2, 5, "pw-record", "", "Stream/Input/Audio", "", ""}};
    other.links = {{1, 2}};
    s = findHostCapture(other, std::nullopt);
    CHECK(!s.found);
    CHECK_EQ(s.candidates, 0);

    // A client named for its binary, a node for what the program calls
    // itself (pw-cat run as data2g-host, the bench's stand-in).
    other.clients = {{5, "pw-cat", 9}};
    other.nodes[1].appName = "data2g-host";
    s = findHostCapture(other, std::nullopt);
    CHECK(s.found && s.found->pid == 9);

    // A node that names its program itself, with no client yet.
    other.nodes[1].appName = "data2g-host";
    other.clients.clear();
    s = findHostCapture(other, std::nullopt);
    CHECK(s.found && s.found->source == 1u);
}

void procNet()
{
    const std::string tcp =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 0100007F:206C 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 4242 1 0 100 0 0 10 0\n"
        "   1: 0100007F:1FA4 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 5151 1 0 100 0 0 10 0\n"
        "   2: 0100007F:206C 0100007F:9C40 01 00000000:00000000 00:00000000 00000000  1000        0 6161 1 0 20 4 30 10 -1\n";
    auto inodes = listeningInodes(tcp, 8300);
    CHECK_EQ(inodes.size(), 1u);
    if (!inodes.empty()) CHECK_EQ(inodes[0], 4242ul);
    CHECK_EQ(listeningInodes(tcp, 8100).size(), 1u);
    CHECK(listeningInodes(tcp, 8301).empty());
    CHECK(listeningInodes("", 8300).empty());

#ifdef __linux__
    // A pretend /proc: process 77 holds the listening socket (symbolic
    // links, so Linux only; Windows has no /proc to read anyway).
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / ("ragtime_ear_proc_" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "net");
    fs::create_directories(root / "77" / "fd");
    fs::create_directories(root / "78" / "fd");
    fs::create_directories(root / "self");
    std::ofstream(root / "net" / "tcp") << tcp;
    fs::create_symlink("socket:[9999]", root / "78" / "fd" / "3");
    fs::create_symlink("/dev/null", root / "77" / "fd" / "0");
    fs::create_symlink("socket:[4242]", root / "77" / "fd" / "5");
    auto pid = processListeningOn(8300, root.string());
    CHECK(pid.has_value());
    if (pid) CHECK_EQ(*pid, 77);
    CHECK(!processListeningOn(8301, root.string()).has_value());
    CHECK(!processListeningOn(8300, (root / "missing").string()).has_value());
    fs::remove_all(root, ec);
#endif
}

// A row of the band's noise through a filter whose -6 dB points are low
// and high, skirts falling 0.4 dB a Hz to 60 dB down, each bin's power
// drawn as noise is (exponentially). extra adds dB over a span.
struct Lift
{
    double lowHz, highHz, db;
};

std::vector<float> noiseRow(std::mt19937& rng, double low, double high, std::vector<Lift> extra = {})
{
    std::exponential_distribution<double> draw(1.0);
    std::vector<float> row(Spectrum::BINS);
    for (int i = 0; i < Spectrum::BINS; i++)
    {
        const double hz = Spectrum::binHz(i);
        double shape = std::min(-6.0 + (hz - low) * 0.4, -6.0 + (high - hz) * 0.4);
        shape = std::max(-60.0, std::min(0.0, shape));
        double db = -50.0 + shape;
        for (const Lift& l : extra)
            if (hz >= l.lowHz && hz <= l.highHz) db += l.db;
        row[i] = (float)(10.0 * std::log10(std::pow(10.0, db / 10.0) * draw(rng)));
    }
    return row;
}

bool near(const std::optional<double>& hz, double want, double within)
{
    return hz && std::fabs(*hz - want) <= within;
}

void edgesFromNoise()
{
    std::mt19937 rng(7);

    // An SSB filter, 300 to 2700 Hz, with Data2G bursts (20 dB up across
    // 350 to 2650 Hz, half the time), a carrier, and our own bursts.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 25; i++) f.addRow(noiseRow(rng, 300, 2700), false);
        CHECK(!f.edges());      // 2.5 s: not yet
        for (int i = 0; i < 400; i++)
        {
            std::vector<Lift> lift = {{1075, 1085, 25.0}};
            const bool burst = (i / 30) % 2 == 1;
            if (burst) lift.push_back({350, 2650, 20.0});
            const bool keyed = i % 100 >= 90;
            f.addRow(noiseRow(rng, 300, 2700, keyed ? std::vector<Lift>{{0, 4000, 60.0}} : lift), keyed);
        }
        std::optional<Edges> e = f.edges();
        CHECK(e.has_value());
        if (e)
        {
            CHECK(near(e->lowHz, 300, 20));
            CHECK(near(e->highHz, 2700, 20));
            CHECK(std::fabs(e->passbandDb - -50.0) < 3.0);
            CHECK(e->passbandDb - e->stopbandDb > 40.0);
            CHECK(e->quietSeconds >= EdgeFinder::NEEDED_S);
        }
    }

    // A 500 Hz filter, with narrow bursts in it 25 dB up half the time:
    // left out, or they would set the level.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 400; i++)
        {
            const bool burst = (i / 20) % 2 == 1;
            f.addRow(noiseRow(rng, 1250, 1750, burst ? std::vector<Lift>{{1400, 1600, 25.0}} : std::vector<Lift>{}),
                     false);
        }
        std::optional<Edges> e = f.edges();
        CHECK(e.has_value());
        if (e)
        {
            CHECK(near(e->lowHz, 1250, 20));
            CHECK(near(e->highHz, 1750, 20));
        }
    }

    // Wider than the picture at the top: only the low edge.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 100; i++) f.addRow(noiseRow(rng, 200, 5000), false);
        std::optional<Edges> e = f.edges();
        CHECK(e.has_value());
        if (e)
        {
            CHECK(near(e->lowHz, 200, 20));
            CHECK(!e->highHz);
        }
    }

    // Flat noise (the sound card's own, or no filter): no edges.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 100; i++) f.addRow(noiseRow(rng, -1000, 9000), false);
        CHECK(!f.edges());
    }

    // Only ever transmitting: nothing.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 100; i++) f.addRow(noiseRow(rng, 300, 2700), true);
        CHECK(!f.edges());
        CHECK(f.quietSeconds() == 0.0);
        CHECK(f.average().empty());
    }

    // A new speed starts afresh.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 100; i++) f.addRow(noiseRow(rng, 300, 2700), false);
        CHECK(f.edges().has_value());
        f.setRowsPerSecond(20.0);
        CHECK(!f.edges());
    }

    CHECK(percentile({3, 1, 2}, 0.5) == 2.0f);
    CHECK(percentile({3, 1, 2}, 1.0) == 3.0f);
    CHECK(percentile({}, 0.5) == 0.0f);
}

} // namespace

int main()
{
    fftOfImpulse();
    toneAtZeroDb();
    rowsAtTheSpeedAsked();
    noiseIsFlat();
    passbands();
    findsData2GGui();
    linkBeatsTarget();
    twoHostsByPort();
    procNet();
    edgesFromNoise();
    return ragtime_test::finish();
}
