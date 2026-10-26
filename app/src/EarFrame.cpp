#include "EarFrame.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <wx/choice.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "EarScope.h"
#include "Prefs.h"
#include "ragtime/ear/ProcNet.h"
#include "ragtime/radio/Noise.h"

using namespace ragtime;

namespace
{

constexpr double SPEEDS[] = {5.0, 10.0, 20.0};
const char* const SPEED_NAMES[] = {"Slow", "Normal", "Fast"};
constexpr int TICK_MS = 50;
constexpr int HOST_LOOK_TICKS = 100;    // every 5 s

wxString utf8(const std::string& s)
{
    return wxString::FromUTF8(s);
}

const wxString DOT = wxString::FromUTF8(" \xc2\xb7 ");

bool onThisMachine(const std::string& host)
{
    return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
}

} // namespace

EarFrame::EarFrame(wxWindow* parent, std::function<void()> onClosed)
    : wxFrame(parent, wxID_ANY, wxString::FromUTF8("Ear \xc2\xb7 what the radio hears"), wxDefaultPosition,
              wxDefaultSize, wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
    , audio_(audio::makeEarAudio())
    , onClosed_(std::move(onClosed))
    , timer_(this)
{
    auto* panel = new wxPanel(this);
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    source_ = new wxChoice(panel, wxID_ANY);
    source_->SetToolTip("What the Ear listens to: by default the sound card data2g-host records from, alongside it. "
                        "It only records, and never plays a sound.");
    speed_ = new wxChoice(panel, wxID_ANY);
    for (const char* name : SPEED_NAMES) speed_->Append(name);
    speed_->SetToolTip("How fast the waterfall runs: 5, 10 or 20 rows a second");
    format_ = new wxStaticText(panel, wxID_ANY, "");
    top->Add(new wxStaticText(panel, wxID_ANY, "Listen to"), 0, wxALIGN_CENTER_VERTICAL);
    top->Add(source_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    top->Add(new wxStaticText(panel, wxID_ANY, "Speed"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
    top->Add(speed_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    top->AddStretchSpacer();
    top->Add(format_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));

    // What the rig says of its filter, and what the noise shows.
    auto* filter = new wxBoxSizer(wxHORIZONTAL);
    rigSays_ = new wxStaticText(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    noiseShows_ = new wxStaticText(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    filter->Add(rigSays_, 1, wxALIGN_CENTER_VERTICAL);
    filter->Add(noiseShows_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));

    scope_ = new EarScope(panel);
    status_ = new wxStaticText(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(top, 0, wxEXPAND | wxALL, FromDIP(10));
    outer->Add(filter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    outer->Add(scope_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));
    outer->Add(status_, 0, wxEXPAND | wxALL, FromDIP(10));
    panel->SetSizer(outer);
    // Fits a 1366 x 768 screen with room to spare.
    SetClientSize(FromDIP(wxSize(900, 560)));
    SetMinClientSize(FromDIP(wxSize(640, 400)));

    chosen_ = loadString(keys::EAR_SOURCE, "");
    const int rows = loadInt(keys::EAR_SPEED, 10);
    int speed = 1;
    for (int i = 0; i < 3; i++)
        if ((int)SPEEDS[i] == rows) speed = i;
    speed_->SetSelection(speed);
    spectrum_.setRowsPerSecond(SPEEDS[speed]);
    scope_->setRowsPerSecond(SPEEDS[speed]);
    edges_.setRowsPerSecond(SPEEDS[speed]);

    source_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { chooseSource(); });
    speed_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const double rps = SPEEDS[std::max(0, speed_->GetSelection())];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            spectrum_.setRowsPerSecond(rps);
            pending_.clear();
        }
        scope_->setRowsPerSecond(rps);
        edges_.setRowsPerSecond(rps);
        saveInt(keys::EAR_SPEED, (int)rps);
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        timer_.Stop();
        audio_->stop();
        if (onClosed_) onClosed_();
        Destroy();
    });

    audio_->start(
        [this](const float* samples, std::size_t n, int rate) {
            std::lock_guard<std::mutex> lock(mutex_);
            spectrum_.setRate(rate);
            for (auto& row : spectrum_.push(samples, n)) pending_.push_back(std::move(row));
            // A window closed without draining: never grow without bound.
            if (pending_.size() > 400) pending_.erase(pending_.begin(), pending_.end() - 400);
        },
        [this] { changed_ = true; });
    audio_->choose(chosen_);
    timer_.Start(TICK_MS);
}

EarFrame::~EarFrame()
{
    timer_.Stop();
    audio_->stop();
}

void EarFrame::setHost(const std::string& host, int commandPort)
{
    if (host == host_ && commandPort == commandPort_) return;
    host_ = host;
    commandPort_ = commandPort;
    lookForHost();
}

void EarFrame::setNarrow(bool narrow)
{
    scope_->setNarrow(narrow);
}

void EarFrame::setTransmitting(bool on)
{
    transmitting_ = on;
}

void EarFrame::setRig(std::function<EarRig()> rig)
{
    rig_ = std::move(rig);
    changed_ = true;
}

void EarFrame::lookForHost()
{
    if (commandPort_ > 0 && onThisMachine(host_))
        audio_->setHostPid(ear::processListeningOn(commandPort_));
    else
        audio_->setHostPid(std::nullopt);
}

void EarFrame::tick()
{
    std::vector<std::vector<float>> rows;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rows.swap(pending_);
    }
    if (!rows.empty())
    {
        scope_->addRows(rows, transmitting_);
        for (const auto& row : rows) edges_.addRow(row, transmitting_);
    }

    if (++ticks_ % HOST_LOOK_TICKS == 0)
    {
        // Only worth the look when there is a choice to make.
        if (audio_->status().host.candidates > 1) lookForHost();
    }
    // Whether audio is coming changes without a word from PipeWire.
    if (changed_.exchange(false) || ticks_ % 10 == 0)
    {
        const audio::EarStatus status = audio_->status();
        showFilter(status);
        showStatus(status);
    }
}

void EarFrame::showStatus(const audio::EarStatus& s)
{
    // "Listen to": data2g-host's card first, then every source.
    std::vector<std::string> names{""};
    std::vector<wxString> labels;
    wxString follow = "Same as data2g-host";
    if (s.host.found)
    {
        follow = "Same as " + utf8(s.host.found->program);
        if (!s.host.found->sourceDescription.empty()) follow += DOT + utf8(s.host.found->sourceDescription);
    }
    labels.push_back(follow);
    for (const ear::GraphNode& n : s.sources)
    {
        names.push_back(n.name);
        labels.push_back(utf8(n.description.empty() ? n.name : n.description));
    }
    // A source picked before, now gone, stays on the list as missing.
    if (!chosen_.empty() && std::find(names.begin(), names.end(), chosen_) == names.end())
    {
        names.push_back(chosen_);
        labels.push_back(utf8(chosen_) + " (not there)");
    }
    bool same = names == sourceNames_ && (int)source_->GetCount() == (int)labels.size();
    for (std::size_t i = 0; same && i < labels.size(); i++) same = source_->GetString((unsigned)i) == labels[i];
    if (!same)
    {
        source_->Freeze();
        source_->Clear();
        for (const wxString& l : labels) source_->Append(l);
        source_->Thaw();
        sourceNames_ = names;
    }
    const int index = (int)(std::find(names.begin(), names.end(), chosen_) - names.begin());
    if (source_->GetSelection() != index) source_->SetSelection(index);
    source_->Enable(s.available);

    wxString format = s.streaming && s.rate > 0 ? wxString::Format("%g kHz", s.rate / 1000.0) : wxString();
    if (!noiseText_.empty()) format += (format.empty() ? wxString() : DOT) + noiseText_;
    if (format_->GetLabel() != format || !same)
    {
        format_->SetLabel(format);
        source_->InvalidateBestSize();
        source_->GetParent()->Layout();
    }

    wxString line;
    if (!s.available)
    {
        line = utf8(s.problem);
    }
    else if (s.following)
    {
        if (s.host.found && s.host.found->source != 0)
        {
            line = "Listening alongside " + utf8(s.host.found->program) + " to " +
                   utf8(s.host.found->sourceDescription) + ".";
        }
        else
        {
            line = utf8(s.host.why);
            if (!line.empty()) line[0] = wxToupper(line[0]);
            line += ".";
            if (!s.listeningDescription.empty()) line += " Still on " + utf8(s.listeningDescription) + ".";
            else if (s.host.candidates == 0) line += " Pick a source in Listen to, or start data2g-host.";
        }
        if (s.listening != 0 && !s.streaming)
            line += " Waiting for audio: the sound card runs only while data2g-host records.";
    }
    else
    {
        line = s.listeningDescription.empty() ? wxString("That source isn't there.")
                                              : "Listening to " + utf8(s.listeningDescription) + ".";
        if (s.listening != 0 && !s.streaming) line += " Waiting for audio.";
    }
    status_->SetLabel(line);
    status_->SetToolTip(line);
}

void EarFrame::showFilter(const audio::EarStatus& s)
{
    const wxString dash = wxString::FromUTF8("\xe2\x80\x93");
    const EarRig rig = rig_ ? rig_() : EarRig();
    wxString says;
    if (rig.via.empty())
        says = "The rig: none in Settings > Rig, so only the noise shows the filter";
    else if (!rig.filter)
        says = "The rig hasn't said its filter yet (" + utf8(rig.via) + ")";
    else
        says = "The rig says " + utf8(radio::describe(*rig.filter));
    scope_->setRigWidth(rig.filter ? rig.filter->widthHz : 0);
    noiseText_ = rig.noiseDb ? "QRN " + utf8(radio::sUnitText(*rig.noiseDb)) : wxString();

    const std::optional<ear::Edges> e = s.streaming ? edges_.edges() : std::nullopt;
    scope_->setEdges(e);
    auto hz = [](double v) { return wxString::Format("%d", (int)std::lround(v / 10.0) * 10); };
    wxString shows;
    if (!s.streaming)
        shows = "The noise: no audio";
    else if (e && e->lowHz && e->highHz)
        shows = "The noise shows " + hz(*e->lowHz) + dash + hz(*e->highHz) + " Hz, " +
                wxString::Format("%d", (int)std::lround((*e->highHz - *e->lowHz) / 10.0) * 10) + " Hz wide";
    else if (e && e->lowHz)
        shows = "The noise shows a low edge at " + hz(*e->lowHz) + " Hz; the top is past 4 kHz";
    else if (e && e->highHz)
        shows = "The noise shows a high edge at " + hz(*e->highHz) + " Hz";
    else if (edges_.quietSeconds() < ear::EdgeFinder::NEEDED_S)
        shows = "The noise: listening for a quiet moment";
    else
        shows = "The noise shows no clear filter edges";
    if (rigSays_->GetLabel() != says)
    {
        rigSays_->SetLabel(says);
        rigSays_->SetToolTip(says + (rig.filter ? ", read between bursts while the Ear is open" : ""));
    }
    if (noiseShows_->GetLabel() != shows)
    {
        noiseShows_->SetLabel(shows);
        noiseShows_->SetToolTip("Where the band's noise falls 6 dB below the passband, from the quiet moments "
                                "between signals: the filter's real edges, whatever the rig says.");
    }
}

void EarFrame::chooseSource()
{
    const int i = source_->GetSelection();
    if (i < 0 || i >= (int)sourceNames_.size()) return;
    chosen_ = sourceNames_[(std::size_t)i];
    saveString(keys::EAR_SOURCE, chosen_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
    }
    scope_->clear();
    edges_.clear();
    audio_->choose(chosen_);
}
