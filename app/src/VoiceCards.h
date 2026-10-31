#ifndef RAGTIME_VOICE_CARDS_H
#define RAGTIME_VOICE_CARDS_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/string.h>

#include "ragtime/voice/ArrivalPlayer.h"
#include "ragtime/voice/Voice.h"

class wxWindow;

namespace ragtime
{
namespace audio
{
class VoiceAudio;
}
} // namespace ragtime

// Voice cards (docs/STAGE1.md, "Voice cards"): a voice message among the
// messages, on both tabs, as a play button and a pitch strip. The strip is
// the clip's melody (pitch as height, loudness as thickness, silence as a
// gap) drawn straight from the frames' bits with no decoding, so a hundred
// cost nothing; while a clip arrives it fills from the left, pieces lost
// hatched; while it plays a playhead runs along it, and a click on it
// plays from there.
//
// A card's links (an image map over the button and the strip):
//   voice:play:0       the button, while it isn't playing
//   voice:stop:0       the button, while it plays
//   voice:seek:<frame> the strip, playing from that frame
// Pages hand these on with the card's key.

// The shelf: each clip's bytes as they stand, read with voice::readSot.
// Keys as PictureShelf's: "b<id>" a broadcast file, "s<id>" a session
// transfer, "p<path>" one on disk (a card from the log).
class VoiceShelf
{
public:
    struct Shelved
    {
        ragtime::voice::Clip clip;
        ragtime::voice::ReadResult result = ragtime::voice::ReadResult::Short;
        uint64_t have = 0;      // bytes read from
        uint64_t size = 0;      // of this many in all
        size_t holes = 0;       // byte ranges missing
        bool final = false;     // ended, whole or not: won't grow
        int version = 0;        // moves each time it is read anew

        bool playable() const { return result == ragtime::voice::ReadResult::Ok && !clip.frames.empty(); }
    };

    // A clip's bytes as they stand: bytes is the file as far as have (a
    // session's, in order) or, for D2G, its whole size with the holes in
    // missing. Read again only if it changed. True if what shows changed.
    bool update(const std::string& key, const std::vector<uint8_t>& bytes, uint64_t size, uint64_t have,
                const std::vector<ragtime::voice::ByteRange>& missing, bool final);
    // A clip on disk, read once under "p" + path. False if it can't be
    // read (gone, or not a voice message this Ragtime plays).
    bool shelveFile(const std::string& path);
    // Final and read: nothing more will come.
    bool settled(const std::string& key) const;
    const Shelved* find(const std::string& key) const;
    void forget(const std::string& key);

    // How a card looks now: playing (the button a stop square), paused by
    // Push to talk, and the playhead's frame; waiting its turn in the
    // queue.
    struct Look
    {
        bool playing = false;
        bool paused = false;
        bool waiting = false;
        double playheadFrame = -1.0;    // < 0: none
        // Playing as it arrives, but waiting for the bytes to keep ahead:
        // about how long until it starts, in seconds (0: any moment), or
        // < 0 if not (docs/STAGE1.md, "Voice while it arrives").
        double startsIn = -1.0;     // infinite: can't tell yet
        // A session clip coming to us: the stop sign after the strip,
        // linked here (FileCancel 0x84, keeping what came).
        wxString stopHref;
    };
    // The card's button and strip, then its length ("0:12") and, while it
    // arrives, how much has come; a line saying why while its header hasn't.
    // It starts a line of its own: the caller puts no <br> before it.
    wxString cardHtml(const std::string& key, const wxWindow* window, const Look& look, bool picked);

private:
    struct Entry
    {
        Shelved shelved;
        std::string memoryName;     // its picture in wxHTML's memory files
        int drawnVersion = -1;
        int drawnPx = 0;
        bool drawnPlaying = false;
        bool drawnPaused = false;
        int drawnHead = -2;         // the playhead's pixel
        bool drawnDark = false;
        std::vector<uint8_t> lastBytes;
        std::vector<ragtime::voice::ByteRange> lastMissing;
    };
    std::map<std::string, Entry> entries_;
    int nextName_ = 1;
};

// "0:12", "1:05", "12:30".
std::string clipLength(double seconds);

// Plays a clip: decodes it with libsotto (voice::Sotto) and hands the
// sound to the program's one VoiceAudio, the deck's too. Decoding is quick
// (sotto's synthesis runs at a thousandth of real time), so it happens on
// the spot. One clip at a time; play replaces what plays.
class VoicePlayer
{
public:
    // done comes on the UI thread (posted through post), with the key
    // played and whether it was heard to its end.
    using Done = std::function<void(const std::string& key, bool played)>;
    using Post = std::function<void(std::function<void()>)>;

    VoicePlayer(Post post, Done done)
        : post_(std::move(post))
        , done_(std::move(done))
    {
    }

    // False, with why, when there's no libsotto to decode with.
    static bool canDecode(std::string& why);

    // Decodes clip and plays it from fromFrame on audio. False, with why,
    // if it can't (no libsotto, an empty clip).
    bool play(ragtime::audio::VoiceAudio& audio, const std::string& key, const ragtime::voice::Clip& clip,
              uint32_t fromFrame, std::string& why);
    // A session clip still arriving (docs/STAGE1.md, "Voice while it
    // arrives"): plays from its bytes as they stand, starting once they
    // will keep ahead of the speech, and fed more by arriving(). The
    // session's mode gives a first guess at the rate (bytes a second) and
    // the gap between its bursts (seconds), until arrivals show them.
    bool playArriving(ragtime::audio::VoiceAudio& audio, const std::string& key, const std::vector<uint8_t>& bytes,
                      uint64_t size, uint32_t fromFrame, double expectedRate, double expectedGap, uint64_t nowMs,
                      std::string& why);
    // More of the clip playing as it arrives (the same bytes, longer), and
    // whether the transfer has ended: whole, or short (the stop sign, a
    // cancel, the link gone), when what came plays out and fades.
    void arriving(const std::string& key, const std::vector<uint8_t>& bytes, uint64_t size, bool ended,
                  uint64_t nowMs);
    // Hands the speaker the sound now due, a little ahead; call often
    // (each of the window's voice ticks) while a clip plays as it arrives.
    void tick(ragtime::audio::VoiceAudio& audio, uint64_t nowMs);
    // The clip playing is one still arriving.
    bool isArriving() const { return arrival_ != nullptr; }
    // Playing as it arrives but not started (or run dry): about how long
    // until it plays, as Look::startsIn has it.
    double startsIn(uint64_t nowMs) const;
    // Stops at once; done(key, false) follows.
    void stop(ragtime::audio::VoiceAudio& audio);
    // The clip playing ("" if none) and how far into it, in frames.
    const std::string& playing() const { return playing_; }
    double positionFrames(const ragtime::audio::VoiceAudio& audio) const;

private:
    Post post_;
    Done done_;
    std::string playing_;
    uint32_t fromFrame_ = 0;
    uint64_t generation_ = 0;
    std::shared_ptr<uint64_t> current_ = std::make_shared<uint64_t>(0);
    // Playing as it arrives: the engine (its decoder in its calls), and
    // the samples handed to the speaker so far.
    std::unique_ptr<ragtime::voice::ArrivalPlayer> arrival_;
    uint64_t appended_ = 0;
    bool streamEnded_ = false;
    // Starts a clip on the speaker; done when it has been heard.
    std::function<void(bool)> finisher(const std::string& key, uint64_t generation);
};

#endif
