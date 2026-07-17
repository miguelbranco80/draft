// FIPS 180-4 SHA-256 compression and padding in direct C++.

#include "base/sha256.h"

#include <algorithm>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace draft {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] std::uint32_t read_be32(const std::uint8_t *bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
      (static_cast<std::uint32_t>(bytes[1]) << 16U) |
      (static_cast<std::uint32_t>(bytes[2]) << 8U) |
      static_cast<std::uint32_t>(bytes[3]);
}

void write_be32(std::uint32_t value, std::uint8_t *bytes) {
  bytes[0] = static_cast<std::uint8_t>(value >> 24U);
  bytes[1] = static_cast<std::uint8_t>(value >> 16U);
  bytes[2] = static_cast<std::uint8_t>(value >> 8U);
  bytes[3] = static_cast<std::uint8_t>(value);
}

} // namespace

std::string Sha256Digest::hex() const {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

Sha256::Sha256()
    : state_{
          0x6a09e667U,
          0xbb67ae85U,
          0x3c6ef372U,
          0xa54ff53aU,
          0x510e527fU,
          0x9b05688cU,
          0x1f83d9abU,
          0x5be0cd19U,
      } {}

void Sha256::compress(const std::uint8_t *block) {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = read_be32(block + index * 4);
  }
  for (std::size_t index = 16; index < schedule.size(); ++index) {
    const std::uint32_t first = std::rotr(schedule[index - 15], 7) ^
        std::rotr(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3U);
    const std::uint32_t second = std::rotr(schedule[index - 2], 17) ^
        std::rotr(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10U);
    schedule[index] = schedule[index - 16] + first + schedule[index - 7] + second;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t sigma_a =
        std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t sigma_e =
        std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t first = h + sigma_e + choose + kRoundConstants[index] +
        schedule[index];
    const std::uint32_t second = sigma_a + majority;
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> bytes) {
  assert(!finalized_);
  assert(total_bytes_ <= std::numeric_limits<std::uint64_t>::max() - bytes.size());
  total_bytes_ += static_cast<std::uint64_t>(bytes.size());
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t copy_count = std::min(
        pending_.size() - pending_size_, bytes.size() - offset);
    std::copy_n(bytes.data() + offset, copy_count, pending_.data() + pending_size_);
    pending_size_ += copy_count;
    offset += copy_count;
    if (pending_size_ == pending_.size()) {
      compress(pending_.data());
      pending_size_ = 0;
    }
  }
}

void Sha256::update(std::string_view bytes) {
  update(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
}

Sha256Digest Sha256::finalize() {
  assert(!finalized_);
  finalized_ = true;
  const std::uint64_t bit_count = total_bytes_ * 8U;
  pending_[pending_size_++] = 0x80U;
  if (pending_size_ > 56) {
    std::fill(pending_.begin() + static_cast<std::ptrdiff_t>(pending_size_), pending_.end(), 0);
    compress(pending_.data());
    pending_size_ = 0;
  }
  std::fill(
      pending_.begin() + static_cast<std::ptrdiff_t>(pending_size_),
      pending_.begin() + 56,
      0);
  for (std::size_t index = 0; index < 8; ++index) {
    pending_[63 - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
  }
  compress(pending_.data());

  Sha256Digest digest;
  for (std::size_t index = 0; index < state_.size(); ++index) {
    write_be32(state_[index], digest.bytes.data() + index * 4);
  }
  return digest;
}

Sha256Digest sha256(std::span<const std::uint8_t> bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.finalize();
}

Sha256Digest sha256(std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.finalize();
}

} // namespace draft
