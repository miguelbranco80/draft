// Arbitrary-precision signed integers for Draft compile-time semantics.
//
// BigInteger stores a sign and little-endian base-2^32 magnitude limbs. Zero is
// canonical: it has no limbs and sign zero. Every mutating private operation
// normalizes back to that representation. Public arithmetic returns new values,
// keeping constant-evaluator control flow explicit and avoiding hidden global
// allocation state.
//
// Division truncates toward zero and gives the remainder the dividend's sign.
// Bitwise operations and right shift implement Draft's infinite two's-complement
// rules, not the host C++ rules for any fixed integer width. Algorithms favor
// clarity over asymptotic sophistication; compile-time resource limits remain a
// separate evaluator policy and can reject unreasonable source operations.
//
// Relevant specification: 01-core-language.md, "Expressions and evaluation"
// and "Constants and compile-time evaluation".

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

class BigInteger {
public:
  BigInteger() = default;
  bool operator==(const BigInteger &) const = default;

  [[nodiscard]] static BigInteger from_i64(std::int64_t value);
  [[nodiscard]] static BigInteger from_u64(std::uint64_t value);

  // Parses a source integer spelling, including 0b/0o/0x and separating
  // underscores. A leading sign is intentionally absent from Draft literals and
  // is represented by the unary expression around the token.
  [[nodiscard]] static std::optional<BigInteger> parse_literal(std::string_view spelling);

  [[nodiscard]] bool is_zero() const;
  [[nodiscard]] bool is_negative() const;
  [[nodiscard]] int sign() const;
  [[nodiscard]] std::size_t bit_count() const;
  [[nodiscard]] BigInteger absolute() const;
  [[nodiscard]] BigInteger negated() const;

  // compare returns -1, 0, or 1 according to mathematical signed ordering.
  [[nodiscard]] int compare(const BigInteger &other) const;
  [[nodiscard]] BigInteger added(const BigInteger &other) const;
  [[nodiscard]] BigInteger subtracted(const BigInteger &other) const;
  [[nodiscard]] BigInteger multiplied(const BigInteger &other) const;

  // Returns false only for division by zero. quotient and remainder may alias
  // neither input; callers provide distinct outputs to keep ownership obvious.
  [[nodiscard]] bool divide(
      const BigInteger &divisor,
      BigInteger &quotient,
      BigInteger &remainder) const;

  [[nodiscard]] BigInteger shifted_left(std::size_t count) const;
  [[nodiscard]] BigInteger shifted_right(std::size_t count) const;
  [[nodiscard]] BigInteger bitwise_and(const BigInteger &other) const;
  [[nodiscard]] BigInteger bitwise_or(const BigInteger &other) const;
  [[nodiscard]] BigInteger bitwise_xor(const BigInteger &other) const;
  [[nodiscard]] BigInteger bitwise_not() const;

  [[nodiscard]] std::optional<std::uint64_t> to_u64() const;
  [[nodiscard]] std::optional<std::int64_t> to_i64() const;
  [[nodiscard]] std::string to_decimal() const;

private:
  explicit BigInteger(int sign, std::vector<std::uint32_t> limbs);

  void normalize();
  [[nodiscard]] static int compare_magnitude(
      const BigInteger &left, const BigInteger &right);
  [[nodiscard]] static std::vector<std::uint32_t> add_magnitude(
      const std::vector<std::uint32_t> &left,
      const std::vector<std::uint32_t> &right);
  [[nodiscard]] static std::vector<std::uint32_t> subtract_magnitude(
      const std::vector<std::uint32_t> &left,
      const std::vector<std::uint32_t> &right);
  void multiply_small(std::uint32_t factor);
  void add_small(std::uint32_t value);
  [[nodiscard]] bool magnitude_bit(std::size_t index) const;
  void set_magnitude_bit(std::size_t index);
  [[nodiscard]] bool has_discarded_bits(std::size_t count) const;
  [[nodiscard]] std::vector<std::uint32_t> twos_complement(std::size_t limbs) const;
  [[nodiscard]] static BigInteger from_twos_complement(
      std::vector<std::uint32_t> limbs);

  int sign_ = 0;
  std::vector<std::uint32_t> limbs_;
};

// ExactRational is the compile-time domain of decimal floating literals. The
// denominator is always positive and nonzero; construction normalizes by the
// greatest common divisor. This representation has no NaN or infinity because
// untyped source decimals and their exact arithmetic are mathematical rationals.
class ExactRational {
public:
  ExactRational();
  explicit ExactRational(BigInteger integer);
  bool operator==(const ExactRational &) const = default;

  [[nodiscard]] static std::optional<ExactRational> parse_decimal(
      std::string_view spelling);
  [[nodiscard]] static std::optional<ExactRational> from_fraction(
      BigInteger numerator, BigInteger denominator);

  [[nodiscard]] const BigInteger &numerator() const;
  [[nodiscard]] const BigInteger &denominator() const;
  [[nodiscard]] bool is_zero() const;
  [[nodiscard]] int compare(const ExactRational &other) const;
  [[nodiscard]] ExactRational added(const ExactRational &other) const;
  [[nodiscard]] ExactRational subtracted(const ExactRational &other) const;
  [[nodiscard]] ExactRational multiplied(const ExactRational &other) const;
  [[nodiscard]] bool divide(const ExactRational &other, ExactRational &result) const;
  [[nodiscard]] ExactRational negated() const;
  [[nodiscard]] std::string to_fraction() const;

private:
  ExactRational(BigInteger numerator, BigInteger denominator);
  void normalize();

  BigInteger numerator_;
  BigInteger denominator_ = BigInteger::from_u64(1);
};

} // namespace draft
