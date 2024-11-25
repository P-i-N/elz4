#pragma once

#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

enum elz4_result : uint8_t
{
	ELZ4_RESULT_OK,
	ELZ4_RESULT_EOF,
	ELZ4_RESULT_INVALID_HEADER,
	ELZ4_RESULT_INVALID_BLOCK_SIZE,
};

typedef struct
{
	void *phase_func;                  // Internal phase function pointer
	uint32_t current_block_size;       // Current block size
	uint8_t scratch[15], scratch_size; // 15+1 bytes of scratch space
	uint8_t flags;                     // Flags from the header
	uint8_t current_token;             // Current token (litersls + match length)
} elz4_ctx;

elz4_result elz4_decompress( elz4_ctx *ctx, const uint8_t *src, size_t *src_size, uint8_t *dst, size_t *dst_size );

#if __cplusplus
}
#endif
