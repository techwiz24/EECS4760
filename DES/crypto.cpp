/*
 * Copyright (c) 2016 Nathan Lowe
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * crypto.cpp - An implementation of DES in ECB mode
 */

#include "stdafx.h"
#include "crypto.h"
#include "Boxes.h"
#include "DESMath.h"
#include "ExitCodes.h"
#include <iostream>
#include <fstream>

inline uint64_t permute(uint64_t in, const uint8_t* table, size_t inputSize, size_t outputSize)
{
	uint64_t out = 0;

	for(size_t i = 0; i < outputSize; i++)
	{
		out |= ((in >> (inputSize - table[outputSize - 1 - i])) & 1) << i;
	}

	return out;
}

inline uint64_t substitute(uint64_t in)
{
	auto b1 = extract6(in, 1);
	auto b2 = extract6(in, 2);
	auto b3 = extract6(in, 3);
	auto b4 = extract6(in, 4);
	auto b5 = extract6(in, 5);
	auto b6 = extract6(in, 6);
	auto b7 = extract6(in, 7);
	auto b8 = extract6(in, 8);

	return S[0][srow(b1)][scol(b1)] << 28 |
		   S[1][srow(b2)][scol(b2)] << 24 |
		   S[2][srow(b3)][scol(b3)] << 20 |
		   S[3][srow(b4)][scol(b4)] << 16 |
		   S[4][srow(b5)][scol(b5)] << 12 |
		   S[5][srow(b6)][scol(b6)] << 8  |
		   S[6][srow(b7)][scol(b7)] << 4  |
		   S[7][srow(b8)][scol(b8)];
}

void computeRoundKeys(uint64_t key, uint64_t (&keys)[16])
{
	// Initialize the key
	//   1. Compress and Permute the key into 56 bits
	//   2. Split the key into two 28 bit halves
	uint64_t keyLeft, keyRight;
	split56(permute(key, KeyPC64To56, 64, 56), keyLeft, keyRight);

	for(auto i = 0; i < 16; i++)
	{
		rotL28(keyLeft, RotationSchedule[i]);
		rotL28(keyRight, RotationSchedule[i]);

		keys[i] = permute(join56(keyLeft, keyRight), KeyPC56To48, 56, 48);
	}
}

uint64_t TransformBlock(uint64_t block, uint64_t (&keys)[16], DES::Action action)
{
	// Perform the initial permutation on the plaintext
	auto permutedBlock = permute(block, InitalBlockPermutation, 64, 64);

	// Split the plaintext into 32 bit left and right halves
	uint64_t left, right;

	// Perform the initial permutation
	split64(permutedBlock, left, right);

	// 16 fistel rounds
	for(auto i = 0; i < 16; i++)
	{
		// Expand and permute the right half of the block to 48 bits
		auto expandedRightHalf = permute(right, BlockPE32To48, 32, 48) & MASK48;

		// XOR with the round key
		auto roundKey = keys[action == DES::Action::ENCRYPT ? i : 15-i];
		expandedRightHalf ^= roundKey;

		// Substitute via S-Boxes
		auto substituted = substitute(expandedRightHalf);

		// Perform the final permutation
		auto ciphertext = permute(substituted, BlockP32, 32, 32) & MASK32;

		// XOR with the left half
		ciphertext ^= left;

		// Swap the half-blocks for the next round
		left = right;
		right = ciphertext;
	}

	auto finalBlock = join64(right, left);
	return permute(finalBlock, FinalBlockPermutation, 64, 64);
}


int DES::EncryptFile(std::string inputFile, std::string outputFile, uint64_t key, Mode mode, Optional<uint64_t> CBCInitialVector)
{
	std::ifstream reader;
	reader.open(inputFile, std::ios::binary | std::ios::ate | std::ios::in);

	if(!reader.good())
	{
		std::cerr << "Unable to open file for read: " << inputFile << std::endl;
		return EXIT_ERR_BAD_INPUT;
	}

	size_t len = reader.tellg();
	if (len > MASK31)
	{
		std::cerr << "Input file too large according to spec. Must be less than 2GiB" << std::endl;
		return EXIT_ERR_TOO_BIG;
	}

	reader.seekg(0, std::ios::beg);

	std::ofstream writer;
	writer.open(outputFile, std::ios::binary | std::ios::out);

	if(!writer.good())
	{
		std::cerr << "Unable to open file for write: " << outputFile << std::endl;
		reader.close();
		return EXIT_ERR_BAD_OUTPUT;
	}

	uint64_t keys[16];
	computeRoundKeys(key, keys);

	// Write the length of the file so we can determine how much padding we used when decrypting
	auto headerBlock = join64(RandomHalfBlock(), len);

	// Used in CBC mode only
	uint64_t previousBlock;
	if(CBCInitialVector.HasValue())
	{
		previousBlock = CBCInitialVector.GetValue();
		headerBlock ^= previousBlock;
	}

	auto encryptedHeader = TransformBlock(headerBlock, keys, DES::Action::ENCRYPT);
	previousBlock = encryptedHeader;

	auto outputBuffer = _byteswap_uint64(encryptedHeader);

	// Write encrypted header
	writer.write(reinterpret_cast<const char*>(&outputBuffer), DES_BLOCK_SIZE_BYTES);

	// Read file into memory
	auto bytes = new char[len]{ 0 };
	reader.read(bytes, len);
	reader.close();

	size_t written = 0;
	while(written < len)
	{
		uint64_t block;
		if(len - written >= DES_BLOCK_SIZE_BYTES)
		{
			block = extract64FromBuff(bytes, written);
			written += DES_BLOCK_SIZE_BYTES;
		}
		else
		{
			// Need padding
			block = RandomBlock();
			for(auto i = 0; i < len - written; i++)
			{
				block |= charToUnsigned64(bytes[written++]) << (56 - (8 * i));
			}
		}

		if(CBCInitialVector.HasValue())
		{
			block ^= previousBlock;
		}
		auto encryptedBlock = TransformBlock(block, keys, DES::Action::ENCRYPT);
		if(CBCInitialVector.HasValue())
		{
			previousBlock = encryptedBlock;
		}

		outputBuffer = _byteswap_uint64(encryptedBlock);

		// Write block
		writer.write(reinterpret_cast<const char*>(&outputBuffer), DES_BLOCK_SIZE_BYTES);
	}

	writer.flush();
	writer.close();

	delete[] bytes;
	return EXIT_SUCCESS;
}

int DES::DecryptFile(std::string inputFile, std::string outputFile, uint64_t key, Mode mode, Optional<uint64_t> CBCInitialVector)
{
	std::ifstream reader;
	reader.open(inputFile, std::ios::binary | std::ios::ate | std::ios::in);

	if(!reader.good())
	{
		std::cerr << "Unable to open file for read: " << inputFile << std::endl;
		return EXIT_ERR_BAD_INPUT;
	}

	size_t len = reader.tellg();
	len -= DES_BLOCK_SIZE_BYTES;
	if (len > MASK31)
	{
		std::cerr << "Input file too large according to spec. Must be less than 2GiB" << std::endl;
		return EXIT_ERR_TOO_BIG;
	}

	if(len % 8 != 0)
	{
		std::cerr << "Input file not 64-bit aligned" << std::endl;
		return EXIT_ERR_BAD_INPUT;
	}

	reader.seekg(0, std::ios::beg);

	std::ofstream writer;
	writer.open(outputFile, std::ios::binary | std::ios::out);

	if(!writer.good())
	{
		std::cerr << "Unable to open file for write: " << outputFile << std::endl;
		reader.close();
		return EXIT_ERR_BAD_OUTPUT;
	}

	auto bytes = new char[len];

	uint64_t keys[16];
	computeRoundKeys(key, keys);

	char rawheader[8] = { 0 };
	reader.read(rawheader, DES_BLOCK_SIZE_BYTES);
	reader.read(bytes, len);

	// Read the length of the file so we can determine how much padding we used when encrypting
	auto headerBlock = extract64FromBuff(rawheader, 0);

	auto decryptedHeader = TransformBlock(headerBlock, keys, DES::Action::DECRYPT);

	// Used in CBC mode only
	uint64_t previousBlock;
	if(CBCInitialVector.HasValue())
	{
		previousBlock = CBCInitialVector.GetValue();
		headerBlock ^= previousBlock;
	}
	previousBlock = decryptedHeader;

	// How much padding did we use?
	auto padding = len - (decryptedHeader & MASK32);

	size_t written = 0;
	while(written != len)
	{
		auto block = extract64FromBuff(bytes, written);
		written += DES_BLOCK_SIZE_BYTES;

		auto decryptedBlock = TransformBlock(block, keys, DES::Action::DECRYPT);
		if(CBCInitialVector.HasValue())
		{
			decryptedBlock ^= previousBlock;
			previousBlock = decryptedBlock;
		}

		auto isPaddingBlock = written == len && padding > 0;
		if (isPaddingBlock)
		{
			// Padding Block
			uint64_t mask = 0;
			for (auto i = 0; i < padding; i++)
			{
				mask |= 0xFF;
				mask <<= 8;
			}

			mask <<= (56 - (8 * padding));

			decryptedBlock &= mask;
		}
		
		auto outputBuffer = _byteswap_uint64(decryptedBlock);
		writer.write(reinterpret_cast<const char*>(&outputBuffer), isPaddingBlock ? padding : DES_BLOCK_SIZE_BYTES);
	}
	 
	delete[] bytes;
	return EXIT_SUCCESS;
}

