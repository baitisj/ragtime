// The Broadcast tab's engine: messages on the D2G group, both ways
// (docs/PLAN.md, "Broadcast mode"; docs/PROTOCOL.md, "Broadcast").
//
// Typed text becomes Text records, numbered by the station's broadcast
// Numbering (kept across restarts by the app), packed into frames and
// handed to HostLink as one burst. Each message then shows Queued,
// Sending, Waiting for the session to go idle, Sent or Not sent from the
// host's ACKMODE reports, as HostLink settles its burst.
//
// - "@W1AW hello" addresses the message to W1AW and asks for an Ack. W1AW
//   answers on the group by itself, and the message shows Acknowledged.
// - A message too long for one frame goes as several Texts, split between
//   words, one frame each, all in one burst.
// - A Hello rides along in the first frame after the group opens, after
//   the station's settings change, and at most every 10 minutes after
//   that, so the station list learns locators without a burst of its own.
// - History: with a BroadcastLog, the tab opens with what was said before,
//   a rule where Ragtime last closed, and the log carries on below.
//
// No sockets and no clock: the time is passed in, and HostLink is the
// socket-free one the app feeds.
#ifndef RAGTIME_BROADCAST_BROADCAST_CHAT_H
#define RAGTIME_BROADCAST_BROADCAST_CHAT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"
#include "ragtime/text/HamText.h"

namespace ragtime
{
namespace broadcast
{

struct Message
{
    enum class State
    {
        Heard,          // someone else's
        Queued,         // ours, still with us (Pause sending, or waiting its turn)
        Sending,        // with the host, waiting for a clear channel
        Held,           // with the host, held by a session until it goes idle
        Sent,
        NotSent,        // the host dropped it, the link closed, or two minutes passed
        Cancelled,      // taken back before it went
        Acknowledged,   // addressed, and the station's Ack came back
    };

    // A row is a message, a broadcast file (a live card, or one line from
    // the log), or the rule drawn where Ragtime last closed.
    enum class Kind
    {
        Text,
        File,
        Rule,
    };

    uint64_t id = 0;            // this list's own number
    Kind kind = Kind::Text;
    bool ours = false;
    std::string from;
    std::string to;             // addressed to one station, or empty
    bool ackWanted = false;
    std::string text;           // the exact UTF-8
    uint32_t seq = 0;
    uint32_t time = 0;          // the record's, Unix seconds
    uint64_t atMs = 0;          // when we made or heard it
    State state = State::Heard;
    uint64_t burst = 0;         // ours: HostLink's burst id
    std::string mode;           // the mode it went (or is going) out in, when known
    // A broadcast file's card rather than a message: BroadcastFiles' id
    // for it, with text the file's name. 0 for a file line from the log.
    uint64_t file = 0;
    bool past = false;          // from before Ragtime last closed, read from the BroadcastLog
    std::string outcome;        // a file line from the log: how it ended, when known
};

// "Queued", "Sending", "Waiting for the session to go idle", "Sent",
// "Not sent", "Cancelled", "Acknowledged"; "" for one heard.
const char* stateLabel(Message::State state);

// How often a Hello rides along with our messages.
constexpr uint64_t HELLO_EVERY_MS = 10 * 60 * 1000;
// The longest message taken at once, in bytes: about a screenful, a few
// frames.
constexpr size_t MAX_TYPED = 2000;
// Messages kept in the list; the oldest go first.
constexpr size_t MAX_MESSAGES = 2000;

class BroadcastLog;

class BroadcastChat
{
public:
    struct Settings
    {
        std::string callsign;
        std::string locator;
        bool acceptsSessions = true;
        std::string software;       // "Ragtime 0.1.0"
    };

    explicit BroadcastChat(link::HostLink& link);

    // A change makes the next message carry a Hello. A new callsign starts
    // its own numbering at newestItem.
    void setSettings(const Settings& settings, uint32_t newestItem);
    const Settings& settings() const { return settings_; }

    // Where the chat is kept: what it holds already goes in front of
    // what this run says, which is added as it happens. None: the tab
    // starts empty.
    void setLog(BroadcastLog* log, uint64_t nowMs);
    // Ragtime is closing: the log's rule goes here.
    void closeLog(uint64_t nowMs);

    // The newest broadcast item's number, for the app to keep.
    uint32_t newestItem() const { return numbering_.newestItem(); }

    // A record of ours for the group, numbered by the broadcast numbering:
    // a FileAnnounce takes the next item number, a FilePiece its file's
    // (item), and anything else the newest item's.
    protocol::Record record(protocol::Body body, uint32_t time, uint32_t item = 0);

    // Told of every burst of our items the chat hands HostLink, first time
    // or again, so they can be sealed (broadcast::Seals).
    using ItemsFn = std::function<void(const std::vector<protocol::Record>& items, uint64_t burst,
                                       const std::string& mode, uint64_t nowMs)>;
    void onItems(ItemsFn fn) { onItems_ = std::move(fn); }

    // A broadcast file's card among the messages, ours or heard; file is
    // BroadcastFiles' id for it. Returns the card's message id.
    uint64_t addFile(uint64_t file, bool ours, const std::string& from, const std::string& name, uint64_t nowMs);
    // Something a file card shows has changed.
    void fileChanged() { version_++; }
    // A file card's file has ended, as outcome ("Saved", "Sent"), for the
    // log to show next time.
    void fileEnded(uint64_t file, const std::string& outcome);

    // "@W1AW" at the start of typed text: the station, and the text after
    // it. False when it isn't addressed.
    static bool addressee(const std::string& typed, std::string& call, std::string& rest);
    // The ham phrases send() will code as one symbol each, as spans of
    // typed (an "@W1AW" in front goes in the record's To, not its text),
    // for the entry line to show as they are typed.
    static std::vector<text::HamText::PhraseSpan> phrases(const std::string& typed);

    // Sends typed text in mode (empty: the group's mode as it stands).
    // Returns the messages it became; none for blank text, text over
    // MAX_TYPED, or no callsign set.
    std::vector<uint64_t> send(const std::string& typed, const std::string& mode, uint64_t nowMs);

    // A Hello of its own: "check in".
    bool checkIn(const std::string& mode, uint64_t nowMs);

    // Our session state on the group (docs/PLAN.md, "Busy stations"): a
    // QRL naming the partner on entering a session, a QRV on leaving it.
    // A QRV goes only after a QRL; a QRL still queued when the session ends
    // is taken back instead, and then no QRV is needed. False when nothing
    // went.
    bool announce(protocol::Status::State state, const std::string& partner, const std::string& mode,
                  uint64_t nowMs);

    // Takes back a message HostLink hasn't handed to the host yet (with the
    // rest of its burst). False once the host has it.
    bool cancel(uint64_t id);
    // Re-send: one of our messages goes again, once it has settled (Sent,
    // Not sent, Cancelled or Acknowledged). It goes as the same record, so
    // a station that heard it the first time doesn't list it twice, and an
    // addressed one is acknowledged again. False while it is still going.
    // One from the log goes again too, if it was sent under the callsign
    // set now.
    bool resend(uint64_t id, const std::string& mode, uint64_t nowMs);
    bool canResend(const Message& message) const;

    // Every HostLink event goes here: bursts settling, frames heard, the
    // group opening.
    // Returns the records heard in a frame (for the station list, and the
    // broadcast files to come), every one of them, ours to answer or not.
    std::vector<protocol::Record> onEvent(const link::HostLink::Event& event, uint64_t nowMs);

    const std::deque<Message>& messages() const { return messages_; }
    const Message* find(uint64_t id) const;
    // Goes up with every change, so a view knows when to redraw.
    uint64_t version() const { return version_; }
    // Frames heard that weren't Ragtime's, or broke off.
    size_t foreignFrames() const { return foreign_; }

private:
    using ItemKey = std::tuple<std::string, uint32_t, uint32_t>;  // call, seq, time

    void onRecord(const protocol::Record& record, uint64_t nowMs);
    Message* findMine(uint64_t id);
    protocol::Record helloRecord(uint32_t time);
    bool helloDue(uint64_t nowMs) const;
    // Splits text into pieces that each fit a frame beside `room` other
    // bytes, between words where it can.
    std::vector<std::string> split(const std::string& text, const std::string& to, size_t firstRoom);
    void trim();
    void flushAcks(const std::string& mode, uint64_t nowMs);
    void logState(const Message& m);

    link::HostLink& link_;
    ItemsFn onItems_;
    BroadcastLog* log_ = nullptr;
    Settings settings_;
    protocol::Numbering numbering_;
    std::deque<Message> messages_;
    std::map<uint64_t, protocol::Record> records_;  // ours, by message id, for resending
    std::set<ItemKey> seen_;
    std::set<uint64_t> ackBursts_;
    std::vector<protocol::Record> acksDue_;
    uint64_t nextId_ = 1;
    uint64_t version_ = 0;
    size_t foreign_ = 0;

    uint64_t qrlBurst_ = 0;         // our latest QRL's burst
    bool qrlOut_ = false;           // a QRL has gone (or is going) with no QRV since

    bool helloSent_ = false;        // since the group opened or the settings changed
    uint64_t helloMs_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
