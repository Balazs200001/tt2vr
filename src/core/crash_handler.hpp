// core::CrashHandler (PLAN Phase 10): on an unhandled exception, writes <mod dir>\logs\crash_<date>_<time>.dmp
// (MiniDumpWriteDump) and one line with the exception and the faulting module + offset to tt2vr.log, then hands the
// exception to the filter it replaced (the game's CrashRpt), so the game's own crash behaviour is unchanged.
#pragma once

namespace core {

// Thread: game main thread, once the game has installed its own filter (first Present). Loads dbghelp now, not in
// the crash. Safe to call more than once.
void InstallCrashHandler();

// Test mode "crashtest": crashes the process from a new thread (to check the handler). Never returns normally.
void CrashForTest();

} // namespace core
