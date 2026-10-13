#pragma once

// M0 hardware/toolchain probes. Results go to the log and to sdmc:/openmw/probe.txt.
// Returns the number of failed probes.
int runProbes(bool includeSdBenchmark);
