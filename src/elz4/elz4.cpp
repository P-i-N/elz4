#include "elz4.h"

#include <memory.h>
#include <stdio.h>

enum
{
	ELZ4_FLAG_DICT = 1 << 1,          // Dictionary ID is present
	ELZ4_FLAG_CONTENT_CRC32 = 1 << 2, // CRC32 checksum for the compressed data is present
	ELZ4_FLAG_CONTENT_SIZE = 1 << 3,  // Uncompressed size is present
	ELZ4_FLAG_BLOCK_CRC32 = 1 << 4,   // CRC32 checksum for each block is present
};

// Very dumb and minimal "span" implementation
template <typename T> struct span
{
	T *data;
	size_t size;

	operator bool() const { return size > 0; }                           // Bool check
	T next() { return size -= 1, *data++; }                              // Eat one element and return it
	span &operator+=( size_t n ) { return size -= n, data += n, *this; } // Skip `n` elements
};

struct math
{
	template <typename T> static T minimum( T a, T b ) { return a < b ? a : b; }
	template <typename T> static T minimum( T a, T b, T c ) { return minimum( minimum( a, b ), c ); }
};

// Phase function pointer
using phase_func_t = elz4_result ( * )( elz4_ctx *, span<const uint8_t> &, span<uint8_t> & );

/* Forward delcarations */
static elz4_result phase_header( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & );
static elz4_result phase_block_size( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & );
static elz4_result phase_crc32( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & );
static elz4_result phase_decompress_block( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & );
static elz4_result phase_memcpy_block( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & );

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------------------------------------------------
static void set_phase( elz4_ctx *ctx, phase_func_t phase_func )
{
	ctx->scratch_size = 0;
	ctx->phase_func = phase_func;
}

// Try to fill the scratch area with the specified number of bytes from the source `src`. Once the scratch area
// is filled, execute the `if` block beneath the macro.
#define FILL_AND_CHECK_SCRATCH_AREA( _NumBytes ) \
	while ( ctx->scratch_size < ( _NumBytes ) && src ) \
		ctx->scratch[ctx->scratch_size++] = src.next(); \
	if ( ctx->scratch_size != ( _NumBytes ) ) \
		return ELZ4_RESULT_OK;

//----------------------------------------------------------------------------------------------------------------------
static elz4_result phase_header( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & )
{
	// The header is 6 bytes long:
	// - 4B: Magic number (0x184D2204)
	// - 1B: Flags
	// - 1B: Block max. size (not used)
	FILL_AND_CHECK_SCRATCH_AREA( 6 );

	auto magic = *( ( const uint32_t * )ctx->scratch );
	if ( magic != 0x184D2204 )
		return ELZ4_RESULT_INVALID_HEADER;

	ctx->flags = ctx->scratch[4];

	// Additional bytes needed to parse the header
	size_t additionalBytes = 1; // +1 for header checksum at the end
	additionalBytes += ( ctx->flags & ELZ4_FLAG_CONTENT_SIZE ) ? 8 : 0;
	additionalBytes += ( ctx->flags & ELZ4_FLAG_DICT ) ? 4 : 0;

	FILL_AND_CHECK_SCRATCH_AREA( 6 + additionalBytes );

	set_phase( ctx, phase_block_size );
	return ELZ4_RESULT_OK;
}

//----------------------------------------------------------------------------------------------------------------------
static elz4_result phase_block_size( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & )
{
	FILL_AND_CHECK_SCRATCH_AREA( 4 );

	uint32_t blockSize = *( ( const uint32_t * )ctx->scratch );
	if ( blockSize == 0 )
		return ELZ4_RESULT_EOF;

	// Mask out the MSB, which determines if the block is compressed or not
	ctx->current_block_size = blockSize & 0x7FFFFFFF;

	set_phase( ctx, ( blockSize >> 31 ) ? phase_memcpy_block : phase_decompress_block );
	return ELZ4_RESULT_OK;
}

//----------------------------------------------------------------------------------------------------------------------
static elz4_result phase_crc32( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> & )
{
	FILL_AND_CHECK_SCRATCH_AREA( 4 );

	set_phase( ctx, phase_block_size );
	return ELZ4_RESULT_OK;
}

//----------------------------------------------------------------------------------------------------------------------
static elz4_result phase_decompress_block( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> &dst )
{
	uint8_t token;
	uint32_t *scratchU32 = ( uint32_t * )ctx->scratch;

	enum TokenPhase : uint8_t
	{
		STATE_READ_TOKEN,
		STATE_READ_LITERALS_LENGTH,
		STATE_COPY_LITERALS,
		STATE_READ_OFFSET,
		STATE_READ_MATCH_LENGTH,
		STATE_COPY_MATCH
	};

	auto &remainingBlockBytes = *( uint32_t * )( ctx->scratch + 0 );
	auto &literalLength = *( uint32_t * )( ctx->scratch + 4 );
	auto &matchLength = *( uint32_t * )( ctx->scratch + 8 );
	auto &offset = *( uint16_t * )( ctx->scratch + 12 );
	auto &offsetBytes = *( uint8_t * )( ctx->scratch + 14 );
	auto &phase = ( TokenPhase & )ctx->scratch_size; // Treat `scratch_size` as a `TokenPhase` enum value

	while ( src )
	{
		auto origSrcSize = src.size;

		switch ( phase )
		{
			case STATE_READ_TOKEN: {
				// Clear the scratch area
				memset( ctx->scratch, 0, sizeof( ctx->scratch ) );
				phase = STATE_READ_LITERALS_LENGTH;

				token = ctx->current_token = src.next();

				scratchU32[0] = token >> 4;
				if ( scratchU32[0] < 15 )
					phase = STATE_COPY_LITERALS;
			}
			break;

			case STATE_READ_LITERALS_LENGTH: {
				while ( src )
				{
					scratchU32[0] += token = src.next();
					if ( token != 0xFF )
					{
						ctx->scratch_size = STATE_COPY_LITERALS;
						break;
					}
				}
			}
			break;

			case STATE_COPY_LITERALS: {
				// How much data we can get from `src` and copy to `dst`, while respecting the available space?
				size_t numBytes = math::minimum( ( size_t )( scratchU32[0] ), src.size, dst.size );
				memcpy( dst.data, src.data, numBytes );

				src += numBytes;
				dst += numBytes;

				if ( ( scratchU32[0] -= ( uint32_t )numBytes ) == 0 )
				{
					phase = STATE_READ_OFFSET;

					ctx->current_block_size -= ( uint32_t )( origSrcSize - src.size );
					origSrcSize = src.size;

					if ( ctx->current_block_size == 0 )
					{
						set_phase( ctx, ( ctx->flags & ELZ4_FLAG_BLOCK_CRC32 ) ? phase_crc32 : phase_block_size );
						return ELZ4_RESULT_OK;
					}
				}
			}
			break;

			case STATE_READ_OFFSET: {
				if ( scratchU32[1] == 0 && src.size >= 2 )
				{
					scratchU32[0] = *( ( const uint16_t * )src.data );
					scratchU32[1] = 2;
					src += 2;
				}
				else if ( src )
					scratchU32[0] |= ( uint16_t )src.next() << ( 8 * scratchU32[1]++ );

				if ( scratchU32[1] == 2 )
				{
					// Move to next state. scratchU32[0] contains the offset, [1] will be used for the match length
					phase = STATE_READ_MATCH_LENGTH;
					scratchU32[1] = ( ctx->current_token & 0x0F ) + 4;

					if ( scratchU32[1] < 19 )
						phase = STATE_COPY_MATCH;
				}
			}
			break;

			case STATE_READ_MATCH_LENGTH: {
				while ( src )
				{
					scratchU32[1] += token = src.next();
					if ( token != 0xFF )
					{
						phase = STATE_COPY_MATCH;
						break;
					}
				}
			}
			break;

			case STATE_COPY_MATCH: {
				// How much data we can get from `dst` and copy to `dst`, while respecting the available space?
				size_t numBytes = math::minimum( ( size_t )( scratchU32[1] ), dst.size );
				memcpy( dst.data, dst.data - scratchU32[0], numBytes );

				printf( "%d ", ( int )scratchU32[0] );

				dst += numBytes;

				// If all of the match has been copied, move to next token
				if ( ( scratchU32[1] -= ( uint32_t )numBytes ) == 0 )
				{
					scratchU32[0] = 0;
					phase = STATE_READ_TOKEN;
				}
			}
			break;
		}

		ctx->current_block_size -= ( uint32_t )( origSrcSize - src.size );
	}

	return ELZ4_RESULT_OK;
}

//----------------------------------------------------------------------------------------------------------------------
static elz4_result phase_memcpy_block( elz4_ctx *ctx, span<const uint8_t> &src, span<uint8_t> &dst )
{
	return ELZ4_RESULT_OK;
}

//----------------------------------------------------------------------------------------------------------------------
elz4_result elz4_decompress( elz4_ctx *ctx, const uint8_t *src, size_t *src_size, uint8_t *dst, size_t *dst_size )
{
	elz4_result result = ELZ4_RESULT_OK;

	span<const uint8_t> src_span = { src, *src_size };
	span<uint8_t> dst_span = { dst, *dst_size };

	// If the phase function is not set, `ctx` is probably zero-initialized. Just start with the header phase...
	if ( ctx->phase_func == nullptr )
		ctx->phase_func = phase_header;

	while ( src_span )
	{
		auto phase_func = ( phase_func_t )( ctx->phase_func );

		result = phase_func( ctx, src_span, dst_span );
		if ( result != ELZ4_RESULT_OK )
			break;
	}

	// Report the number of bytes read and written by how much the span pointers have moved
	// compared to the original pointers.
	*src_size = src_span.data - src;
	*dst_size = dst_span.data - dst;

	return result;
}
