// pxtone-play-sample: Linux port of the Windows sample player.
// Usage: pxtone-play <file.ptcop|pttune|ptnoise>
//
// Original Windows code used XAudio2 + Win32 file dialog / MessageBox.
// On Linux this port uses SDL2 audio and takes the file path from argv.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL.h>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static const int      _CHANNEL_NUM       = 2;
static const int32_t  _SAMPLE_PER_SECOND = 44100;

// ---- I/O callbacks for pxtnDescriptor ---------------------------------

static bool _pxtn_r( void* user, void* p_dst, int size, int num )
{
	int i = fread( p_dst, size, num, (FILE*)user );
	if( i < num ) return false;
	return true;
}

static bool _pxtn_w( void* user, const void* p_src, int size, int num )
{
	int i = fwrite( p_src, size, num, (FILE*)user );
	if( i < num ) return false;
	return true;
}

static bool _pxtn_s( void* user, int mode, int size )
{
	if( fseek( (FILE*)user, size, mode ) ) return false;
	return true;
}

static bool _pxtn_p( void* user, int32_t* p_pos )
{
	long i = ftell( (FILE*)user );
	if( i < 0 ) return false;
	*p_pos = (int32_t)i;
	return true;
}

// ---- audio ------------------------------------------------------------

static bool _sampling_func( void* user, void* buf, int* p_res_size, int* p_req_size )
{
	pxtnService* pxtn = static_cast<pxtnService*>( user );

	if( !pxtn->Moo( buf, *p_req_size ) ) return false;
	if( p_res_size ) *p_res_size = *p_req_size;

	return true;
}

static void _sdl_audio_callback( void* userdata, Uint8* stream, int len )
{
	if( !_sampling_func( userdata, stream, NULL, &len ) )
	{
		memset( stream, 0, len );
	}
}

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: %s <file.ptcop|file.pttune|file.ptnoise>\n", argv[0] );
		return 1;
	}
	const char* path_src = argv[1];

	bool         b_ret    = false;
	pxtnERR      pxtn_err = pxtnERR_VOID;
	FILE*        fp       = NULL;
	SDL_AudioSpec want     = {0};
	SDL_AudioSpec have     = {0};

	// INIT SDL.
	if( SDL_Init( SDL_INIT_AUDIO ) != 0 )
	{
		fprintf( stderr, "ERROR: SDL_Init: %s\n", SDL_GetError() );
		return 1;
	}

	// INIT PXTONE.
	pxtnService* pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );

	pxtn_err = pxtn->init(); if( pxtn_err != pxtnOK ) goto term;
	if( !pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) )
	{
		pxtn_err = pxtnERR_INIT;
		goto term;
	}

	// LOAD MUSIC FILE.
	if( !( fp = fopen( path_src, "rb" ) ) )
	{
		fprintf( stderr, "ERROR: cannot open file: %s\n", path_src );
		goto term;
	}

	pxtn_err = pxtn->read       ( fp ); if( pxtn_err != pxtnOK ) goto term;
	pxtn_err = pxtn->tones_ready(    ); if( pxtn_err != pxtnOK ) goto term;

	{
		fclose( fp );
		fp = NULL;
	}

	// PREPARATION PLAYING MUSIC.
	{
		pxtnVOMITPREPARATION prep = {0};
		prep.flags          |= pxtnVOMITPREPFLAG_loop;
		prep.start_pos_float =     0;
		prep.master_volume   = 0.80f;

		if( !pxtn->moo_preparation( &prep ) )
		{
			pxtn_err = pxtnERR_memory;
			goto term;
		}
	}

	// INIT SDL AUDIO.
	want.freq     = _SAMPLE_PER_SECOND;
	want.format   = AUDIO_S16SYS;   // pxtone Moo() outputs 16bit PCM
	want.channels = _CHANNEL_NUM;
	want.samples  = 4096;
	want.callback = _sdl_audio_callback;
	want.userdata = pxtn;

	if( SDL_OpenAudio( &want, &have ) != 0 )
	{
		fprintf( stderr, "ERROR: SDL_OpenAudio: %s\n", SDL_GetError() );
		goto term;
	}

	{
		int32_t buf_size    =    0;
		const char* name    = pxtn->text->get_name_buf( &buf_size );
		if( !name || !name[0] ) name = "(none)";

		printf( "file: %s\nname: %s\n", path_src, name );
		printf( "playing... press Ctrl-C to stop\n" );

		// START PLAYING. (loops forever; stop with SIGINT)
		SDL_PauseAudio( 0 );
		for( ;; ) SDL_Delay( 100 );

		b_ret = true;
	}

term:

	if( fp ) fclose( fp );

	if( !b_ret )
	{
		fprintf( stderr, "ERROR: pxtnERR[ %s ]\n", pxtnError_get_string( pxtn_err ) );
	}

	SDL_CloseAudio();
	SDL_Quit();

	delete pxtn;
	return b_ret ? 0 : 1;
}
