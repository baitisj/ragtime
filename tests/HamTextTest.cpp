// HamText, Ragtime's text table 0: every byte string comes back as it went
// in, common chat costs about five bits a character, and the table's codes
// are exactly Glissando's.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/text/HamText.h"
#include "ragtime/text/HamTextTable.h"

using namespace ragtime::text;

namespace
{

std::string hexOf(const std::vector<uint8_t>& bytes)
{
    std::string out;
    char two[3];
    for (uint8_t b : bytes)
    {
        std::snprintf(two, sizeof(two), "%02x", b);
        out += two;
    }
    return out;
}

void testRoundTrips()
{
    const std::string texts[] = {
        "",
        "CQ CQ de AG7EW AG7EW k",
        "Thanks for the nice report, your signal is very good here. 73",
        "name here is Jeff, qth Portland OR, rig IC-7100 at 50 w into a dipole",
        "~`{|}\\^_@[]",
        "caf\xc3\xa9 na\xc3\xafve \xe2\x80\x94 \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xf0\x9f\x93\xbb",
        std::string("tab\there, nul\0byte, cr\r\nlf", 27),
        std::string(1, '\xff'),
    };
    for (const std::string& text : texts)
    {
        std::vector<uint8_t> coded = HamText::encode(text);
        CHECK_EQ(HamText::decode(coded), text);
        CHECK_EQ(coded.size(), (HamText::bits(text) + 7) / 8);
    }

    // Every byte value, alone and in a run.
    std::string all;
    for (int b = 0; b < 256; b++)
    {
        std::string one(1, (char)b);
        CHECK_EQ(HamText::decode(HamText::encode(one)), one);
        all.push_back((char)b);
    }
    CHECK_EQ(HamText::decode(HamText::encode(all)), all);
}

void testPaddingAndTruncation()
{
    // Zero bytes after the text read as padding.
    std::vector<uint8_t> coded = HamText::encode("73 de W1AW");
    coded.push_back(0);
    coded.push_back(0);
    coded.push_back(0);
    CHECK_EQ(HamText::decode(coded), std::string("73 de W1AW"));

    // Cut short, it gives what was whole.
    std::vector<uint8_t> whole = HamText::encode("good morning");
    std::vector<uint8_t> cut(whole.begin(), whole.begin() + (long)whole.size() / 2);
    std::string part = HamText::decode(cut);
    CHECK(std::string("good morning").compare(0, part.size(), part) == 0);
    CHECK(part.size() < 12);
    CHECK_EQ(HamText::decode(nullptr, 0), std::string());
}

void testItSavesBits()
{
    // The corpus the table was built from: about five bits a character.
    std::ifstream in(RAGTIME_SOURCE_DIR "/tools/ham_table/chat_corpus.txt");
    CHECK(in.good());
    size_t characters = 0, bits = 0;
    std::string line;
    while (std::getline(in, line))
    {
        characters += line.size();
        bits += HamText::bits(line);
    }
    CHECK(characters > 1000);
    double perCharacter = (double)bits / (double)characters;
    std::printf("chat corpus: %.2f bits a character\n", perCharacter);
    CHECK(perCharacter < 5.5);

    // Common characters are short; a byte outside ASCII costs its escape.
    CHECK(HamText::characterBits(' ') <= 4);
    CHECK(HamText::characterBits('e') <= 5);
    CHECK(HamText::characterBits(0xC3) > 8);
}

void testPhrases()
{
    std::vector<HamText::PhraseSpan> spans = HamText::phraseSpans("CQ CQ the antenna");
    CHECK(!spans.empty());
    if (spans.empty()) return;
    CHECK(spans[0].start == 0 && spans[0].length == 5); // "CQ CQ"
    // " antenna" outranks " ant" and " an".
    bool antenna = false;
    for (const auto& span : spans) antenna = antenna || (span.start == 9 && span.length == 8);
    CHECK(antenna);
    // A phrase is cheaper than spelling it out.
    CHECK(HamText::bits("CQ CQ") < HamText::bits("C") * 4);
}

void testTheTableIsComplete()
{
    // A full prefix code (Kraft sum one), EMPTY the longest code.
    double kraft = 0;
    int longest = 0;
    for (int s = 0; s < SYMBOL_COUNT; s++)
    {
        kraft += std::ldexp(1.0, -SYMBOL_BITS[s]);
        longest = std::max(longest, (int)SYMBOL_BITS[s]);
    }
    CHECK(std::fabs(kraft - 1.0) < 1e-12);
    CHECK_EQ((int)SYMBOL_BITS[SYMBOL_COUNT - 1], longest);
    CHECK_EQ(PHRASE_COUNT, 42);
}

void testTableZeroIsFrozen()
{
    // Table 0 is a protocol constant: these bytes must never change. A new
    // table takes a new number (docs/PROTOCOL.md, "Text coding"). They are
    // what Glissando's own coder makes of the same text.
    CHECK_EQ(hexOf(HamText::encode("CQ CQ de AG7EW")), std::string("00c21d32450427c0"));
    CHECK_EQ(hexOf(HamText::encode("Thanks for the report, 73")), std::string("05048e0250f088"));
}

} // namespace

int main()
{
    testRoundTrips();
    testPaddingAndTruncation();
    testItSavesBits();
    testPhrases();
    testTheTableIsComplete();
    testTableZeroIsFrozen();
    return ragtime_test::finish();
}
