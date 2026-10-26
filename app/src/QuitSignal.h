#ifndef RAGTIME_QUIT_SIGNAL_H
#define RAGTIME_QUIT_SIGNAL_H

// SIGTERM, SIGINT and SIGHUP close Ragtime as its window's close box
// does, so what it owes others on the way out still happens: data2g-host's
// TX level put back (Auto ALC), the rig unkeyed, the Ear's filter put
// back, the logs saved. The handler only sets a flag; the main window's
// clock sees it within a second and closes.
void watchQuitSignals();
// The signal that asked Ragtime to quit, or 0.
int quitSignal();

#endif
