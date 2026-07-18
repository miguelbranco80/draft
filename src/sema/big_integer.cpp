// Direct arbitrary-precision integer and exact-rational implementation.

#include "sema/big_integer.h"

#include <algorithm>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace draft {
namespace {

constexpr std::uint64_t kLimbBase = std::uint64_t{1} << 32U;

[[nodiscard]] std::uint32_t digit_value(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint32_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint32_t>(character - 'a') + 10U;
  }
  if (character >= 'A' && character <= 'F') {
    return static_cast<std::uint32_t>(character - 'A') + 10U;
  }
  return std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] BigInteger power_of_ten(std::size_t exponent) {
  BigInteger result = BigInteger::from_u64(1);
  const BigInteger ten = BigInteger::from_u64(10);
  for (std::size_t index = 0; index < exponent; ++index) {
    result = result.multiplied(ten);
  }
  return result;
}

[[nodiscard]] BigInteger greatest_common_divisor(BigInteger left, BigInteger right) {
  left = left.absolute();
  right = right.absolute();
  while (!right.is_zero()) {
    BigInteger quotient;
    BigInteger remainder;
    const bool divided = left.divide(right, quotient, remainder);
    assert(divided);
    left = std::move(right);
    right = remainder.absolute();
  }
  return left;
}

} // namespace

BigInteger::BigInteger(int sign, std::vector<std::uint32_t> limbs)
    : sign_(sign), limbs_(std::move(limbs)) {
  normalize();
}

BigInteger BigInteger::from_i64(std::int64_t value) {
  if (value >= 0) {
    return from_u64(static_cast<std::uint64_t>(value));
  }
  // Computing the magnitude through -(value + 1) avoids overflowing at INT64_MIN.
  const std::uint64_t magnitude =
      static_cast<std::uint64_t>(-(value + 1)) + 1U;
  BigInteger result = from_u64(magnitude);
  result.sign_ = -1;
  return result;
}

BigInteger BigInteger::from_u64(std::uint64_t value) {
  if (value == 0) {
    return {};
  }
  std::vector<std::uint32_t> limbs;
  limbs.push_back(static_cast<std::uint32_t>(value));
  const std::uint32_t high = static_cast<std::uint32_t>(value >> 32U);
  if (high != 0) {
    limbs.push_back(high);
  }
  return BigInteger(1, std::move(limbs));
}

std::optional<BigInteger> BigInteger::parse_literal(std::string_view spelling) {
  std::size_t index = 0;
  std::uint32_t base = 10;
  if (spelling.size() >= 2 && spelling[0] == '0') {
    if (spelling[1] == 'b' || spelling[1] == 'B') base = 2;
    if (spelling[1] == 'o' || spelling[1] == 'O') base = 8;
    if (spelling[1] == 'x' || spelling[1] == 'X') base = 16;
    if (base != 10) index = 2;
  }

  BigInteger result;
  bool saw_digit = false;
  for (; index < spelling.size(); ++index) {
    const char character = spelling[index];
    if (character == '_') {
      continue;
    }
    const std::uint32_t digit = digit_value(character);
    if (digit >= base) {
      return std::nullopt;
    }
    result.multiply_small(base);
    result.add_small(digit);
    saw_digit = true;
  }
  if (!saw_digit) {
    return std::nullopt;
  }
  return result;
}

bool BigInteger::is_zero() const {
  return sign_ == 0;
}

bool BigInteger::is_negative() const {
  return sign_ < 0;
}

int BigInteger::sign() const {
  return sign_;
}

std::size_t BigInteger::bit_count() const {
  if (limbs_.empty()) {
    return 0;
  }
  const std::uint32_t high = limbs_.back();
  const std::uint32_t leading_zeroes =
      static_cast<std::uint32_t>(std::countl_zero(high));
  return (limbs_.size() - 1) * 32U +
      static_cast<std::size_t>(32U - leading_zeroes);
}

BigInteger BigInteger::absolute() const {
  BigInteger result = *this;
  if (!result.is_zero()) {
    result.sign_ = 1;
  }
  return result;
}

BigInteger BigInteger::negated() const {
  BigInteger result = *this;
  result.sign_ = -result.sign_;
  return result;
}

void BigInteger::normalize() {
  while (!limbs_.empty() && limbs_.back() == 0) {
    limbs_.pop_back();
  }
  if (limbs_.empty()) {
    sign_ = 0;
  } else if (sign_ == 0) {
    sign_ = 1;
  }
}

int BigInteger::compare_magnitude(const BigInteger &left, const BigInteger &right) {
  if (left.limbs_.size() != right.limbs_.size()) {
    return left.limbs_.size() < right.limbs_.size() ? -1 : 1;
  }
  for (std::size_t remaining = left.limbs_.size(); remaining > 0; --remaining) {
    const std::size_t index = remaining - 1;
    if (left.limbs_[index] != right.limbs_[index]) {
      return left.limbs_[index] < right.limbs_[index] ? -1 : 1;
    }
  }
  return 0;
}

int BigInteger::compare(const BigInteger &other) const {
  if (sign_ != other.sign_) {
    return sign_ < other.sign_ ? -1 : 1;
  }
  if (sign_ == 0) {
    return 0;
  }
  const int magnitude = compare_magnitude(*this, other);
  return sign_ > 0 ? magnitude : -magnitude;
}

std::vector<std::uint32_t> BigInteger::add_magnitude(
    const std::vector<std::uint32_t> &left,
    const std::vector<std::uint32_t> &right) {
  const std::size_t count = std::max(left.size(), right.size());
  std::vector<std::uint32_t> result(count, 0);
  std::uint64_t carry = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t left_limb = index < left.size() ? left[index] : 0;
    const std::uint64_t right_limb = index < right.size() ? right[index] : 0;
    const std::uint64_t sum = left_limb + right_limb + carry;
    result[index] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }
  if (carry != 0) {
    result.push_back(static_cast<std::uint32_t>(carry));
  }
  return result;
}

std::vector<std::uint32_t> BigInteger::subtract_magnitude(
    const std::vector<std::uint32_t> &left,
    const std::vector<std::uint32_t> &right) {
  // Callers establish left >= right. Borrow is represented in base 2^32 so no
  // signed host overflow participates in the arithmetic.
  std::vector<std::uint32_t> result(left.size(), 0);
  std::uint64_t borrow = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const std::uint64_t left_limb = left[index];
    const std::uint64_t right_limb = index < right.size() ? right[index] : 0;
    const std::uint64_t subtrahend = right_limb + borrow;
    if (left_limb < subtrahend) {
      result[index] = static_cast<std::uint32_t>(kLimbBase + left_limb - subtrahend);
      borrow = 1;
    } else {
      result[index] = static_cast<std::uint32_t>(left_limb - subtrahend);
      borrow = 0;
    }
  }
  assert(borrow == 0);
  return result;
}

BigInteger BigInteger::added(const BigInteger &other) const {
  if (is_zero()) return other;
  if (other.is_zero()) return *this;
  if (sign_ == other.sign_) {
    return BigInteger(sign_, add_magnitude(limbs_, other.limbs_));
  }
  const int order = compare_magnitude(*this, other);
  if (order == 0) return {};
  if (order > 0) {
    return BigInteger(sign_, subtract_magnitude(limbs_, other.limbs_));
  }
  return BigInteger(other.sign_, subtract_magnitude(other.limbs_, limbs_));
}

BigInteger BigInteger::subtracted(const BigInteger &other) const {
  return added(other.negated());
}

BigInteger BigInteger::multiplied(const BigInteger &other) const {
  if (is_zero() || other.is_zero()) {
    return {};
  }
  std::vector<std::uint32_t> result(limbs_.size() + other.limbs_.size(), 0);
  for (std::size_t left_index = 0; left_index < limbs_.size(); ++left_index) {
    std::uint64_t carry = 0;
    for (std::size_t right_index = 0; right_index < other.limbs_.size(); ++right_index) {
      const std::size_t output = left_index + right_index;
      const std::uint64_t product =
          static_cast<std::uint64_t>(limbs_[left_index]) * other.limbs_[right_index] +
          result[output] + carry;
      result[output] = static_cast<std::uint32_t>(product);
      carry = product >> 32U;
    }
    std::size_t output = left_index + other.limbs_.size();
    while (carry != 0) {
      const std::uint64_t sum = static_cast<std::uint64_t>(result[output]) + carry;
      result[output] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32U;
      ++output;
      if (carry != 0 && output == result.size()) {
        result.push_back(0);
      }
    }
  }
  return BigInteger(sign_ == other.sign_ ? 1 : -1, std::move(result));
}

bool BigInteger::magnitude_bit(std::size_t index) const {
  const std::size_t limb = index / 32U;
  const std::size_t bit = index % 32U;
  return limb < limbs_.size() && ((limbs_[limb] >> bit) & 1U) != 0;
}

void BigInteger::set_magnitude_bit(std::size_t index) {
  const std::size_t limb = index / 32U;
  const std::size_t bit = index % 32U;
  if (limbs_.size() <= limb) {
    limbs_.resize(limb + 1, 0);
  }
  limbs_[limb] |= std::uint32_t{1} << bit;
  sign_ = 1;
}

bool BigInteger::divide(
    const BigInteger &divisor,
    BigInteger &quotient,
    BigInteger &remainder) const {
  if (divisor.is_zero()) {
    return false;
  }
  const BigInteger dividend_magnitude = absolute();
  const BigInteger divisor_magnitude = divisor.absolute();
  BigInteger result_quotient;
  BigInteger result_remainder;

  // Binary long division keeps the implementation independent of host
  // double-width division. It is quadratic but predictable for compiler-sized
  // constants; evaluator resource policy bounds pathological inputs.
  for (std::size_t remaining_bits = dividend_magnitude.bit_count();
       remaining_bits > 0;
       --remaining_bits) {
    result_remainder = result_remainder.shifted_left(1);
    if (dividend_magnitude.magnitude_bit(remaining_bits - 1)) {
      result_remainder.add_small(1);
    }
    if (compare_magnitude(result_remainder, divisor_magnitude) >= 0) {
      result_remainder = BigInteger(
          1,
          subtract_magnitude(result_remainder.limbs_, divisor_magnitude.limbs_));
      result_quotient.set_magnitude_bit(remaining_bits - 1);
    }
  }
  if (!result_quotient.is_zero() && sign_ != divisor.sign_) {
    result_quotient.sign_ = -1;
  }
  if (!result_remainder.is_zero() && sign_ < 0) {
    result_remainder.sign_ = -1;
  }
  quotient = std::move(result_quotient);
  remainder = std::move(result_remainder);
  return true;
}

BigInteger BigInteger::shifted_left(std::size_t count) const {
  if (is_zero() || count == 0) {
    return *this;
  }
  const std::size_t whole_limbs = count / 32U;
  const std::uint32_t bits = static_cast<std::uint32_t>(count % 32U);
  std::vector<std::uint32_t> result(whole_limbs + limbs_.size() + 1, 0);
  std::uint64_t carry = 0;
  for (std::size_t index = 0; index < limbs_.size(); ++index) {
    const std::uint64_t shifted =
        (static_cast<std::uint64_t>(limbs_[index]) << bits) | carry;
    result[whole_limbs + index] = static_cast<std::uint32_t>(shifted);
    carry = shifted >> 32U;
  }
  result[whole_limbs + limbs_.size()] = static_cast<std::uint32_t>(carry);
  return BigInteger(sign_, std::move(result));
}

bool BigInteger::has_discarded_bits(std::size_t count) const {
  const std::size_t full_limbs = std::min(count / 32U, limbs_.size());
  for (std::size_t index = 0; index < full_limbs; ++index) {
    if (limbs_[index] != 0) return true;
  }
  const std::uint32_t partial = static_cast<std::uint32_t>(count % 32U);
  if (partial != 0 && full_limbs < limbs_.size()) {
    const std::uint32_t mask = (std::uint32_t{1} << partial) - 1U;
    return (limbs_[full_limbs] & mask) != 0;
  }
  return false;
}

BigInteger BigInteger::shifted_right(std::size_t count) const {
  if (is_zero() || count == 0) {
    return *this;
  }
  if (count >= bit_count()) {
    return is_negative() ? BigInteger::from_i64(-1) : BigInteger{};
  }
  const bool discarded = has_discarded_bits(count);
  const std::size_t whole_limbs = count / 32U;
  const std::uint32_t bits = static_cast<std::uint32_t>(count % 32U);
  std::vector<std::uint32_t> result(limbs_.size() - whole_limbs, 0);
  std::uint32_t carry = 0;
  for (std::size_t remaining = limbs_.size(); remaining > whole_limbs; --remaining) {
    const std::size_t input = remaining - 1;
    const std::size_t output = input - whole_limbs;
    if (bits == 0) {
      result[output] = limbs_[input];
    } else {
      result[output] = (limbs_[input] >> bits) | carry;
      carry = limbs_[input] << (32U - bits);
    }
  }
  BigInteger shifted(sign_, std::move(result));
  if (is_negative() && discarded) {
    shifted = shifted.subtracted(BigInteger::from_u64(1));
  }
  return shifted;
}

std::vector<std::uint32_t> BigInteger::twos_complement(std::size_t limbs) const {
  std::vector<std::uint32_t> result(limbs, 0);
  for (std::size_t index = 0; index < std::min(limbs, limbs_.size()); ++index) {
    result[index] = limbs_[index];
  }
  if (!is_negative()) {
    return result;
  }
  for (std::uint32_t &limb : result) {
    limb = ~limb;
  }
  std::uint64_t carry = 1;
  for (std::uint32_t &limb : result) {
    const std::uint64_t sum = static_cast<std::uint64_t>(limb) + carry;
    limb = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
    if (carry == 0) break;
  }
  return result;
}

BigInteger BigInteger::from_twos_complement(std::vector<std::uint32_t> limbs) {
  if (limbs.empty()) return {};
  const bool negative = (limbs.back() & 0x80000000U) != 0;
  if (!negative) {
    return BigInteger(1, std::move(limbs));
  }
  for (std::uint32_t &limb : limbs) {
    limb = ~limb;
  }
  std::uint64_t carry = 1;
  for (std::uint32_t &limb : limbs) {
    const std::uint64_t sum = static_cast<std::uint64_t>(limb) + carry;
    limb = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
    if (carry == 0) break;
  }
  return BigInteger(-1, std::move(limbs));
}

BigInteger BigInteger::bitwise_and(const BigInteger &other) const {
  const std::size_t count = std::max(limbs_.size(), other.limbs_.size()) + 1;
  std::vector<std::uint32_t> left = twos_complement(count);
  const std::vector<std::uint32_t> right = other.twos_complement(count);
  for (std::size_t index = 0; index < count; ++index) left[index] &= right[index];
  return from_twos_complement(std::move(left));
}

BigInteger BigInteger::bitwise_or(const BigInteger &other) const {
  const std::size_t count = std::max(limbs_.size(), other.limbs_.size()) + 1;
  std::vector<std::uint32_t> left = twos_complement(count);
  const std::vector<std::uint32_t> right = other.twos_complement(count);
  for (std::size_t index = 0; index < count; ++index) left[index] |= right[index];
  return from_twos_complement(std::move(left));
}

BigInteger BigInteger::bitwise_xor(const BigInteger &other) const {
  const std::size_t count = std::max(limbs_.size(), other.limbs_.size()) + 1;
  std::vector<std::uint32_t> left = twos_complement(count);
  const std::vector<std::uint32_t> right = other.twos_complement(count);
  for (std::size_t index = 0; index < count; ++index) left[index] ^= right[index];
  return from_twos_complement(std::move(left));
}

BigInteger BigInteger::bitwise_not() const {
  return negated().subtracted(BigInteger::from_u64(1));
}

std::optional<std::uint64_t> BigInteger::to_u64() const {
  if (is_negative() || limbs_.size() > 2) return std::nullopt;
  std::uint64_t result = limbs_.empty() ? 0 : limbs_[0];
  if (limbs_.size() == 2) result |= static_cast<std::uint64_t>(limbs_[1]) << 32U;
  return result;
}

std::optional<std::int64_t> BigInteger::to_i64() const {
  const std::optional<std::uint64_t> magnitude = absolute().to_u64();
  if (!magnitude.has_value()) return std::nullopt;
  const std::uint64_t positive_limit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (!is_negative()) {
    if (*magnitude > positive_limit) return std::nullopt;
    return static_cast<std::int64_t>(*magnitude);
  }
  if (*magnitude > positive_limit + 1U) return std::nullopt;
  if (*magnitude == positive_limit + 1U) return std::numeric_limits<std::int64_t>::min();
  return -static_cast<std::int64_t>(*magnitude);
}

void BigInteger::multiply_small(std::uint32_t factor) {
  if (factor == 0 || is_zero()) {
    if (factor == 0) {
      limbs_.clear();
      sign_ = 0;
    }
    return;
  }
  std::uint64_t carry = 0;
  for (std::uint32_t &limb : limbs_) {
    const std::uint64_t product = static_cast<std::uint64_t>(limb) * factor + carry;
    limb = static_cast<std::uint32_t>(product);
    carry = product >> 32U;
  }
  if (carry != 0) limbs_.push_back(static_cast<std::uint32_t>(carry));
}

void BigInteger::add_small(std::uint32_t value) {
  if (value == 0) return;
  if (is_zero()) {
    sign_ = 1;
    limbs_.push_back(value);
    return;
  }
  assert(sign_ > 0);
  std::uint64_t carry = value;
  for (std::uint32_t &limb : limbs_) {
    const std::uint64_t sum = static_cast<std::uint64_t>(limb) + carry;
    limb = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
    if (carry == 0) return;
  }
  limbs_.push_back(static_cast<std::uint32_t>(carry));
}

std::string BigInteger::to_decimal() const {
  if (is_zero()) return "0";
  BigInteger remaining = absolute();
  const BigInteger ten = from_u64(10);
  std::string digits;
  while (!remaining.is_zero()) {
    BigInteger quotient;
    BigInteger remainder;
    const bool divided = remaining.divide(ten, quotient, remainder);
    assert(divided);
    const std::optional<std::uint64_t> digit = remainder.to_u64();
    assert(digit.has_value() && *digit < 10);
    digits.push_back(static_cast<char>('0' + *digit));
    remaining = std::move(quotient);
  }
  if (is_negative()) digits.push_back('-');
  std::reverse(digits.begin(), digits.end());
  return digits;
}

ExactRational::ExactRational() = default;

ExactRational::ExactRational(BigInteger integer) : numerator_(std::move(integer)) {}

ExactRational::ExactRational(BigInteger numerator, BigInteger denominator)
    : numerator_(std::move(numerator)), denominator_(std::move(denominator)) {
  normalize();
}

std::optional<ExactRational> ExactRational::parse_decimal(std::string_view spelling) {
  std::string digits;
  std::size_t fractional_digits = 0;
  bool after_decimal = false;
  std::int64_t exponent = 0;
  bool negative_exponent = false;
  bool in_exponent = false;
  bool saw_exponent_digit = false;

  for (std::size_t index = 0; index < spelling.size(); ++index) {
    const char character = spelling[index];
    if (character == '_') continue;
    if (character == '.') {
      if (after_decimal || in_exponent) return std::nullopt;
      after_decimal = true;
      continue;
    }
    if (character == 'e' || character == 'E') {
      if (in_exponent) return std::nullopt;
      in_exponent = true;
      if (index + 1 < spelling.size() &&
          (spelling[index + 1] == '+' || spelling[index + 1] == '-')) {
        negative_exponent = spelling[index + 1] == '-';
        ++index;
      }
      continue;
    }
    if (character < '0' || character > '9') return std::nullopt;
    if (in_exponent) {
      saw_exponent_digit = true;
      const std::int64_t digit = character - '0';
      if (exponent > (1000000 - digit) / 10) return std::nullopt;
      exponent = exponent * 10 + digit;
    } else {
      digits.push_back(character);
      if (after_decimal) ++fractional_digits;
    }
  }
  if (digits.empty() || (in_exponent && !saw_exponent_digit)) return std::nullopt;
  const std::optional<BigInteger> parsed = BigInteger::parse_literal(digits);
  if (!parsed.has_value()) return std::nullopt;

  std::int64_t scale = static_cast<std::int64_t>(fractional_digits);
  scale += negative_exponent ? exponent : -exponent;
  if (scale >= 0) {
    return ExactRational(*parsed, power_of_ten(static_cast<std::size_t>(scale)));
  }
  return ExactRational(
      parsed->multiplied(power_of_ten(static_cast<std::size_t>(-scale))),
      BigInteger::from_u64(1));
}

std::optional<ExactRational> ExactRational::from_fraction(
    BigInteger numerator, BigInteger denominator) {
  if (denominator.is_zero()) return std::nullopt;
  return ExactRational(std::move(numerator), std::move(denominator));
}

const BigInteger &ExactRational::numerator() const { return numerator_; }
const BigInteger &ExactRational::denominator() const { return denominator_; }
bool ExactRational::is_zero() const { return numerator_.is_zero(); }

void ExactRational::normalize() {
  assert(!denominator_.is_zero());
  if (denominator_.is_negative()) {
    numerator_ = numerator_.negated();
    denominator_ = denominator_.negated();
  }
  if (numerator_.is_zero()) {
    denominator_ = BigInteger::from_u64(1);
    return;
  }
  const BigInteger divisor = greatest_common_divisor(numerator_, denominator_);
  BigInteger numerator_quotient;
  BigInteger numerator_remainder;
  BigInteger denominator_quotient;
  BigInteger denominator_remainder;
  const bool numerator_divided =
      numerator_.divide(divisor, numerator_quotient, numerator_remainder);
  const bool denominator_divided =
      denominator_.divide(divisor, denominator_quotient, denominator_remainder);
  assert(numerator_divided && denominator_divided);
  assert(numerator_remainder.is_zero() && denominator_remainder.is_zero());
  numerator_ = std::move(numerator_quotient);
  denominator_ = std::move(denominator_quotient);
}

int ExactRational::compare(const ExactRational &other) const {
  return numerator_.multiplied(other.denominator_).compare(
      other.numerator_.multiplied(denominator_));
}

ExactRational ExactRational::added(const ExactRational &other) const {
  return ExactRational(
      numerator_.multiplied(other.denominator_).added(
          other.numerator_.multiplied(denominator_)),
      denominator_.multiplied(other.denominator_));
}

ExactRational ExactRational::subtracted(const ExactRational &other) const {
  return added(other.negated());
}

ExactRational ExactRational::multiplied(const ExactRational &other) const {
  return ExactRational(
      numerator_.multiplied(other.numerator_),
      denominator_.multiplied(other.denominator_));
}

bool ExactRational::divide(const ExactRational &other, ExactRational &result) const {
  if (other.is_zero()) return false;
  result = ExactRational(
      numerator_.multiplied(other.denominator_),
      denominator_.multiplied(other.numerator_));
  return true;
}

ExactRational ExactRational::negated() const {
  return ExactRational(numerator_.negated(), denominator_);
}

std::string ExactRational::to_fraction() const {
  return numerator_.to_decimal() + "/" + denominator_.to_decimal();
}

} // namespace draft
