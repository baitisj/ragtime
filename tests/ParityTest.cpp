// Parity repairs: the Cauchy Reed-Solomon code rebuilds any lost data
// symbols from any as many parity symbols, whichever they are; it refuses
// what it can't do; and wantFor asks for enough to fill the gaps in one
// round most of the time.
#include <cstdint>
#include <algorithm>
#include <random>
#include <vector>

#include "Check.h"
#include "ragtime/files/Parity.h"

namespace parity = ragtime::files::parity;
using parity::Symbol;

namespace
{

std::vector<std::vector<uint8_t>> randomData(std::mt19937& rng, size_t n, size_t length)
{
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::vector<uint8_t>> data(n, std::vector<uint8_t>(length));
    for (auto& d : data)
        for (auto& b : d) b = (uint8_t)byte(rng);
    return data;
}

// Loses `lose` of n data symbols at random, makes `extra` parity symbols
// named from base up, and rebuilds the lost ones from the rest.
bool roundTrip(std::mt19937& rng, size_t n, size_t length, size_t lose, uint16_t base, size_t skipParity = 0)
{
    std::vector<std::vector<uint8_t>> data = randomData(rng, n, length);
    std::vector<Symbol> all;
    for (size_t i = 0; i < n; i++) all.push_back({(uint16_t)i, &data[i]});

    std::vector<std::vector<uint8_t>> par;
    for (size_t j = 0; j < lose + skipParity; j++) par.push_back(parity::parityOf((uint16_t)(base + j), all, length));

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<bool> lost(n, false);
    for (size_t i = 0; i < lose; i++) lost[order[i]] = true;

    std::vector<Symbol> known, parity;
    std::vector<uint16_t> missing;
    for (size_t i = 0; i < n; i++)
    {
        if (lost[i])
            missing.push_back((uint16_t)i);
        else
            known.push_back(all[i]);
    }
    // Parity symbols other than the first ones do as well.
    for (size_t j = skipParity; j < par.size(); j++) parity.push_back({(uint16_t)(base + j), &par[j]});

    std::vector<std::vector<uint8_t>> out;
    if (!parity::rebuild(known, parity, missing, length, out)) return false;
    if (out.size() != missing.size()) return false;
    for (size_t i = 0; i < missing.size(); i++)
    {
        if (out[i] != data[missing[i]]) return false;
    }
    return true;
}

void testRebuilds()
{
    std::mt19937 rng(7);
    CHECK(roundTrip(rng, 1, 2, 1, 1));
    CHECK(roundTrip(rng, 10, 8, 1, 10));
    CHECK(roundTrip(rng, 10, 8, 10, 10));     // every data symbol lost
    CHECK(roundTrip(rng, 30, 64, 7, 30, 5));  // later parity symbols, not the first
    CHECK(roundTrip(rng, 200, 220, 120, 200));
    // Items in a seal: places 0..254, parity named from 256.
    CHECK(roundTrip(rng, 20, 40, 4, 256));
    // A big file: many pieces, parity near the top of the names.
    CHECK(roundTrip(rng, 1400, 32, 60, 65000 - 60));
}

void testShortSymbols()
{
    // A short symbol counts as padded with zeros (a file's last piece).
    std::vector<uint8_t> a = {1, 2, 3, 4}, b = {5, 6, 7};
    std::vector<uint8_t> bPadded = {5, 6, 7, 0};
    std::vector<Symbol> data = {{0, &a}, {1, &b}};
    std::vector<Symbol> padded = {{0, &a}, {1, &bPadded}};
    std::vector<uint8_t> p = parity::parityOf(2, data, 4);
    CHECK(p == parity::parityOf(2, padded, 4));
    std::vector<std::vector<uint8_t>> out;
    std::vector<Symbol> par = {{2, &p}};
    CHECK(parity::rebuild({{0, &a}}, par, {1}, 4, out));
    CHECK(out.size() == 1 && out[0] == bPadded);
}

void testRefusals()
{
    std::vector<uint8_t> a = {1, 2}, b = {3, 4};
    std::vector<Symbol> data = {{0, &a}, {1, &b}};
    CHECK(parity::parityOf(1, data, 2).empty());    // a data symbol's name
    CHECK(parity::parityOf(5, data, 3).empty());    // odd length
    std::vector<uint8_t> p = parity::parityOf(5, data, 2);
    std::vector<std::vector<uint8_t>> out;
    std::vector<Symbol> par = {{5, &p}};
    CHECK(!parity::rebuild({}, par, {0, 1}, 2, out));               // too few parity symbols
    CHECK(!parity::rebuild({{0, &a}}, {{0, &p}}, {1}, 2, out));     // a parity name taken
    CHECK(!parity::rebuild({{0, &a}}, par, {0}, 2, out));           // missing and known at once
    CHECK(parity::rebuild({{0, &a}, {1, &b}}, {}, {}, 2, out) && out.empty());  // nothing to do
}

void testWant()
{
    CHECK_EQ(parity::wantFor(0, 0.5), (uint32_t)0);
    CHECK_EQ(parity::wantFor(10, 0.0), (uint32_t)10);
    CHECK_EQ(parity::wantFor(10, 0.5), (uint32_t)27);
    CHECK_EQ(parity::wantFor(10, 0.1), (uint32_t)13);
    // Held at 75% loss, so a station that heard nothing doesn't ask for everything forever.
    CHECK_EQ(parity::wantFor(10, 0.99), parity::wantFor(10, 0.75));
    CHECK_EQ(parity::wantFor(60000, 0.75), (uint32_t)65535);

    // One round of what it asks fills every gap about nine times in ten.
    std::mt19937 rng(3);
    for (double loss : {0.1, 0.3, 0.5, 0.7})
    {
        uint32_t need = 20, want = parity::wantFor(need, loss);
        int filled = 0, trials = 2000;
        std::bernoulli_distribution heard(1.0 - loss);
        for (int t = 0; t < trials; t++)
        {
            uint32_t got = 0;
            for (uint32_t i = 0; i < want; i++) got += heard(rng) ? 1 : 0;
            if (got >= need) filled++;
        }
        CHECK(filled > trials * 8 / 10);
    }
}

} // namespace

int main()
{
    testRebuilds();
    testShortSymbols();
    testRefusals();
    testWant();
    return ragtime_test::finish();
}
