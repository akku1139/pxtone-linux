// pxtone-visualizer: GUI piano-roll style note visualizer for pxtone music.
// Links against the shared libpxtn.so core.
//
// Usage: pxtone-visualizer <file.ptcop|pttune>
//
// - Plays the song via SDL2 audio (same as pxtone-play).
// - Shows notes scrolling in a piano roll, synced to playback.
// - Close window / Ctrl-C / ESC to stop.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>

#include <SDL.h>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static const int      _CHANNEL_NUM       = 2;
static const int32_t  _SAMPLE_PER_SECOND = 44100;

static const int _WIN_W = 1024;
static const int _WIN_H = 600;

// ---- I/O callbacks ------------------------------------------------------

static bool _pxtn_r( void* user, void* p_dst, int size, int num )
{
	return fread( p_dst, size, num, (FILE*)user ) >= num;
}
static bool _pxtn_w( void* user, const void* p_src, int size, int num )
{
	return fwrite( p_src, size, num, (FILE*)user ) >= num;
}
static bool _pxtn_s( void* user, int mode, int size )
{
	return !fseek( (FILE*)user, size, mode );
}
static bool _pxtn_p( void* user, int32_t* p_pos )
{
	long i = ftell( (FILE*)user );
	if( i < 0 ) return false;
	*p_pos = (int32_t)i;
	return true;
}

// ---- note data ----------------------------------------------------------

struct Note
{
	int32_t clock    ; // start time in clocks
	int32_t duration ; // length in clocks (0 if unknown)
	int32_t key      ; // EVENTKIND_KEY value
	int     unit     ;
};

struct VisualizerData
{
	std::vector<Note>   notes;
	int                 unit_num = 0;
	double              sec_per_clock = 60.0 / (EVENTDEFAULT_BEATTEMPO * EVENTDEFAULT_BEATCLOCK);
	int32_t             total_sample  = 0;   // song length in samples (0 = unknown)

	std::atomic<int64_t> played_samples {0}; // frames rendered by the audio callback
	std::atomic<bool>    b_quit         {false};
};

static VisualizerData g_data;

// SDL converts SIGINT to an SDL_QUIT event, so handle it ourselves.
static volatile sig_atomic_t _b_sigint = 0;
static void _sigint_handler( int ){ _b_sigint = 1; }

// ---- audio --------------------------------------------------------------

static void _sdl_audio_callback( void* userdata, Uint8* stream, int len )
{
	pxtnService* pxtn = static_cast<pxtnService*>( userdata );

	if( !pxtn->Moo( stream, len ) )
	{
		memset( stream, 0, len );
	}
	g_data.played_samples += len / ( _CHANNEL_NUM * sizeof(int16_t) );
}

// ---- load ---------------------------------------------------------------

static bool _load( pxtnService* pxtn, const char* path, std::string* p_err )
{
	FILE* fp = fopen( path, "rb" );
	if( !fp ){ *p_err = "cannot open file: " + std::string( path ); return false; }

	pxtnERR err = pxtn->read( fp );
	fclose( fp );
	if( err != pxtnOK ){ *p_err = pxtnError_get_string( err ); return false; }

	err = pxtn->tones_ready();
	if( err != pxtnOK ){ *p_err = pxtnError_get_string( err ); return false; }

	return true;
}

// Collect ON events (+ key at that point) into g_data.notes.
static void _collect_notes( pxtnService* pxtn )
{
	g_data.unit_num = pxtn->Unit_Num();

	// tempo (first BEATTEMPO event if any, else default)
	double tempo = EVENTDEFAULT_BEATTEMPO;
	int32_t beat_clock = pxtn->master->get_beat_clock();
	if( beat_clock <= 0 ) beat_clock = EVENTDEFAULT_BEATCLOCK;
	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind == EVENTKIND_BEATTEMPO ){ tempo = p->value; break; }
	}
	g_data.sec_per_clock = 60.0 / ( tempo * beat_clock );

	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON ) continue;

		Note note;
		note.clock    = p->clock;
		note.duration = p->value;
		note.unit     = p->unit_no;
		note.key      = pxtn->evels->get_Value( p->clock, p->unit_no, EVENTKIND_KEY );
		g_data.notes.push_back( note );
	}
}

// ---- drawing ------------------------------------------------------------

static void _unit_color( int unit, Uint8* r, Uint8* g, Uint8* b )
{
	static const Uint8 pal[][3] =
	{
		{  90, 200, 250 }, { 250, 120, 120 }, { 140, 240, 140 },
		{ 250, 220, 100 }, { 220, 140, 250 }, { 250, 170,  90 },
		{ 120, 250, 220 }, { 250, 120, 200 },
	};
	const Uint8* c = pal[ unit % 8 ];
	*r = c[0]; *g = c[1]; *b = c[2];
}

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: %s <file.ptcop|file.pttune>\n", argv[0] );
		return 1;
	}

	pxtnERR      pxtn_err = pxtnERR_VOID;
	std::string  err;

	if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_AUDIO ) != 0 )
	{
		fprintf( stderr, "ERROR: SDL_Init: %s\n", SDL_GetError() );
		return 1;
	}

	pxtnService* pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );

	pxtn_err = pxtn->init(); if( pxtn_err != pxtnOK ) goto term;
	if( !pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ) goto term;

	if( !_load( pxtn, argv[1], &err ) ) goto term;

	_collect_notes( pxtn );
	g_data.total_sample = pxtn->moo_get_total_sample();

	{
		pxtnVOMITPREPARATION prep = {0};
		prep.flags          |= pxtnVOMITPREPFLAG_loop;
		prep.start_pos_float = 0;
		prep.master_volume   = 0.80f;
		if( !pxtn->moo_preparation( &prep ) ) goto term;

		SDL_AudioSpec want = {0};
		want.freq     = _SAMPLE_PER_SECOND;
		want.format   = AUDIO_S16SYS;
		want.channels = _CHANNEL_NUM;
		want.samples  = 2048;
		want.callback = _sdl_audio_callback;
		want.userdata = pxtn;

		SDL_Window*   window = NULL;
		SDL_Renderer* rend   = NULL;
		const int     playhead_x = _WIN_W / 3;
		const double  px_per_sec = 150.0;

		if( SDL_OpenAudio( &want, NULL ) != 0 )
		{
			err = SDL_GetError();
			goto term;
		}
		if( SDL_CreateWindowAndRenderer( _WIN_W, _WIN_H, 0, &window, &rend ) != 0 )
		{
			err = SDL_GetError();
			goto term;
		}
		SDL_SetWindowTitle( window, "pxtone-visualizer" );

		signal( SIGINT, _sigint_handler );
		SDL_PauseAudio( 0 );

		while( !g_data.b_quit && !_b_sigint )
		{
			SDL_Event ev;
			while( SDL_PollEvent( &ev ) )
			{
				if( ev.type == SDL_QUIT ) g_data.b_quit = true;
				if( ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE ) g_data.b_quit = true;
			}

			// current playback position (seconds), respecting loop
			int64_t smp = g_data.played_samples;
			if( g_data.total_sample > 0 ) smp %= g_data.total_sample;
			double cur_sec = (double)smp / _SAMPLE_PER_SECOND;

			SDL_SetRenderDrawColor( rend, 16, 16, 24, 255 );
			SDL_RenderClear( rend );

			// unit lanes (separator lines)
			SDL_SetRenderDrawColor( rend, 40, 40, 56, 255 );
			for( int u = 1; u < g_data.unit_num; u++ )
			{
				int y = _WIN_H * u / g_data.unit_num;
				SDL_RenderDrawLine( rend, 0, y, _WIN_W, y );
			}

			// notes
			for( size_t i = 0; i < g_data.notes.size(); i++ )
			{
				const Note& n = g_data.notes[ i ];
				if( n.unit >= g_data.unit_num ) continue;

				double t0 = n.clock    * g_data.sec_per_clock;
				double t1 = ( n.clock + ( n.duration > 0 ? n.duration : 240 ) ) * g_data.sec_per_clock;

				double x0 = playhead_x + ( t0 - cur_sec ) * px_per_sec;
				double x1 = playhead_x + ( t1 - cur_sec ) * px_per_sec;
				if( x1 < 0 || x0 > _WIN_W ) continue;

				int lane_h = _WIN_H / ( g_data.unit_num > 0 ? g_data.unit_num : 1 );
				int y = _WIN_H * n.unit / ( g_data.unit_num > 0 ? g_data.unit_num : 1 );

				// pitch -> height inside the lane
				double key_rel = ( n.key - 0x2000 ) / (double)0x8000; // ~0..1
				if( key_rel < 0 ) key_rel = 0;
				if( key_rel > 1 ) key_rel = 1;
				int nh = 4 + (int)( ( lane_h - 12 ) * key_rel );
				if( nh > lane_h - 6 ) nh = lane_h - 6;

				int rx = (int)x0;
				int rw = (int)( x1 - x0 ); if( rw < 4 ) rw = 4;
				if( rx + rw < 0 || rx > _WIN_W ) continue;

				Uint8 r, g, b;
				_unit_color( n.unit, &r, &g, &b );

				// notes already played are dimmed
				if( x0 < playhead_x ){ r /= 3; g /= 3; b /= 3; }

				SDL_SetRenderDrawColor( rend, r, g, b, 255 );
				SDL_Rect rect = { rx, y + lane_h - nh - 2, rw, nh };
				SDL_RenderFillRect( rend, &rect );
			}

			// playhead
			SDL_SetRenderDrawColor( rend, 240, 240, 240, 255 );
			SDL_RenderDrawLine( rend, playhead_x, 0, playhead_x, _WIN_H );

			SDL_RenderPresent( rend );
			SDL_Delay( 16 );
		}

		SDL_DestroyRenderer( rend );
		SDL_DestroyWindow( window );
		SDL_CloseAudio();
		SDL_Quit();
	}

	printf( "stopping...\n" );
	return 0;

term:
	if( !err.empty() )
	{
		fprintf( stderr, "ERROR: %s\n", err.c_str() );
	}
	else if( pxtn_err != pxtnOK )
	{
		fprintf( stderr, "ERROR: pxtnERR[ %s ]\n", pxtnError_get_string( pxtn_err ) );
	}
	return 1;
}
