#include <fstream>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "elz4.h"

#define print( _Fmt, ... ) std::cout << std::format( _Fmt "\n", __VA_ARGS__ )
#define print_err( _Fmt, ... ) std::cerr << std::format( _Fmt "\n", __VA_ARGS__ )

using bytes = std::vector<uint8_t>;

//----------------------------------------------------------------------------------------------------------------------
std::optional<bytes> read_file( std::string_view fileName )
{
	std::vector<uint8_t> data;

	if ( fileName.starts_with( "\\\\" ) )
	{
		fileName = fileName.substr( 2 );

		for ( auto c : fileName )
			data.push_back( ( uint8_t )c );

		return data;
	}

	std::ifstream file( std::string( fileName ), std::ios::binary );
	if ( !file.is_open() )
		return std::nullopt;

	file.seekg( 0, std::ios::end );
	data.resize( file.tellg() );
	file.seekg( 0, std::ios::beg );

	file.read( reinterpret_cast<char *>( data.data() ), data.size() );
	file.close();

	return data;
}

//----------------------------------------------------------------------------------------------------------------------
bool write_file( std::string_view fileName, std::span<uint8_t> data )
{
	std::ofstream file( fileName.data(), std::ios::binary );
	if ( !file.is_open() )
		return false;

	file.write( ( const char * )data.data(), data.size() );
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
int main( int argc, char *argv[] )
{
	auto fileBytes = read_file( "firmware.bin.lz5" );
	if ( !fileBytes )
	{
		print_err( "Failed to read file" );
		return 1;
	}

	elz4_ctx ctx = { 0 };

	bytes decompressed( 1024 * 1024 );

	uint8_t *dst = decompressed.data();
	size_t dstSize = decompressed.size();

	for ( auto b : fileBytes.value() )
	{
		size_t srcSize = 1;
		size_t dataSize = dstSize;
		auto r = elz4_decompress( &ctx, &b, &srcSize, dst, &dataSize );

		if ( r != ELZ4_RESULT_OK )
		{
			print_err( "Failed to decompress" );
			return 1;
		}

		dst += dataSize;
		dstSize -= dataSize;
	}

	return 0;
}
