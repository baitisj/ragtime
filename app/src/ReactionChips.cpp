#include "ReactionChips.h"

#include <cstddef>
#include <utility>

#include <wx/menu.h>

#include "Html.h"

using namespace ragtime::protocol;

wxString reactionChipsHtml(const std::vector<Reacted>& theirs, uint8_t mine, uint8_t waiting, const std::string& me,
                           bool canReact, bool canTakeBack, bool picked)
{
    std::vector<ReactionChip> chips = reactionChips(theirs, mine, waiting, me);
    if (chips.empty()) return wxString();
    wxString font = picked ? wxString() : " color=\"" + htmlColour(secondaryText()) + "\"";
    wxString html;
    for (const ReactionChip& chip : chips)
    {
        if (!html.empty()) html << "&nbsp;&nbsp;&nbsp; ";
        const ReactionEntry* entry = reactionEntry(chip.code);
        wxString shown = htmlEscape(reactionShown(chip.code));
        if (entry != nullptr && entry->word) shown = "<b>" + shown + "</b>";
        // Clicking adds ours; ours still waiting, clicking takes it back.
        bool link = canReact && chip.code != 0 && entry != nullptr && (!chip.mine || (chip.waiting && canTakeBack));
        html << (link ? htmlLink(wxString::Format("react:%u", (unsigned)chip.code), shown, picked) : shown);
        html << "&nbsp;<font size=\"-1\"" << font << ">";
        for (size_t i = 0; i < chip.calls.size(); i++)
        {
            if (i > 0) html << ", ";
            bool ours = chip.mine && chip.calls[i] == me;
            wxString call = htmlEscape(chip.calls[i]);
            if (ours) call = chip.waiting ? "<b><i>" + call + "</i></b>" : "<b>" + call + "</b>";
            html << call;
        }
        html << "</font>";
    }
    return html;
}

uint8_t reactionHref(const wxString& href)
{
    unsigned long code = 0;
    if (!href.StartsWith("react:") || !href.AfterFirst(':').ToULong(&code) || code == 0 || code > 255) return 0;
    return (uint8_t)code;
}

wxMenu* reactionMenu(int firstId, uint8_t current, bool canTakeBack, std::function<void(uint8_t code)> pick)
{
    auto* menu = new wxMenu;
    const auto& palette = reactionPalette();
    int id = firstId;
    bool words = false;
    for (const ReactionEntry& e : palette)
    {
        if (id >= firstId + REACTION_MENU_IDS - 1) break;
        // The ham words after the emoji, below a line.
        if (e.word && !words)
        {
            menu->AppendSeparator();
            words = true;
        }
        menu->AppendCheckItem(id, wxString::FromUTF8(e.shown) + "   " + wxString::FromUTF8(e.name));
        if (e.code == current) menu->Check(id, true);
        id++;
    }
    int takeBack = firstId + REACTION_MENU_IDS - 1;
    if (canTakeBack)
    {
        menu->AppendSeparator();
        menu->Append(takeBack, "Take back mine (not sent yet)");
    }
    menu->Bind(
        wxEVT_MENU,
        [firstId, takeBack, pick](wxCommandEvent& e) {
            if (e.GetId() == takeBack)
            {
                pick(0);
                return;
            }
            size_t index = (size_t)(e.GetId() - firstId);
            const auto& all = reactionPalette();
            if (index < all.size()) pick(all[index].code);
        },
        firstId, firstId + REACTION_MENU_IDS - 1);
    return menu;
}

std::string reactionNote(const std::string& from, uint8_t code, const std::string& line)
{
    const ReactionEntry* e = reactionEntry(code);
    std::string what = reactionShown(code);
    if (e != nullptr && !e->word) what += std::string(" (") + e->name + ")";
    std::string quoted = line;
    // Cut short between UTF-8 characters.
    if (line.size() > 40)
    {
        size_t at = 37;
        while (at > 0 && ((uint8_t)line[at] & 0xC0) == 0x80) at--;
        quoted = line.substr(0, at) + "...";
    }
    return from + " reacted " + what + " to your line: " + quoted;
}
