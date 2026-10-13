#pragma once

// M0 hardware/toolchain probes. Results go to the log and to sdmc:/openmw/probe.txt.
// Returns the number of failed probes.
// onProgress (optional) is called after every probe line, e.g. to refresh the bottom screen.
int runProbes(bool includeSdBenchmark, void (*onProgress)() = nullptr);
