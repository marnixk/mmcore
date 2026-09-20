/* Standalone DEFLATE (RFC 1951) decompressor used by UNPACK and the package
 * mount reader. Not tied to PNG/upng: the output size is known up front from
 * the ZIP central directory, so callers supply an exact-size buffer. */

#include "mmb_priv.h"

#define INF_NUM_DEFLATE_CODE_SYMBOLS 288
#define INF_NUM_DISTANCE_SYMBOLS 32
#define INF_NUM_CODE_LENGTH_CODES 19
#define INF_MAX_SYMBOLS 288

#define INF_DEFLATE_CODE_BITLEN 15
#define INF_DISTANCE_BITLEN 15
#define INF_CODE_LENGTH_BITLEN 7
#define INF_MAX_BIT_LENGTH 15

#define INF_LENGTH_CODE_FIRST 257
#define INF_LENGTH_CODE_LAST 285

static const unsigned inf_length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};

static const unsigned inf_length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static const unsigned inf_dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	12289, 16385, 24577};

static const unsigned inf_dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static const unsigned inf_clcl[INF_NUM_CODE_LENGTH_CODES] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static const unsigned inf_fixed_deflate_tree[INF_NUM_DEFLATE_CODE_SYMBOLS * 2] = {
	289, 370, 290, 307, 546, 291, 561, 292, 293, 300, 294, 297, 295, 296, 0, 1,
	2, 3, 298, 299, 4, 5, 6, 7, 301, 304, 302, 303, 8, 9, 10, 11, 305, 306, 12,
	13, 14, 15, 308, 339, 309, 324, 310, 317, 311, 314, 312, 313, 16, 17, 18,
	19, 315, 316, 20, 21, 22, 23, 318, 321, 319, 320, 24, 25, 26, 27, 322, 323,
	28, 29, 30, 31, 325, 332, 326, 329, 327, 328, 32, 33, 34, 35, 330, 331, 36,
	37, 38, 39, 333, 336, 334, 335, 40, 41, 42, 43, 337, 338, 44, 45, 46, 47,
	340, 355, 341, 348, 342, 345, 343, 344, 48, 49, 50, 51, 346, 347, 52, 53,
	54, 55, 349, 352, 350, 351, 56, 57, 58, 59, 353, 354, 60, 61, 62, 63, 356,
	363, 357, 360, 358, 359, 64, 65, 66, 67, 361, 362, 68, 69, 70, 71, 364,
	367, 365, 366, 72, 73, 74, 75, 368, 369, 76, 77, 78, 79, 371, 434, 372,
	403, 373, 388, 374, 381, 375, 378, 376, 377, 80, 81, 82, 83, 379, 380, 84,
	85, 86, 87, 382, 385, 383, 384, 88, 89, 90, 91, 386, 387, 92, 93, 94, 95,
	389, 396, 390, 393, 391, 392, 96, 97, 98, 99, 394, 395, 100, 101, 102, 103,
	397, 400, 398, 399, 104, 105, 106, 107, 401, 402, 108, 109, 110, 111, 404,
	419, 405, 412, 406, 409, 407, 408, 112, 113, 114, 115, 410, 411, 116, 117,
	118, 119, 413, 416, 414, 415, 120, 121, 122, 123, 417, 418, 124, 125, 126,
	127, 420, 427, 421, 424, 422, 423, 128, 129, 130, 131, 425, 426, 132, 133,
	134, 135, 428, 431, 429, 430, 136, 137, 138, 139, 432, 433, 140, 141, 142,
	143, 435, 483, 436, 452, 568, 437, 438, 445, 439, 442, 440, 441, 144, 145,
	146, 147, 443, 444, 148, 149, 150, 151, 446, 449, 447, 448, 152, 153, 154,
	155, 450, 451, 156, 157, 158, 159, 453, 468, 454, 461, 455, 458, 456, 457,
	160, 161, 162, 163, 459, 460, 164, 165, 166, 167, 462, 465, 463, 464, 168,
	169, 170, 171, 466, 467, 172, 173, 174, 175, 469, 476, 470, 473, 471, 472,
	176, 177, 178, 179, 474, 475, 180, 181, 182, 183, 477, 480, 478, 479, 184,
	185, 186, 187, 481, 482, 188, 189, 190, 191, 484, 515, 485, 500, 486, 493,
	487, 490, 488, 489, 192, 193, 194, 195, 491, 492, 196, 197, 198, 199, 494,
	497, 495, 496, 200, 201, 202, 203, 498, 499, 204, 205, 206, 207, 501, 508,
	502, 505, 503, 504, 208, 209, 210, 211, 506, 507, 212, 213, 214, 215, 509,
	512, 510, 511, 216, 217, 218, 219, 513, 514, 220, 221, 222, 223, 516, 531,
	517, 524, 518, 521, 519, 520, 224, 225, 226, 227, 522, 523, 228, 229, 230,
	231, 525, 528, 526, 527, 232, 233, 234, 235, 529, 530, 236, 237, 238, 239,
	532, 539, 533, 536, 534, 535, 240, 241, 242, 243, 537, 538, 244, 245, 246,
	247, 540, 543, 541, 542, 248, 249, 250, 251, 544, 545, 252, 253, 254, 255,
	547, 554, 548, 551, 549, 550, 256, 257, 258, 259, 552, 553, 260, 261, 262,
	263, 555, 558, 556, 557, 264, 265, 266, 267, 559, 560, 268, 269, 270, 271,
	562, 565, 563, 564, 272, 273, 274, 275, 566, 567, 276, 277, 278, 279, 569,
	572, 570, 571, 280, 281, 282, 283, 573, 574, 284, 285, 286, 287, 0, 0};

static const unsigned inf_fixed_distance_tree[INF_NUM_DISTANCE_SYMBOLS * 2] = {
	33, 48, 34, 41, 35, 38, 36, 37, 0, 1, 2, 3, 39, 40, 4, 5, 6, 7, 42, 45, 43,
	44, 8, 9, 10, 11, 46, 47, 12, 13, 14, 15, 49, 56, 50, 53, 51, 52, 16, 17,
	18, 19, 54, 55, 20, 21, 22, 23, 57, 60, 58, 59, 24, 25, 26, 27, 61, 62, 28,
	29, 30, 31, 0, 0};

typedef struct inf_tree {
	unsigned *tree2d;
	unsigned numcodes;
	unsigned maxbitlen;
} inf_tree;

typedef struct inf_bits {
	const unsigned char *in;
	unsigned in_len;
	unsigned bp;
	int err;
} inf_bits;

static unsigned inf_read_bit(inf_bits *b)
{
	unsigned bit;
	if ((b->bp >> 3) >= b->in_len)
	{
		b->err = 1;
		b->bp++;
		return 0;
	}
	bit = (b->in[b->bp >> 3] >> (b->bp & 7)) & 1u;
	b->bp++;
	return bit;
}

static unsigned inf_read_bits(inf_bits *b, unsigned nbits)
{
	unsigned result = 0, i;
	for (i = 0; i < nbits; i++)
		result |= inf_read_bit(b) << i;
	return result;
}

static int inf_tree_create(inf_tree *tree, const unsigned *bitlen)
{
	unsigned tree1d[INF_MAX_SYMBOLS];
	unsigned blcount[INF_MAX_BIT_LENGTH + 1];
	unsigned nextcode[INF_MAX_BIT_LENGTH + 2];
	unsigned bits, n, i;
	unsigned nodefilled = 0, treepos = 0;

	memset(blcount, 0, sizeof(blcount));
	memset(nextcode, 0, sizeof(nextcode));
	for (bits = 0; bits < tree->numcodes; bits++)
	{
		if (bitlen[bits] > tree->maxbitlen)
			return -1;
		blcount[bitlen[bits]]++;
	}
	for (bits = 1; bits <= tree->maxbitlen; bits++)
		nextcode[bits] = (nextcode[bits - 1] + blcount[bits - 1]) << 1;
	for (n = 0; n < tree->numcodes; n++)
		if (bitlen[n])
			tree1d[n] = nextcode[bitlen[n]]++;
	for (n = 0; n < tree->numcodes * 2; n++)
		tree->tree2d[n] = 32767;
	for (n = 0; n < tree->numcodes; n++)
	{
		for (i = 0; i < bitlen[n]; i++)
		{
			unsigned char bit = (unsigned char)((tree1d[n] >> (bitlen[n] - i - 1)) & 1);
			if (treepos > tree->numcodes - 2)
				return -1;
			if (tree->tree2d[2 * treepos + bit] == 32767)
			{
				if (i + 1 == bitlen[n])
				{
					tree->tree2d[2 * treepos + bit] = n;
					treepos = 0;
				}
				else
				{
					nodefilled++;
					tree->tree2d[2 * treepos + bit] = nodefilled + tree->numcodes;
					treepos = nodefilled;
				}
			}
			else
			{
				treepos = tree->tree2d[2 * treepos + bit] - tree->numcodes;
			}
		}
	}
	for (n = 0; n < tree->numcodes * 2; n++)
		if (tree->tree2d[n] == 32767)
			tree->tree2d[n] = 0;
	return 0;
}

static int inf_decode_symbol(inf_bits *b, const inf_tree *tree)
{
	unsigned treepos = 0, ct;
	for (;;)
	{
		unsigned char bit;
		if (b->err)
			return -1;
		bit = (unsigned char)inf_read_bit(b);
		if (b->err)
			return -1;
		ct = tree->tree2d[(treepos << 1) | bit];
		if (ct < tree->numcodes)
			return (int)ct;
		treepos = ct - tree->numcodes;
		if (treepos >= tree->numcodes)
			return -1;
	}
}

static int inf_get_dynamic(inf_bits *b, inf_tree *codetree, inf_tree *codetreeD,
			   inf_tree *codelengthcodetree)
{
	unsigned codelengthcode[INF_NUM_CODE_LENGTH_CODES];
	unsigned bitlen[INF_NUM_DEFLATE_CODE_SYMBOLS];
	unsigned bitlenD[INF_NUM_DISTANCE_SYMBOLS];
	unsigned n, hlit, hdist, hclen, i;

	memset(bitlen, 0, sizeof(bitlen));
	memset(bitlenD, 0, sizeof(bitlenD));
	hlit = inf_read_bits(b, 5) + 257;
	hdist = inf_read_bits(b, 5) + 1;
	hclen = inf_read_bits(b, 4) + 4;
	if (b->err)
		return -1;
	if (hlit > INF_NUM_DEFLATE_CODE_SYMBOLS || hdist > INF_NUM_DISTANCE_SYMBOLS)
		return -1;
	for (i = 0; i < INF_NUM_CODE_LENGTH_CODES; i++)
		codelengthcode[inf_clcl[i]] = i < hclen ? inf_read_bits(b, 3) : 0;
	if (b->err)
		return -1;
	if (inf_tree_create(codelengthcodetree, codelengthcode) != 0)
		return -1;
	i = 0;
	while (i < hlit + hdist)
	{
		int code = inf_decode_symbol(b, codelengthcodetree);
		if (code < 0)
			return -1;
		if (code <= 15)
		{
			if (i < hlit)
				bitlen[i] = (unsigned)code;
			else
				bitlenD[i - hlit] = (unsigned)code;
			i++;
		}
		else if (code == 16)
		{
			unsigned replength = 3 + inf_read_bits(b, 2);
			unsigned value;
			if (b->err || i == 0)
				return -1;
			value = i - 1 < hlit ? bitlen[i - 1] : bitlenD[i - hlit - 1];
			for (n = 0; n < replength; n++)
			{
				if (i >= hlit + hdist)
					return -1;
				if (i < hlit)
					bitlen[i] = value;
				else
					bitlenD[i - hlit] = value;
				i++;
			}
		}
		else if (code == 17)
		{
			unsigned replength = 3 + inf_read_bits(b, 3);
			if (b->err)
				return -1;
			for (n = 0; n < replength; n++)
			{
				if (i >= hlit + hdist)
					return -1;
				if (i < hlit)
					bitlen[i] = 0;
				else
					bitlenD[i - hlit] = 0;
				i++;
			}
		}
		else if (code == 18)
		{
			unsigned replength = 11 + inf_read_bits(b, 7);
			if (b->err)
				return -1;
			for (n = 0; n < replength; n++)
			{
				if (i >= hlit + hdist)
					return -1;
				if (i < hlit)
					bitlen[i] = 0;
				else
					bitlenD[i - hlit] = 0;
				i++;
			}
		}
		else
		{
			return -1;
		}
	}
	if (bitlen[256] == 0)
		return -1;
	if (inf_tree_create(codetree, bitlen) != 0)
		return -1;
	if (inf_tree_create(codetreeD, bitlenD) != 0)
		return -1;
	return 0;
}

static int inf_huffman(inf_bits *b, unsigned char *out, unsigned out_cap,
		       unsigned *pos, unsigned btype)
{
	unsigned codetree_buf[INF_NUM_DEFLATE_CODE_SYMBOLS * 2];
	unsigned codetreeD_buf[INF_NUM_DISTANCE_SYMBOLS * 2];
	inf_tree codetree, codetreeD;
	unsigned done = 0;

	if (btype == 1)
	{
		codetree.tree2d = (unsigned *)inf_fixed_deflate_tree;
		codetree.numcodes = INF_NUM_DEFLATE_CODE_SYMBOLS;
		codetree.maxbitlen = INF_DEFLATE_CODE_BITLEN;
		codetreeD.tree2d = (unsigned *)inf_fixed_distance_tree;
		codetreeD.numcodes = INF_NUM_DISTANCE_SYMBOLS;
		codetreeD.maxbitlen = INF_DISTANCE_BITLEN;
	}
	else
	{
		unsigned codelength_buf[INF_NUM_CODE_LENGTH_CODES * 2];
		inf_tree codelengthcodetree;
		codetree.tree2d = codetree_buf;
		codetree.numcodes = INF_NUM_DEFLATE_CODE_SYMBOLS;
		codetree.maxbitlen = INF_DEFLATE_CODE_BITLEN;
		codetreeD.tree2d = codetreeD_buf;
		codetreeD.numcodes = INF_NUM_DISTANCE_SYMBOLS;
		codetreeD.maxbitlen = INF_DISTANCE_BITLEN;
		codelengthcodetree.tree2d = codelength_buf;
		codelengthcodetree.numcodes = INF_NUM_CODE_LENGTH_CODES;
		codelengthcodetree.maxbitlen = INF_CODE_LENGTH_BITLEN;
		if (inf_get_dynamic(b, &codetree, &codetreeD, &codelengthcodetree) != 0)
			return -1;
	}

	while (!done)
	{
		int code = inf_decode_symbol(b, &codetree);
		if (code < 0)
			return -1;
		if (code == 256)
		{
			done = 1;
		}
		else if (code <= 255)
		{
			if (*pos >= out_cap)
				return -1;
			out[(*pos)++] = (unsigned char)code;
		}
		else if (code >= INF_LENGTH_CODE_FIRST && code <= INF_LENGTH_CODE_LAST)
		{
			unsigned idx = (unsigned)(code - INF_LENGTH_CODE_FIRST);
			unsigned long length = inf_length_base[idx];
			unsigned codeD, distance, numextraD, numextra;
			unsigned start, forward, backward;

			numextra = inf_length_extra[idx];
			length += inf_read_bits(b, numextra);
			if (b->err)
				return -1;

			codeD = (unsigned)inf_decode_symbol(b, &codetreeD);
			if (codeD > 29)
				return -1;
			distance = inf_dist_base[codeD];
			numextraD = inf_dist_extra[codeD];
			distance += inf_read_bits(b, numextraD);
			if (b->err)
				return -1;

			start = *pos;
			if (distance > start)
				return -1;
			backward = start - distance;
			if (*pos + length > out_cap)
				return -1;
			for (forward = 0; forward < length; forward++)
			{
				out[(*pos)++] = out[backward];
				backward++;
				if (backward >= start)
					backward = start - distance;
			}
		}
		else
		{
			return -1;
		}
	}
	return 0;
}

static int inf_uncompressed(inf_bits *b, unsigned char *out, unsigned out_cap,
			    unsigned *pos)
{
	unsigned p, len, nlen, n;

	b->bp = (b->bp + 7) & ~7u;
	p = b->bp >> 3;
	if (p + 4 > b->in_len)
		return -1;
	len = b->in[p] | (b->in[p + 1] << 8);
	p += 2;
	nlen = b->in[p] | (b->in[p + 1] << 8);
	p += 2;
	if (len + nlen != 65535)
		return -1;
	if (p + len > b->in_len || *pos + len > out_cap)
		return -1;
	for (n = 0; n < len; n++)
		out[(*pos)++] = b->in[p++];
	b->bp = p * 8;
	return 0;
}

int mmb_inflate(const unsigned char *in, unsigned in_len,
		unsigned char *out, unsigned out_cap, unsigned *out_len)
{
	inf_bits b;
	unsigned pos = 0, done = 0;

	if (!in || !out || !out_len)
		return -1;
	b.in = in;
	b.in_len = in_len;
	b.bp = 0;
	b.err = 0;
	while (!done)
	{
		unsigned btype;
		if ((b.bp >> 3) >= b.in_len)
			return -1;
		done = inf_read_bit(&b);
		btype = inf_read_bit(&b) | (inf_read_bit(&b) << 1);
		if (b.err)
			return -1;
		if (btype == 3)
			return -1;
		if (btype == 0)
		{
			if (inf_uncompressed(&b, out, out_cap, &pos) != 0)
				return -1;
		}
		else if (inf_huffman(&b, out, out_cap, &pos, btype) != 0)
		{
			return -1;
		}
	}
	*out_len = pos;
	return 0;
}
