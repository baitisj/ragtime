// The Ear's core: the spectrum (a tone lands in its bin at 0 dB, noise
// spreads, rows come at the speed asked), finding data2g-host's capture in
// a PipeWire graph (by link, by target, by the port's process), and the
// port's process from a pretend /proc, and the filter's edges measured
// from the band's noise (bursts, carriers and our own transmissions left
// out), and setting the filter: the twin PBT model, Keep and Undo on
// their clock, and Calibrate against a pretend rig whose noise follows
// its knobs.
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
#include "ragtime/ear/AutoSpan.h"
#include "ragtime/ear/Edges.h"
#include "ragtime/ear/FilterControl.h"
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

std::vector<float> noiseRow(std::mt19937& rng, double low, double high, std::vector<Lift> extra = {},
                            double topHz = Spectrum::DEFAULT_TOP_HZ)
{
    std::exponential_distribution<double> draw(1.0);
    std::vector<float> row(Spectrum::BINS);
    for (int i = 0; i < Spectrum::BINS; i++)
    {
        const double hz = Spectrum::binHz(i, topHz);
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

// A pretend IC-7100: its filter's passband follows its width and knobs,
// by the model, with its own centre, Hz per knob, real width and a cut in
// the audio that hides what lies under it.
struct PretendRig
{
    KnobModel truth;
    RigFilter filter;

    // The edges the noise shows after this many quiet seconds.
    std::optional<Edges> listen(std::mt19937& rng, double seconds = 4.0)
    {
        EdgeFinder f(10.0);
        const Span s = visible(spanOf(filter, truth), truth);
        for (int i = 0; i < (int)(seconds * 10); i++) f.addRow(noiseRow(rng, s.lowHz, s.highHz), false);
        return f.edges();
    }
};

KnobModel knobs(double k, double centre, double width = 0, double nominal = 0, double floor = 0)
{
    KnobModel m;
    m.hzPerKnob = k;
    m.centreHz = centre;
    m.widthHz = width;
    m.nominalHz = nominal;
    m.floorHz = floor;
    return m;
}

Span spanFrom(const Edges& e)
{
    Span s;
    s.lowHz = e.lowHz.value_or(0);
    s.highHz = e.highHz.value_or(Spectrum::DEFAULT_TOP_HZ);
    return s;
}

RigFilter usb(int width, double in = 0.5, double out = 0.5)
{
    RigFilter f;
    f.mode = "USB";
    f.widthHz = width;
    f.pbtIn = in;
    f.pbtOut = out;
    return f;
}

void twinPbtModel()
{
    // Centred: centre +- width / 2.
    Span s = spanOf(usb(2400), 1500, 2000);
    CHECK(std::fabs(s.lowHz - 300) < 1e-9 && std::fabs(s.highHz - 2700) < 1e-9);
    // One knob up: the low edge rises; the other down: the high edge falls.
    s = spanOf(usb(2400, 0.6, 0.5), 1500, 2000);
    CHECK(std::fabs(s.lowHz - 500) < 1e-9 && std::fabs(s.highHz - 2700) < 1e-9);
    s = spanOf(usb(2400, 0.5, 0.4), 1500, 2000);
    CHECK(std::fabs(s.lowHz - 300) < 1e-9 && std::fabs(s.highHz - 2500) < 1e-9);
    // Both: a shift.
    s = spanOf(usb(2400, 0.6, 0.6), 1500, 2000);
    CHECK(std::fabs(s.lowHz - 500) < 1e-9 && std::fabs(s.highHz - 2900) < 1e-9);
    CHECK(std::fabs(centreOf(s, usb(2400, 0.6, 0.6), 2000) - 1500) < 1e-9);

    // Widths the rig takes.
    CHECK_EQ(roundWidth(2449, false), 2400);
    CHECK_EQ(roundWidth(2401, true), 2500);
    CHECK_EQ(roundWidth(430, false), 450);
    CHECK_EQ(roundWidth(10, true), 50);
    CHECK_EQ(roundWidth(2400, true), 2400);

    // The high edge down to 2450: the out knob, the width kept.
    Span want{300, 2450};
    RigFilter f = settingFor(want, usb(2400), 1500, 2000);
    CHECK_EQ(f.widthHz, 2400);
    s = spanOf(f, 1500, 2000);
    CHECK(std::fabs(s.lowHz - 300) < 5 && std::fabs(s.highHz - 2450) < 5);
    // Wider than the filter: a wider width.
    f = settingFor(Span{250, 2900}, usb(2400), 1500, 2000);
    CHECK_EQ(f.widthHz, 2700);
    s = spanOf(f, 1500, 2000);
    CHECK(std::fabs(s.lowHz - 250) < 5 && std::fabs(s.highHz - 2900) < 5);
    // Narrower than the knobs reach: a narrower width too.
    f = settingFor(Span{1250, 1750}, usb(3600), 1500, 1000);
    CHECK(f.widthHz < 3600);
    s = spanOf(f, 1500, 1000);
    CHECK(std::fabs(s.lowHz - 1250) < 5 && std::fabs(s.highHz - 1750) < 5);
    // A knob that runs the other way.
    f = settingFor(want, usb(2400), 1500, -2000);
    s = spanOf(f, 1500, -2000);
    CHECK(std::fabs(s.highHz - 2450) < 5);
    // Not calibrated: the width alone, about the centre.
    f = settingFor(Span{500, 2500}, usb(2400, 0.5, 0.5), 1500, 0);
    CHECK_EQ(f.widthHz, 2000);
    CHECK(f.pbtIn && *f.pbtIn == 0.5);

    // The model as Calibrate learns it on Jeff's IC-7100: a 3000 Hz filter
    // that passes 3140 Hz, its low edge under a cut at 70 Hz.
    const KnobModel ic7100 = knobs(3000, 1499, 3140, 3000, 70);
    s = spanOf(usb(3000), ic7100);
    CHECK(std::fabs(s.lowHz + 71) < 1e-9 && std::fabs(s.highHz - 3069) < 1e-9);
    s = visible(s, ic7100);
    CHECK(std::fabs(s.lowHz - 70) < 1e-9 && std::fabs(s.highHz - 3069) < 1e-9);
    // Another width keeps the extra 140 Hz.
    s = spanOf(usb(2400), ic7100);
    CHECK(std::fabs(s.width() - 2540) < 1e-9);
    // The high edge dragged from what the noise shows: the hidden low edge
    // stays where it is, and the low knob centred.
    const Span shown{66, 3052};
    Span target = dragTarget(shown, Span{66, 2452}, usb(3000), ic7100);
    CHECK(std::fabs(target.lowHz + 71) < 1e-9 && std::fabs(target.highHz - 2469) < 1e-9);
    // The hidden low edge dragged by itself goes where it was dropped
    // (Hamlet: dropped at 400, it landed at 280 when moved by the drag);
    // in a shift it moves as far as the other.
    target = dragTarget(shown, Span{400, shown.highHz}, usb(3000), ic7100);
    CHECK(std::fabs(target.lowHz - 400) < 1e-9 && std::fabs(target.highHz - 3069) < 1e-9);
    target = dragTarget(shown, Span{shown.lowHz + 300, shown.highHz + 300}, usb(3000), ic7100);
    CHECK(std::fabs(target.lowHz - 229) < 1e-9);
    target = dragTarget(shown, Span{66, 2452}, usb(3000), ic7100);
    f = settingFor(target, usb(3000), ic7100);
    CHECK_EQ(f.widthHz, 3000);
    CHECK(f.pbtIn && std::fabs(*f.pbtIn - 0.5) <= 1.0 / 255);
    CHECK(f.pbtOut && *f.pbtOut < 0.5);
    // Kept in the settings file as text, in any locale.
    std::optional<KnobModel> back = KnobModel::fromText(ic7100.text());
    CHECK(back && back->hzPerKnob == 3000 && back->centreHz == 1499 && back->widthHz == 3140 &&
          back->nominalHz == 3000 && back->floorHz == 70 && back->ceilingHz == 0);
    CHECK_EQ(ic7100.text(), std::string("3000.0 1499.0 3140.0 3000.0 70.0 0.0"));
    CHECK(!KnobModel::fromText("2890"));
    CHECK(!KnobModel::fromText(""));
    CHECK(!KnobModel::fromText("0 1500 2400 2400 0 0"));

    // A shift before Calibrate: both knobs turned alike by the guess (the
    // width per full turn), the width kept; stopped where a knob runs out.
    CHECK_EQ(guessHzPerKnob(usb(3000)), 3000.0);
    f = shiftedBy(usb(3000, 0.5, 0.5), 300, guessHzPerKnob(usb(3000)));
    CHECK_EQ(f.widthHz, 3000);
    CHECK(f.pbtIn && f.pbtOut && *f.pbtIn == *f.pbtOut && std::fabs(*f.pbtIn - 0.6) <= 1.0 / 255);
    s = spanOf(f, ic7100);
    CHECK(std::fabs(s.lowHz - spanOf(usb(3000), ic7100).lowHz - 300) < 12);
    f = shiftedBy(usb(3000, 0.4, 0.8), 3000, 3000);
    CHECK(f.pbtOut && *f.pbtOut == 1.0 && f.pbtIn && std::fabs(*f.pbtIn - 0.6) <= 1.0 / 255);
    f = shiftedBy(usb(3000, 0.1, 0.5), -3000, 3000);
    CHECK(f.pbtIn && *f.pbtIn == 0.0 && f.pbtOut && std::fabs(*f.pbtOut - 0.4) <= 1.0 / 255);
    RigFilter noKnobs = usb(3000);
    noKnobs.pbtIn.reset();
    noKnobs.pbtOut.reset();
    CHECK(shiftedBy(noKnobs, 300, 3000) == noKnobs);

    // Disable: the knobs centred and the notch off, the width kept; and
    // which presets the rig holds.
    RigFilter busy = usb(2400, 0.6, 0.45);
    busy.notchOn = true;
    busy.notchRaw = 0.3;
    CHECK(!filtersOff(busy));
    RigFilter plain = disabled(busy);
    CHECK(filtersOff(plain));
    CHECK_EQ(plain.widthHz, 2400);
    CHECK(plain.notchOn == std::optional<bool>(false) && plain.notchRaw == std::optional<double>(0.3));
    CHECK(filtersOff(noKnobs));
    RigFilter notchOnly = usb(2400);
    notchOnly.notchOn = true;
    CHECK(!filtersOff(notchOnly));
    CHECK(holds(busy, busy));
    RigFilter readBack = busy;
    readBack.pbtIn = 0.6 + 1.0 / 255;
    readBack.notchRaw = 0.3 - 1.0 / 255;
    CHECK(holds(readBack, busy));
    CHECK(!holds(plain, busy));
    CHECK(!holds(usb(1800, 0.6, 0.45), usb(2400, 0.6, 0.45)));
    // A preset from before the notch holds whatever the notch does.
    CHECK(holds(busy, usb(2400, 0.6, 0.45)));
    RigFilter off = busy;
    off.notchOn = false;
    off.notchRaw = 0.9;
    RigFilter offPreset = busy;
    offPreset.notchOn = false;
    CHECK(holds(off, offPreset));

    // Cutting Data2G.
    CHECK(!cutsData2G(Span{300, 2700}, false));
    CHECK(cutsData2G(Span{300, 2450}, false));
    CHECK(!cutsData2G(Span{1100, 1900}, true));
    CHECK(cutsData2G(Span{1300, 1900}, true));

    CHECK_EQ(changeText(Span{300, 2700}, Span{300, 2450}), std::string("High edge 2700 \xe2\x86\x92 2450 Hz"));
    CHECK_EQ(changeText(Span{300, 2700}, Span{450, 2850}), std::string("Shifted 150 Hz up"));
    // Measured edges that round apart still read as a shift.
    CHECK_EQ(changeText(Span{294, 2716}, Span{714, 3136}), std::string("Shifted 420 Hz up"));
    CHECK_EQ(changeText(Span{300, 2700}, Span{600, 2400}), std::string("Width 2400 \xe2\x86\x92 1800 Hz"));
    CHECK_EQ(changeText(Span{300, 2700}, Span{300, 2700}), std::string("No change"));
}

void keepAndUndo()
{
    const uint64_t t = 1000000;
    FilterEdit e;
    CHECK(!e.due(t, true));
    e.seen(usb(2400));
    CHECK(e.found().has_value());

    // A change waits for quiet, then is on trial.
    e.change(usb(2400, 0.5, 0.4), t);
    CHECK(e.state() == FilterEdit::State::Waiting);
    CHECK(!e.due(t + 100, false));
    std::optional<RigFilter> w = e.due(t + 200, true);
    CHECK(w && *w == usb(2400, 0.5, 0.4));
    CHECK(e.state() == FilterEdit::State::Trial);
    CHECK(std::fabs(e.secondsLeft(t + 200 + 5000) - 10.0) < 1e-9);
    e.seen(usb(2400, 0.5, 0.4));

    // A second drag on trial: Undo still goes back to before the first.
    e.change(usb(2400, 0.6, 0.4), t + 3000);
    w = e.due(t + 3100, true);
    CHECK(w && *w == usb(2400, 0.6, 0.4));
    CHECK(e.before() && *e.before() == usb(2400));

    // The clock runs out: back, once quiet, and no trial after it.
    CHECK(!e.due(t + 3100 + FilterEdit::KEEP_MS, false));
    CHECK(e.reverting());
    w = e.due(t + 3100 + FilterEdit::KEEP_MS + 500, true);
    CHECK(w && *w == usb(2400));
    CHECK(e.state() == FilterEdit::State::Idle);

    // Kept: it stays, and Undo has nothing to undo.
    e.change(usb(1800), t + 30000);
    w = e.due(t + 30000, true);
    CHECK(w && w->widthHz == 1800);
    e.keep();
    CHECK(e.state() == FilterEdit::State::Idle);
    CHECK(!e.due(t + 60000, true));
    e.undo();
    CHECK(!e.due(t + 60000, true));

    // Undo before anything was written: just dropped.
    e.change(usb(1500), t + 70000);
    e.undo();
    CHECK(e.state() == FilterEdit::State::Idle);
    CHECK(!e.due(t + 70000, true));

    // Undo on trial.
    e.seen(usb(1800));
    e.change(usb(2000), t + 80000);
    e.due(t + 80000, true);
    e.undo();
    w = e.due(t + 80100, true);
    CHECK(w && w->widthHz == 1800);

    // Closing on trial goes back; quitting leaves it as found.
    e.change(usb(2100), t + 90000);
    e.due(t + 90000, true);
    w = e.close();
    CHECK(w && w->widthHz == 1800);
    CHECK(e.state() == FilterEdit::State::Idle);
    CHECK(e.quit() && *e.quit() == usb(2400));
    // Put back.
    e.putBack();
    w = e.due(t + 100000, true);
    CHECK(w && *w == usb(2400));
    CHECK(e.state() == FilterEdit::State::Idle);

    // The operator turns the mode knob mid-trial: the trial and what the
    // rig had are dropped, never written back in the old mode.
    FilterEdit moved;
    moved.seen(usb(2400));
    moved.change(usb(2400, 0.6, 0.5), t);
    CHECK(moved.due(t, true).has_value());
    RigFilter lsb = usb(2400);
    lsb.mode = "LSB";
    moved.seen(lsb);
    CHECK(moved.state() == FilterEdit::State::Idle);
    CHECK(!moved.due(t + FilterEdit::KEEP_MS + 1, true));
    CHECK(!moved.close());
    CHECK(!moved.quit());
    CHECK(moved.found() && moved.found()->mode == "LSB");

    // Never changed: nothing to put back on quitting.
    FilterEdit untouched;
    untouched.seen(usb(2400));
    CHECK(!untouched.quit());
    CHECK(!untouched.close());
}

void calibrate()
{
    std::mt19937 rng(11);
    // Calibrate a pretend IC-7100, as measured on Jeff's (2026-10-26): a
    // full turn moves an edge 3000 Hz, its 3000 Hz filter passes 3140 Hz
    // about 1499 Hz, and the audio cuts at 70 Hz, hiding the low edge with
    // the knobs centred. Started with the out knob a little down.
    PretendRig rig{knobs(3000, 1499, 3140, 3000, 70), usb(3000, 0.5, 0.45)};
    Calibration c;
    RigFilter noKnobs;
    noKnobs.mode = "USB";
    noKnobs.widthHz = 3000;
    CHECK(!c.start(noKnobs));
    CHECK(c.start(rig.filter));
    int steps = 0;
    while (c.running() && steps++ < 10)
    {
        CHECK(!c.stepText().empty());
        std::optional<RigFilter> f = c.setting();
        CHECK(f.has_value());
        if (!f) break;
        rig.filter = *f;
        std::optional<Edges> e = rig.listen(rng);
        CHECK(e.has_value());
        c.measured(e ? spanFrom(*e) : Span{});
    }
    CHECK_EQ(steps, Calibration::MEASURES + 1);
    CHECK(rig.filter == usb(3000, 0.5, 0.45));     // put back
    CHECK(c.result().has_value());
    const KnobModel m = c.result().value_or(KnobModel{});
    CHECK(std::fabs(m.hzPerKnob - 3000) < 3000 * 0.08);
    CHECK(std::fabs(m.centreHz - 1499) < 40);
    CHECK(std::fabs(m.widthHz - 3140) < 80);
    CHECK_EQ(m.nominalHz, 3000.0);
    CHECK(m.floorHz > 40 && m.floorHz < 110);
    CHECK_EQ(m.ceilingHz, 0.0);

    // Then a drag of the high edge lands where it was meant to, the low
    // edge still hidden.
    rig.filter = usb(3000);
    std::optional<Edges> now = rig.listen(rng);
    CHECK(now.has_value());
    if (now)
    {
        const Span from = spanFrom(*now);
        rig.filter = settingFor(dragTarget(from, Span{from.lowHz, 2450}, rig.filter, m), rig.filter, m);
        std::optional<Edges> after = rig.listen(rng);
        CHECK(after && near(after->lowHz, from.lowHz, 30) && near(after->highHz, 2450, 40));
        // The middle 300 Hz up: the low edge comes into sight.
        if (after)
        {
            const Span a = spanFrom(*after);
            rig.filter =
                settingFor(dragTarget(a, Span{a.lowHz + 300, a.highHz + 300}, rig.filter, m), rig.filter, m);
            std::optional<Edges> shifted = rig.listen(rng);
            CHECK(shifted && near(shifted->lowHz, 229, 40) && near(shifted->highHz, 2750, 40));
        }
    }

    // A rig whose knobs do nothing: no measure, and says why.
    PretendRig deaf{knobs(0, 1500, 2400, 2400), usb(2400)};
    Calibration d;
    d.start(deaf.filter);
    while (d.running())
    {
        deaf.filter = *d.setting();
        std::optional<Edges> e = deaf.listen(rng);
        d.measured(e ? spanFrom(*e) : Span{});
    }
    CHECK(!d.result());
    CHECK(d.problem().find("hardly moved") != std::string::npos);
}

Edges edgesAt(double lowHz, std::optional<double> highHz)
{
    Edges e;
    e.lowHz = lowHz;
    e.highHz = highHz;
    return e;
}

void spans()
{
    // A tone at 9 kHz, on a 12 kHz picture.
    Spectrum s(48000, 10.0);
    s.setTopHz(12000);
    CHECK_EQ(s.topHz(), 12000.0);
    std::vector<float> x = tone(48000, 9000.0, 1.0, 1.0);
    auto rows = s.push(x.data(), x.size());
    CHECK(!rows.empty());
    if (!rows.empty())
    {
        CHECK_EQ(peakBin(rows.back()), Spectrum::binOf(9000.0, 12000.0));
        CHECK(rows.back()[Spectrum::binOf(9000.0, 12000.0)] > -6.0f);
    }
    // A narrow span: steps finer than the FFT's bins still read.
    s.setTopHz(2500);
    x = tone(48000, 1000.0, 1.0, 1.0);
    rows = s.push(x.data(), x.size());
    CHECK(!rows.empty() && std::abs(peakBin(rows.back()) - Spectrum::binOf(1000.0, 2500.0)) <= 1);
    s.setTopHz(100000);
    CHECK_EQ(s.topHz(), Spectrum::MAX_TOP_HZ);

    // FM audio, 300 to 6000 Hz, on an 8 kHz picture.
    std::mt19937 rng(5);
    EdgeFinder f(10.0);
    f.setTopHz(8000);
    for (int i = 0; i < 40; i++) f.addRow(noiseRow(rng, 300, 6000, {}, 8000), false);
    std::optional<Edges> e = f.edges();
    CHECK(e && near(e->lowHz, 300, 40) && near(e->highHz, 6000, 40));
    // And on a 4 kHz one its top runs off the picture.
    EdgeFinder g(10.0);
    for (int i = 0; i < 40; i++) g.addRow(noiseRow(rng, 300, 6000), false);
    e = g.edges();
    CHECK(!e || !e->highHz);

    // Auto's fit: past the edge by 400 Hz, rounded up to 500 Hz.
    CHECK_EQ(AutoSpan::fit(3050), 3500.0);
    CHECK_EQ(AutoSpan::fit(3100), 3500.0);
    CHECK_EQ(AutoSpan::fit(3101), 4000.0);
    CHECK_EQ(AutoSpan::fit(100), Spectrum::MIN_TOP_HZ);
    CHECK_EQ(AutoSpan::fit(20000), Spectrum::MAX_TOP_HZ);
    CHECK(fmMode("PKTFM") && fmMode("FMN") && !fmMode("PKTUSB"));
    CHECK(wideAudioMode("AM") && !wideAudioMode("USB"));

    const uint64_t t = 1000000;
    AutoSpan a;
    // No rig: 4 kHz until the edges say.
    CHECK_EQ(a.update(t, 0, std::nullopt, std::nullopt, false, 24000), 4000.0);
    // The rig on PKTUSB, edges at 70 and 3050 Hz: narrows to 3.5 kHz, but
    // only once that has held for 10 s.
    CHECK_EQ(a.update(t, 0, std::string("PKTUSB"), std::nullopt, false, 24000), 4000.0);
    CHECK_EQ(a.update(t + 100, 0, std::string("PKTUSB"), edgesAt(70, 3050), false, 24000), 4000.0);
    CHECK_EQ(a.update(t + 5000, 0, std::string("PKTUSB"), edgesAt(70, 3050), false, 24000), 4000.0);
    CHECK_EQ(a.update(t + 10100, 0, std::string("PKTUSB"), edgesAt(70, 3050), false, 24000), 3500.0);
    // The edges start afresh with the span: it holds meanwhile.
    CHECK_EQ(a.update(t + 10600, 0, std::string("PKTUSB"), std::nullopt, false, 24000), 3500.0);
    // A wider filter: wider at once.
    CHECK_EQ(a.update(t + 11000, 0, std::string("PKTUSB"), edgesAt(70, 3700), false, 24000), 4500.0);
    // Its top off the picture: the next step at once.
    CHECK_EQ(a.update(t + 12000, 0, std::string("PKTUSB"), edgesAt(70, std::nullopt), false, 24000), 8000.0);
    // A narrowing that doesn't hold doesn't happen.
    CHECK_EQ(a.update(t + 13000, 0, std::string("PKTUSB"), edgesAt(70, 2700), false, 24000), 8000.0);
    CHECK_EQ(a.update(t + 20000, 0, std::string("PKTUSB"), edgesAt(70, 3900), false, 24000), 8000.0);
    CHECK_EQ(a.update(t + 24000, 0, std::string("PKTUSB"), edgesAt(70, 2700), false, 24000), 8000.0);
    // FM: 8 kHz at once.
    CHECK_EQ(a.update(t + 30000, 0, std::string("USB"), std::nullopt, false, 24000), 4000.0);
    CHECK_EQ(a.update(t + 31000, 0, std::string("FM"), std::nullopt, false, 24000), 8000.0);
    // FM's audio reaching the top: wider; on SSB that is more likely the
    // sound card's own noise, and it stays.
    CHECK_EQ(a.update(t + 31500, 0, std::string("FM"), std::nullopt, true, 24000), 12000.0);
    AutoSpan b;
    CHECK_EQ(b.update(t, 0, std::string("USB"), std::nullopt, true, 24000), 4000.0);
    CHECK_EQ(b.update(t + 100, 0, std::string("USB"), std::nullopt, true, 24000), 4000.0);
    CHECK(f.reachesTop() == false);
    CHECK(g.reachesTop());
    // Fixed choices, and never past half the sound card's rate.
    CHECK_EQ(a.update(t + 32000, 12000, std::string("FM"), std::nullopt, false, 24000), 12000.0);
    CHECK_EQ(a.update(t + 33000, 12000, std::string("FM"), std::nullopt, false, 4000), 4000.0);
    CHECK_EQ(a.update(t + 34000, 0, std::string("FM"), std::nullopt, false, 4000), 4000.0);
}

} // namespace

// The notch: its model, the dip it leaves in the noise, the passband
// bridged over it, and the edges held off while the rig's AGC settles.
void notch()
{
    std::mt19937 rng(11);
    // A notch's V: 340 Hz across, 45 dB down at its centre.
    const std::vector<Lift> hole = {{1330, 1670, -10.0}, {1400, 1600, -10.0}, {1450, 1550, -10.0}, {1480, 1520, -15.0}};

    // Unbridged, the 340 Hz hole splits the passband and the edges go wrong.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 60; i++) f.addRow(noiseRow(rng, 300, 2700, hole), false);
        std::optional<Edges> e = f.edges();
        CHECK(e && !(near(e->lowHz, 300, 20) && near(e->highHz, 2700, 20)));
        // Bridged over where the notch is, they're right, and the dip is found.
        f.setBridge(1050, 1950);
        e = f.edges();
        CHECK(e && near(e->lowHz, 300, 20) && near(e->highHz, 2700, 20));
        std::optional<Dip> d = f.dip(1050, 1950);
        CHECK(d && std::fabs(d->hz - 1500) < 30 && d->depthDb > 30);
        // No dip where there is none.
        CHECK(!f.dip(2000, 2500));
    }

    // After a write the noise is left out for the hold.
    {
        EdgeFinder f(10.0);
        for (int i = 0; i < 60; i++) f.addRow(noiseRow(rng, 300, 2700), false);
        CHECK(f.edges().has_value());
        CHECK(!f.settling());
        f.restart(7.0);
        CHECK(f.settling());
        for (int i = 0; i < 70; i++) f.addRow(noiseRow(rng, 300, 2700, {{0, 4000, 10.0}}), false);
        CHECK(f.quietSeconds() == 0.0);
        CHECK(f.quietRunSeconds() == 0.0);
        CHECK(!f.settling());
        CHECK(!f.edges());
        for (int i = 0; i < 30; i++) f.addRow(noiseRow(rng, 300, 2700), false);
        CHECK(f.edges().has_value());
    }

    // The model: the IC-7100's line, then corrected by dips.
    {
        NotchModel m;
        CHECK(std::fabs(m.hzOf(0.498) - 1505) < 1);
        CHECK(std::fabs(m.hzOf(m.rawFor(1500)) - 1500) < 15);
        CHECK(m.rawFor(-5000) == 0.0 && m.rawFor(9000) == 1.0);
        CHECK(!m.learned());
        // One dip moves the line.
        m.learn(0.5, 1600);
        CHECK(std::fabs(m.hzOf(0.5) - 1600) < 1e-6);
        CHECK(m.hzPerUnit == NotchModel::HZ_PER_UNIT);
        // A second close by moves it again; one far enough away sets the slope.
        m.learn(0.55, 1850);
        CHECK(m.hzPerUnit == NotchModel::HZ_PER_UNIT);
        m.learn(0.75, 2850);
        CHECK(std::fabs(m.hzPerUnit - 5000) < 1e-6);
        CHECK(std::fabs(m.hzOf(0.75) - 2850) < 1e-6);
        // An unbelievable slope is ignored.
        NotchModel n;
        n.learn(0.3, 500);
        n.learn(0.6, 520);
        CHECK(n.hzPerUnit == NotchModel::HZ_PER_UNIT);
        // Kept as text and back.
        std::optional<NotchModel> back = NotchModel::fromText(m.text());
        CHECK(back && std::fabs(back->hzPerUnit - m.hzPerUnit) < 0.1 && std::fabs(back->offsetHz - m.offsetHz) < 0.1);
        CHECK(back && back->seenRaw && std::fabs(*back->seenRaw - 0.75) < 1e-4);
        back = NotchModel::fromText(NotchModel().text());
        CHECK(back && !back->learned());
        CHECK(!NotchModel::fromText(""));
        CHECK(!NotchModel::fromText("0 5 - -"));
    }

    // Data2G's passband, and the change's words.
    CHECK(notchCutsData2G(1500, false));
    CHECK(notchCutsData2G(200, false));
    CHECK(!notchCutsData2G(100, false));
    CHECK(!notchCutsData2G(1000, true));
    CHECK(notchCutsData2G(1100, true));
    {
        const NotchModel m;
        RigFilter off = usb(2400);
        off.notchOn = false;
        off.notchRaw = 0.5;
        const RigFilter on = withNotch(off, 1500.0, m);
        CHECK(on.notchOn && *on.notchOn && on.notchRaw && std::fabs(m.hzOf(*on.notchRaw) - 1500) < 15);
        CHECK(on.pbtIn == off.pbtIn && on.widthHz == off.widthHz);
        const RigFilter moved = withNotch(on, 1200.0, m);
        const RigFilter offAgain = withNotch(moved, std::nullopt, m);
        CHECK(offAgain.notchOn && !*offAgain.notchOn && offAgain.notchRaw == moved.notchRaw);
        // In the rig's steps, about 20 Hz apart.
        CHECK_EQ(notchChangeText(off, on, m), std::string("Notch on at 1510 Hz"));
        CHECK_EQ(notchChangeText(on, moved, m), std::string("Notch 1510 \xe2\x86\x92 1190 Hz"));
        CHECK_EQ(notchChangeText(moved, offAgain, m), std::string("Notch off"));
        CHECK_EQ(notchChangeText(off, off, m), std::string());
        CHECK_EQ(notchChangeText(usb(2400), usb(1800), m), std::string());
    }
}

// The clock stands still while held.
void clockHeld()
{
    const uint64_t t = 1000000;
    FilterEdit e;
    e.seen(usb(2400));
    e.change(usb(1800), t);
    CHECK(e.due(t, true).has_value());
    e.hold(true, t + 5000);
    CHECK(!e.due(t + 40000, true));
    CHECK(e.state() == FilterEdit::State::Trial);
    CHECK(std::fabs(e.secondsLeft(t + 40000) - 10.0) < 1e-9);
    e.hold(false, t + 40000);
    CHECK(!e.due(t + 49000, true));
    CHECK(std::fabs(e.secondsLeft(t + 45000) - 5.0) < 1e-9);
    std::optional<RigFilter> w = e.due(t + 50000, true);
    CHECK(w && w->widthHz == 2400);
}

// Keep pressed while a change waits: kept as it is written, no trial; a
// newer drag or Undo drops it.
void keepAhead()
{
    const uint64_t t = 2000000;
    FilterEdit e;
    e.seen(usb(2400));
    e.change(usb(1800), t);
    e.keep();
    CHECK(e.keptAhead());
    CHECK(e.state() == FilterEdit::State::Waiting);
    CHECK(!e.due(t + 100, false));
    std::optional<RigFilter> w = e.due(t + 7000, true);
    CHECK(w && w->widthHz == 1800);
    CHECK(e.state() == FilterEdit::State::Idle);
    CHECK(!e.keptAhead());
    // Nothing goes back later.
    CHECK(!e.due(t + 60000, true));
    CHECK(e.current() && e.current()->widthHz == 1800);

    // A second drag during a trial, kept while it waits: kept as written,
    // and the first goes with it.
    e.seen(usb(1800));
    e.change(usb(2000), t + 70000);
    CHECK(e.due(t + 70000, true).has_value());
    CHECK(e.state() == FilterEdit::State::Trial);
    e.change(usb(2100), t + 72000);
    e.keep();
    w = e.due(t + 80000, true);
    CHECK(w && w->widthHz == 2100);
    CHECK(e.state() == FilterEdit::State::Idle);
    CHECK(!e.due(t + 100000, true));

    // A newer drag after Keep starts a trial of its own.
    e.seen(usb(2100));
    e.change(usb(2200), t + 110000);
    e.keep();
    e.change(usb(2300), t + 110500);
    CHECK(!e.keptAhead());
    CHECK(e.due(t + 111000, true).has_value());
    CHECK(e.state() == FilterEdit::State::Trial);
    // Undo while waiting forgets it: nothing is written.
    e.keep();
    e.change(usb(2400), t + 112000);
    e.keep();
    e.undo();
    CHECK(!e.keptAhead());
    CHECK(e.state() == FilterEdit::State::Idle);
    CHECK(!e.due(t + 113000, true));
    CHECK(e.current() && e.current()->widthHz == 2300);
}

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
    twinPbtModel();
    keepAndUndo();
    calibrate();
    spans();
    notch();
    clockHeld();
    keepAhead();
    return ragtime_test::finish();
}
