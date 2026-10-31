#include "VoiceCards.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/filesys.h>
#include <wx/fs_mem.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/window.h>

#include "Html.h"
#include "Pictures.h"
#include "Theme.h"
#include "ragtime/audio/VoiceAudio.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/voice/Sotto.h"

using namespace ragtime;

namespace
{

// The card's picture, in DIPs: the round button, a gap, the strip.
constexpr int BUTTON_DIP = 28;
constexpr int GAP_DIP = 8;
constexpr int STRIP_DIP = 240;
constexpr int HEIGHT_DIP = 32;
// One column of the strip every this many pixels (at 1x).
constexpr int COLUMN_DIP = 3;
// The strip's click areas: enough to aim at a word, few enough for wxHTML.
constexpr int SEEK_AREAS = 48;

void addMemoryHandlers()
{
    if (!wxFileSystem::HasHandlerForPath("memory:x")) wxFileSystem::AddHandler(new wxMemoryFSHandler);
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
}

bool sameRanges(const std::vector<voice::ByteRange>& a, const std::vector<voice::ByteRange>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].begin != b[i].begin || a[i].end != b[i].end) return false;
    return true;
}

wxColour mix(const wxColour& a, const wxColour& b, double t)
{
    auto m = [t](unsigned char x, unsigned char y) { return (unsigned char)std::lround(x + (y - x) * t); };
    return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
}

// The strip and its button, drawn into an image w by h pixels.
wxImage drawCard(const voice::Clip& clip, const wxWindow* window, int w, int h, const VoiceShelf::Look& look,
                 int headPx)
{
    wxBitmap bitmap(w, h, 32);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(*wxTRANSPARENT_BRUSH);
        dc.Clear();
    }
    wxImage image = bitmap.ConvertToImage();
    if (!image.HasAlpha()) image.InitAlpha();
    std::fill(image.GetAlpha(), image.GetAlpha() + (size_t)w * h, (unsigned char)0);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(image));
    if (!gc) return image;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxColour accent = accentText();
    const wxColour faint = secondaryText();
    const wxColour background = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);

    // The button: a round face in the accent, a triangle to play, a square
    // while it plays, two bars while Push to talk holds it.
    const double button = window->FromDIP(BUTTON_DIP);
    const double by = (h - button) / 2.0;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(look.waiting ? mix(accent, background, 0.35) : accent));
    gc->DrawEllipse(0.5, by + 0.5, button - 1.0, button - 1.0);
    gc->SetBrush(wxBrush(background));
    const double c = button / 2.0, cy = by + button / 2.0, s = button * 0.22;
    if (look.playing && look.paused)
    {
        gc->DrawRectangle(c - s * 0.9, cy - s, s * 0.6, s * 2.0);
        gc->DrawRectangle(c + s * 0.3, cy - s, s * 0.6, s * 2.0);
    }
    else if (look.playing)
    {
        gc->DrawRectangle(c - s * 0.85, cy - s * 0.85, s * 1.7, s * 1.7);
    }
    else
    {
        wxGraphicsPath triangle = gc->CreatePath();
        triangle.MoveToPoint(c - s * 0.7, cy - s);
        triangle.AddLineToPoint(c + s * 1.05, cy);
        triangle.AddLineToPoint(c - s * 0.7, cy + s);
        triangle.CloseSubpath();
        gc->FillPath(triangle);
    }

    // The strip.
    const double x0 = button + window->FromDIP(GAP_DIP);
    const double sw = w - x0;
    const double pad = window->FromDIP(3);
    const int columns = std::max(8, (int)(sw / window->FromDIP(COLUMN_DIP)));
    const voice::PitchStrip strip = voice::pitchStrip(clip, columns);
    const double colW = sw / columns;
    const double maxThick = h * 0.42;
    const double midY = h / 2.0;
    // A hairline along the middle where nothing has come yet.
    for (int i = 0; i < columns; i++)
    {
        const voice::StripColumn& col = strip.columns[(size_t)i];
        const double x = x0 + i * colW;
        const bool played = look.playing && headPx >= 0 && x + colW / 2.0 <= headPx;
        if (!col.arrived)
        {
            gc->SetBrush(wxBrush(mix(faint, background, 0.55)));
            gc->DrawRectangle(x, midY - 0.5, std::max(1.0, colW - 1.0), 1.0);
            continue;
        }
        if (col.lost)
        {
            // Hatched: a piece that didn't come, a short quiet gap.
            gc->SetBrush(wxBrush(mix(faint, background, 0.7)));
            gc->DrawRectangle(x, pad, colW, h - 2 * pad);
            gc->SetPen(wxPen(mix(faint, background, 0.25), 1));
            gc->StrokeLine(x, h - pad, x + colW, pad);
            gc->SetPen(*wxTRANSPARENT_PEN);
            continue;
        }
        if (col.silent) continue;
        const double thick = std::max(1.5, strip.thickness(col) * maxThick);
        double y = midY;
        wxColour ink = played ? mix(accent, background, 0.45) : accent;
        if (col.voiced)
        {
            // Pitch as height, within the room the thickness leaves.
            const double room = h - 2 * pad - thick;
            y = pad + thick / 2.0 + (1.0 - strip.height(col)) * room;
        }
        else
        {
            // Hiss has no pitch: a softer mark along the middle.
            ink = mix(ink, background, 0.45);
        }
        gc->SetBrush(wxBrush(ink));
        gc->DrawRoundedRectangle(x, y - thick / 2.0, std::max(1.0, colW - 0.5), thick, std::min(thick, colW) / 2.5);
    }
    // The playhead.
    if (look.playing && headPx >= 0)
    {
        gc->SetBrush(wxBrush(badText()));
        gc->DrawRectangle(headPx - 1.0, 0, 2.0, h);
    }
    gc.reset();
    return image;
}

} // namespace

std::string clipLength(double seconds)
{
    long total = std::lround(std::max(0.0, seconds));
    // Under a second still reads 0:01, so a card never claims nothing.
    if (total == 0 && seconds > 0) total = 1;
    char text[32];
    std::snprintf(text, sizeof text, "%ld:%02ld", total / 60, total % 60);
    return text;
}

bool VoiceShelf::update(const std::string& key, const std::vector<uint8_t>& bytes, uint64_t size, uint64_t have,
                        const std::vector<voice::ByteRange>& missing, bool final)
{
    Entry& e = entries_[key];
    if (e.shelved.final && e.shelved.version > 0) return false;
    if (e.shelved.version > 0 && e.shelved.have == have && e.shelved.size == size && e.lastBytes == bytes
        && sameRanges(e.lastMissing, missing))
    {
        bool changed = final != e.shelved.final;
        e.shelved.final = final;
        return changed;
    }
    e.lastBytes = bytes;
    e.lastMissing = missing;
    voice::Clip clip;
    voice::ReadResult result = voice::readSot(bytes.data(), size, std::min<uint64_t>(have, bytes.size()), missing, clip);
    e.shelved.clip = std::move(clip);
    e.shelved.result = result;
    e.shelved.have = have;
    e.shelved.size = size;
    e.shelved.holes = missing.size();
    e.shelved.final = final;
    e.shelved.version++;
    return true;
}

bool VoiceShelf::shelveFile(const std::string& path)
{
    const std::string key = "p" + path;
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.shelved.final) return it->second.shelved.playable();
    std::ifstream in(files::pathFromUtf8(path), std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > files::SessionFiles::MAX_FILE_BYTES) bytes.resize(files::SessionFiles::MAX_FILE_BYTES);
    update(key, bytes, bytes.size(), bytes.size(), {}, true);
    return entries_[key].shelved.playable();
}

bool VoiceShelf::settled(const std::string& key) const
{
    auto it = entries_.find(key);
    return it != entries_.end() && it->second.shelved.final && it->second.shelved.version > 0;
}

const VoiceShelf::Shelved* VoiceShelf::find(const std::string& key) const
{
    auto it = entries_.find(key);
    return it == entries_.end() || it->second.shelved.version == 0 ? nullptr : &it->second.shelved;
}

void VoiceShelf::forget(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    if (!it->second.memoryName.empty()) wxMemoryFSHandler::RemoveFile(it->second.memoryName);
    entries_.erase(it);
}

wxString VoiceShelf::cardHtml(const std::string& key, const wxWindow* window, const Look& look, bool picked)
{
    auto it = entries_.find(key);
    if (it == entries_.end()) return wxString();
    Entry& e = it->second;
    const Shelved& s = e.shelved;
    // Each starts a line of its own: the text ones with a break, the card
    // with its map, where wxHTML breaks the line anyway.
    auto grey = [picked](const wxString& text) {
        return picked ? text : "<font color=\"" + htmlColour(secondaryText()) + "\">" + text + "</font>";
    };
    auto line = [&grey](const wxString& text) { return "<br>" + grey(text); };
    switch (s.result)
    {
        case voice::ReadResult::Ok:
            break;
        case voice::ReadResult::Short:
            // Its header rides in the first piece; on D2G that piece can be
            // the one lost, and the card waits for its repair.
            if (s.final) return line("<i>Too little of it came to play</i>");
            return line("<i>The card shows when its first piece comes</i>");
        case voice::ReadResult::UnknownCodec:
        case voice::ReadResult::UnknownFlags:
            return line("<i>A voice message this Ragtime can't play (a newer sotto)</i>");
        default:
            return line("<i>Not a voice message Ragtime can read</i>");
    }

    const int w = window->FromDIP(BUTTON_DIP + GAP_DIP + STRIP_DIP);
    const int h = window->FromDIP(HEIGHT_DIP);
    const int x0 = window->FromDIP(BUTTON_DIP + GAP_DIP);
    int headPx = -1;
    if (look.playing && look.playheadFrame >= 0 && s.clip.length > 0)
        headPx = x0 + (int)std::lround(std::min(1.0, look.playheadFrame / s.clip.length) * (w - x0));
    const bool dark = themeIsDark();
    if (e.drawnVersion != s.version || e.drawnPx != w || e.drawnPlaying != look.playing
        || e.drawnPaused != look.paused || e.drawnHead != headPx || e.drawnDark != dark || e.memoryName.empty())
    {
        addMemoryHandlers();
        if (!e.memoryName.empty()) wxMemoryFSHandler::RemoveFile(e.memoryName);
        // A new name each time, so wxHTML never shows one it cached.
        e.memoryName = wxString::Format("ragtime-voice-%d.png", nextName_++).ToStdString();
        wxImage image = drawCard(s.clip, window, w, h, look, headPx);
        wxMemoryFSHandler::AddFile(e.memoryName, image, wxBITMAP_TYPE_PNG);
        e.drawnVersion = s.version;
        e.drawnPx = w;
        e.drawnPlaying = look.playing;
        e.drawnPaused = look.paused;
        e.drawnHead = headPx;
        e.drawnDark = dark;
    }

    // The button plays or stops; the strip plays from where it's clicked.
    const wxString map = wxString::Format("ragtime-voice-map-%d", nextName_++);
    // The map before the picture: wxHTML breaks the line at a map, which
    // starts the card's line, and its length stays beside it.
    wxString html = "<map name=\"" + map + "\">";
    const int button = window->FromDIP(BUTTON_DIP);
    html << wxString::Format("<area shape=\"rect\" coords=\"0,0,%d,%d\" href=\"voice:%s:0\">", button - 1, h - 1,
                             look.playing ? "stop" : "play");
    if (s.clip.length > 0)
    {
        for (int a = 0; a < SEEK_AREAS; a++)
        {
            const int left = x0 + (w - x0) * a / SEEK_AREAS;
            const int right = x0 + (w - x0) * (a + 1) / SEEK_AREAS - 1;
            const uint32_t frame = voice::frameAt(s.clip, left - x0, w - x0);
            // Only where frames have come: past them there's nothing to play.
            if (frame >= s.clip.frames.size()) break;
            html << wxString::Format("<area shape=\"rect\" coords=\"%d,0,%d,%d\" href=\"voice:seek:%u\">", left, right,
                                     h - 1, frame);
        }
    }
    html << "</map>";
    html << wxString::Format("<img src=\"memory:%s\" width=\"%d\" height=\"%d\" usemap=\"#%s\" align=\"center\">",
                             wxString(e.memoryName), w, h, map);
    // A clip coming to us in a session: the stop sign beside the strip,
    // for when we have heard enough of it.
    if (!look.stopHref.empty()) html << "&nbsp;" << stopSignHtml(window, look.stopHref);

    // Its length, and while it comes, how far it has come.
    wxString after = wxString::FromUTF8(clipLength(s.clip.seconds()));
    if (!s.clip.whole && !s.final)
        after << " &middot; " << wxString::Format("%d%% here", (int)std::lround(100.0 * s.clip.frames.size()
                                                                                / std::max<uint32_t>(1, s.clip.length)));
    else if (!s.clip.whole)
        after << " &middot; part of it";
    if (look.waiting) after << " &middot; next to play";
    // Playing as it comes, but holding back until the bytes keep ahead.
    if (look.startsIn >= 0)
    {
        if (std::isinf(look.startsIn))
            after << " &middot; waiting for more of it";
        else if (look.startsIn < 1.0)
            after << " &middot; starting";
        else
            after << " &middot; " << wxString::Format("starts in %d s", (int)std::ceil(look.startsIn));
    }
    html << "&nbsp;&nbsp;" << grey(after);
    return html;
}

//-------------------------------------------------------------------------
// Playing
//-------------------------------------------------------------------------

bool VoicePlayer::canDecode(std::string& why)
{
    voice::Sotto& lib = voice::Sotto::library();
    if (lib.available()) return true;
    why = lib.info().why.empty() ? std::string("libsotto, the voice codec, isn't installed.") : lib.info().why;
    return false;
}

bool VoicePlayer::play(audio::VoiceAudio& audio, const std::string& key, const voice::Clip& clip, uint32_t fromFrame,
                       std::string& why)
{
    if (!canDecode(why)) return false;
    if (clip.frames.empty())
    {
        why = "Nothing of it has come yet.";
        return false;
    }
    std::unique_ptr<voice::SottoDecoder> decoder = voice::Sotto::library().decoder();
    if (!decoder)
    {
        why = "libsotto couldn't make a decoder.";
        return false;
    }
    std::vector<voice::SottoFrame> frames(clip.frames.begin(), clip.frames.end());
    std::vector<int16_t> sound = decoder->decodeClip(frames, clip.good);
    fromFrame = std::min<uint32_t>(fromFrame, (uint32_t)clip.frames.size() - 1);
    const size_t start = std::min(sound.size(), (size_t)fromFrame * voice::SOTTO_SAMPLES);
    std::vector<float> samples(sound.size() - start);
    for (size_t i = 0; i < samples.size(); i++) samples[i] = sound[start + i] / 32768.0f;

    const uint64_t generation = ++generation_;
    *current_ = generation;
    playing_ = key;
    fromFrame_ = fromFrame;
    arrival_.reset();
    audio.play(std::move(samples), voice::SOTTO_RATE, finisher(key, generation));
    return true;
}

std::function<void(bool)> VoicePlayer::finisher(const std::string& key, uint64_t generation)
{
    // A clip replaced or stopped says so later, on the audio thread: only
    // the latest one's word counts.
    std::weak_ptr<uint64_t> alive = current_;
    Post post = post_;
    Done done = done_;
    VoicePlayer* self = this;
    return [alive, post, done, self, generation, key](bool played) {
        post([alive, done, self, generation, key, played]() {
            std::shared_ptr<uint64_t> current = alive.lock();
            if (!current || *current != generation) return;
            self->playing_.clear();
            self->arrival_.reset();
            done(key, played);
        });
    };
}

namespace
{

// Sound handed to the speaker ahead of the ear while a clip plays as it
// arrives: enough to ride over a late tick, little enough that the stop
// sign or Push to talk is heard at once.
constexpr uint64_t LEAD_MS = 400;

} // namespace

bool VoicePlayer::playArriving(audio::VoiceAudio& audio, const std::string& key, const std::vector<uint8_t>& bytes,
                               uint64_t size, uint32_t fromFrame, double expectedRate, double expectedGap,
                               uint64_t nowMs, std::string& why)
{
    if (!canDecode(why)) return false;
    std::shared_ptr<voice::SottoDecoder> decoder(voice::Sotto::library().decoder());
    if (!decoder)
    {
        why = "libsotto couldn't make a decoder.";
        return false;
    }
    voice::FrameDecoder calls;
    calls.decode = [decoder](const voice::Frame& f, bool good, int16_t* out) { decoder->decode(f, good, out); };
    calls.flush = [decoder](int16_t* out) { decoder->flush(out); };
    auto player = std::make_unique<voice::ArrivalPlayer>(calls);
    player->setExpectedRate(expectedRate);
    if (expectedGap > 0) player->setExpectedGap(expectedGap);
    if (player->arrived(bytes.data(), size, bytes.size(), nowMs) != voice::ReadResult::Ok)
    {
        why = "Not enough of it has come to play.";
        return false;
    }
    if (fromFrame > 0) player->seek(fromFrame);

    const uint64_t generation = ++generation_;
    *current_ = generation;
    playing_ = key;
    fromFrame_ = 0;
    arrival_ = std::move(player);
    appended_ = 0;
    streamEnded_ = false;
    audio.playStream(voice::SOTTO_RATE, finisher(key, generation));
    tick(audio, nowMs);
    return true;
}

void VoicePlayer::arriving(const std::string& key, const std::vector<uint8_t>& bytes, uint64_t size, bool ended,
                           uint64_t nowMs)
{
    if (!arrival_ || key != playing_) return;
    arrival_->arrived(bytes.data(), size, bytes.size(), nowMs);
    if (ended && !arrival_->hasEnded()) arrival_->ended();
}

void VoicePlayer::tick(audio::VoiceAudio& audio, uint64_t nowMs)
{
    if (!arrival_ || streamEnded_) return;
    // How far ahead of the ear the speaker has been handed sound.
    const uint64_t heard = (uint64_t)std::llround(std::max(0.0, audio.playPosition()) * voice::SOTTO_RATE);
    const uint64_t lead = LEAD_MS * voice::SOTTO_RATE / 1000;
    uint64_t ahead = appended_ > heard ? appended_ - heard : 0;
    std::vector<int16_t> pcm;
    std::vector<float> samples;
    while (ahead < lead)
    {
        pcm.assign((size_t)(lead - ahead), 0);
        const size_t got = arrival_->pull(pcm.data(), pcm.size(), nowMs + ahead * 1000 / voice::SOTTO_RATE);
        if (got == 0) break;
        samples.resize(got);
        for (size_t i = 0; i < got; i++) samples[i] = pcm[i] / 32768.0f;
        audio.append(samples.data(), got);
        appended_ += got;
        ahead += got;
    }
    if (arrival_->state() == voice::ArrivalPlayer::State::Done)
    {
        // All it will get has been handed over: the speaker says when it
        // has been heard.
        audio.endStream();
        streamEnded_ = true;
    }
}

double VoicePlayer::startsIn(uint64_t nowMs) const
{
    if (!arrival_) return -1.0;
    using State = voice::ArrivalPlayer::State;
    if (arrival_->state() != State::Waiting && arrival_->state() != State::Stalled) return -1.0;
    // No rate yet to tell by: waiting for more of it.
    const double s = arrival_->startsIn(nowMs);
    return s < 0 ? std::numeric_limits<double>::infinity() : s;
}

void VoicePlayer::stop(audio::VoiceAudio& audio)
{
    if (playing_.empty()) return;
    audio.stopPlaying();
}

double VoicePlayer::positionFrames(const audio::VoiceAudio& audio) const
{
    if (playing_.empty()) return -1.0;
    if (arrival_)
    {
        // Frames decoded, less what the speaker still holds of them.
        const double heard = std::max(0.0, audio.playPosition()) * voice::SOTTO_RATE;
        const double ahead = std::max(0.0, (double)appended_ - heard) / voice::SOTTO_SAMPLES;
        return std::max(0.0, arrival_->playedFrames() - ahead);
    }
    return fromFrame_ + audio.playPosition() * voice::FRAMES_PER_SECOND;
}
