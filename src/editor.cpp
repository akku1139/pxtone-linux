// pxtone-editor: simple GTK4 piano-roll editor for .ptcop files.
// Links against the shared libpxtn.so core.
//
// Usage: pxtone-editor <file.ptcop>
//
// - Left-click / drag : add a note (snapped) and stretch its length
// - Right-click       : delete a note
// - Wheel / Shift+Wheel / Ctrl+Wheel : scroll V / scroll H / zoom
// - Space : play / stop,  Ctrl+S : save (overwrite)
// - 1-4 : note snap (quarter / 8th / 16th / 32th)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <SDL.h>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static const int      _CHANNEL_NUM       = 2;
static const int32_t  _SAMPLE_PER_SECOND = 44100;

// pitch rows: 0x24 (C2) .. 0x94 (B8)
static const int _ROW_MIN = 0x24;
static const int _ROW_MAX = 0x94;
static const int _ROW_H   = 14;   // px per semitone row

static const int32_t _BEAT_CLOCK = 480; // assumed grid base

// ---- I/O callbacks ------------------------------------------------------

static bool _pxtn_r( void* u, void* p, int s, int n ){ return fread( p, s, n, (FILE*)u ) >= n; }
static bool _pxtn_w( void* u, const void* p, int s, int n ){ return fwrite( p, s, n, (FILE*)u ) >= n; }
static bool _pxtn_s( void* u, int m, int s ){ return !fseek( (FILE*)u, s, m ); }
static bool _pxtn_p( void* u, int32_t* p ){ long i = ftell( (FILE*)u ); if( i < 0 ) return false; *p = (int32_t)i; return true; }

// ---- editor state -------------------------------------------------------

enum DragMode { DRAG_NONE = 0, DRAG_RESIZE, DRAG_MOVE };

struct Editor
{
	pxtnService* pxtn       = NULL;
	std::string  path       ;
	std::string  err        ;
	bool         loaded     = false;
	int          unit_num   = 0;
	double       tempo      = EVENTDEFAULT_BEATTEMPO;

	// view
	double  px_per_clock = 80.0 / _BEAT_CLOCK;
	double  h_offset     = 0;   // px
	double  v_offset     = 0;   // px
	int     snap         = 240; // clocks (8th note)

	// edit drag
	bool    dragging   = false;
	int32_t drag_clock = 0;
	int32_t drag_unit  = 0;

	// note resize / move (drag left/right on an existing note)
	DragMode mode        = DRAG_NONE;
	int32_t drag_orig_clock = 0; // note's original start clock
	int32_t drag_cur_clock  = 0; // current position while moving
	int     drag_cur_row    = 0;
	int32_t drag_dur        = 0;
	int32_t drag_key        = 0;

	// selection (last touched note)
	bool    has_sel   = false;
	int32_t sel_clock = 0;
	int     sel_unit  = 0;

	// playback
	std::atomic<int64_t> played_samples {0};
	std::atomic<bool>    playing        {false};

	// preview (note audition)
	SDL_AudioDeviceID preview_dev = 0;

	// widgets
	GtkWidget* window    = NULL;
	GtkWidget* draw_area = NULL;
	GtkWidget* unit_combo = NULL;
	GtkWidget* snap_combo = NULL;
	GtkWidget* status     = NULL;
};

static Editor g_ed;

static const double PI = 3.141592653589793;

static void _preview_note( int unit, int row ); // fwd

// ---- undo/redo (project snapshots) --------------------------------------

static void _set_status( const char* fmt, ... ); // fwd

struct SongSnap
{
	std::vector<EVERECORD> eves;
	int32_t beat_num, beat_clock, meas_num, repeat_meas, last_meas;
	float   tempo;
};
static std::vector<SongSnap> g_undo, g_redo;

struct ClipNote { int32_t rel_clock; int unit; int32_t key_row; int32_t dur; };
static std::vector<ClipNote> g_clipboard;

static SongSnap _snapshot()
{
	SongSnap s{};
	for( const EVERECORD* p = g_ed.pxtn->evels->get_Records(); p; p = p->next ) s.eves.push_back( *p );
	pxtnMaster* m = g_ed.pxtn->master;
	s.beat_num    = m->get_beat_num();
	s.beat_clock  = m->get_beat_clock();
	s.tempo       = m->get_beat_tempo();
	s.meas_num    = m->get_meas_num();
	s.repeat_meas = m->get_repeat_meas();
	s.last_meas   = m->get_last_meas();
	return s;
}

static void _restore_snapshot( const SongSnap& s )
{
	g_ed.pxtn->evels->Allocate( s.eves.size() + 4096 );
	for( const EVERECORD& r : s.eves )
		g_ed.pxtn->evels->Record_Add_i( r.clock, r.unit_no, r.kind, r.value );
	g_ed.pxtn->master->Set( s.beat_num, s.tempo, s.beat_clock );
	g_ed.pxtn->master->set_meas_num   ( s.meas_num );
	g_ed.pxtn->master->set_repeat_meas( s.repeat_meas );
	g_ed.pxtn->master->set_last_meas  ( s.last_meas );
	if( s.tempo > 0 ) g_ed.tempo = s.tempo;
}

static void _push_undo()
{
	g_undo.push_back( _snapshot() );
	if( g_undo.size() > 100 ) g_undo.erase( g_undo.begin() );
	g_redo.clear();
}

static void _undo()
{
	if( g_undo.empty() ) return;
	g_redo.push_back( _snapshot() );
	SDL_LockAudio();
	_restore_snapshot( g_undo.back() );
	SDL_UnlockAudio();
	g_undo.pop_back();
	_set_status( "undo (%d left)", (int)g_undo.size() );
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _redo()
{
	if( g_redo.empty() ) return;
	g_undo.push_back( _snapshot() );
	SDL_LockAudio();
	_restore_snapshot( g_redo.back() );
	SDL_UnlockAudio();
	g_redo.pop_back();
	_set_status( "redo" );
	gtk_widget_queue_draw( g_ed.draw_area );
}

// ---- helpers ------------------------------------------------------------

static void _set_status( const char* fmt, ... )
{
	char buf[ 512 ];
	va_list ap; va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	gtk_label_set_text( GTK_LABEL( g_ed.status ), buf );
}

static double _sec_per_clock()
{
	return 60.0 / ( g_ed.tempo * _BEAT_CLOCK );
}

static int32_t _snap_clock( int32_t clock )
{
	return ( clock / g_ed.snap ) * g_ed.snap;
}

static void Record_Value_Set_safe( pxtnEvelist* ev, int32_t clock, int unit, int32_t dur )
{
	if( dur < 1 ) dur = 1;
	ev->Record_Value_Set( clock, clock + 1, (uint8_t)unit, EVENTKIND_ON, dur );
}

// After changing an ON event's length, resolve overlaps like Record_Add_i does:
// shorten the previous note ending inside ours, delete notes starting inside ours.
static void _fix_overlaps( int32_t clock, int unit, int32_t dur )
{
	pxtnEvelist* ev = g_ed.pxtn->evels;

	std::vector<int32_t> inside;
	int32_t prev_clock = -1;

	for( const EVERECORD* p = ev->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON || p->unit_no != unit || p->clock == clock ) continue;
		if( p->clock < clock && p->clock + p->value > clock ) prev_clock = p->clock; // overlaps our start
		if( p->clock > clock && p->clock < clock + dur ) inside.push_back( p->clock );    // starts inside our span
	}

	if( prev_clock >= 0 )
		Record_Value_Set_safe( ev, prev_clock, unit, clock - prev_clock );
	for( int32_t c : inside )
		ev->Record_Delete( c, c + 1, (uint8_t)unit, EVENTKIND_ON );
}

static void _unit_color( int unit, double* r, double* g, double* b )
{
	static const double pal[][3] =
	{
		{ 0.35, 0.78, 0.98 }, { 0.98, 0.47, 0.47 }, { 0.55, 0.94, 0.55 },
		{ 0.98, 0.86, 0.39 }, { 0.86, 0.55, 0.98 }, { 0.98, 0.67, 0.35 },
		{ 0.47, 0.98, 0.86 }, { 0.98, 0.47, 0.78 },
	};
	const double* c = pal[ unit % 8 ];
	*r = c[0]; *g = c[1]; *b = c[2];
}

// Make sure there is room for new events (Allocate wipes, so re-record).
static void _ensure_evels_capacity()
{
	pxtnService* pxtn = g_ed.pxtn;
	int count = 0;
	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next ) count++;

	if( pxtn->evels->get_Num_Max() >= count + 4096 ) return;

	std::vector<EVERECORD> recs;
	recs.reserve( count );
	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next )
		recs.push_back( *p );

	pxtn->evels->Allocate( count + 4096 );
	for( const EVERECORD& r : recs )
		pxtn->evels->Record_Add_i( r.clock, r.unit_no, r.kind, r.value );
}

// ---- audio --------------------------------------------------------------

static void _sdl_audio_callback( void*, Uint8* stream, int len )
{
	if( !g_ed.pxtn || !g_ed.playing ) { memset( stream, 0, len ); return; }
	if( !g_ed.pxtn->Moo( stream, len ) ) memset( stream, 0, len );
	g_ed.played_samples += len / ( _CHANNEL_NUM * sizeof(int16_t) );
}

static void _start_play()
{
	if( !g_ed.loaded || g_ed.playing ) return;

	pxtnVOMITPREPARATION prep = {0};
	prep.flags          |= pxtnVOMITPREPFLAG_loop;
	prep.start_pos_float = 0;
	prep.master_volume   = 0.80f;
	if( !g_ed.pxtn->moo_preparation( &prep ) ){ _set_status( "play preparation failed" ); return; }

	g_ed.played_samples = 0;
	g_ed.playing = true;
	SDL_PauseAudio( 0 );
	_set_status( "playing" );
}

static void _stop_play()
{
	if( !g_ed.playing ) return;
	g_ed.playing = false;
	SDL_PauseAudio( 1 );
	_set_status( "stopped" );
	gtk_widget_queue_draw( g_ed.draw_area );
}

// ---- editing ------------------------------------------------------------

static bool _screen_to_clock_row( int x, int y, int* p_clock, int* p_row )
{
	int32_t clock = (int32_t)( ( g_ed.h_offset + x ) / g_ed.px_per_clock );
	int row = _ROW_MAX - (int)floor( ( y + g_ed.v_offset ) / (double)_ROW_H );
	if( row < _ROW_MIN || row > _ROW_MAX ) return false;
	if( clock < 0 ) return false;
	*p_clock = clock; *p_row = row;
	return true;
}

static void _add_note( int32_t clock, int row )
{
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;

	int32_t c = _snap_clock( clock );

	SDL_LockAudio();
	_push_undo();
	_ensure_evels_capacity();

	// set key only if it differs from the effective key at that point
	if( g_ed.pxtn->evels->get_Value( c, (uint8_t)unit, EVENTKIND_KEY ) != ( row << 8 ) )
	{
		g_ed.pxtn->evels->Record_Delete( c, c + 1, (uint8_t)unit, EVENTKIND_KEY );
		g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, EVENTKIND_KEY, row << 8 );
	}
	g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, EVENTKIND_ON, g_ed.snap );
	SDL_UnlockAudio();

	_preview_note( unit, row );

	g_ed.dragging   = true;
	g_ed.drag_clock = c;
	g_ed.drag_unit  = unit;
	g_ed.mode       = DRAG_NONE;

	g_ed.has_sel   = true;
	g_ed.sel_clock = c;
	g_ed.sel_unit  = unit;

	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _drag_update( int x, int y )
{
	pxtnEvelist* ev = g_ed.pxtn->evels;
	int32_t c = _snap_clock( (int32_t)( ( g_ed.h_offset + x ) / g_ed.px_per_clock ) );
	int32_t max = ev->get_Max_Clock() + g_ed.snap * 64;
	if( c < 0 ) c = 0;

	if( g_ed.mode == DRAG_RESIZE )
	{
		// resize: dragging left/right on an existing note changes its length
		int32_t dur = c - g_ed.drag_orig_clock;
		if( dur < g_ed.snap ) dur = g_ed.snap;
		if( dur > max ) dur = max;

		SDL_LockAudio();
		Record_Value_Set_safe( ev, g_ed.drag_orig_clock, g_ed.drag_unit, dur );
		_fix_overlaps( g_ed.drag_orig_clock, g_ed.drag_unit, dur );
		SDL_UnlockAudio();
		g_ed.drag_dur = dur;
	}
	else if( g_ed.mode == DRAG_MOVE )
	{
		// move: the note follows the cursor (snapped clock + pitch row)
		int row = _ROW_MAX - (int)floor( ( y + g_ed.v_offset ) / (double)_ROW_H );
		if( row < _ROW_MIN ) row = _ROW_MIN;
		if( row > _ROW_MAX ) row = _ROW_MAX;
		if( c == g_ed.drag_cur_clock && row == g_ed.drag_cur_row ) return;

		SDL_LockAudio();
		ev->Record_Delete( g_ed.drag_cur_clock, g_ed.drag_cur_clock + 1, (uint8_t)g_ed.drag_unit, EVENTKIND_ON );
		ev->Record_Delete( g_ed.drag_cur_clock, g_ed.drag_cur_clock + 1, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY );
		if( ev->get_Value( c, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY ) != ( g_ed.drag_key << 8 ) )
		{
			ev->Record_Delete( c, c + 1, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY );
			ev->Record_Add_i( c, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY, g_ed.drag_key << 8 );
		}
		ev->Record_Add_i( c, (uint8_t)g_ed.drag_unit, EVENTKIND_ON, g_ed.drag_dur );
		_fix_overlaps( c, g_ed.drag_unit, g_ed.drag_dur );
		SDL_UnlockAudio();

		g_ed.drag_cur_clock = c;
		g_ed.drag_cur_row   = row;
	}
	else if( g_ed.dragging )
	{
		// stretch the note created at drag start
		int32_t dur = c - g_ed.drag_clock;
		if( dur < g_ed.snap ) dur = g_ed.snap;
		if( dur > max ) dur = max;

		SDL_LockAudio();
		Record_Value_Set_safe( ev, g_ed.drag_clock, g_ed.drag_unit, dur );
		_fix_overlaps( g_ed.drag_clock, g_ed.drag_unit, dur );
		SDL_UnlockAudio();
		g_ed.drag_dur = dur;
	}
	else return;

	gtk_widget_queue_draw( g_ed.draw_area );
}

// Find the ON event under the cursor (clock position + pitch row).
static const EVERECORD* _find_note( int32_t clock, int row, int unit )
{
	for( const EVERECORD* p = g_ed.pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON || p->unit_no != unit ) continue;
		if( p->clock > clock ) break; // records are sorted by clock
		int32_t dur = p->value > 0 ? p->value : g_ed.snap;
		if( clock >= p->clock && clock < p->clock + dur )
		{
			int k = g_ed.pxtn->evels->get_Value( p->clock, (uint8_t)unit, EVENTKIND_KEY ) >> 8;
			if( k == row ) return p;
		}
	}
	return NULL;
}

static void _delete_note( int32_t clock, int row )
{
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;

	const EVERECORD* hit = _find_note( clock, row, unit );
	if( !hit ) return;

	SDL_LockAudio();
	_push_undo();
	g_ed.pxtn->evels->Record_Delete( hit->clock, hit->clock + 1, (uint8_t)unit, EVENTKIND_ON );
	SDL_UnlockAudio();

	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _save()
{
	if( !g_ed.loaded ) return;
	FILE* fp = fopen( g_ed.path.c_str(), "wb" );
	if( !fp ){ _set_status( "ERROR: cannot write %s", g_ed.path.c_str() ); return; }
	pxtnERR err = g_ed.pxtn->write( fp, false, 0x0500 ); // b_tune=false: .ptcop project format (rough=1, lossless)
	fclose( fp );
	if( err != pxtnOK ){ _set_status( "ERROR: %s", pxtnError_get_string( err ) ); return; }
	_set_status( "saved: %s", g_ed.path.c_str() );
}

// ---- note preview (audition) --------------------------------------------

// Render ~0.35s of the unit's woice pitched to the key row and queue it.
static void _preview_note( int unit, int row )
{
	if( !g_ed.preview_dev || unit < 0 || unit >= g_ed.unit_num ) return;

	const pxtnWoice* woice = g_ed.pxtn->Unit_Get( unit )->get_woice();
	if( !woice ) return;

	const double PI = 3.141592653589793;
	const int32_t SPSEC = _SAMPLE_PER_SECOND;
	const int dur_frames = SPSEC * 35 / 100; // 0.35s

	std::vector<float> buf( dur_frames * 2, 0.0f );

	for( int v = 0; v < woice->get_voice_num(); v++ )
	{
		const pxtnVOICEINSTANCE* vi = woice->get_instance( v );
		const pxtnVOICEUNIT*     vc = woice->get_voice( v );
		if( !vi->p_smp_w || vi->smp_body_w <= 0 ) continue;

		double ratio = pow( 2.0, ( row - ( vc->basic_key >> 8 ) ) / 12.0 );
		double vol   = vc->volume / 128.0;
		const int16_t* p = (const int16_t*)vi->p_smp_w;

		for( int i = 0; i < dur_frames; i++ )
		{
			int64_t si = (int64_t)( i * ratio ) % vi->smp_body_w;
			double env = 1.0;
			if( i < SPSEC * 3 / 1000 ) env = i / (double)( SPSEC * 3 / 1000 );          // 3ms attack
			if( i > dur_frames - SPSEC / 10 ) env = ( dur_frames - i ) / (double)( SPSEC / 10 ); // 100ms release
			buf[ i * 2 + 0 ] += p[ si * 2 + 0 ] / 32768.0f * vol * env;
			buf[ i * 2 + 1 ] += p[ si * 2 + 1 ] / 32768.0f * vol * env;
		}
	}

	std::vector<int16_t> out( dur_frames * 2 );
	for( int i = 0; i < dur_frames * 2; i++ )
	{
		double s = buf[ i ] * 0.6;
		if( s >  1 ) s =  1;
		if( s < -1 ) s = -1;
		out[ i ] = (int16_t)( s * 32767 );
	}
	SDL_ClearQueuedAudio( g_ed.preview_dev );
	if( SDL_QueueAudio( g_ed.preview_dev, out.data(), out.size() * sizeof( int16_t ) ) != 0 )
		_set_status( "preview queue error: %s", SDL_GetError() );
}

// ---- track (unit) add ---------------------------------------------------

static void _refresh_unit_combo()
{
	GtkStringList* list = GTK_STRING_LIST( gtk_drop_down_get_model( GTK_DROP_DOWN( g_ed.unit_combo ) ) );
	guint n = g_list_model_get_n_items( G_LIST_MODEL( list ) );
	gtk_string_list_splice( list, 0, n, NULL ); // remove all
	for( int i = 0; i < g_ed.unit_num; i++ )
	{
		int32_t size = 0;
		const char* name = g_ed.pxtn->Unit_Get( i )->get_name_buf( &size );
		gtk_string_list_append( list, name && name[0] ? name : "(no name)" );
	}
	if( g_ed.unit_num > 0 ) gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.unit_combo ), g_ed.unit_num - 1 );
}

static void _add_unit()
{
	if( !g_ed.loaded ) return;
	if( !g_ed.pxtn->Unit_AddNew() ){ _set_status( "unit max reached" ); return; }
	int idx = g_ed.pxtn->Unit_Num() - 1;

	// assign woice 0 (create one if the tune has none)
	if( g_ed.pxtn->Woice_Num() == 0 )
	{
		int w = g_ed.pxtn->Woice_AddNew();
		g_ed.pxtn->Woice_Get_variable( w )->Voice_Allocate( 1 );
		g_ed.pxtn->Woice_ReadyTone( w );
	}
	g_ed.pxtn->Unit_Get_variable( idx )->set_woice( g_ed.pxtn->Woice_Get( 0 ) );

	char name[ 32 ]; snprintf( name, sizeof( name ), "unit %d", idx );
	g_ed.pxtn->Unit_Get_variable( idx )->set_name_buf( name, strlen( name ) );

	_ensure_evels_capacity();
	_refresh_unit_combo();
	_set_status( "added %s", name );
}

// ---- sound creation -------------------------------------------------------

static void _make_wave_points( int type, pxtnPOINT* pts, int32_t* p_num )
{
	// coordinate-wave presets, y in [-100,100]
	switch( type )
	{
	case 1: // saw
		pts[0].x =     0; pts[0].y = -100;
		pts[1].x =  9999; pts[1].y =  100; *p_num = 2; break;
	case 2: // square
		pts[0].x =    0; pts[0].y =  100;
		pts[1].x = 4999; pts[1].y =  100;
		pts[2].x = 5000; pts[2].y = -100;
		pts[3].x = 9999; pts[3].y = -100; *p_num = 4; break;
	case 3: // triangle
		pts[0].x =    0; pts[0].y = -100;
		pts[1].x = 4999; pts[1].y =  100;
		pts[2].x = 9999; pts[2].y = -100; *p_num = 3; break;
	case 4: // pulse 1/4
		pts[0].x =    0; pts[0].y =  100;
		pts[1].x = 2499; pts[1].y =  100;
		pts[2].x = 2500; pts[2].y = -100;
		pts[3].x = 9999; pts[3].y = -100; *p_num = 4; break;
	default: // sine
		*p_num = 32;
		for( int i = 0; i < 32; i++ )
		{
			pts[ i ].x = (int)( 10000.0 * i / 32 );
			pts[ i ].y = (int)( sin( 2.0 * PI * i / 32 ) * 100 );
		}
		break;
	}
}

static void _create_sound( int type, int wave, int volume, int basic_row,
                           int noise_type, double nfreq, double noffset, double nvol )
{
	int idx = g_ed.pxtn->Woice_AddNew();
	if( idx < 0 ){ _set_status( "woice max reached" ); return; }

	pxtnWoice* w = g_ed.pxtn->Woice_Get_variable( idx );
	if( !w->Voice_Allocate( 1 ) ){ _set_status( "voice allocate failed" ); return; }

	pxtnVOICEUNIT* v = w->get_voice_variable( 0 );
	char wname[ 32 ];

	if( type == 0 ) // PTV
	{
		v->type      = pxtnVOICE_Coodinate;
		v->basic_key = basic_row << 8;
		v->volume    = volume;
		v->pan       = 64;
		v->tuning    = 1.0f;
		v->voice_flags = PTV_VOICEFLAG_SMOOTH;
		v->data_flags  = PTV_DATAFLAG_WAVE;
		v->wave.reso   = 10000;
		v->wave.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 32 );
		_make_wave_points( wave, v->wave.points, &v->wave.num );

		snprintf( wname, sizeof( wname ), "ptv %d", idx );
	}
	else // PTN noise
	{
		if( !v->p_ptn->Allocate( 1, 0 ) ){ _set_status( "noise allocate failed" ); return; }
		pxNOISEDESIGN_UNIT* du = v->p_ptn->get_unit( 0 );
		du->bEnable = true;
		du->enve_num = 0;
		du->pan    = 64;
		du->main.type   = (pxWAVETYPE)( pxWAVETYPE_None + 1 + noise_type );
		du->main.freq   = (float)nfreq;
		du->main.volume = (float)nvol;
		du->main.offset = (float)noffset;
		du->main.b_rev  = false;
		du->freq.type = pxWAVETYPE_None; du->freq.volume = 0;
		du->volu.type = pxWAVETYPE_None; du->volu.volume = 0;
		v->p_ptn->set_smp_num_44k( _SAMPLE_PER_SECOND / 4 ); // 0.25s
		v->p_ptn->Fix();

		v->type      = pxtnVOICE_Noise;
		v->basic_key = basic_row << 8;
		v->volume    = volume;
		v->pan       = 64;

		snprintf( wname, sizeof( wname ), "ptn %d", idx );
	}
	w->set_name_buf( wname, strlen( wname ) );

	pxtnERR err = g_ed.pxtn->Woice_ReadyTone( idx );
	if( err != pxtnOK ){ _set_status( "tone ready: %s", pxtnError_get_string( err ) ); return; }

	// assign to the selected unit and preview it
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit >= 0 && unit < g_ed.unit_num )
		g_ed.pxtn->Unit_Get_variable( unit )->set_woice( w );

	_preview_note( unit < 0 ? 0 : unit, basic_row );
	_set_status( "created %s (woice %d)", wname, idx );
}

static void _on_create_clicked( GtkButton*, gpointer user_data )
{
	// user_data: struct of dialog widgets
	struct Dlg {
		GtkWidget *type, *wave, *volume, *basic_row, *ntype, *nfreq, *noffset, *nvol, *dlgwin;
	} *d = (Dlg*)user_data;

	int type = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) );
	_create_sound(
		type,
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->wave ) ),
		(int)gtk_range_get_value( GTK_RANGE( d->volume ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->basic_row ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->ntype ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->nfreq ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->noffset ) ),
		gtk_range_get_value( GTK_RANGE( d->nvol ) ) );

	gtk_window_destroy( GTK_WINDOW( d->dlgwin ) );
	delete d;
}

static void _sound_dialog()
{
	struct Dlg {
		GtkWidget *type, *wave, *volume, *basic_row, *ntype, *nfreq, *noffset, *nvol, *dlgwin;
	};
	Dlg* d = new Dlg{};

	GtkWidget* win = gtk_window_new();
	gtk_window_set_title( GTK_WINDOW( win ), "create sound" );
	gtk_window_set_default_size( GTK_WINDOW( win ), 380, 320 );
	d->dlgwin = win;

	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing( GTK_GRID( grid ), 6 );
	gtk_grid_set_column_spacing( GTK_GRID( grid ), 8 );
	gtk_widget_set_margin_start ( grid, 10 ); gtk_widget_set_margin_end  ( grid, 10 );
	gtk_widget_set_margin_top   ( grid, 10 ); gtk_widget_set_margin_bottom( grid, 10 );
	gtk_window_set_child( GTK_WINDOW( win ), grid );
	int r = 0;

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "type:" ), 0, r, 1, 1 );
	d->type = gtk_drop_down_new_from_strings( (const char*[]){ "PTV wave", "PTN noise", NULL } );
	gtk_grid_attach( GTK_GRID( grid ), d->type, 1, r++, 2, 1 );

	// PTV params
	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "wave:" ), 0, r, 1, 1 );
	d->wave = gtk_drop_down_new_from_strings( (const char*[]){ "sine", "saw", "square", "triangle", "pulse 1/4", NULL } );
	gtk_grid_attach( GTK_GRID( grid ), d->wave, 1, r++, 2, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "volume:" ), 0, r, 1, 1 );
	d->volume = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0, 128, 1 );
	gtk_range_set_value( GTK_RANGE( d->volume ), 100 );
	gtk_widget_set_hexpand( d->volume, TRUE );
	gtk_grid_attach( GTK_GRID( grid ), d->volume, 1, r++, 2, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "basic key row:" ), 0, r, 1, 1 );
	d->basic_row = gtk_spin_button_new_with_range( 0x24, 0x94, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->basic_row ), 0x60 );
	gtk_grid_attach( GTK_GRID( grid ), d->basic_row, 1, r++, 2, 1 );

	// PTN params
	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "noise osc:" ), 0, r, 1, 1 );
	d->ntype = gtk_drop_down_new_from_strings( (const char*[]){ "random", "sine", "saw", "rect", "saw2", "rect2", "tri", "random2", NULL } );
	gtk_grid_attach( GTK_GRID( grid ), d->ntype, 1, r++, 2, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "noise freq:" ), 0, r, 1, 1 );
	d->nfreq = gtk_spin_button_new_with_range( 0, 100, 0.5 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->nfreq ), 10 );
	gtk_grid_attach( GTK_GRID( grid ), d->nfreq, 1, r++, 2, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "noise offset:" ), 0, r, 1, 1 );
	d->noffset = gtk_spin_button_new_with_range( 0, 100, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->noffset ), 0 );
	gtk_grid_attach( GTK_GRID( grid ), d->noffset, 1, r++, 2, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "noise volume:" ), 0, r, 1, 1 );
	d->nvol = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01 );
	gtk_range_set_value( GTK_RANGE( d->nvol ), 0.8 );
	gtk_widget_set_hexpand( d->nvol, TRUE );
	gtk_grid_attach( GTK_GRID( grid ), d->nvol, 1, r++, 2, 1 );

	GtkWidget* btn = gtk_button_new_with_label( "create & assign to unit" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_create_clicked ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn, 0, r, 3, 1 );

	gtk_window_present( GTK_WINDOW( win ) );
}

// ---- event editing (VELOCITY / VOLUME / PAN_VOLUME / PAN_TIME) ----------

struct EventKindInfo { uint8_t kind; const char* name; double min, max, def; };
static const EventKindInfo _event_kinds[] =
{
	{ EVENTKIND_VELOCITY,   "velocity",   0, 129, EVENTDEFAULT_VELOCITY   },
	{ EVENTKIND_VOLUME,     "volume",     0, 129, EVENTDEFAULT_VOLUME     },
	{ EVENTKIND_PAN_VOLUME, "pan volume", 0, 128, EVENTDEFAULT_PAN_VOLUME },
	{ EVENTKIND_PAN_TIME,   "pan time",   0, 128, EVENTDEFAULT_PAN_TIME   },
};
static const int _event_kind_num = sizeof( _event_kinds ) / sizeof( _event_kinds[0] );

// Write (replace) an event of the given kind at the snapped clock for a unit.
static void _set_event( uint8_t kind, int32_t clock, int unit, int32_t value )
{
	if( !g_ed.loaded || unit < 0 || unit >= g_ed.unit_num ) return;
	int32_t c = _snap_clock( clock );

	SDL_LockAudio();
	_push_undo();
	_ensure_evels_capacity();
	g_ed.pxtn->evels->Record_Delete( c, c + 1, (uint8_t)unit, kind );
	g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, kind, value );
	SDL_UnlockAudio();

	for( int i = 0; i < _event_kind_num; i++ )
		if( _event_kinds[ i ].kind == kind ){ _set_status( "%s = %d @ clock %d (unit %d)", _event_kinds[ i ].name, value, c, unit ); break; }
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _on_event_set_clicked( GtkButton*, gpointer user_data )
{
	struct D { GtkWidget *type, *value; }* d = (D*)user_data;
	int t = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) );
	if( t < 0 || t >= _event_kind_num ) return;
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	// selected note takes priority; otherwise the snapped view start
	int32_t clock = g_ed.has_sel ? g_ed.sel_clock : (int32_t)( g_ed.h_offset / g_ed.px_per_clock );
	_set_event( _event_kinds[ t ].kind, clock, unit,
		(int32_t)gtk_range_get_value( GTK_RANGE( d->value ) ) );
}

static void _event_dialog()
{
	typedef struct { GtkWidget *type, *value, *dlgwin; } D;
	D* d = new D{};

	GtkWidget* win = gtk_window_new();
	gtk_window_set_title( GTK_WINDOW( win ), "set event" );
	gtk_window_set_default_size( GTK_WINDOW( win ), 360, 160 );
	d->dlgwin = win;

	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing( GTK_GRID( grid ), 6 );
	gtk_grid_set_column_spacing( GTK_GRID( grid ), 8 );
	gtk_widget_set_margin_start ( grid, 10 ); gtk_widget_set_margin_end  ( grid, 10 );
	gtk_widget_set_margin_top   ( grid, 10 ); gtk_widget_set_margin_bottom( grid, 10 );
	gtk_window_set_child( GTK_WINDOW( win ), grid );

	const char* names[ _event_kind_num + 1 ];
	for( int i = 0; i < _event_kind_num; i++ ) names[ i ] = _event_kinds[ i ].name;
	names[ _event_kind_num ] = NULL;

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "kind:" ), 0, 0, 1, 1 );
	d->type = gtk_drop_down_new_from_strings( names );
	gtk_grid_attach( GTK_GRID( grid ), d->type, 1, 0, 1, 1 );

	d->value = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0, 129, 1 );
	gtk_range_set_value( GTK_RANGE( d->value ), EVENTDEFAULT_VELOCITY );
	gtk_widget_set_hexpand( d->value, TRUE );
	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "value:" ), 0, 1, 1, 1 );
	gtk_grid_attach( GTK_GRID( grid ), d->value, 1, 1, 1, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "at: view start (snapped), selected unit" ), 0, 2, 2, 1 );

	GtkWidget* btn = gtk_button_new_with_label( "set event" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_event_set_clicked ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn, 0, 3, 2, 1 );

	gtk_window_present( GTK_WINDOW( win ) );
}

// ---- song settings (tempo / beats / measures / repeat / last) -----------

static void _apply_song( double tempo, int beat_num, int32_t beat_clock,
                         int meas_num, int repeat_meas, int last_meas )
{
	if( !g_ed.loaded ) return;
	if( last_meas < 0 ) last_meas = meas_num - 1; // keep song length consistent (meas is derived from content on load)
	SDL_LockAudio();
	_push_undo();
	g_ed.pxtn->master->Set( beat_num, (float)tempo, beat_clock );
	g_ed.pxtn->master->set_meas_num   ( meas_num );
	g_ed.pxtn->master->set_repeat_meas( repeat_meas );
	g_ed.pxtn->master->set_last_meas  ( last_meas );
	// song length is derived from the last measure on load; keep in-sync
	g_ed.pxtn->master->set_meas_num   ( last_meas );
	SDL_UnlockAudio();

	g_ed.tempo = tempo;
	_set_status( "song: tempo=%.1f beats=%d clock=%d meas=%d repeat=%d last=%d",
		tempo, beat_num, beat_clock, meas_num, repeat_meas, last_meas );
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _on_song_apply( GtkButton*, gpointer user_data )
{
	struct D { GtkWidget *tempo,*beat_num,*beat_clock,*meas,*repeat,*last,*dlgwin; }* d = (D*)user_data;
	int rm = (int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->repeat ) );
	int lm = (int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->last ) );
	_apply_song(
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->tempo ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->beat_num ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->beat_clock ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->meas ) ),
		rm - 1, // -1 = none (spin is 0-based)
		lm - 1 );
	gtk_window_destroy( GTK_WINDOW( d->dlgwin ) );
	delete d;
}

static void _song_dialog()
{
	typedef struct { GtkWidget *tempo,*beat_num,*beat_clock,*meas,*repeat,*last,*dlgwin; } D;
	D* d = new D{};

	pxtnMaster* m = g_ed.pxtn->master;

	GtkWidget* win = gtk_window_new();
	gtk_window_set_title( GTK_WINDOW( win ), "song settings" );
	gtk_window_set_default_size( GTK_WINDOW( win ), 340, 300 );
	d->dlgwin = win;

	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing( GTK_GRID( grid ), 6 );
	gtk_grid_set_column_spacing( GTK_GRID( grid ), 8 );
	gtk_widget_set_margin_start ( grid, 10 ); gtk_widget_set_margin_end  ( grid, 10 );
	gtk_widget_set_margin_top   ( grid, 10 ); gtk_widget_set_margin_bottom( grid, 10 );
	gtk_window_set_child( GTK_WINDOW( win ), grid );
	int r = 0;

	auto row_label = [&]( const char* txt ){ gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( txt ), 0, r, 1, 1 ); };

	row_label( "tempo" );
	d->tempo = gtk_spin_button_new_with_range( 30, 300, 0.5 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->tempo ), m->get_beat_tempo() );
	gtk_grid_attach( GTK_GRID( grid ), d->tempo, 1, r++, 1, 1 );

	row_label( "beats / measure" );
	d->beat_num = gtk_spin_button_new_with_range( 1, 16, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->beat_num ), m->get_beat_num() );
	gtk_grid_attach( GTK_GRID( grid ), d->beat_num, 1, r++, 1, 1 );

	row_label( "clock / beat" );
	d->beat_clock = gtk_spin_button_new_with_range( 96, 1920, 48 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->beat_clock ), m->get_beat_clock() );
	gtk_grid_attach( GTK_GRID( grid ), d->beat_clock, 1, r++, 1, 1 );

	row_label( "measures" );
	d->meas = gtk_spin_button_new_with_range( 1, 999, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->meas ), m->get_meas_num() );
	gtk_grid_attach( GTK_GRID( grid ), d->meas, 1, r++, 1, 1 );

	row_label( "repeat measure (0=none)" );
	d->repeat = gtk_spin_button_new_with_range( 0, 998, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->repeat ), m->get_repeat_meas() + 1 );
	gtk_grid_attach( GTK_GRID( grid ), d->repeat, 1, r++, 1, 1 );

	row_label( "last measure (0=none)" );
	d->last = gtk_spin_button_new_with_range( 0, 999, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->last ), m->get_last_meas() + 1 );
	gtk_grid_attach( GTK_GRID( grid ), d->last, 1, r++, 1, 1 );

	GtkWidget* btn = gtk_button_new_with_label( "apply" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_song_apply ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn, 0, r, 2, 1 );

	gtk_window_present( GTK_WINDOW( win ) );
}

// ---- drawing ------------------------------------------------------------

static void _draw_cb( GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer )
{
	// background
	cairo_set_source_rgb( cr, 0.06, 0.06, 0.10 );
	cairo_paint( cr );

	const int rows = _ROW_MAX - _ROW_MIN + 1;
	const double content_h = rows * _ROW_H;

	// key rows (black stripes) + octave lines (offset by vertical scroll)
	for( int r = _ROW_MIN; r <= _ROW_MAX; r++ )
	{
		static const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
		double y = ( _ROW_MAX - r ) * _ROW_H - g_ed.v_offset;
		if( y + _ROW_H < 0 || y > h ) continue;
		if( black[ ((r % 12) + 12) % 12 ] )
		{
			cairo_set_source_rgb( cr, 0.10, 0.10, 0.14 );
			cairo_rectangle( cr, 0, y, w, _ROW_H );
			cairo_fill( cr );
		}
		if( r % 12 == 0 )
		{
			cairo_set_source_rgb( cr, 0.25, 0.25, 0.35 );
			cairo_move_to( cr, 0, y + _ROW_H );
			cairo_line_to( cr, w, y + _ROW_H );
			cairo_stroke( cr );
		}
	}

	// beat / snap / measure grid
	{
		int32_t first_clock = (int32_t)( g_ed.h_offset / g_ed.px_per_clock );
		int32_t last_clock  = (int32_t)( ( g_ed.h_offset + w ) / g_ed.px_per_clock ) + g_ed.snap;
		int32_t beat_clock  = g_ed.pxtn->master->get_beat_clock();
		int32_t meas_clock  = beat_clock * g_ed.pxtn->master->get_beat_num();

		for( int32_t c = _snap_clock( first_clock ); c <= last_clock; c += g_ed.snap )
		{
			bool is_meas = ( meas_clock > 0 && c % meas_clock == 0 );
			bool is_beat = ( c % beat_clock == 0 );
			double x = c * g_ed.px_per_clock - g_ed.h_offset;
			cairo_set_source_rgb( cr,
				is_meas ? 0.55 : is_beat ? 0.30 : 0.16,
				is_meas ? 0.45 : is_beat ? 0.30 : 0.16,
				is_meas ? 0.75 : is_beat ? 0.42 : 0.22 );
			cairo_set_line_width( cr, is_meas ? 2.0 : 1.0 );
			cairo_move_to( cr, x, 0 );
			cairo_line_to( cr, x, content_h );
			cairo_stroke( cr );
		}
		cairo_set_line_width( cr, 1.0 );
	}

	// notes
	for( const EVERECORD* p = g_ed.pxtn ? g_ed.pxtn->evels->get_Records() : NULL; p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON ) continue;

		double x0 = p->clock * g_ed.px_per_clock - g_ed.h_offset;
		double x1 = ( p->clock + ( p->value > 0 ? p->value : g_ed.snap ) ) * g_ed.px_per_clock - g_ed.h_offset;
		if( x1 < 0 || x0 > w ) continue;

		int row = g_ed.pxtn->evels->get_Value( p->clock, p->unit_no, EVENTKIND_KEY ) >> 8;
		if( row < _ROW_MIN ) row = _ROW_MIN;
		if( row > _ROW_MAX ) row = _ROW_MAX;
		double y = ( _ROW_MAX - row ) * _ROW_H - g_ed.v_offset;
		if( y + _ROW_H < 0 || y > h ) continue;

		double r, g, b;
		_unit_color( p->unit_no, &r, &g, &b );
		// shade by velocity if one is set
		{
			int32_t vel = g_ed.pxtn->evels->get_Value( p->clock, p->unit_no, EVENTKIND_VELOCITY );
			double f = 0.35 + 0.65 * ( vel / 129.0 );
			r *= f; g *= f; b *= f;
		}
		cairo_set_source_rgb( cr, r, g, b );
		cairo_rectangle( cr, x0, y + 1, x1 - x0, _ROW_H - 2 );
		cairo_fill( cr );
		cairo_set_source_rgb( cr, r * 0.5, g * 0.5, b * 0.5 );
		cairo_rectangle( cr, x0, y + 1, x1 - x0, _ROW_H - 2 );
		cairo_stroke( cr );
	}

	// playhead
	if( g_ed.playing )
	{
		double sec  = (double)g_ed.played_samples / _SAMPLE_PER_SECOND;
		double x    = sec / _sec_per_clock() * g_ed.px_per_clock - g_ed.h_offset;
		cairo_set_source_rgb( cr, 0.95, 0.95, 0.95 );
		cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, h ); cairo_stroke( cr );
	}

	// bottom area below rows
	cairo_set_source_rgb( cr, 0.03, 0.03, 0.05 );
	cairo_rectangle( cr, 0, content_h, w, h - content_h );
	cairo_fill( cr );
}

// ---- events -------------------------------------------------------------

// right-button press/drag: delete the note under the cursor (continuously)
static void _on_rdrag_begin( GtkGestureDrag*, double x, double y, gpointer )
{
	int32_t clock; int row;
	if( _screen_to_clock_row( (int)x, (int)y, &clock, &row ) ) _delete_note( clock, row );
}

static void _on_rdrag_update( GtkGestureDrag* gesture, double, double, gpointer )
{
	double x = 0, y = 0;
	gtk_gesture_get_point( GTK_GESTURE( gesture ), NULL, &x, &y );
	int32_t clock; int row;
	if( _screen_to_clock_row( (int)x, (int)y, &clock, &row ) ) _delete_note( clock, row );
}

// left-button press/drag: create a new note, or resize an existing one
static void _on_drag_begin( GtkGestureDrag*, double x, double y, gpointer )
{
	int32_t clock; int row;
	if( !_screen_to_clock_row( (int)x, (int)y, &clock, &row ) ) return;

	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;

	// existing note -> move/resize mode (right edge = resize, body = move);
	// empty cell -> create new note
	const EVERECORD* hit = _find_note( clock, row, unit );
	if( hit )
	{
		_push_undo(); // one history entry per drag gesture

		double note_end_x = ( hit->clock + hit->value ) * g_ed.px_per_clock - g_ed.h_offset;
		bool on_edge      = ( x > note_end_x - 10.0 );

		g_ed.drag_unit       = unit;
		g_ed.drag_orig_clock = hit->clock;
		g_ed.drag_cur_clock  = hit->clock;
		g_ed.drag_cur_row    = row;
		g_ed.drag_dur        = hit->value > 0 ? hit->value : g_ed.snap;
		g_ed.drag_key        = g_ed.pxtn->evels->get_Value( hit->clock, (uint8_t)unit, EVENTKIND_KEY ) >> 8;
		g_ed.mode            = on_edge ? DRAG_RESIZE : DRAG_MOVE;
		g_ed.has_sel         = true;
		g_ed.sel_clock       = hit->clock;
		g_ed.sel_unit        = unit;

		_set_status( "%s: unit=%d clock=%d len=%d", on_edge ? "resize" : "move", unit, hit->clock, g_ed.drag_dur );
	}
	else
	{
		_set_status( "new note: unit=%d clock=%d row=0x%02x", unit, _snap_clock( clock ), row );
		_add_note( clock, row );
	}
}

static void _on_drag_update( GtkGestureDrag* gesture, double, double, gpointer )
{
	double x = 0, y = 0;
	gtk_gesture_get_point( GTK_GESTURE( gesture ), NULL, &x, &y );
	_drag_update( (int)x, (int)y ); // follows the mouse while the button is held
}

static void _on_drag_end( GtkGestureDrag*, double, double, gpointer )
{
	g_ed.dragging = false;
	g_ed.mode     = DRAG_NONE;
}

static gboolean _on_scroll( GtkEventControllerScroll*, double dx, double dy, gpointer state )
{
	GdkModifierType mod = (GdkModifierType)(uintptr_t)state;

	if( mod & GDK_CONTROL_MASK )
	{
		double zoom = ( dy < 0 ) ? 1.2 : 1 / 1.2;
		double px = g_ed.px_per_clock * zoom;
		if( px > 2.0 ) px = 2.0;
		if( px < 0.02 ) px = 0.02;
		g_ed.px_per_clock = px;
	}
	else if( mod & GDK_SHIFT_MASK )
	{
		g_ed.h_offset += dx * 40 + dy * 40;
	}
	else
	{
		g_ed.v_offset += dy * 40; // plain wheel: vertical scroll
	}
	if( g_ed.h_offset < 0 ) g_ed.h_offset = 0;
	if( g_ed.v_offset < 0 ) g_ed.v_offset = 0;
	{   // clamp to content height
		double max_v = ( _ROW_MAX - _ROW_MIN + 1 ) * (double)_ROW_H - 100;
		if( max_v < 0 ) max_v = 0;
		if( g_ed.v_offset > max_v ) g_ed.v_offset = max_v;
	}
	gtk_widget_queue_draw( g_ed.draw_area );
	return TRUE;
}

static gboolean _on_key( GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer )
{
	if( ( state & GDK_CONTROL_MASK ) && keyval == 's' ){ _save(); return TRUE; }
	if( ( state & GDK_CONTROL_MASK ) && keyval == 'z' ){ _undo(); return TRUE; }
	if( ( state & GDK_CONTROL_MASK ) && keyval == 'y' ){ _redo(); return TRUE; }
	if( ( state & GDK_CONTROL_MASK ) && keyval == 'c' )
	{
		if( !g_ed.has_sel ){ _set_status( "copy: no note selected" ); return TRUE; }
		// find the selected note by clock + unit
		const EVERECORD* p = NULL;
		for( const EVERECORD* q = g_ed.pxtn->evels->get_Records(); q; q = q->next )
		{
			if( q->kind == EVENTKIND_ON && q->unit_no == g_ed.sel_unit && q->clock >= g_ed.sel_clock )
			{ if( q->clock == g_ed.sel_clock ) p = q; break; }
		}
		if( p )
		{
			int k = g_ed.pxtn->evels->get_Value( p->clock, (uint8_t)g_ed.sel_unit, EVENTKIND_KEY ) >> 8;
			g_clipboard.clear();
			g_clipboard.push_back( { 0, g_ed.sel_unit, k, p->value > 0 ? p->value : g_ed.snap } );
			_set_status( "copied note (unit=%d key=0x%02x len=%d)", g_ed.sel_unit, k, p->value );
		}
		return TRUE;
	}
	if( ( state & GDK_CONTROL_MASK ) && keyval == 'v' )
	{
		if( g_clipboard.empty() || !g_ed.loaded ){ _set_status( "paste: clipboard empty" ); return TRUE; }
		SDL_LockAudio();
		_push_undo();
		_ensure_evels_capacity();
		int32_t base = _snap_clock( (int32_t)( g_ed.h_offset / g_ed.px_per_clock ) + g_ed.snap );
		for( const ClipNote& cn : g_clipboard )
		{
			int32_t c = base + cn.rel_clock;
			if( g_ed.pxtn->evels->get_Value( c, (uint8_t)cn.unit, EVENTKIND_KEY ) != ( cn.key_row << 8 ) )
			{
				g_ed.pxtn->evels->Record_Delete( c, c + 1, (uint8_t)cn.unit, EVENTKIND_KEY );
				g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)cn.unit, EVENTKIND_KEY, cn.key_row << 8 );
			}
			g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)cn.unit, EVENTKIND_ON, cn.dur );
			_fix_overlaps( c, cn.unit, cn.dur );
		}
		SDL_UnlockAudio();
		_set_status( "pasted %d note(s)", (int)g_clipboard.size() );
		gtk_widget_queue_draw( g_ed.draw_area );
		return TRUE;
	}
	if( keyval == GDK_KEY_space ){ g_ed.playing ? _stop_play() : _start_play(); return TRUE; }

	if( keyval >= '1' && keyval <= '4' )
	{
		static const int snaps[] = { _BEAT_CLOCK, _BEAT_CLOCK / 2, _BEAT_CLOCK / 4, _BEAT_CLOCK / 8 };
		g_ed.snap = snaps[ keyval - '1' ];
		gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.snap_combo ), keyval - '1' );
		_set_status( "snap: %d clocks", g_ed.snap );
		return TRUE;
	}
	return FALSE;
}

static gboolean _tick( gpointer )
{
	if( g_ed.playing )
	{
		// follow playhead
		double sec = (double)g_ed.played_samples / _SAMPLE_PER_SECOND;
		double x   = sec / _sec_per_clock() * g_ed.px_per_clock;
		GtkAllocation alloc;
		gtk_widget_get_allocation( g_ed.draw_area, &alloc );
		if( x < g_ed.h_offset || x > g_ed.h_offset + alloc.width * 0.75 )
		{
			g_ed.h_offset = x - alloc.width * 0.25;
			if( g_ed.h_offset < 0 ) g_ed.h_offset = 0;
		}
		gtk_widget_queue_draw( g_ed.draw_area );
	}
	return G_SOURCE_CONTINUE;
}

static void _on_snap_changed( GtkDropDown*, GParamSpec*, gpointer )
{
	static const int snaps[] = { _BEAT_CLOCK, _BEAT_CLOCK / 2, _BEAT_CLOCK / 4, _BEAT_CLOCK / 8 };
	guint sel = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.snap_combo ) );
	if( sel < 4 ) g_ed.snap = snaps[ sel ];
}

// ---- app ----------------------------------------------------------------

static void _activate( GtkApplication* app, gpointer )
{
	g_ed.window = gtk_application_window_new( app );
	gtk_window_set_title( GTK_WINDOW( g_ed.window ), "pxtone-editor" );
	gtk_window_set_default_size( GTK_WINDOW( g_ed.window ), 1100, 700 );

	GtkWidget* vbox = gtk_box_new( GTK_ORIENTATION_VERTICAL, 0 );
	gtk_window_set_child( GTK_WINDOW( g_ed.window ), vbox );

	// header controls
	GtkWidget* hbox = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	gtk_widget_set_margin_start ( hbox, 8 );
	gtk_widget_set_margin_end   ( hbox, 8 );
	gtk_widget_set_margin_top   ( hbox, 6 );
	gtk_widget_set_margin_bottom( hbox, 6 );
	gtk_box_append( GTK_BOX( vbox ), hbox );

	gtk_box_append( GTK_BOX( hbox ), gtk_label_new( "unit:" ) );
	GtkStringList* unit_list = gtk_string_list_new( NULL );
	for( int i = 0; i < g_ed.unit_num; i++ )
	{
		int32_t size = 0;
		const char* name = g_ed.pxtn->Unit_Get( i )->get_name_buf( &size );
		gtk_string_list_append( unit_list, name && name[0] ? name : "(no name)" );
	}
	g_ed.unit_combo = gtk_drop_down_new( G_LIST_MODEL( unit_list ), NULL );
	gtk_box_append( GTK_BOX( hbox ), g_ed.unit_combo );

	GtkWidget* btn_unit  = gtk_button_new_with_label( "+unit" );
	GtkWidget* btn_sound = gtk_button_new_with_label( "sound..." );
	GtkWidget* btn_event = gtk_button_new_with_label( "event..." );
	GtkWidget* btn_song  = gtk_button_new_with_label( "song..." );
	GtkWidget* btn_play  = gtk_button_new_with_label( "▶ play" );
	GtkWidget* btn_stop  = gtk_button_new_with_label( "■ stop" );
	g_signal_connect_swapped( btn_unit,  "clicked", G_CALLBACK( +[]( gpointer ){ _add_unit(); } ), NULL );
	g_signal_connect_swapped( btn_sound, "clicked", G_CALLBACK( +[]( gpointer ){ _sound_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_event, "clicked", G_CALLBACK( +[]( gpointer ){ _event_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_song,  "clicked", G_CALLBACK( +[]( gpointer ){ _song_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_play,  "clicked", G_CALLBACK( +[]( gpointer ){ _start_play(); } ), NULL );
	g_signal_connect_swapped( btn_stop,  "clicked", G_CALLBACK( +[]( gpointer ){ _stop_play(); } ), NULL );
	gtk_box_append( GTK_BOX( hbox ), btn_unit );
	gtk_box_append( GTK_BOX( hbox ), btn_sound );
	gtk_box_append( GTK_BOX( hbox ), btn_song );
	gtk_box_append( GTK_BOX( hbox ), btn_event );
	gtk_box_append( GTK_BOX( hbox ), btn_play );
	gtk_box_append( GTK_BOX( hbox ), btn_stop );

	gtk_box_append( GTK_BOX( hbox ), gtk_label_new( "snap:" ) );
	g_ed.snap_combo = gtk_drop_down_new_from_strings( (const char*[]){ "1/4", "1/8", "1/16", "1/32", NULL } );
	gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.snap_combo ), 1 );
	g_signal_connect( g_ed.snap_combo, "notify::selected", G_CALLBACK( _on_snap_changed ), NULL );
	gtk_box_append( GTK_BOX( hbox ), g_ed.snap_combo );

	g_ed.status = gtk_label_new( g_ed.loaded ? "ready" : g_ed.err.c_str() );
	gtk_widget_set_hexpand( g_ed.status, TRUE );
	gtk_widget_set_halign( g_ed.status, GTK_ALIGN_END );
	gtk_box_append( GTK_BOX( hbox ), g_ed.status );

	// drawing area
	g_ed.draw_area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func( GTK_DRAWING_AREA( g_ed.draw_area ), _draw_cb, NULL, NULL );
	gtk_widget_set_vexpand( g_ed.draw_area, TRUE );
	gtk_box_append( GTK_BOX( vbox ), g_ed.draw_area );

	// right-click drag: delete notes under the cursor (continuously)
	GtkGesture* rdrag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button( GTK_GESTURE_SINGLE( rdrag ), GDK_BUTTON_SECONDARY );
	g_signal_connect( rdrag, "drag-begin",  G_CALLBACK( _on_rdrag_begin ),  NULL );
	g_signal_connect( rdrag, "drag-update", G_CALLBACK( _on_rdrag_update ), NULL );
	gtk_widget_add_controller( g_ed.draw_area, GTK_EVENT_CONTROLLER( rdrag ) );

	// left-click drag: create / resize notes (updates while button held)
	GtkGesture* drag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button( GTK_GESTURE_SINGLE( drag ), GDK_BUTTON_PRIMARY );
	g_signal_connect( drag, "drag-begin",  G_CALLBACK( _on_drag_begin ),  NULL );
	g_signal_connect( drag, "drag-update", G_CALLBACK( _on_drag_update ), NULL );
	g_signal_connect( drag, "drag-end",    G_CALLBACK( _on_drag_end ),    NULL );
	gtk_widget_add_controller( g_ed.draw_area, GTK_EVENT_CONTROLLER( drag ) );

	GtkEventController* scroll = gtk_event_controller_scroll_new(
		(GtkEventControllerScrollFlags)( GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE ) );
	g_signal_connect( scroll, "scroll", G_CALLBACK( +[]( GtkEventControllerScroll* c, double dx, double dy, gpointer ) -> gboolean
	{
		GdkModifierType mod = gtk_event_controller_get_current_event_state( GTK_EVENT_CONTROLLER( c ) );
		return _on_scroll( c, dx, dy, (gpointer)(uintptr_t)mod );
	} ), NULL );
	gtk_widget_add_controller( g_ed.draw_area, scroll );

	GtkEventController* key = gtk_event_controller_key_new();
	g_signal_connect( key, "key-pressed", G_CALLBACK( _on_key ), NULL );
	gtk_widget_add_controller( g_ed.window, key );

	g_timeout_add( 33, _tick, NULL );

	gtk_window_present( GTK_WINDOW( g_ed.window ) );
}

// ---- load ---------------------------------------------------------------

static bool _load_tune()
{
	pxtnService* pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );
	g_ed.pxtn = pxtn;

	pxtnERR err = pxtn->init();
	if( err != pxtnOK ){ g_ed.err = pxtnError_get_string( err ); return false; }
	if( !pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ){ g_ed.err = "set_destination_quality"; return false; }

	FILE* fp = fopen( g_ed.path.c_str(), "rb" );
	if( !fp ){ g_ed.err = "cannot open " + g_ed.path; return false; }
	err = pxtn->read( fp );
	fclose( fp );
	if( err != pxtnOK ){ g_ed.err = pxtnError_get_string( err ); return false; }

	err = pxtn->tones_ready();
	if( err != pxtnOK ){ g_ed.err = pxtnError_get_string( err ); return false; }

	_ensure_evels_capacity();

	g_ed.unit_num = pxtn->Unit_Num();
	g_ed.tempo    = pxtn->master->get_beat_tempo();
	if( g_ed.tempo <= 0 ) g_ed.tempo = EVENTDEFAULT_BEATTEMPO;
	g_ed.loaded = true;
	return true;
}

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: %s <file.ptcop>\n", argv[0] );
		return 1;
	}
	g_ed.path = argv[1];

	if( SDL_Init( SDL_INIT_AUDIO ) != 0 )
	{
		fprintf( stderr, "ERROR: SDL_Init: %s\n", SDL_GetError() );
		return 1;
	}

	if( !_load_tune() )
	{
		fprintf( stderr, "ERROR: %s\n", g_ed.err.c_str() );
		return 1;
	}

	SDL_AudioSpec want = {0};
	want.freq     = _SAMPLE_PER_SECOND;
	want.format   = AUDIO_S16SYS;
	want.channels = _CHANNEL_NUM;
	want.samples  = 2048;
	want.callback = _sdl_audio_callback;
	if( SDL_OpenAudio( &want, NULL ) != 0 )
	{
		fprintf( stderr, "ERROR: SDL_OpenAudio: %s\n", SDL_GetError() );
		return 1;
	}

	// separate device for note previews (no callback: we use SDL_QueueAudio)
	SDL_AudioSpec pv = want;
	pv.callback = NULL;
	pv.userdata = NULL;
	g_ed.preview_dev = SDL_OpenAudioDevice( NULL, 0, &pv, NULL, 0 );
	if( g_ed.preview_dev ) SDL_PauseAudioDevice( g_ed.preview_dev, 0 ); // devices open PAUSED

	GtkApplication* app = gtk_application_new( "com.github.pxtone.editor", G_APPLICATION_NON_UNIQUE );
	g_signal_connect( app, "activate", G_CALLBACK( _activate ), NULL );
	int ret = g_application_run( G_APPLICATION( app ), 0, NULL );
	g_object_unref( app );

	_stop_play();
	SDL_CloseAudio();
	if( g_ed.preview_dev ) SDL_CloseAudioDevice( g_ed.preview_dev );
	SDL_Quit();
	return ret;
}
