// pxtone-visualizer: GUI piano-roll style note visualizer for pxtone music.
// Links against the shared libpxtn.so core.
//
// Usage: pxtone-visualizer <file.ptcop|pttune>
//
// Display modes (switch with 1-4):
//   1: Lanes     - one lane per unit, simple blocks
//   2: Lanes+PR  - lanes with a pitch-accurate piano-roll overlay
//   3: Roll V    - full-screen piano roll, notes flow downward
//   4: Roll H    - full-screen piano roll, notes flow right-to-left
//
// Close window / Ctrl-C / ESC to stop. Up/Down adjusts speed in roll modes.

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

enum DisplayMode
{
	MODE_LANES    = 0,
	MODE_LANES_PR = 1,
	MODE_ROLL_V   = 2,
	MODE_ROLL_H   = 3,
	MODE_NUM      = 4,
};

static const char* _mode_name[] =
{
	"Lanes", "Lanes + PianoRoll", "PianoRoll Vertical", "PianoRoll Horizontal",
};

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

// ---- data ---------------------------------------------------------------

struct Note
{
	int32_t clock    ; // start time in clocks
	int32_t duration ; // length in clocks (EVENTKIND_ON value)
	int32_t key      ; // EVENTKIND_KEY value (0xOOOO = octave/note)
	int     unit     ;
};

struct VisualizerData
{
	std::vector<Note>   notes;
	int                 unit_num = 0;
	double              tempo         = EVENTDEFAULT_BEATTEMPO;
	double              sec_per_clock = 60.0 / (EVENTDEFAULT_BEATTEMPO * EVENTDEFAULT_BEATCLOCK);
	int32_t             total_sample  = 0;   // song length in samples (0 = unknown)
	int                 key_min       = 0x4000;
	int                 key_max       = 0x8000;

	std::atomic<int64_t> played_samples {0}; // frames rendered by the audio callback
	std::atomic<bool>    b_quit         {false};
};

static VisualizerData g_data;

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

static void _collect_notes( pxtnService* pxtn )
{
	g_data.unit_num = pxtn->Unit_Num();

	int32_t beat_clock = pxtn->master->get_beat_clock();
	if( beat_clock <= 0 ) beat_clock = EVENTDEFAULT_BEATCLOCK;
	g_data.tempo = pxtn->master->get_beat_tempo();
	if( g_data.tempo <= 0 ) g_data.tempo = EVENTDEFAULT_BEATTEMPO;
	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind == EVENTKIND_BEATTEMPO ){ g_data.tempo = p->value; break; }
	}
	g_data.sec_per_clock = 60.0 / ( g_data.tempo * beat_clock );

	g_data.key_min = 0x7fffffff;
	g_data.key_max = 0x80000000;

	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON ) continue;

		Note note;
		note.clock    = p->clock;
		note.duration = p->value;
		note.unit     = p->unit_no;
		note.key      = pxtn->evels->get_Value( p->clock, p->unit_no, EVENTKIND_KEY );
		g_data.notes.push_back( note );

		int row = note.key >> 8;
		if( row < g_data.key_min ) g_data.key_min = row;
		if( row > g_data.key_max ) g_data.key_max = row;
	}
	if( g_data.key_min == 0x7fffffff ){ g_data.key_min = 0x40; g_data.key_max = 0x80; }
	g_data.key_min -= 2;   // small margin
	g_data.key_max += 2;
}

// ---- helpers ------------------------------------------------------------

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

// approximate semitone row of a KEY value (0x4500 = A4 -> row 0x45)
static int _key_row( int32_t key ){ return key >> 8; }

static bool _is_black_key( int row )
{
	static const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
	return black[ ((row % 12) + 12) % 12 ];
}

struct ViewInfo
{
	double cur_sec      ;
	double sec_per_beat ;
};

static ViewInfo _view_info()
{
	ViewInfo v;
	int64_t smp = g_data.played_samples;
	if( g_data.total_sample > 0 ) smp %= g_data.total_sample;
	v.cur_sec      = (double)smp / _SAMPLE_PER_SECOND;
	v.sec_per_beat = 60.0 / g_data.tempo;
	return v;
}

// ---- mode 1: plain lanes -------------------------------------------------

static void _draw_lanes( SDL_Renderer* rend, const ViewInfo& v, double px_per_sec )
{
	const int playhead_x = _WIN_W / 3;
	const int lane_h = _WIN_H / ( g_data.unit_num > 0 ? g_data.unit_num : 1 );

	SDL_SetRenderDrawColor( rend, 40, 40, 56, 255 );
	for( int u = 1; u < g_data.unit_num; u++ )
		SDL_RenderDrawLine( rend, 0, _WIN_H * u / g_data.unit_num, _WIN_W, _WIN_H * u / g_data.unit_num );

	for( const Note& n : g_data.notes )
	{
		if( n.unit >= g_data.unit_num ) continue;

		double t0 = n.clock * g_data.sec_per_clock;
		double t1 = ( n.clock + ( n.duration > 0 ? n.duration : 240 ) ) * g_data.sec_per_clock;
		double x0 = playhead_x + ( t0 - v.cur_sec ) * px_per_sec;
		double x1 = playhead_x + ( t1 - v.cur_sec ) * px_per_sec;
		if( x1 < 0 || x0 > _WIN_W ) continue;

		int y = _WIN_H * n.unit / ( g_data.unit_num > 0 ? g_data.unit_num : 1 );

		double key_rel = ( _key_row( n.key ) - g_data.key_min ) /
			(double)( g_data.key_max - g_data.key_min );
		int nh = 4 + (int)( ( lane_h - 12 ) * key_rel );
		if( nh > lane_h - 6 ) nh = lane_h - 6;

		int rx = (int)x0;
		int rw = (int)( x1 - x0 ); if( rw < 4 ) rw = 4;

		Uint8 r, g, b;
		_unit_color( n.unit, &r, &g, &b );
		if( x0 < playhead_x ){ r /= 3; g /= 3; b /= 3; }

		SDL_SetRenderDrawColor( rend, r, g, b, 255 );
		SDL_Rect rect = { rx, y + lane_h - nh - 2, rw, nh };
		SDL_RenderFillRect( rend, &rect );
	}
}

// ---- mode 2: lanes + pitch-accurate piano-roll overlay -------------------

static void _draw_lane_pianoroll( SDL_Renderer* rend, const ViewInfo& v, double px_per_sec )
{
	const int playhead_x = _WIN_W / 3;
	const int lane_h = _WIN_H / ( g_data.unit_num > 0 ? g_data.unit_num : 1 );
	const int rows   = g_data.key_max - g_data.key_min + 1;
	const float row_h = (float)lane_h / rows;

	// background stripes: white/black key rows (per lane)
	for( int u = 0; u < g_data.unit_num; u++ )
	{
		for( int i = 0; i < rows; i++ )
		{
			if( !_is_black_key( g_data.key_min + i ) ) continue;
			SDL_SetRenderDrawColor( rend, 26, 26, 36, 255 );
			SDL_Rect rect = { 0, (int)(_WIN_H * u / g_data.unit_num + i * row_h), _WIN_W, (int)row_h + 1 };
			SDL_RenderFillRect( rend, &rect );
		}
		// octave separator lines (every 12 rows from C)
		SDL_SetRenderDrawColor( rend, 50, 50, 70, 255 );
		for( int i = 0; i <= rows; i++ )
		{
			if( ( g_data.key_min + i ) % 12 != 0 ) continue;
			int y = (int)(_WIN_H * u / g_data.unit_num + i * row_h);
			SDL_RenderDrawLine( rend, 0, y, _WIN_W, y );
		}
		// lane border
		SDL_SetRenderDrawColor( rend, 80, 80, 110, 255 );
		SDL_RenderDrawLine( rend, 0, _WIN_H * u / g_data.unit_num, _WIN_W, _WIN_H * u / g_data.unit_num );
	}
	SDL_SetRenderDrawColor( rend, 80, 80, 110, 255 );
	SDL_RenderDrawLine( rend, 0, _WIN_H - 1, _WIN_W, _WIN_H - 1 );

	for( const Note& n : g_data.notes )
	{
		if( n.unit >= g_data.unit_num ) continue;

		double t0 = n.clock * g_data.sec_per_clock;
		double t1 = ( n.clock + ( n.duration > 0 ? n.duration : 240 ) ) * g_data.sec_per_clock;
		double x0 = playhead_x + ( t0 - v.cur_sec ) * px_per_sec;
		double x1 = playhead_x + ( t1 - v.cur_sec ) * px_per_sec;
		if( x1 < 0 || x0 > _WIN_W ) continue;

		float rh = row_h < 3 ? 3 : row_h;
		int y = (int)( _WIN_H * n.unit / g_data.unit_num +
			( rows - 1 - ( _key_row( n.key ) - g_data.key_min ) ) * row_h +
			( row_h - rh ) / 2 );

		int rx = (int)x0;
		int rw = (int)( x1 - x0 ); if( rw < 4 ) rw = 4;

		Uint8 r, g, b;
		_unit_color( n.unit, &r, &g, &b );
		if( x0 < playhead_x ){ r /= 3; g /= 3; b /= 3; }

		SDL_SetRenderDrawColor( rend, r, g, b, 255 );
		SDL_Rect rect = { rx, y, rw, (int)rh };
		SDL_RenderFillRect( rend, &rect );
	}
}

// ---- modes 3/4: full-screen scrolling piano roll --------------------------

static void _draw_full_pianoroll( SDL_Renderer* rend, const ViewInfo& v,
                                  double px_per_sec, bool b_vertical )
{
	const int rows = g_data.key_max - g_data.key_min + 1;
	const int span = b_vertical ? _WIN_W : _WIN_H;
	const float row_size = (float)span / rows;
	const int playhead = ( b_vertical ? _WIN_H : _WIN_W ) / 4;

	// background: black-key stripes + octave lines
	for( int i = 0; i < rows; i++ )
	{
		int row = g_data.key_min + i;
		if( _is_black_key( row ) )
		{
			SDL_SetRenderDrawColor( rend, 26, 26, 36, 255 );
			SDL_Rect rect = b_vertical
				? SDL_Rect{ 0, 0, (int)( i * row_size ), _WIN_H }        // x axis = pitch
				: SDL_Rect{ 0, (int)( span - ( i + 1 ) * row_size ), _WIN_W, (int)row_size + 1 };
			SDL_RenderFillRect( rend, &rect );
		}
		if( row % 12 == 0 )
		{
			SDL_SetRenderDrawColor( rend, 50, 50, 70, 255 );
			if( b_vertical ) SDL_RenderDrawLine( rend, (int)( (i + 1) * row_size ), 0, (int)( (i + 1) * row_size ), _WIN_H );
			else             SDL_RenderDrawLine( rend, 0, (int)( span - i * row_size ), _WIN_W, (int)( span - i * row_size ) );
		}
	}

	// beat grid lines
	SDL_SetRenderDrawColor( rend, 38, 38, 54, 255 );
	{
		double first_beat = floor( v.cur_sec / v.sec_per_beat ) * v.sec_per_beat;
		for( double t = first_beat; ; t += v.sec_per_beat )
		{
			double d = ( t - v.cur_sec ) * px_per_sec;
			if( b_vertical )
			{
				int y = playhead + (int)d;
				if( y > _WIN_H ) break;
				if( y >= 0 ) SDL_RenderDrawLine( rend, 0, y, _WIN_W, y );
			}
			else
			{
				int x = playhead + (int)d;
				if( x > _WIN_W ) break;
				if( x >= 0 ) SDL_RenderDrawLine( rend, x, 0, x, _WIN_H );
			}
		}
	}

	auto row_to_y = [&]( int row ) -> int  // top/left coordinate of the row rect
	{
		int idx = row - g_data.key_min;
		return b_vertical ? (int)( idx * row_size )
		                  : (int)( span - ( idx + 1 ) * row_size );
	};

	for( const Note& n : g_data.notes )
	{
		double t0 = n.clock * g_data.sec_per_clock;
		double t1 = ( n.clock + ( n.duration > 0 ? n.duration : 240 ) ) * g_data.sec_per_clock;
		double d0 = ( t0 - v.cur_sec ) * px_per_sec;
		double d1 = ( t1 - v.cur_sec ) * px_per_sec;
		if( d1 < 0 || d0 > ( b_vertical ? _WIN_H : _WIN_W ) ) continue;

		int pos, len;
		if( b_vertical ){ pos = playhead + (int)d0; len = (int)( d1 - d0 ); }
		else            { pos = playhead + (int)d0; len = (int)( d1 - d0 ); }
		if( len < 4 ) len = 4;

		int thick = (int)row_size; if( thick < 3 ) thick = 3;

		int rx, ry, rw, rh;
		if( b_vertical ){ rx = row_to_y( _key_row( n.key ) ); ry = pos; rw = thick; rh = len; }
		else            { rx = pos; ry = row_to_y( _key_row( n.key ) ); rw = len; rh = thick; }

		Uint8 r, g, b;
		_unit_color( n.unit, &r, &g, &b );
		bool played = b_vertical ? ( ry < playhead ) : ( rx < playhead );
		if( played ){ r /= 3; g /= 3; b /= 3; }

		SDL_SetRenderDrawColor( rend, r, g, b, 255 );
		SDL_Rect rect = { rx, ry, rw, rh };
		SDL_RenderFillRect( rend, &rect );
	}

	// playhead
	SDL_SetRenderDrawColor( rend, 240, 240, 240, 255 );
	if( b_vertical ) SDL_RenderDrawLine( rend, 0, playhead, _WIN_W, playhead );
	else             SDL_RenderDrawLine( rend, playhead, 0, playhead, _WIN_H );
}

// ---- main ----------------------------------------------------------------

// SDL converts SIGINT to an SDL_QUIT event, so handle it ourselves.
static volatile sig_atomic_t _b_sigint = 0;
static void _sigint_handler( int ){ _b_sigint = 1; }

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
		if( !pxtn->moo_preparation( &prep ) )
		{
			err = "moo_preparation failed";
			goto term;
		}

		SDL_AudioSpec want = {0};
		want.freq     = _SAMPLE_PER_SECOND;
		want.format   = AUDIO_S16SYS;
		want.channels = _CHANNEL_NUM;
		want.samples  = 2048;
		want.callback = _sdl_audio_callback;
		want.userdata = pxtn;

		SDL_Window*   window = NULL;
		SDL_Renderer* rend   = NULL;
		int           disp_mode = MODE_LANES_PR;
		double        speed     = 1.0;   // px_per_sec = 150 * speed

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

		signal( SIGINT, _sigint_handler );
		SDL_PauseAudio( 0 );

		while( !g_data.b_quit && !_b_sigint )
		{
			SDL_Event ev;
			while( SDL_PollEvent( &ev ) )
			{
				if( ev.type == SDL_QUIT ) g_data.b_quit = true;
				if( ev.type == SDL_KEYDOWN )
				{
					SDL_Keycode k = ev.key.keysym.sym;
					if( k == SDLK_ESCAPE ) g_data.b_quit = true;
					if( k >= SDLK_1 && k < SDLK_1 + MODE_NUM ) disp_mode = k - SDLK_1;
					if( k == SDLK_UP   && speed < 8.0 ) speed *= 1.25;
					if( k == SDLK_DOWN && speed > 0.05 ) speed /= 1.25;
				}
			}
			{
				char title[ 512 ];
				snprintf( title, sizeof( title ), "pxtone-visualizer  [%d/%d %s]  speed x%.2f (Up/Down)",
					disp_mode + 1, MODE_NUM, _mode_name[ disp_mode ], speed );
				SDL_SetWindowTitle( window, title );
			}

			ViewInfo v = _view_info();
			double px_per_sec = 150.0 * speed;

			SDL_SetRenderDrawColor( rend, 16, 16, 24, 255 );
			SDL_RenderClear( rend );

			switch( disp_mode )
			{
			case MODE_LANES:    _draw_lanes         ( rend, v, px_per_sec ); break;
			case MODE_LANES_PR: _draw_lane_pianoroll( rend, v, px_per_sec ); break;
			case MODE_ROLL_V:   _draw_full_pianoroll( rend, v, px_per_sec, true  ); break;
			case MODE_ROLL_H:   _draw_full_pianoroll( rend, v, px_per_sec, false ); break;
			}

			// common playhead for lane modes
			if( disp_mode == MODE_LANES || disp_mode == MODE_LANES_PR )
			{
				SDL_SetRenderDrawColor( rend, 240, 240, 240, 255 );
				int ph = _WIN_W / 3;
				SDL_RenderDrawLine( rend, ph, 0, ph, _WIN_H );
			}

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
