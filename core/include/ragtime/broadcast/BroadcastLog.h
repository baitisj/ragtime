// The Broadcast tab's lasting log: what was said on D2G, kept on disk so
// the tab opens with what came before, greyed, a rule where Ragtime last
// closed.
//
// One file, appended to as things happen, one line each, fields split by
// tabs (text escaped as SessionLog's is):
//
//   ragtime broadcast log 1                the first line
//   B <ms>                                 Ragtime opened the tab
//   M <id> <atMs> <time> <seq> <ours> <state> <from> <to> <ack> <mode> <text> [<before>]
//   S <id> <state> <mode>                  a message of ours settled
//   F <id> <atMs> <ours> <from> <name>     a broadcast file's card
//   R <id> <outcome> [<path> <inline>]     how that file ended ("Saved"),
//                                          where it is, and p for a
//                                          picture, v for a voice message
//   X <from> <seq> <time> <call> <code>    call reacted to a line (its
//                                          sender, number and record time)
//   Y <from> <seq> <time> <code>           our reaction to a line went
//   E <ms>                                 Ragtime closed
//
// Times are Unix ms; time is the record's, in seconds. <id> is the row's
// id in that run (BroadcastChat's, which isn't kept across restarts, so
// it only names a row within its own B ... E). States are H, Q, G, W, S,
// N, C and A (Heard, Queued, Sending, Held, Sent, Not sent, Cancelled,
// Acknowledged). <before>, when there and not 0, is the row of the same
// run the message goes above (a line repaired, put in its place among
// its sender's). A reaction names its line as the record does, so it
// finds a line of an earlier run too; one whose line was cut away is
// skipped. Lines of kinds this reader doesn't know are skipped.
//
// Not thread safe. Disk errors are ignored: the chat goes on without its
// log rather than stop.
#ifndef RAGTIME_BROADCAST_BROADCAST_LOG_H
#define RAGTIME_BROADCAST_BROADCAST_LOG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/broadcast/BroadcastChat.h"

namespace ragtime
{
namespace broadcast
{

class BroadcastLog
{
public:
    // The file is cut back to its later half, at a run's start, once it
    // grows past this.
    static constexpr uint64_t MAX_FILE_BYTES = 1024 * 1024;

    // "" keeps nothing. Its folder is made when first written to.
    explicit BroadcastLog(std::string path = std::string());
    void setPath(const std::string& path) { path_ = path; }
    const std::string& path() const { return path_; }

    // What is kept, oldest first, the newest max rows: messages and file
    // lines marked past, and a Rule after each run that had rows (one cut
    // short, by a crash say, ends at its last row). A message of ours
    // still Queued, Sending or Held when Ragtime closed comes back Not
    // sent. Ids are 0, for the caller to give.
    std::vector<Message> load(size_t max) const;

    void began(uint64_t nowMs);
    // before: the id of the row it goes above, 0 for the end.
    void added(const Message& message, uint64_t before = 0);
    void settled(const Message& message);
    // A station's reaction to a line, and ours once it has gone.
    void reacted(const Message& line, const std::string& call, uint8_t code);
    void weReacted(const Message& line, uint8_t code);
    // path: where it was saved or sent from, "" if not kept; the card's
    // picture and voice say what it is.
    void fileEnded(const Message& card, const std::string& outcome, const std::string& path = std::string());
    void ended(uint64_t nowMs);

private:
    void append(const std::string& line);
    void compact();

    std::string path_;
};

// The log's text, parsed as load() does it, for tests.
std::vector<Message> parseBroadcastLog(const std::string& text, size_t max);

} // namespace broadcast
} // namespace ragtime

#endif
