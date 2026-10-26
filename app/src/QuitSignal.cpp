#include "QuitSignal.h"

#include <csignal>

namespace
{

volatile std::sig_atomic_t asked = 0;

extern "C" void onQuitSignal(int signal)
{
    asked = signal;
}

} // namespace

void watchQuitSignals()
{
    std::signal(SIGTERM, onQuitSignal);
    std::signal(SIGINT, onQuitSignal);
#ifdef SIGHUP
    std::signal(SIGHUP, onQuitSignal);
#endif
}

int quitSignal()
{
    return asked;
}
