#pragma once

// Binary persistence blob (see wallet_blob.cpp for the layout and the
// issue #15 background). The loader dispatches on the magic; JSON blobs
// keep loading via proofs_from_json for migration compatibility.

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "cashu.hpp"

namespace cashu {

bool blob_is_binary(const char *data, size_t len);

// Serialize proofs into the NPB1 binary form. Returns a heap buffer the
// caller frees (plain free()) — the binary form is ~half the JSON size,
// so it fits comfortably inside the largest free heap block at proof
// counts where the JSON form could not allocate.
char *proofs_to_bin_buf(const std::vector<Proof> &proofs, size_t *len);

// Parse the NPB1 binary form. Returns false on any malformed input
// (caller falls back to / errors out — never a partial parse).
bool proofs_from_blob(const char *data, size_t len, std::vector<Proof> &out);

} // namespace cashu
