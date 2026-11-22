#pragma once

// language-screen (V3.1.0) - the language picker: every language with its flag and its own name for itself, four a row.
// The first start shows it before anything else (no way back but choosing; English is where the cursor starts);
// Settings > Language opens it too. The title and the hints are in the language under the cursor, so someone who reads
// no English finds theirs.
void openLanguagePicker(int firstStart);
