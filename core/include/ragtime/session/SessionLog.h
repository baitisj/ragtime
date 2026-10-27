// The Session tab's lasting log: what was said with each station, kept on
// disk so a later session with it carries on where the last one ended.
//
// One file per station in the folder given, named for its callsign
// ("W1AW.log", "TEST1_P.log"), appended to as things happen, one line
// each, fields split by tabs:
//
//   ragtime session log 1                 the first line
//   B <ms>                                a session began
//   M <id> <atMs> <time> <seq> <ours> <state> <from> <text>
//   S <id> <state>                        a message of ours settled
//   F <atMs> <ours> <from> <text>         a file that went, as one line
//   X <id> <call> <code>                  call reacted to a message
//   Y <id> <code>                         we reacted to a message
//   E <ms>                                the session ended
//
// Times are Unix ms; time is the record's, in seconds. <id> is the
// message's id in that session (SessionChat's, which isn't kept across
// restarts, so it only names a message within its own B ... E). States
// are H, Q, S, D and N (Heard, Queued, Sent, Delivered, Not delivered).
// Text escapes backslash, tab, CR and LF as \\, \t, \r and \n. Lines of
// kinds this reader doesn't know are skipped.
//
// Not thread safe. Disk errors are ignored: the chat goes on without its
// log rather than stop.
#ifndef RAGTIME_SESSION_SESSION_LOG_H
#define RAGTIME_SESSION_SESSION_LOG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/session/SessionChat.h"

namespace ragtime
{
namespace session
{

class SessionLog
{
public:
    // A station's file is cut back to its later half, at a session's
    // start, once it grows past this.
    static constexpr uint64_t MAX_FILE_BYTES = 1024 * 1024;

    // "" keeps nothing. The folder is made when first written to.
    explicit SessionLog(std::string folder = std::string());
    void setFolder(const std::string& folder) { folder_ = folder; }
    const std::string& folder() const { return folder_; }

    // The station's file: "" with no folder or no callsign.
    std::string pathFor(const std::string& peer) const;

    // What is kept for the station, oldest first, the newest max rows:
    // messages and files marked past, and a Rule after each session that
    // ended (one cut short, by a crash say, ends at its last row). A
    // message of ours still Sent or Queued when its session ended comes
    // back Not delivered. Ids are 0, for the caller to give.
    std::vector<Message> load(const std::string& peer, size_t max) const;

    void began(const std::string& peer, uint64_t nowMs);
    void added(const std::string& peer, const Message& message);
    void settled(const std::string& peer, const Message& message);
    // A reaction to a message of this session: the far end's, or ours.
    void reacted(const std::string& peer, const Message& message, const std::string& call, uint8_t code);
    void weReacted(const std::string& peer, const Message& message, uint8_t code);
    void file(const std::string& peer, const Message& note);
    void ended(const std::string& peer, uint64_t nowMs);

private:
    void append(const std::string& peer, const std::string& line);
    void compact(const std::string& peer);

    std::string folder_;
};

// The log's text, parsed as load() does it, for tests.
std::vector<Message> parseSessionLog(const std::string& text, size_t max);
// "W1AW", "TEST1_P": a callsign made safe for a file name.
std::string sessionLogName(const std::string& callsign);

} // namespace session
} // namespace ragtime

#endif
