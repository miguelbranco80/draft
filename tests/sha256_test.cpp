// SHA-256 standard-vector and incremental-chunk tests.

#include "base/sha256.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
  int failures = 0;
  const auto expect = [&failures](bool condition, std::string_view label) {
    if (!condition) {
      ++failures;
      std::cerr << "SHA-256 expectation failed: " << label << '\n';
    }
  };

  expect(
      draft::sha256("").hex() ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "empty vector");
  expect(
      draft::sha256("abc").hex() ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "abc vector");

  draft::Sha256 incremental;
  incremental.update("a");
  incremental.update("b");
  incremental.update("c");
  expect(
      incremental.finalize().hex() ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "incremental chunks");

  if (failures != 0) return EXIT_FAILURE;
  std::cout << "all SHA-256 tests passed\n";
  return EXIT_SUCCESS;
}
