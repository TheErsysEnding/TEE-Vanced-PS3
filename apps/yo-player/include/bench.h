#pragma once

// bench - (V3.1.0 Test 3) playback measured without anyone at the console. A job file, /dev_hdd0/tmp/TEE_Vanced/bench.txt,
// lists videos with a start, a length and a quality; at the next start the app plays them one after the other, writes one
// line of numbers per run to the log and to bench-results.txt beside it, and goes back to the XMB. Without a job file
// nothing changes. See bench.c for the file's format.

// main.c after initAccount: takes the job file if there is one (renamed bench.running at once, so a crash never repeats it)
void initBench(void);
// main.c, every frame before beginGfxFrame: starts, measures and ends the runs
void updateBench(void);
// a job is running: no welcome page, no update check, nothing saved about what was watched
int  isBenchActive(void);
// a global of a job file still waiting (bench.txt), read before initBench - main.c asks for log= before the log writer
// starts. 1 with the value in `value` (cut to cap - 1 bytes), 0 without a job or without that key
int  peekBenchGlobal(const char *key, char *value, int cap);
