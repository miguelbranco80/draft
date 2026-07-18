// Small deterministic SHA-256 implementation for compiler content identities.
//
// Draft hashes attachment bytes, canonical interfaces, generated source, pins,
// evidence, and build inputs. This module owns only the standard byte-to-digest
// primitive; semantic framing and field ordering belong to each format's owner.
// It has no filesystem, source, syntax, or target dependency.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>

namespace draft {

struct Sha256Digest {
  std::array<std::uint8_t, 32> bytes{};

  bool operator==(const Sha256Digest &) const = default;
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] static std::optional<Sha256Digest> from_hex(
      std::string_view text);
};

class Sha256 {
public:
  Sha256();

  // update may be called with arbitrary chunk boundaries. finalize returns the
  // digest of all bytes supplied so far and leaves the object finalized; a
  // second finalize is an internal misuse guarded by an assertion.
  void update(std::span<const std::uint8_t> bytes);
  void update(std::string_view bytes);
  [[nodiscard]] Sha256Digest finalize();

private:
  void compress(const std::uint8_t *block);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> pending_{};
  std::size_t pending_size_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finalized_ = false;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> bytes);
[[nodiscard]] Sha256Digest sha256(std::string_view bytes);

} // namespace draft
