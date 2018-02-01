// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "puffin/src/huffman_table.h"

#include <algorithm>
#include <vector>

#include "puffin/src/include/puffin/errors.h"
#include "puffin/src/set_errors.h"

namespace puffin {

// clang-format off
// Permutations of input Huffman code lengths (used only to read code lengths
// necessary for reading Huffman table.)
const uint8_t kPermutations[19] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// The bases of each alphabet which is added to the integer value of extra
// bits that comes after the Huffman code in the input to create the given
// length value. The last element is a guard.
const uint16_t kLengthBases[30] = {
  3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,  27, 31, 35, 43,
  51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0xFFFF};

// Number of extra bits that comes after the associating Huffman code.
const uint8_t kLengthExtraBits[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
  5, 5, 0};

// Same as |kLengthBases| but for the distances instead of lengths. The last
// element is a guard.
const uint16_t kDistanceBases[31] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0xFFFF};

// Same as |kLengthExtraBits| but for distances instead of lengths.
const uint8_t kDistanceExtraBits[30] = {
  0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4, 4, 5,  5,  6,  6,  7,  7,  8,  8, 9,
  9, 10, 10, 11, 11, 12, 12, 13, 13};
// clang-format on

// 288 is the maximum number of needed huffman codes for an alphabet. Fixed
// huffman table needs 288 and dynamic huffman table needs maximum 286.
// 286 = 256 (coding a byte) +
//         1 (coding the end of block symbole) +
//        29 (coding the lengths)
HuffmanTable::HuffmanTable() : codeindexpairs_(288), initialized_(false) {}

bool HuffmanTable::InitHuffmanCodes(const Buffer& lens, size_t* max_bits) {
  // Temporary buffers used in |InitHuffmanCodes|.
  uint16_t len_count_[kMaxHuffmanBits + 1] = {0};
  uint16_t next_code_[kMaxHuffmanBits + 1] = {0};

  // 1. Count the number of codes for each length;
  for (auto len : lens) {
    len_count_[len]++;
  }

  for (*max_bits = kMaxHuffmanBits; *max_bits >= 1; (*max_bits)--) {
    if (len_count_[*max_bits] != 0) {
      break;
    }
  }

  // No codes found! However, This is not invalid because you can have no
  // length/distance codes in a block (all literals).
  if (lens.size() == len_count_[0]) {
    LOG(WARNING)
        << "No non-zero lengths are given in the Huffman code Length array.";
  }

  // Check for oversubscribed code lengths. (A code with length 'L' cannot have
  // more than 2^L items.
  for (size_t idx = 1; idx <= *max_bits; idx++) {
    if (len_count_[idx] > (1 << idx)) {
      LOG(ERROR) << "Oversubscribed code lengths error!";
      return false;
    }
  }

  // 2. Compute the coding of the first element for each length.
  uint16_t code = 0;
  len_count_[0] = 0;
  for (size_t bits = 1; bits <= kMaxHuffmanBits; bits++) {
    code = (code + len_count_[bits - 1]) << 1;
    next_code_[bits] = code;
  }

  codeindexpairs_.clear();
  // 3. Calculate all the code values.
  for (size_t idx = 0; idx < lens.size(); idx++) {
    auto len = lens[idx];
    if (len == 0) {
      continue;
    }

    // Reverse the code
    CodeIndexPair cip;
    cip.code = 0;
    auto tmp_code = next_code_[len];
    for (size_t r = 0; r < len; r++) {
      cip.code <<= 1;
      cip.code |= tmp_code & 1U;
      tmp_code >>= 1;
    }
    cip.index = idx;
    codeindexpairs_.push_back(cip);
    next_code_[len]++;
  }
  return true;
}

bool HuffmanTable::BuildHuffmanCodes(const Buffer& lens,
                                     std::vector<uint16_t>* hcodes,
                                     size_t* max_bits) {
  TEST_AND_RETURN_FALSE(InitHuffmanCodes(lens, max_bits));
  // Sort descending based on the bit-length of the code.
  std::sort(codeindexpairs_.begin(),
            codeindexpairs_.end(),
            [&lens](const CodeIndexPair& a, const CodeIndexPair& b) {
              return lens[a.index] > lens[b.index];
            });

  // Only zero out the part of hcodes which is valuable.
  memset(hcodes->data(), 0, (1 << *max_bits) * sizeof(uint16_t));
  for (const auto& cip : codeindexpairs_) {
    // The MSB bit of the code in hcodes is set if it is a valid code and its
    // code exists in the input Huffman table.
    (*hcodes)[cip.code] = cip.index | 0x8000;
    auto fill_bits = *max_bits - lens[cip.index];
    for (auto idx = 1; idx < (1 << fill_bits); idx++) {
      auto location = (idx << lens[cip.index]) | cip.code;
      if (!((*hcodes)[location] & 0x8000)) {
        (*hcodes)[location] = cip.index | 0x8000;
      }
    }
  }
  return true;
}

bool HuffmanTable::BuildHuffmanReverseCodes(const Buffer& lens,
                                            std::vector<uint16_t>* rcodes,
                                            size_t* max_bits) {
  TEST_AND_RETURN_FALSE(InitHuffmanCodes(lens, max_bits));
  // Sort ascending based on the index.
  std::sort(codeindexpairs_.begin(),
            codeindexpairs_.end(),
            [](const CodeIndexPair& a, const CodeIndexPair& b) -> bool {
              return a.index < b.index;
            });

  size_t index = 0;
  for (size_t idx = 0; idx < rcodes->size(); idx++) {
    if (index < codeindexpairs_.size() && idx == codeindexpairs_[index].index) {
      (*rcodes)[idx] = codeindexpairs_[index].code;
      index++;
    } else {
      (*rcodes)[idx] = 0;
    }
  }
  return true;
}

bool HuffmanTable::BuildFixedHuffmanTable() {
  if (!initialized_) {
    // For all the vectors used in this class, we set the size in the
    // constructor and we do not change the size later. This is for optimization
    // purposes. The total size of data in this class is approximately
    // 2KB. Because it is a constructor return values cannot be checked.
    lit_len_lens_.resize(288);
    lit_len_rcodes_.resize(288);
    lit_len_hcodes_.resize(1 << 9);

    distance_lens_.resize(30);
    distance_rcodes_.resize(30);
    distance_hcodes_.resize(1 << 5);

    size_t i = 0;
    while (i < 144) {
      lit_len_lens_[i++] = 8;
    }
    while (i < 256) {
      lit_len_lens_[i++] = 9;
    }
    while (i < 280) {
      lit_len_lens_[i++] = 7;
    }
    while (i < 288) {
      lit_len_lens_[i++] = 8;
    }

    i = 0;
    while (i < 30) {
      distance_lens_[i++] = 5;
    }

    TEST_AND_RETURN_FALSE(
        BuildHuffmanCodes(lit_len_lens_, &lit_len_hcodes_, &lit_len_max_bits_));

    TEST_AND_RETURN_FALSE(BuildHuffmanCodes(
        distance_lens_, &distance_hcodes_, &distance_max_bits_));

    TEST_AND_RETURN_FALSE(BuildHuffmanReverseCodes(
        lit_len_lens_, &lit_len_rcodes_, &lit_len_max_bits_));

    TEST_AND_RETURN_FALSE(BuildHuffmanReverseCodes(
        distance_lens_, &distance_rcodes_, &distance_max_bits_));

    initialized_ = true;
  }
  return true;
}

bool HuffmanTable::BuildDynamicHuffmanTable(BitReaderInterface* br,
                                            uint8_t* buffer,
                                            size_t* length,
                                            Error* error) {
  // Initilize only once and reuse.
  if (!initialized_) {
    // Only resizing the arrays needed.
    code_lens_.resize(19);
    code_hcodes_.resize(1 << 7);

    lit_len_lens_.resize(286);
    lit_len_hcodes_.resize(1 << 15);

    distance_lens_.resize(30);
    distance_hcodes_.resize(1 << 15);
    initialized_ = true;
  }

  // Read the header. Reads the first portion of the Huffman data from input and
  // writes it into the puff |buffer|. The first portion includes the size
  // (|num_lit_len|) of the literals/lengths Huffman code length array
  // (|dynamic_lit_len_lens_|), the size (|num_distance|) of distance Huffman
  // code length array (|dynamic_distance_lens_|), and the size (|num_codes|) of
  // Huffman code length array (dynamic_code_lens_) for reading
  // |dynamic_lit_len_lens_| and |dynamic_distance_lens_|. Then it follows by
  // reading |dynamic_code_lens_|.

  TEST_AND_RETURN_FALSE_SET_ERROR(*length >= 3, Error::kInsufficientOutput);
  size_t index = 0;
  TEST_AND_RETURN_FALSE_SET_ERROR(br->CacheBits(14), Error::kInsufficientInput);
  buffer[index++] = br->ReadBits(5);  // HLIST
  auto num_lit_len = br->ReadBits(5) + 257;
  br->DropBits(5);

  buffer[index++] = br->ReadBits(5);  // HDIST
  auto num_distance = br->ReadBits(5) + 1;
  br->DropBits(5);

  buffer[index++] = br->ReadBits(4);  // HCLEN
  auto num_codes = br->ReadBits(4) + 4;
  br->DropBits(4);

  TEST_AND_RETURN_FALSE_SET_ERROR(
      CheckHuffmanArrayLengths(num_lit_len, num_distance, num_codes),
      Error::kInvalidInput);

  bool checked = false;
  size_t idx = 0;
  TEST_AND_RETURN_FALSE_SET_ERROR(*length - index >= (num_codes + 1) / 2,
                                  Error::kInsufficientOutput);
  // Two codes per byte
  for (; idx < num_codes; idx++) {
    TEST_AND_RETURN_FALSE_SET_ERROR(br->CacheBits(3),
                                    Error::kInsufficientInput);
    code_lens_[kPermutations[idx]] = br->ReadBits(3);
    if (checked) {
      buffer[index++] |= br->ReadBits(3);
    } else {
      buffer[index] = br->ReadBits(3) << 4;
    }
    checked = !checked;
    br->DropBits(3);
  }
  // Pad the last byte if odd number of codes.
  if (checked) {
    index++;
  }
  for (; idx < 19; idx++) {
    code_lens_[kPermutations[idx]] = 0;
  }

  TEST_AND_RETURN_FALSE_SET_ERROR(
      BuildHuffmanCodes(code_lens_, &code_hcodes_, &code_max_bits_),
      Error::kInvalidInput);

  // Build literals/lengths Huffman code length array.
  auto bytes_available = (*length - index);
  TEST_AND_RETURN_FALSE(BuildHuffmanCodeLengths(br,
                                                buffer + index,
                                                &bytes_available,
                                                code_max_bits_,
                                                num_lit_len,
                                                &lit_len_lens_,
                                                error));
  index += bytes_available;

  // Build literals/lengths Huffman codes.
  TEST_AND_RETURN_FALSE_SET_ERROR(
      BuildHuffmanCodes(lit_len_lens_, &lit_len_hcodes_, &lit_len_max_bits_),
      Error::kInvalidInput);

  // Build distance Huffman code length array.
  bytes_available = (*length - index);
  TEST_AND_RETURN_FALSE(BuildHuffmanCodeLengths(br,
                                                buffer + index,
                                                &bytes_available,
                                                code_max_bits_,
                                                num_distance,
                                                &distance_lens_,
                                                error));
  index += bytes_available;

  // Build distance Huffman codes.
  TEST_AND_RETURN_FALSE_SET_ERROR(
      BuildHuffmanCodes(distance_lens_, &distance_hcodes_, &distance_max_bits_),
      Error::kInvalidInput);

  *length = index;
  return true;
}

bool HuffmanTable::BuildHuffmanCodeLengths(BitReaderInterface* br,
                                           uint8_t* buffer,
                                           size_t* length,
                                           size_t max_bits,
                                           size_t num_codes,
                                           Buffer* lens,
                                           Error* error) {
  size_t index = 0;
  lens->clear();
  for (size_t idx = 0; idx < num_codes;) {
    TEST_AND_RETURN_FALSE_SET_ERROR(br->CacheBits(max_bits),
                                    Error::kInsufficientInput);
    auto bits = br->ReadBits(max_bits);
    uint16_t code;
    size_t nbits;
    TEST_AND_RETURN_FALSE_SET_ERROR(CodeAlphabet(bits, &code, &nbits),
                                    Error::kInvalidInput);
    TEST_AND_RETURN_FALSE_SET_ERROR(index < *length,
                                    Error::kInsufficientOutput);
    br->DropBits(nbits);
    if (code < 16) {
      buffer[index++] = code;
      lens->push_back(code);
      idx++;
    } else {
      TEST_AND_RETURN_FALSE_SET_ERROR(code < 19, Error::kInvalidInput);
      size_t copy_num = 0;
      uint8_t copy_val;
      switch (code) {
        case 16:
          TEST_AND_RETURN_FALSE_SET_ERROR(idx != 0, Error::kInvalidInput);
          TEST_AND_RETURN_FALSE_SET_ERROR(br->CacheBits(2),
                                          Error::kInsufficientInput);
          copy_num = 3 + br->ReadBits(2);
          buffer[index++] = 16 + br->ReadBits(2);  // 3 - 6 times
          copy_val = (*lens)[idx - 1];
          br->DropBits(2);
          break;

        case 17:
          TEST_AND_RETURN_FALSE_SET_ERROR(br->CacheBits(3),
                                          Error::kInsufficientInput);
          copy_num = 3 + br->ReadBits(3);
          buffer[index++] = 20 + br->ReadBits(3);  // 3 - 10 times
          copy_val = 0;
          br->DropBits(3);
          break;

        case 18:
          TEST_AND_RETURN_FALSE_SET_ERROR(br->CacheBits(7),
                                          Error::kInsufficientInput);
          copy_num = 11 + br->ReadBits(7);
          buffer[index++] = 28 + br->ReadBits(7);  // 11 - 138 times
          copy_val = 0;
          br->DropBits(7);
          break;

        default:
          LOG(ERROR) << "Invalid code!";
          *error = Error::kInvalidInput;
          return false;
          break;
      }
      idx += copy_num;
      while (copy_num--) {
        lens->push_back(copy_val);
      }
    }
  }
  *length = index;
  return true;
}

bool HuffmanTable::BuildDynamicHuffmanTable(const uint8_t* buffer,
                                            size_t length,
                                            BitWriterInterface* bw,
                                            Error* error) {
  if (!initialized_) {
    // Only resizing the arrays needed.
    code_lens_.resize(19);
    code_rcodes_.resize(19);

    lit_len_lens_.resize(286);
    lit_len_rcodes_.resize(286);

    distance_lens_.resize(30);
    distance_rcodes_.resize(30);

    initialized_ = true;
  }

  TEST_AND_RETURN_FALSE_SET_ERROR(length >= 3, Error::kInsufficientInput);
  size_t index = 0;
  // Write the header.
  size_t num_lit_len = buffer[index] + 257;
  TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(5, buffer[index++]),
                                  Error::kInsufficientOutput);

  size_t num_distance = buffer[index] + 1;
  TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(5, buffer[index++]),
                                  Error::kInsufficientOutput);

  size_t num_codes = buffer[index] + 4;
  TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(4, buffer[index++]),
                                  Error::kInsufficientOutput);

  TEST_AND_RETURN_FALSE_SET_ERROR(
      CheckHuffmanArrayLengths(num_lit_len, num_distance, num_codes),
      Error::kInvalidInput);

  TEST_AND_RETURN_FALSE_SET_ERROR(length - index >= (num_codes + 1) / 2,
                                  Error::kInsufficientInput);
  bool checked = false;
  size_t idx = 0;
  for (; idx < num_codes; idx++) {
    uint8_t len;
    if (checked) {
      len = buffer[index++] & 0x0F;
    } else {
      len = buffer[index] >> 4;
    }
    checked = !checked;
    code_lens_[kPermutations[idx]] = len;
    TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(3, len),
                                    Error::kInsufficientOutput);
  }
  if (checked) {
    index++;
  }
  for (; idx < 19; idx++) {
    code_lens_[kPermutations[idx]] = 0;
  }

  TEST_AND_RETURN_FALSE_SET_ERROR(
      BuildHuffmanReverseCodes(code_lens_, &code_rcodes_, &code_max_bits_),
      Error::kInvalidInput);

  // Build literal/lengths Huffman code lengths.
  auto bytes_available = length - index;
  TEST_AND_RETURN_FALSE(BuildHuffmanCodeLengths(buffer + index,
                                                &bytes_available,
                                                bw,
                                                num_lit_len,
                                                &lit_len_lens_,
                                                error));
  index += bytes_available;

  // Build literal/lengths Huffman reverse codes.
  TEST_AND_RETURN_FALSE_SET_ERROR(
      BuildHuffmanReverseCodes(
          lit_len_lens_, &lit_len_rcodes_, &lit_len_max_bits_),
      Error::kInvalidInput);

  // Build distance Huffman code lengths array.
  bytes_available = length - index;
  TEST_AND_RETURN_FALSE(BuildHuffmanCodeLengths(buffer + index,
                                                &bytes_available,
                                                bw,
                                                num_distance,
                                                &distance_lens_,
                                                error));
  index += bytes_available;

  // Build distance Huffman reverse codes.
  TEST_AND_RETURN_FALSE_SET_ERROR(
      BuildHuffmanReverseCodes(
          distance_lens_, &distance_rcodes_, &distance_max_bits_),
      Error::kInvalidInput);

  TEST_AND_RETURN_FALSE_SET_ERROR(length == index, Error::kInvalidInput);

  return true;
}

bool HuffmanTable::BuildHuffmanCodeLengths(const uint8_t* buffer,
                                           size_t* length,
                                           BitWriterInterface* bw,
                                           size_t num_codes,
                                           Buffer* lens,
                                           Error* error) {
  lens->clear();
  uint16_t hcode;
  size_t nbits;
  size_t index = 0;
  for (size_t idx = 0; idx < num_codes;) {
    TEST_AND_RETURN_FALSE_SET_ERROR(index < *length, Error::kInsufficientInput);
    auto pcode = buffer[index++];
    TEST_AND_RETURN_FALSE_SET_ERROR(pcode <= 155, Error::kInvalidInput);

    auto code = pcode < 16 ? pcode : pcode < 20 ? 16 : pcode < 28 ? 17 : 18;
    TEST_AND_RETURN_FALSE_SET_ERROR(CodeHuffman(code, &hcode, &nbits),
                                    Error::kInvalidInput);
    TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(nbits, hcode),
                                    Error::kInsufficientOutput);
    if (code < 16) {
      lens->push_back(code);
      idx++;
    } else {
      size_t copy_num = 0;
      uint8_t copy_val;
      switch (code) {
        case 16:
          // Cannot repeat a non-existent last code if idx == 0.
          TEST_AND_RETURN_FALSE_SET_ERROR(idx != 0, Error::kInvalidInput);
          TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(2, pcode - 16),
                                          Error::kInsufficientOutput);
          copy_num = 3 + pcode - 16;
          copy_val = (*lens)[idx - 1];
          break;

        case 17:
          TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(3, pcode - 20),
                                          Error::kInsufficientOutput);
          copy_num = 3 + pcode - 20;
          copy_val = 0;
          break;

        case 18:
          TEST_AND_RETURN_FALSE_SET_ERROR(bw->WriteBits(7, pcode - 28),
                                          Error::kInsufficientOutput);
          copy_num = 11 + pcode - 28;
          copy_val = 0;
          break;

        default:
          break;
      }
      idx += copy_num;
      while (copy_num--) {
        lens->push_back(copy_val);
      }
    }
  }
  *length = index;
  return true;
}

std::string BlockTypeToString(BlockType type) {
  switch (type) {
    case BlockType::kUncompressed:
      return "Uncompressed";

    case BlockType::kFixed:
      return "Fixed";

    case BlockType::kDynamic:
      return "Dynamic";

    default:
      return "Unknown";
  }
}

}  // namespace puffin