// Recompresses uncompressed 32-bit textures inside RenderWare D3D8 TXD files
// to DXT1 (opaque) or DXT5 (alpha) with a full mip chain. Textures that are
// already compressed, palettized or tiny are copied through untouched.
//
//   txdcompress <file.txd> [more.txd ...]
//
// A converted file is fully validated and written to a sibling temporary file
// before the original is replaced atomically.
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

static bool checkedAdd(size_t a, size_t b, size_t &result)
{
	if(b > std::numeric_limits<size_t>::max() - a) return false;
	result = a + b;
	return true;
}

static bool checkedMul(size_t a, size_t b, size_t &result)
{
	if(a != 0 && b > std::numeric_limits<size_t>::max() / a) return false;
	result = a * b;
	return true;
}

static bool rangeFits(size_t length, size_t offset, size_t count)
{
	return offset <= length && count <= length - offset;
}

static bool readU16(const u8 *data, size_t length, size_t offset, u16 &value)
{
	if(!rangeFits(length, offset, 2)) return false;
	value = (u16)data[offset] | ((u16)data[offset + 1] << 8);
	return true;
}

static bool readU32(const u8 *data, size_t length, size_t offset, u32 &value)
{
	if(!rangeFits(length, offset, 4)) return false;
	value = (u32)data[offset] |
	        ((u32)data[offset + 1] << 8) |
	        ((u32)data[offset + 2] << 16) |
	        ((u32)data[offset + 3] << 24);
	return true;
}

static bool prepareAppend(std::vector<u8> &out, size_t count, std::string &error)
{
	if(count > out.max_size() - out.size()){
		error = "output size overflow";
		return false;
	}
	return true;
}

static bool appendBytes(std::vector<u8> &out, const u8 *data, size_t count, std::string &error)
{
	if(!prepareAppend(out, count, error)) return false;
	if(count != 0) out.insert(out.end(), data, data + count);
	return true;
}

static bool appendU16(std::vector<u8> &out, u16 value, std::string &error)
{
	if(!prepareAppend(out, 2, error)) return false;
	out.push_back((u8)(value & 0xFF));
	out.push_back((u8)(value >> 8));
	return true;
}

static bool appendU32(std::vector<u8> &out, u32 value, std::string &error)
{
	if(!prepareAppend(out, 4, error)) return false;
	out.push_back((u8)(value & 0xFF));
	out.push_back((u8)((value >> 8) & 0xFF));
	out.push_back((u8)((value >> 16) & 0xFF));
	out.push_back((u8)(value >> 24));
	return true;
}

static bool sizeToU32(size_t value, u32 &result, std::string &error)
{
	if(value > std::numeric_limits<u32>::max()){
		error = "RenderWare chunk exceeds the 32-bit size field";
		return false;
	}
	result = (u32)value;
	return true;
}

static bool addCount(size_t &value, size_t increment, std::string &error)
{
	size_t result;
	if(!checkedAdd(value, increment, result)){
		error = "statistics counter overflow";
		return false;
	}
	value = result;
	return true;
}

// ---- DXT block compression (bounding-box range fit) ----

static u16 pack565(int r, int g, int b)
{
	return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// pixels: 16 RGBA entries (row-major 4x4)
static void encodeColorBlock(const u8 px[16][4], u8 out[8], bool opaqueDxt1)
{
	int minc[3] = {255, 255, 255}, maxc[3] = {0, 0, 0};
	for(int i = 0; i < 16; i++) for(int c = 0; c < 3; c++){
		if(px[i][c] < minc[c]) minc[c] = px[i][c];
		if(px[i][c] > maxc[c]) maxc[c] = px[i][c];
	}
	for(int c = 0; c < 3; c++){
		int inset = (maxc[c] - minc[c]) / 16;
		minc[c] += inset;
		maxc[c] -= inset;
		if(minc[c] > maxc[c]){
			int middle = (minc[c] + maxc[c]) / 2;
			minc[c] = maxc[c] = middle;
		}
	}
	u16 c0 = pack565(maxc[0], maxc[1], maxc[2]);
	u16 c1 = pack565(minc[0], minc[1], minc[2]);
	if(opaqueDxt1 && c0 == c1 && c0 > 0) c1--;
	else if(opaqueDxt1 && c0 == c1) c0++;
	if(opaqueDxt1 && c0 < c1){
		u16 swap = c0;
		c0 = c1;
		c1 = swap;
	}
	int palette[4][3];
	palette[0][0] = (c0 >> 11) << 3;
	palette[0][1] = ((c0 >> 5) & 63) << 2;
	palette[0][2] = (c0 & 31) << 3;
	palette[1][0] = (c1 >> 11) << 3;
	palette[1][1] = ((c1 >> 5) & 63) << 2;
	palette[1][2] = (c1 & 31) << 3;
	for(int c = 0; c < 3; c++){
		palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
		palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
	}
	u32 indices = 0;
	for(int i = 0; i < 16; i++){
		int best = 0, bestDistance = 1 << 30;
		for(int j = 0; j < 4; j++){
			int distance = 0;
			for(int c = 0; c < 3; c++){
				int difference = px[i][c] - palette[j][c];
				distance += difference * difference;
			}
			if(distance < bestDistance){
				bestDistance = distance;
				best = j;
			}
		}
		indices |= (u32)best << (i * 2);
	}
	out[0] = (u8)(c0 & 0xFF);
	out[1] = (u8)(c0 >> 8);
	out[2] = (u8)(c1 & 0xFF);
	out[3] = (u8)(c1 >> 8);
	out[4] = (u8)(indices & 0xFF);
	out[5] = (u8)((indices >> 8) & 0xFF);
	out[6] = (u8)((indices >> 16) & 0xFF);
	out[7] = (u8)(indices >> 24);
}

static void encodeAlphaBlock(const u8 px[16][4], u8 out[8])
{
	int minAlpha = 255, maxAlpha = 0;
	for(int i = 0; i < 16; i++){
		if(px[i][3] < minAlpha) minAlpha = px[i][3];
		if(px[i][3] > maxAlpha) maxAlpha = px[i][3];
	}
	if(minAlpha == maxAlpha){
		if(maxAlpha < 255) maxAlpha++;
		else minAlpha--;
	}
	out[0] = (u8)maxAlpha;
	out[1] = (u8)minAlpha;
	int palette[8];
	palette[0] = maxAlpha;
	palette[1] = minAlpha;
	for(int j = 1; j < 7; j++) palette[j + 1] = ((7 - j) * maxAlpha + j * minAlpha) / 7;
	u8 indices[16];
	for(int i = 0; i < 16; i++){
		int best = 0, bestDistance = 1 << 30;
		for(int j = 0; j < 8; j++){
			int distance = px[i][3] - palette[j];
			distance *= distance;
			if(distance < bestDistance){
				bestDistance = distance;
				best = j;
			}
		}
		indices[i] = (u8)best;
	}
	uint64_t bits = 0;
	for(int i = 0; i < 16; i++) bits |= (uint64_t)indices[i] << (i * 3);
	for(int i = 0; i < 6; i++) out[2 + i] = (u8)(bits >> (i * 8));
}

static bool pixelOffset(int x, int y, int width, size_t dataSize, size_t &offset)
{
	if(x < 0 || y < 0 || width <= 0) return false;
	size_t rowStart, pixelIndex;
	if(!checkedMul((size_t)y, (size_t)width, rowStart) ||
	   !checkedAdd(rowStart, (size_t)x, pixelIndex) ||
	   !checkedMul(pixelIndex, 4, offset)) return false;
	return rangeFits(dataSize, offset, 4);
}

static bool compressLevel(const std::vector<u8> &rgba, int width, int height, bool dxt5,
	std::vector<u8> &out, std::string &error)
{
	if(width <= 0 || height <= 0){
		error = "invalid zero-sized mip level";
		return false;
	}
	size_t pixels, expectedBytes;
	if(!checkedMul((size_t)width, (size_t)height, pixels) ||
	   !checkedMul(pixels, 4, expectedBytes) || expectedBytes > rgba.size()){
		error = "mip pixel buffer is truncated or too large";
		return false;
	}
	const int blockWidth = (width + 3) / 4;
	const int blockHeight = (height + 3) / 4;
	size_t blockCount, encodedBytes;
	if(!checkedMul((size_t)blockWidth, (size_t)blockHeight, blockCount) ||
	   !checkedMul(blockCount, dxt5 ? 16u : 8u, encodedBytes) ||
	   !prepareAppend(out, encodedBytes, error)) return false;
	out.reserve(out.size() + encodedBytes);
	for(int blockY = 0; blockY < blockHeight; blockY++) for(int blockX = 0; blockX < blockWidth; blockX++){
		u8 pixelsBlock[16][4];
		for(int y = 0; y < 4; y++) for(int x = 0; x < 4; x++){
			int sourceX = blockX * 4 + x;
			int sourceY = blockY * 4 + y;
			if(sourceX >= width) sourceX = width - 1;
			if(sourceY >= height) sourceY = height - 1;
			size_t sourceOffset;
			if(!pixelOffset(sourceX, sourceY, width, rgba.size(), sourceOffset)){
				error = "mip pixel offset overflow";
				return false;
			}
			memcpy(pixelsBlock[y * 4 + x], rgba.data() + sourceOffset, 4);
		}
		u8 block[8];
		if(dxt5){
			encodeAlphaBlock(pixelsBlock, block);
			out.insert(out.end(), block, block + 8);
		}
		encodeColorBlock(pixelsBlock, block, !dxt5);
		out.insert(out.end(), block, block + 8);
	}
	return true;
}

static bool downsample(const std::vector<u8> &source, int width, int height,
	std::vector<u8> &destination, int &newWidth, int &newHeight, std::string &error)
{
	if(width <= 0 || height <= 0){
		error = "invalid zero-sized mip level";
		return false;
	}
	newWidth = width > 1 ? width / 2 : 1;
	newHeight = height > 1 ? height / 2 : 1;
	size_t sourcePixels, sourceBytes, destinationPixels, destinationBytes;
	if(!checkedMul((size_t)width, (size_t)height, sourcePixels) ||
	   !checkedMul(sourcePixels, 4, sourceBytes) || sourceBytes > source.size() ||
	   !checkedMul((size_t)newWidth, (size_t)newHeight, destinationPixels) ||
	   !checkedMul(destinationPixels, 4, destinationBytes) ||
	   destinationBytes > destination.max_size()){
		error = "mip size overflow";
		return false;
	}
	destination.resize(destinationBytes);
	for(int y = 0; y < newHeight; y++) for(int x = 0; x < newWidth; x++){
		int sourceX = x * 2, sourceY = y * 2;
		int sourceX1 = sourceX + 1 < width ? sourceX + 1 : sourceX;
		int sourceY1 = sourceY + 1 < height ? sourceY + 1 : sourceY;
		size_t offsets[4], destinationOffset;
		if(!pixelOffset(sourceX, sourceY, width, source.size(), offsets[0]) ||
		   !pixelOffset(sourceX1, sourceY, width, source.size(), offsets[1]) ||
		   !pixelOffset(sourceX, sourceY1, width, source.size(), offsets[2]) ||
		   !pixelOffset(sourceX1, sourceY1, width, source.size(), offsets[3]) ||
		   !pixelOffset(x, y, newWidth, destination.size(), destinationOffset)){
			error = "mip pixel offset overflow";
			return false;
		}
		for(int channel = 0; channel < 4; channel++){
			int value = source[offsets[0] + channel] + source[offsets[1] + channel] +
			            source[offsets[2] + channel] + source[offsets[3] + channel];
			destination[destinationOffset + channel] = (u8)(value / 4);
		}
	}
	return true;
}

// ---- TXD rewriting ----

struct Stats {
	size_t converted;
	size_t kept;
	size_t saved;
	Stats() : converted(0), kept(0), saved(0) {}
};

enum TransformResult {
	TRANSFORM_UNCHANGED,
	TRANSFORM_CHANGED,
	TRANSFORM_MALFORMED
};

// Maximum texture dimension. Levels above it are dropped. Zero disables the cap.
static int gDimensionCap = 0;
static std::string gRepairAlphaTexture;
static bool gRepairAlphaTextureSeen = false;

static bool equalsAsciiInsensitive(const char *left, const std::string &right)
{
	const size_t leftLength = strlen(left);
	if(leftLength != right.size()) return false;
	for(size_t i = 0; i < leftLength; i++){
		unsigned char a = (unsigned char)left[i];
		unsigned char b = (unsigned char)right[i];
		if(a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
		if(b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
		if(a != b) return false;
	}
	return true;
}

static bool validateLevelBlobs(const u8 *data, size_t size, u8 levels,
	std::vector<size_t> &levelOffsets, std::string &error)
{
	if(levels == 0){
		error = "texture has zero mip levels";
		return false;
	}
	size_t offset = 88;
	levelOffsets.clear();
	levelOffsets.reserve((size_t)levels + 1);
	levelOffsets.push_back(offset);
	for(unsigned int level = 0; level < levels; level++){
		u32 levelSize;
		if(!readU32(data, size, offset, levelSize)){
			error = "truncated mip size field";
			return false;
		}
		size_t pixelsOffset, nextOffset;
		if(!checkedAdd(offset, 4, pixelsOffset) ||
		   !checkedAdd(pixelsOffset, (size_t)levelSize, nextOffset) || nextOffset > size){
			error = "mip payload exceeds texture STRUCT";
			return false;
		}
		offset = nextOffset;
		levelOffsets.push_back(offset);
	}
	if(offset != size){
		error = "unexpected trailing bytes in texture STRUCT";
		return false;
	}
	return true;
}

static unsigned int bc2AlphaNibble(const std::vector<u8> &data,
	size_t pixelsOffset, int width, int x, int y)
{
	const size_t blocksWide = ((size_t)width + 3) / 4;
	const size_t block = ((size_t)y / 4) * blocksWide + (size_t)x / 4;
	const unsigned int pixel = ((unsigned int)y & 3u) * 4u + ((unsigned int)x & 3u);
	const u8 packed = data[pixelsOffset + block * 16 + pixel / 2];
	return (packed >> ((pixel & 1u) * 4u)) & 0xFu;
}

static void setBc2AlphaNibble(std::vector<u8> &data, size_t pixelsOffset,
	int width, int x, int y, unsigned int alpha)
{
	const size_t blocksWide = ((size_t)width + 3) / 4;
	const size_t block = ((size_t)y / 4) * blocksWide + (size_t)x / 4;
	const unsigned int pixel = ((unsigned int)y & 3u) * 4u + ((unsigned int)x & 3u);
	u8 &packed = data[pixelsOffset + block * 16 + pixel / 2];
	const unsigned int shift = (pixel & 1u) * 4u;
	packed = (u8)((packed & ~(0xFu << shift)) | ((alpha & 0xFu) << shift));
}

// Some third-party DXT3 foliage packs ship progressively emptier alpha mips;
// the final levels can become fully transparent. Repair only an explicitly
// named texture and only raise alpha values needed to retain level-zero cutout
// coverage. This is an offline asset fix and has no per-frame rendering cost.
static TransformResult repairBc2AlphaMips(const u8 *data, size_t size,
	const char *txdName, Stats &stats, std::vector<u8> &out,
	std::vector<std::string> &reports, std::string &error)
{
	if(gRepairAlphaTexture.empty()) return TRANSFORM_UNCHANGED;
	u32 platform;
	if(!readU32(data, size, 0, platform)){
		error = "truncated texture platform field";
		return TRANSFORM_MALFORMED;
	}
	if(platform != 8) return TRANSFORM_UNCHANGED;
	if(size < 88){
		error = "truncated D3D8 texture STRUCT header";
		return TRANSFORM_MALFORMED;
	}
	char name[33] = {0};
	memcpy(name, data + 8, 32);
	if(!equalsAsciiInsensitive(name, gRepairAlphaTexture)) return TRANSFORM_UNCHANGED;
	gRepairAlphaTextureSeen = true;
	const u8 compression = data[87];
	if(compression != 2 && compression != 3){
		error = "requested alpha-mip texture is not DXT2/DXT3";
		return TRANSFORM_MALFORMED;
	}
	u16 width, height;
	if(!readU16(data, size, 80, width) || !readU16(data, size, 82, height) ||
	   width == 0 || height == 0){
		error = "requested alpha-mip texture has invalid dimensions";
		return TRANSFORM_MALFORMED;
	}
	const u8 levels = data[85];
	std::vector<size_t> levelOffsets;
	if(!validateLevelBlobs(data, size, levels, levelOffsets, error))
		return TRANSFORM_MALFORMED;
	out.assign(data, data + size);
	const size_t basePixels = (size_t)width * height;
	size_t baseCovered = 0;
	const size_t baseOffset = levelOffsets[0] + 4;
	for(int y = 0; y < height; y++) for(int x = 0; x < width; x++)
		if(bc2AlphaNibble(out, baseOffset, width, x, y) >= 8) baseCovered++;
	if(baseCovered == 0) return TRANSFORM_UNCHANGED;

	int mipWidth = width, mipHeight = height;
	size_t promoted = 0;
	for(unsigned int level = 1; level < levels; level++){
		mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
		mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
		u32 levelSize;
		if(!readU32(data, size, levelOffsets[level], levelSize)){
			error = "truncated DXT3 mip size";
			return TRANSFORM_MALFORMED;
		}
		const size_t blocksWide = ((size_t)mipWidth + 3) / 4;
		const size_t blocksHigh = ((size_t)mipHeight + 3) / 4;
		size_t expectedBytes;
		if(!checkedMul(blocksWide, blocksHigh, expectedBytes) ||
		   !checkedMul(expectedBytes, 16, expectedBytes) || expectedBytes > levelSize){
			error = "DXT3 mip payload is smaller than its dimensions";
			return TRANSFORM_MALFORMED;
		}
		const size_t pixels = (size_t)mipWidth * mipHeight;
		size_t coverageNumerator;
		if(!checkedMul(baseCovered, pixels, coverageNumerator) ||
		   !checkedAdd(coverageNumerator, basePixels - 1, coverageNumerator)){
			error = "alpha coverage calculation overflow";
			return TRANSFORM_MALFORMED;
		}
		const size_t wanted = coverageNumerator / basePixels;
		const size_t pixelsOffset = levelOffsets[level] + 4;
		size_t covered = 0;
		for(int y = 0; y < mipHeight; y++) for(int x = 0; x < mipWidth; x++)
			if(bc2AlphaNibble(out, pixelsOffset, mipWidth, x, y) >= 8) covered++;
		for(int sourceAlpha = 7; sourceAlpha >= 0 && covered < wanted; sourceAlpha--){
			for(int y = 0; y < mipHeight && covered < wanted; y++){
				for(int x = 0; x < mipWidth && covered < wanted; x++){
					if(bc2AlphaNibble(out, pixelsOffset, mipWidth, x, y) ==
					   (unsigned int)sourceAlpha){
						setBc2AlphaNibble(out, pixelsOffset, mipWidth, x, y, 8);
						covered++;
						promoted++;
					}
				}
			}
		}
	}
	if(promoted == 0){
		out.clear();
		return TRANSFORM_UNCHANGED;
	}
	if(!addCount(stats.converted, 1, error)) return TRANSFORM_MALFORMED;
	char report[256];
	snprintf(report, sizeof(report),
		"  %-14s %-24s preserved %.1f%% alpha coverage across %u mips (%zu texels raised)",
		txdName, name, baseCovered * 100.0 / basePixels, (unsigned int)levels, promoted);
	reports.push_back(report);
	return TRANSFORM_CHANGED;
}

static TransformResult capStruct(const u8 *data, size_t size, const char *txdName,
	Stats &stats, std::vector<u8> &out, std::vector<std::string> &reports, std::string &error)
{
	u32 platform;
	if(!readU32(data, size, 0, platform)){
		error = "truncated texture platform field";
		return TRANSFORM_MALFORMED;
	}
	if(platform != 8) return TRANSFORM_UNCHANGED;
	if(size < 88){
		error = "truncated D3D8 texture STRUCT header";
		return TRANSFORM_MALFORMED;
	}
	u16 width, height;
	if(!readU16(data, size, 80, width) || !readU16(data, size, 82, height)){
		error = "truncated texture dimensions";
		return TRANSFORM_MALFORMED;
	}
	const u8 levels = data[85];
	const u8 compression = data[87];
	if(compression == 0) return TRANSFORM_UNCHANGED;
	if(width == 0 || height == 0){
		error = "compressed texture has a zero dimension";
		return TRANSFORM_MALFORMED;
	}
	std::vector<size_t> levelOffsets;
	if(!validateLevelBlobs(data, size, levels, levelOffsets, error)) return TRANSFORM_MALFORMED;
	if(gDimensionCap <= 0) return TRANSFORM_UNCHANGED;
	int maxDimension = width > height ? width : height;
	if(maxDimension <= gDimensionCap || levels <= 1) return TRANSFORM_UNCHANGED;
	int drop = 0;
	int newWidth = width, newHeight = height;
	while((newWidth > gDimensionCap || newHeight > gDimensionCap) && drop < levels - 1){
		newWidth = newWidth > 1 ? newWidth / 2 : 1;
		newHeight = newHeight > 1 ? newHeight / 2 : 1;
		drop++;
	}
	if(drop == 0) return TRANSFORM_UNCHANGED;
	const size_t retainedOffset = levelOffsets[(size_t)drop];
	const size_t dropped = retainedOffset - 88;
	out.clear();
	if(!appendBytes(out, data, 80, error) ||
	   !appendU16(out, (u16)newWidth, error) ||
	   !appendU16(out, (u16)newHeight, error) ||
	   !prepareAppend(out, 4, error)) return TRANSFORM_MALFORMED;
	out.push_back(data[84]);
	out.push_back((u8)(levels - drop));
	out.push_back(data[86]);
	out.push_back(compression);
	if(!appendBytes(out, data + retainedOffset, size - retainedOffset, error) ||
	   !addCount(stats.converted, 1, error) || !addCount(stats.saved, dropped, error))
		return TRANSFORM_MALFORMED;
	char name[33] = {0};
	memcpy(name, data + 8, 32);
	char report[256];
	snprintf(report, sizeof(report), "  %-14s %-24s %4ux%-4u -> %dx%d (%d mips dropped, %.1f MB)",
		txdName, name, width, height, newWidth, newHeight, drop, dropped / 1048576.0);
	reports.push_back(report);
	return TRANSFORM_CHANGED;
}

static TransformResult convertStruct(const u8 *data, size_t size, const char *txdName,
	Stats &stats, std::vector<u8> &out, std::vector<std::string> &reports, std::string &error)
{
	u32 platform;
	if(!readU32(data, size, 0, platform)){
		error = "truncated texture platform field";
		return TRANSFORM_MALFORMED;
	}
	if(platform != 8) return TRANSFORM_UNCHANGED;
	if(size < 88){
		error = "truncated D3D8 texture STRUCT header";
		return TRANSFORM_MALFORMED;
	}
	u32 rasterFormat;
	u16 width, height;
	if(!readU32(data, size, 72, rasterFormat) ||
	   !readU16(data, size, 80, width) || !readU16(data, size, 82, height)){
		error = "truncated D3D8 texture header";
		return TRANSFORM_MALFORMED;
	}
	const u8 depth = data[84];
	const u8 levels = data[85];
	const u8 type = data[86];
	const u8 compression = data[87];
	if(compression != 0) return TRANSFORM_UNCHANGED;
	if((rasterFormat & 0x6000) != 0 || depth != 32) return TRANSFORM_UNCHANGED;
	if(width == 0 || height == 0){
		error = "uncompressed texture has a zero dimension";
		return TRANSFORM_MALFORMED;
	}
	std::vector<size_t> levelOffsets;
	if(!validateLevelBlobs(data, size, levels, levelOffsets, error)) return TRANSFORM_MALFORMED;
	size_t pixelCount, originalPixelBytes;
	if(!checkedMul((size_t)width, (size_t)height, pixelCount) ||
	   !checkedMul(pixelCount, 4, originalPixelBytes)){
		error = "level-zero pixel size overflow";
		return TRANSFORM_MALFORMED;
	}
	u32 levelZeroSize;
	if(!readU32(data, size, 88, levelZeroSize) || originalPixelBytes > levelZeroSize ||
	   !rangeFits(size, 92, originalPixelBytes)){
		error = "level-zero pixels are truncated";
		return TRANSFORM_MALFORMED;
	}
	if(width < 8 || height < 8) return TRANSFORM_UNCHANGED;
	std::vector<u8> rgba(originalPixelBytes);
	for(size_t i = 0; i < pixelCount; i++){
		size_t offset, diskOffset;
		if(!checkedMul(i, 4, offset) || !rangeFits(originalPixelBytes, offset, 4) ||
		   !checkedAdd(92, offset, diskOffset) || !rangeFits(size, diskOffset, 4)){
			error = "level-zero pixel offset overflow";
			return TRANSFORM_MALFORMED;
		}
		rgba[offset + 0] = data[diskOffset + 2];
		rgba[offset + 1] = data[diskOffset + 1];
		rgba[offset + 2] = data[diskOffset + 0];
		rgba[offset + 3] = data[diskOffset + 3];
	}
	bool hasAlpha = false;
	for(size_t i = 0; i < pixelCount; i++){
		size_t alphaOffset;
		if(!checkedMul(i, 4, alphaOffset) || !checkedAdd(alphaOffset, 3, alphaOffset) ||
		   alphaOffset >= rgba.size()){
			error = "alpha pixel offset overflow";
			return TRANSFORM_MALFORMED;
		}
		if(rgba[alphaOffset] != 255){
			hasAlpha = true;
			break;
		}
	}
	const bool dxt5 = hasAlpha;
	const u16 originalWidth = width, originalHeight = height;
	int outputWidth = width, outputHeight = height;
	while(gDimensionCap > 0 && (outputWidth > gDimensionCap || outputHeight > gDimensionCap)){
		std::vector<u8> half;
		int newWidth, newHeight;
		if(!downsample(rgba, outputWidth, outputHeight, half, newWidth, newHeight, error))
			return TRANSFORM_MALFORMED;
		rgba.swap(half);
		outputWidth = newWidth;
		outputHeight = newHeight;
	}
	int numberOfLevels = 1;
	for(int mipWidth = outputWidth, mipHeight = outputHeight; mipWidth > 1 || mipHeight > 1; numberOfLevels++){
		mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
		mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
		if(numberOfLevels >= 255){
			error = "generated mip count exceeds the on-disk field";
			return TRANSFORM_MALFORMED;
		}
	}
	out.clear();
	if(!appendU32(out, 8, error) ||
	   !appendBytes(out, data + 4, 68, error)) return TRANSFORM_MALFORMED;
	const u32 newFormat = (dxt5 ? 0x0500u : 0x0200u) | 0x8000u;
	if(!appendU32(out, newFormat, error) ||
	   !appendU32(out, dxt5 ? 1u : 0u, error) ||
	   !appendU16(out, (u16)outputWidth, error) ||
	   !appendU16(out, (u16)outputHeight, error) ||
	   !prepareAppend(out, 4, error)) return TRANSFORM_MALFORMED;
	out.push_back(16);
	out.push_back((u8)numberOfLevels);
	out.push_back(type);
	out.push_back(dxt5 ? 5 : 1);
	std::vector<u8> current = rgba, next;
	int currentWidth = outputWidth, currentHeight = outputHeight;
	size_t newPixelBytes = 0;
	for(int level = 0; level < numberOfLevels; level++){
		std::vector<u8> block;
		if(!compressLevel(current, currentWidth, currentHeight, dxt5, block, error))
			return TRANSFORM_MALFORMED;
		u32 blockSize;
		if(!sizeToU32(block.size(), blockSize, error) ||
		   !appendU32(out, blockSize, error) ||
		   !appendBytes(out, block.data(), block.size(), error) ||
		   !addCount(newPixelBytes, block.size(), error)) return TRANSFORM_MALFORMED;
		if(level + 1 < numberOfLevels){
			int newWidth, newHeight;
			if(!downsample(current, currentWidth, currentHeight, next, newWidth, newHeight, error))
				return TRANSFORM_MALFORMED;
			current.swap(next);
			currentWidth = newWidth;
			currentHeight = newHeight;
		}
	}
	if(!addCount(stats.converted, 1, error)) return TRANSFORM_MALFORMED;
	if(originalPixelBytes > newPixelBytes &&
	   !addCount(stats.saved, originalPixelBytes - newPixelBytes, error)) return TRANSFORM_MALFORMED;
	char name[33] = {0};
	memcpy(name, data + 8, 32);
	char report[256];
	snprintf(report, sizeof(report),
		"  %-14s %-24s %4ux%-4u -> %s %dx%d, %d mips (%.1f -> %.1f MB)",
		txdName, name, originalWidth, originalHeight, dxt5 ? "DXT5" : "DXT1",
		outputWidth, outputHeight, numberOfLevels,
		originalPixelBytes / 1048576.0, newPixelBytes / 1048576.0);
	reports.push_back(report);
	return TRANSFORM_CHANGED;
}

static TransformResult transformStruct(const u8 *data, size_t size, const char *txdName,
	Stats &stats, std::vector<u8> &out, std::vector<std::string> &reports, std::string &error)
{
	TransformResult result = repairBc2AlphaMips(data, size, txdName, stats, out, reports, error);
	if(result != TRANSFORM_UNCHANGED) return result;
	result = convertStruct(data, size, txdName, stats, out, reports, error);
	if(result != TRANSFORM_UNCHANGED) return result;
	return capStruct(data, size, txdName, stats, out, reports, error);
}

static bool rebuildChildren(const u8 *data, size_t length, const char *txdName,
	Stats &stats, std::vector<u8> &out, std::vector<std::string> &reports, std::string &error)
{
	out.clear();
	size_t offset = 0;
	while(offset < length){
		if(!rangeFits(length, offset, 12)){
			error = "truncated RenderWare child chunk header";
			return false;
		}
		u32 type, chunkSize, version;
		if(!readU32(data, length, offset, type) ||
		   !readU32(data, length, offset + 4, chunkSize) ||
		   !readU32(data, length, offset + 8, version)){
			error = "truncated RenderWare child chunk header";
			return false;
		}
		size_t payloadOffset, chunkEnd;
		if(!checkedAdd(offset, 12, payloadOffset) ||
		   !checkedAdd(payloadOffset, (size_t)chunkSize, chunkEnd) || chunkEnd > length){
			error = "RenderWare child chunk exceeds its parent";
			return false;
		}
		if(type == 0x15){
			const u8 *children = data + payloadOffset;
			const size_t childrenLength = chunkSize;
			std::vector<u8> inner;
			size_t childOffset = 0;
			bool sawStruct = false;
			while(childOffset < childrenLength){
				if(!rangeFits(childrenLength, childOffset, 12)){
					error = "truncated Texture Native child header";
					return false;
				}
				u32 childType, childSize, childVersion;
				if(!readU32(children, childrenLength, childOffset, childType) ||
				   !readU32(children, childrenLength, childOffset + 4, childSize) ||
				   !readU32(children, childrenLength, childOffset + 8, childVersion)){
					error = "truncated Texture Native child header";
					return false;
				}
				size_t childPayload, childEnd;
				if(!checkedAdd(childOffset, 12, childPayload) ||
				   !checkedAdd(childPayload, (size_t)childSize, childEnd) || childEnd > childrenLength){
					error = "Texture Native child exceeds its parent";
					return false;
				}
				if(childType == 0x01){
					sawStruct = true;
					std::vector<u8> transformed;
					TransformResult result = transformStruct(children + childPayload, childSize,
						txdName, stats, transformed, reports, error);
					if(result == TRANSFORM_MALFORMED) return false;
					if(result == TRANSFORM_UNCHANGED){
						if(!appendBytes(inner, children + childOffset, childEnd - childOffset, error) ||
						   !addCount(stats.kept, 1, error)) return false;
					}else{
						u32 transformedSize;
						if(!sizeToU32(transformed.size(), transformedSize, error) ||
						   !appendU32(inner, 0x01, error) ||
						   !appendU32(inner, transformedSize, error) ||
						   !appendU32(inner, childVersion, error) ||
						   !appendBytes(inner, transformed.data(), transformed.size(), error)) return false;
					}
				}else if(!appendBytes(inner, children + childOffset, childEnd - childOffset, error)){
					return false;
				}
				childOffset = childEnd;
			}
			if(!sawStruct){
				error = "Texture Native chunk has no STRUCT child";
				return false;
			}
			u32 innerSize;
			if(!sizeToU32(inner.size(), innerSize, error) ||
			   !appendU32(out, 0x15, error) ||
			   !appendU32(out, innerSize, error) ||
			   !appendU32(out, version, error) ||
			   !appendBytes(out, inner.data(), inner.size(), error)) return false;
		}else if(!appendBytes(out, data + offset, chunkEnd - offset, error)){
			return false;
		}
		offset = chunkEnd;
	}
	return true;
}

static bool readWholeFile(const char *path, std::vector<u8> &data, std::string &error)
{
	FILE *file = NULL;
#ifdef _WIN32
	if(fopen_s(&file, path, "rb") != 0) file = NULL;
#else
	file = fopen(path, "rb");
#endif
	if(!file){
		error = "cannot open for reading";
		return false;
	}
	bool ok = true;
	if(fseek(file, 0, SEEK_END) != 0){
		error = "fseek(SEEK_END) failed";
		ok = false;
	}
	long fileSize = ok ? ftell(file) : -1;
	if(ok && fileSize < 0){
		error = "ftell failed";
		ok = false;
	}
	if(ok && (unsigned long long)fileSize > (unsigned long long)std::numeric_limits<size_t>::max()){
		error = "file is too large for this build";
		ok = false;
	}
	if(ok && fseek(file, 0, SEEK_SET) != 0){
		error = "fseek(SEEK_SET) failed";
		ok = false;
	}
	if(ok){
		const size_t size = (size_t)fileSize;
		if(size > data.max_size()){
			error = "file exceeds vector capacity";
			ok = false;
		}else{
			data.resize(size);
			const size_t bytesRead = size == 0 ? 0 : fread(data.data(), 1, size, file);
			if(bytesRead != size){
				error = ferror(file) ? "fread failed" : "file changed or ended during fread";
				ok = false;
			}
		}
	}
	if(fclose(file) != 0){
		if(ok) error = "fclose failed after reading";
		ok = false;
	}
	return ok;
}

static void removeTemporary(const std::string &path)
{
#ifdef _WIN32
	DeleteFileA(path.c_str());
#else
	remove(path.c_str());
#endif
}

static bool openSiblingTemporary(const char *path, std::string &temporaryPath,
	FILE *&file, std::string &error)
{
#ifdef _WIN32
	const unsigned long processId = GetCurrentProcessId();
#else
	const unsigned long processId = (unsigned long)getpid();
#endif
	for(unsigned int attempt = 0; attempt < 1024; attempt++){
		char suffix[80];
		snprintf(suffix, sizeof(suffix), ".txdcompress.tmp.%lu.%u", processId, attempt);
		temporaryPath = std::string(path) + suffix;
#ifdef _WIN32
		HANDLE handle = CreateFileA(temporaryPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY, NULL);
		if(handle == INVALID_HANDLE_VALUE){
			const DWORD createError = GetLastError();
			if(createError == ERROR_FILE_EXISTS || createError == ERROR_ALREADY_EXISTS) continue;
			error = "cannot create sibling temporary file (Windows error " +
				std::to_string((unsigned long)createError) + ")";
			return false;
		}
		int descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
		if(descriptor == -1){
			const bool closed = CloseHandle(handle) != 0;
			removeTemporary(temporaryPath);
			error = closed ? "_open_osfhandle failed" : "_open_osfhandle and CloseHandle failed";
			return false;
		}
		file = _fdopen(descriptor, "wb");
		if(!file){
			const int closeResult = _close(descriptor);
			removeTemporary(temporaryPath);
			error = closeResult == 0 ? "_fdopen failed" : "_fdopen and _close failed";
			return false;
		}
#else
		errno = 0;
		file = fopen(temporaryPath.c_str(), "wbx");
		if(!file){
			if(errno == EEXIST) continue;
			error = "cannot create sibling temporary file";
			return false;
		}
#endif
		return true;
	}
	error = "could not allocate a unique sibling temporary name";
	return false;
}

static bool writeThenReplace(const char *path, const std::vector<u8> &data, std::string &error)
{
	std::string temporaryPath;
	FILE *file = NULL;
	if(!openSiblingTemporary(path, temporaryPath, file, error)) return false;
	bool ok = true;
	size_t written = 0;
	while(written < data.size()){
		const size_t amount = fwrite(data.data() + written, 1, data.size() - written, file);
		if(amount == 0){
			error = "fwrite failed for sibling temporary file";
			ok = false;
			break;
		}
		if(!checkedAdd(written, amount, written)){
			error = "fwrite byte counter overflow";
			ok = false;
			break;
		}
	}
	if(ok && fflush(file) != 0){
		error = "fflush failed for sibling temporary file";
		ok = false;
	}
#ifdef _WIN32
	if(ok && _commit(_fileno(file)) != 0){
		error = "_commit failed for sibling temporary file";
		ok = false;
	}
#endif
	if(fclose(file) != 0){
		if(ok) error = "fclose failed for sibling temporary file";
		ok = false;
	}
	if(!ok){
		removeTemporary(temporaryPath);
		return false;
	}
#ifdef _WIN32
	if(!ReplaceFileA(path, temporaryPath.c_str(), NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL)){
		const DWORD replaceError = GetLastError();
		removeTemporary(temporaryPath);
		error = "atomic ReplaceFile failed (Windows error " +
			std::to_string((unsigned long)replaceError) + ")";
		return false;
	}
#else
	if(rename(temporaryPath.c_str(), path) != 0){
		removeTemporary(temporaryPath);
		error = "atomic rename failed";
		return false;
	}
#endif
	return true;
}

static const char *baseName(const char *path)
{
	const char *backslash = strrchr(path, '\\');
	const char *slash = strrchr(path, '/');
	const char *separator = backslash;
	if(!separator || (slash && slash > separator)) separator = slash;
	return separator ? separator + 1 : path;
}

static bool processTxd(const char *path)
{
	try {
		gRepairAlphaTextureSeen = false;
		std::vector<u8> data;
		std::string error;
		if(!readWholeFile(path, data, error)){
			printf("error: %s: %s\n", path, error.c_str());
			return false;
		}
		u32 rootType, dictionarySize, dictionaryVersion;
		if(!readU32(data.data(), data.size(), 0, rootType) ||
		   !readU32(data.data(), data.size(), 4, dictionarySize) ||
		   !readU32(data.data(), data.size(), 8, dictionaryVersion)){
			printf("error: %s: truncated TXD root header\n", path);
			return false;
		}
		if(rootType != 0x16){
			printf("error: %s: not a TXD dictionary chunk\n", path);
			return false;
		}
		size_t rootEnd;
		if(!checkedAdd(12, (size_t)dictionarySize, rootEnd) || rootEnd != data.size()){
			printf("error: %s: TXD root size does not match the file\n", path);
			return false;
		}
		const char *name = baseName(path);
		Stats stats;
		std::vector<std::string> reports;
		std::vector<u8> body;
		if(!rebuildChildren(data.data() + 12, dictionarySize, name, stats, body, reports, error)){
			printf("error: %s: %s\n", path, error.c_str());
			return false;
		}
		if(!gRepairAlphaTexture.empty() && !gRepairAlphaTextureSeen){
			printf("error: %s: texture '%s' was not found; original preserved\n",
				path, gRepairAlphaTexture.c_str());
			return false;
		}
		if(stats.converted == 0) return true;
		u32 bodySize;
		if(!sizeToU32(body.size(), bodySize, error)){
			printf("error: %s: %s\n", path, error.c_str());
			return false;
		}
		std::vector<u8> output;
		if(!appendU32(output, 0x16, error) ||
		   !appendU32(output, bodySize, error) ||
		   !appendU32(output, dictionaryVersion, error) ||
		   !appendBytes(output, body.data(), body.size(), error)){
			printf("error: %s: %s\n", path, error.c_str());
			return false;
		}
		if(!writeThenReplace(path, output, error)){
			printf("error: %s: %s; original preserved\n", path, error.c_str());
			return false;
		}
		for(size_t i = 0; i < reports.size(); i++) printf("%s\n", reports[i].c_str());
		printf("%s: %zu converted, %zu untouched, %.1f MB pixel data saved, file %.1f -> %.1f MB\n",
			name, stats.converted, stats.kept, stats.saved / 1048576.0,
			data.size() / 1048576.0, output.size() / 1048576.0);
		return true;
	} catch(const std::bad_alloc &) {
		printf("error: %s: out of memory; original preserved\n", path);
		return false;
	} catch(const std::exception &exception) {
		printf("error: %s: %s; original preserved\n", path, exception.what());
		return false;
	} catch(...) {
		printf("error: %s: unexpected exception; original preserved\n", path);
		return false;
	}
}

static bool parseDimensionCap(const char *text, int &value)
{
	errno = 0;
	char *end = NULL;
	const long parsed = strtol(text, &end, 10);
	if(errno == ERANGE || end == text || *end != '\0' || parsed < 0 || parsed > 65535) return false;
	value = (int)parsed;
	return true;
}

int main(int argc, char **argv)
{
	if(argc < 2){
		printf("usage: txdcompress [--cap N | --repair-alpha-mips TEXTURE] <file.txd> ...\n");
		return 1;
	}
	int firstFile = 1;
	if(strcmp(argv[1], "--cap") == 0){
		if(argc < 4 || !parseDimensionCap(argv[2], gDimensionCap)){
			printf("error: --cap requires an integer from 0 through 65535 and at least one file\n");
			return 1;
		}
		firstFile = 3;
	}else if(strcmp(argv[1], "--repair-alpha-mips") == 0){
		if(argc < 4 || argv[2][0] == '\0'){
			printf("error: --repair-alpha-mips requires a texture name and at least one file\n");
			return 1;
		}
		gRepairAlphaTexture = argv[2];
		firstFile = 3;
	}
	int failures = 0;
	for(int i = firstFile; i < argc; i++) if(!processTxd(argv[i])) failures++;
	return failures ? 1 : 0;
}
