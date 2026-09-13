#pragma once

#include <cstddef>
#include <string>

// Board-agnostic token redemption shared by both NFC frontends.
//
// Returns:
//   1  on online success (swapped immediately)
//   0  on offline success (stashed for later drain)
//  -1  on failure
// `expected_unit` is what our payment flow asked for — a different unit
// is accepted (receive() enforces internal consistency) but logged.
// `desc`/`desc_len` receive the formatted received amount for the display.
int nfc_redeem_or_stash(const std::string &token_str,
                        const char *expected_unit,
                        char *desc, size_t desc_len);
