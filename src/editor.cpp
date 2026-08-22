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

	// preview: FIFO mixed into the main audio callback (works before first Play)
	std::vector<int16_t> pv_buf;
	std::atomic<uint64_t> pv_read  {0};
	std::atomic<uint64_t> pv_write {0};

	// widgets
	GtkWidget* window    = NULL;
	GtkWidget* draw_area = NULL;
	GtkToggleButton* tb_play = nullptr;
	GtkAdjustment *hadj = NULL, *vadj = NULL;

	// dialog windows (single instance, driven by toggle buttons)
	GtkWidget* win_sound = NULL;  GtkToggleButton* tb_sound  = NULL;
	GtkWidget* win_event = NULL;  GtkToggleButton* tb_event  = NULL;
	GtkWidget* win_song  = NULL;  GtkToggleButton* tb_song   = NULL;
	GtkWidget* win_rename= NULL;  GtkToggleButton* tb_rename = NULL;
	GtkWidget* win_units = NULL;  GtkToggleButton* tb_units  = NULL;
	GtkWidget* units_list  = NULL;
	GtkWidget* units_stats = NULL;
	bool file_dlg_busy = false;
	GtkWidget* unit_combo = NULL;
	GtkWidget* snap_combo = NULL;
	GtkWidget* status     = NULL;
};

static Editor g_ed;

static const double PI = 3.141592653589793;

static void _win_destroyed( GtkWidget*, gpointer data )
{
	gpointer* arr = (gpointer*)data;
	*(GtkWidget**)arr[0] = NULL;
	GtkToggleButton* btn = GTK_TOGGLE_BUTTON( arr[1] );
	if( gtk_toggle_button_get_active( btn ) ) gtk_toggle_button_set_active( btn, FALSE );
}

static void _on_toggle_dialog( GtkToggleButton* btn, gpointer data )
{
	gpointer* arr = (gpointer*)data;
	GtkWidget** slot = (GtkWidget**)arr[0];
	void (*create)() = (void (*)())arr[2];
	if( gtk_toggle_button_get_active( btn ) ){ if( !*slot ) create(); }
	else if( *slot ) gtk_window_destroy( GTK_WINDOW( *slot ) );
}

static void _preview_note( int unit, int row, int32_t clock, int dur_frames = -1 ); // fwd

// ---- dialog window <-> toggle button binding ----------------------------

static void _win_destroyed( GtkWidget*, gpointer );
static void _on_toggle_dialog( GtkToggleButton*, gpointer );
static gpointer g_bind_sound[3], g_bind_event[3], g_bind_song[3], g_bind_rename[3], g_bind_units[3];

// ---- undo/redo (project snapshots) --------------------------------------

static void _set_status( const char* fmt, ... ); // fwd
static void _save_as_path( const char* path ); // fwd

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

// Restore a project snapshot safely: the audio callback must not run while
// the event array is reallocated (caused a segfault when undoing/redoing
// during playback).
static void _restore_safely( const SongSnap& s )
{
	// LockAudio serializes against the audio callback (which keeps running:
	// pausing it would silently kill note previews until the next Play).
	SDL_LockAudio();
	_restore_snapshot( s );
	SDL_UnlockAudio();
}

static void _undo()
{
	if( g_undo.empty() ) return;
	g_redo.push_back( _snapshot() );
	_restore_safely( g_undo.back() );
	g_undo.pop_back();
	_set_status( "undo (%d left)", (int)g_undo.size() );
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _redo()
{
	if( g_redo.empty() ) return;
	g_undo.push_back( _snapshot() );
	_restore_safely( g_redo.back() );
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
	if( !GTK_IS_LABEL( g_ed.status ) ){ fprintf( stderr, "%s\n", buf ); return; } // widgets not ready yet
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
	if( !g_ed.pxtn || !g_ed.playing ) memset( stream, 0, len );
	else if( !g_ed.pxtn->Moo( stream, len ) ) memset( stream, 0, len );
	g_ed.played_samples += len / ( _CHANNEL_NUM * sizeof(int16_t) );

	// mix queued preview samples on top (works before the first Play)
	// (diagnostic: report output level while playing)
	if( g_ed.playing )
	{
		static int dbg = 0;
		double pk = 0;
		const int16_t* s16 = (const int16_t*)stream;
		for( int i = 0; i < len / (int)sizeof(int16_t); i++ )
			{ double a = fabs( s16[ i ] / 32768.0 ); if( a > pk ) pk = a; }
		if( ++dbg >= 86 ){ dbg = 0; fprintf( stderr, "[mixer] peak=%.3f\n", pk ); }
	}

	int16_t* out = (int16_t*)stream;
	size_t   frames = len / ( _CHANNEL_NUM * sizeof(int16_t) );
	size_t   capf   = g_ed.pv_buf.size() / _CHANNEL_NUM; // capacity in frames
	uint64_t rd     = g_ed.pv_read;
	uint64_t wr     = g_ed.pv_write;
	for( size_t f = 0; f < frames; f++ )
	{
		if( rd >= wr ) break;
		const int16_t* ps = &g_ed.pv_buf[ ( rd % capf ) * _CHANNEL_NUM ];
		for( int c = 0; c < _CHANNEL_NUM; c++ )
		{
			double s = out[ f * _CHANNEL_NUM + c ] / 32768.0 + ps[ c ] / 32768.0;
			if( s >  1 ) s =  1;
			if( s < -1 ) s = -1;
			out[ f * _CHANNEL_NUM + c ] = (int16_t)( s * 32767 );
		}
		rd++;
	}
	g_ed.pv_read = rd;
}

static void _start_play()
{
	if( !g_ed.loaded || g_ed.playing )
	{
		fprintf( stderr, "[play] skipped (loaded=%d playing=%d)\n", g_ed.loaded?1:0, g_ed.playing?1:0 );
		// keep the toggle consistent with the actual state
		if( g_ed.tb_play && gtk_toggle_button_get_active( g_ed.tb_play ) )
			gtk_toggle_button_set_active( g_ed.tb_play, FALSE );
		return;
	}

	pxtnVOMITPREPARATION prep = {0};
	prep.flags          |= pxtnVOMITPREPFLAG_loop | pxtnVOMITPREPFLAG_unit_mute;
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
	g_ed.playing = false;  // keep the mixer running for previews
	_set_status( "stopped" );
	if( g_ed.tb_play && gtk_toggle_button_get_active( g_ed.tb_play ) )
		gtk_toggle_button_set_active( g_ed.tb_play, FALSE );
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

	_preview_note( unit, row, c );

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

static void _save_as_dialog(); // fwd
static void _open_dialog(); // fwd

static void _save()
{
	if( !g_ed.loaded ) return;
	if( g_ed.path.empty() ){ _save_as_dialog(); return; } // new tune: ask for a file name
	FILE* fp = fopen( g_ed.path.c_str(), "wb" );
	if( !fp ){ _set_status( "ERROR: cannot write %s", g_ed.path.c_str() ); return; }
	pxtnERR err = g_ed.pxtn->write( fp, false, 0x0500 ); // b_tune=false: .ptcop project format (rough=1, lossless)
	fclose( fp );
	if( err != pxtnOK ){ _set_status( "ERROR: %s", pxtnError_get_string( err ) ); return; }
	_set_status( "saved: %s", g_ed.path.c_str() );
}

static void _refresh_unit_combo(); // fwd
static void _units_refresh(); // fwd
static void _make_wave_points( int type, pxtnPOINT* pts, int32_t* p_num ); // fwd
static void _add_unit(); // fwd

// ---- units panel ---------------------------------------------------------

static void _refresh_unit_combo(); // fwd
static void _units_refresh();      // fwd

static void _delete_unit( int idx )
{
	if( idx < 0 || idx >= g_ed.unit_num || g_ed.unit_num <= 1 )
		{ _set_status( "cannot delete the last unit" ); return; }

	SDL_LockAudio();
	_push_undo();
	// drop events of the deleted unit, then shift higher unit numbers down
	g_ed.pxtn->evels->Record_UnitNo_Miss( (uint8_t)idx );
	for( int u = idx + 1; u < g_ed.unit_num; u++ )
		g_ed.pxtn->evels->Record_UnitNo_Replace( (uint8_t)u, (uint8_t)( u - 1 ) );
	g_ed.pxtn->Unit_Remove( idx );
	SDL_UnlockAudio();

	g_ed.unit_num--;
	int sel = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( sel >= g_ed.unit_num ) gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.unit_combo ), g_ed.unit_num - 1 );
	_refresh_unit_combo();
	_units_refresh();
	_set_status( "removed unit %d", idx );
}

static void _units_refresh()
{
	if( !g_ed.units_list || !g_ed.pxtn ) return;

	// clear rows
	GtkWidget* child = gtk_widget_get_first_child( g_ed.units_list );
	while( child )
	{
		GtkWidget* next = gtk_widget_get_next_sibling( child );
		gtk_list_box_remove( GTK_LIST_BOX( g_ed.units_list ), child );
		child = next;
	}

	int sel = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	int total_events = 0;

	for( int i = 0; i < g_ed.unit_num; i++ )
	{
		int32_t size = 0;
		const char* name = g_ed.pxtn->Unit_Get( i )->get_name_buf( &size );
		int events = g_ed.pxtn->evels->get_Count( (uint8_t)i );
		int notes  = g_ed.pxtn->evels->get_Count( (uint8_t)i, EVENTKIND_ON );
		total_events += events;

		GtkWidget* row = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );

		GtkWidget* lbl = gtk_label_new( NULL );
		char txt[ 256 ];
		snprintf( txt, sizeof( txt ), "%d: %s   (%d events / %d notes)%s",
			i, name && name[0] ? name : "(no name)", events, notes,
			i == sel ? "  <" : "" );
		gtk_label_set_text( GTK_LABEL( lbl ), txt );
		gtk_widget_set_hexpand( lbl, TRUE );
		gtk_widget_set_halign( lbl, GTK_ALIGN_START );
		gtk_box_append( GTK_BOX( row ), lbl );

		GtkWidget* use = gtk_check_button_new_with_label( "audible" );
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( use ), g_ed.pxtn->Unit_Get( i )->get_played() );
		g_object_set_data( G_OBJECT( use ), "unit-idx", GINT_TO_POINTER( i ) );
		g_signal_connect( use, "toggled", G_CALLBACK( +[]( GtkToggleButton* b, gpointer ud ){
			int i = GPOINTER_TO_INT( g_object_get_data( G_OBJECT( b ), "unit-idx" ) );
			g_ed.pxtn->Unit_Get_variable( i )->set_played( gtk_toggle_button_get_active( b ) );
			_set_status( "unit %d %s", i, gtk_toggle_button_get_active( b ) ? "audible" : "muted" );
		} ), NULL );
		gtk_box_append( GTK_BOX( row ), use );

		if( g_ed.unit_num > 1 )
		{
			GtkWidget* del = gtk_button_new_with_label( "del" );
			g_object_set_data( G_OBJECT( del ), "unit-idx", GINT_TO_POINTER( i ) );
			g_signal_connect_swapped( del, "clicked", G_CALLBACK( +[]( gpointer ud ){
				_delete_unit( GPOINTER_TO_INT( ud ) );
			} ), GINT_TO_POINTER( i ) );
			gtk_box_append( GTK_BOX( row ), del );
		}

		gtk_list_box_append( GTK_LIST_BOX( g_ed.units_list ), row );
	}

	if( g_ed.units_stats )
	{
		char txt[ 128 ];
		int notes = 0;
		for( const EVERECORD* p = g_ed.pxtn->evels->get_Records(); p; p = p->next )
			if( p->kind == EVENTKIND_ON ) notes++;
		snprintf( txt, sizeof( txt ), "units: %d   total events: %d   total notes: %d",
			g_ed.unit_num, total_events, notes );
		gtk_label_set_text( GTK_LABEL( g_ed.units_stats ), txt );
	}
}

static void _on_unit_row_selected( GtkListBox*, GtkListBoxRow* row, gpointer )
{
	if( row )
	{
		int i = gtk_list_box_row_get_index( row );
		if( i >= 0 && i < g_ed.unit_num )
			gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.unit_combo ), i );
	}
}

static void _units_dialog()
{
	GtkWidget* win = gtk_window_new();
	gtk_window_set_title( GTK_WINDOW( win ), "units" );
	gtk_window_set_default_size( GTK_WINDOW( win ), 460, 400 );
	g_ed.win_units = win;

	GtkWidget* vbox = gtk_box_new( GTK_ORIENTATION_VERTICAL, 6 );
	gtk_widget_set_margin_start ( vbox, 10 ); gtk_widget_set_margin_end  ( vbox, 10 );
	gtk_widget_set_margin_top   ( vbox, 10 ); gtk_widget_set_margin_bottom( vbox, 10 );
	gtk_window_set_child( GTK_WINDOW( win ), vbox );

	GtkWidget* sw = gtk_scrolled_window_new();
	gtk_widget_set_vexpand( sw, TRUE );
	gtk_scrolled_window_set_policy( GTK_SCROLLED_WINDOW( sw ), GtkPolicyType::GTK_POLICY_NEVER, GtkPolicyType::GTK_POLICY_AUTOMATIC );
	gtk_box_append( GTK_BOX( vbox ), sw );

	g_ed.units_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode( GTK_LIST_BOX( g_ed.units_list ), GTK_SELECTION_SINGLE );
	g_signal_connect( g_ed.units_list, "row-selected", G_CALLBACK( _on_unit_row_selected ), NULL );
	gtk_scrolled_window_set_child( GTK_SCROLLED_WINDOW( sw ), g_ed.units_list );

	GtkWidget* hbox = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	gtk_box_append( GTK_BOX( vbox ), hbox );

	GtkWidget* btn_add = gtk_button_new_with_label( "+ add unit" );
	g_signal_connect_swapped( btn_add, "clicked", G_CALLBACK( +[]( gpointer ){ _add_unit(); } ), NULL );
	gtk_box_append( GTK_BOX( hbox ), btn_add );

	g_ed.units_stats = gtk_label_new( "" );
	gtk_widget_set_hexpand( g_ed.units_stats, TRUE );
	gtk_widget_set_halign( g_ed.units_stats, GTK_ALIGN_END );
	gtk_box_append( GTK_BOX( vbox ), g_ed.units_stats );

	_units_refresh();
	gtk_window_present( GTK_WINDOW( win ) );
}

// ---- new tune ------------------------------------------------------------

static bool _init_new_project(); // fwd (defined near _load_tune)

static void _new_tune()
{
	SDL_LockAudio();     // serialize against the audio callback
	bool ok = _init_new_project();
	SDL_UnlockAudio();
	if( !ok ) return;

	_refresh_unit_combo();
	_set_status( "new tune created (unsaved)" );
	gtk_widget_queue_draw( g_ed.draw_area );
}

// ---- open / save-as via native file dialog -------------------------------

static void _open_path( const char* path )
{
	SDL_LockAudio();

	pxtnService* p = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );
	pxtnERR err = p->init();
	if( err == pxtnOK && !p->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ) err = pxtnERR_INIT;

	FILE* fp = NULL;
	if( err == pxtnOK && !( fp = fopen( path, "rb" ) ) ) err = pxtnERR_desc_r;
	if( err == pxtnOK ){ err = p->read( fp ); fclose( fp ); }
	if( err == pxtnOK ) err = p->tones_ready();

	if( err != pxtnOK )
	{
		delete p;
		SDL_UnlockAudio();
		_set_status( "open failed: %s (%s)", path, pxtnError_get_string( err ) );
		return;
	}

	delete g_ed.pxtn;
	g_ed.pxtn = p;
	g_ed.path = path;
	_ensure_evels_capacity();
	g_ed.unit_num = g_ed.pxtn->Unit_Num();
	g_ed.tempo    = g_ed.pxtn->master->get_beat_tempo();
	if( g_ed.tempo <= 0 ) g_ed.tempo = EVENTDEFAULT_BEATTEMPO;

	g_undo.clear(); g_redo.clear(); g_clipboard.clear();
	g_ed.has_sel = false; g_ed.dragging = false; g_ed.mode = DRAG_NONE;
	g_ed.h_offset = 0; g_ed.v_offset = 0;

	SDL_UnlockAudio();

	_refresh_unit_combo();
	_set_status( "opened: %s", path );
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _on_open_response( GObject* src, GAsyncResult* res, gpointer )
{
	GError* err = NULL;
	g_ed.file_dlg_busy = false;
	GFile* f = gtk_file_dialog_open_finish( GTK_FILE_DIALOG( src ), res, &err );
	if( f )
	{
		char* path = g_file_get_path( f );
		if( path ) _open_path( path );
		g_free( path );
		g_object_unref( f );
	}
	else g_error_free( err );
}

static void _open_dialog()
{
	if( g_ed.file_dlg_busy ) return; // prevent stacking dialogs
	g_ed.file_dlg_busy = true;
	GtkFileDialog* dlg = gtk_file_dialog_new();
	gtk_file_dialog_set_title( dlg, "open ptcop" );
	GtkFileFilter* f = gtk_file_filter_new();
	gtk_file_filter_set_name( f, "pxtone project (*.ptcop)" );
	gtk_file_filter_add_pattern( f, "*.ptcop" );
	GListStore* store = g_list_store_new( GTK_TYPE_FILE_FILTER );
	g_list_store_append( store, f );
	gtk_file_dialog_set_filters( dlg, G_LIST_MODEL( store ) );
	g_object_unref( store );
	g_object_unref( f );
	gtk_file_dialog_open( dlg, GTK_WINDOW( g_ed.window ), NULL, _on_open_response, NULL );
	g_object_unref( dlg );
}

static void _on_save_response( GObject* src, GAsyncResult* res, gpointer )
{
	GError* err = NULL;
	g_ed.file_dlg_busy = false;
	GFile* f = gtk_file_dialog_save_finish( GTK_FILE_DIALOG( src ), res, &err );
	if( f )
	{
		char* path = g_file_get_path( f );
		if( path ) _save_as_path( path );
		g_free( path );
		g_object_unref( f );
	}
	else g_error_free( err );
}

static void _save_as_dialog()
{
	if( g_ed.file_dlg_busy ) return;
	g_ed.file_dlg_busy = true;
	GtkFileDialog* dlg = gtk_file_dialog_new();
	gtk_file_dialog_set_title( dlg, "save as" );
	gtk_file_dialog_set_initial_name( dlg, g_ed.path.empty() ? "untitled.ptcop" : g_ed.path.c_str() );
	gtk_file_dialog_save( dlg, GTK_WINDOW( g_ed.window ), NULL, _on_save_response, NULL );
	g_object_unref( dlg );
}

static void _save_as_path( const char* path )
{
	g_ed.path = path;
	_save();
}

// ---- note preview (audition) --------------------------------------------

// Render ~0.35s of the woice pitched to the key row into the preview FIFO
// (mixed into the main audio callback).
static void _preview_woice( const pxtnWoice* woice, int row, int dur_frames_arg = -1 )
{
	if( !woice ) return;

	const int32_t SPSEC = _SAMPLE_PER_SECOND;
	const int dur_frames = dur_frames_arg > 0 ? dur_frames_arg : SPSEC * 35 / 100;

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

	// append to the preview FIFO (frame-based ring; drop oldest if full)
	size_t capf = g_ed.pv_buf.size() / _CHANNEL_NUM;
	if( capf == 0 ) return;
	for( int i = 0; i < dur_frames; i++ )
	{
		uint64_t wr = g_ed.pv_write;
		if( wr - g_ed.pv_read >= capf ) g_ed.pv_read = wr - capf + 1;
		g_ed.pv_buf[ ( wr % capf ) * 2 + 0 ] = out[ i * 2 + 0 ];
		g_ed.pv_buf[ ( wr % capf ) * 2 + 1 ] = out[ i * 2 + 1 ];
		g_ed.pv_write = wr + 1;
	}
}

// Resolve the woice the way Moo does: via the unit's VOICENO event at that
// point (loaded tunes do not have the woice pointer set on the unit).
static void _preview_note( int unit, int row, int32_t clock, int dur_frames )
{
	if( unit < 0 || unit >= g_ed.unit_num ) return;
	int32_t vno = g_ed.pxtn->evels->get_Value( clock, (uint8_t)unit, EVENTKIND_VOICENO );
	const pxtnWoice* woice = NULL;
	if( vno >= 0 && vno < g_ed.pxtn->Woice_Num() ) woice = g_ed.pxtn->Woice_Get( vno );
	if( !woice && g_ed.pxtn->Woice_Num() > 0 ) woice = g_ed.pxtn->Woice_Get( 0 );
	_preview_woice( woice, row, dur_frames );
}

// ---- track (unit) add ---------------------------------------------------

static void _refresh_unit_combo()
{
	// rebuild the model wholesale (in-place splice did not refresh the dropdown)
	GtkStringList* list = gtk_string_list_new( NULL );
	for( int i = 0; i < g_ed.unit_num; i++ )
	{
		int32_t size = 0;
		const char* name = g_ed.pxtn->Unit_Get( i )->get_name_buf( &size );
		gtk_string_list_append( list, name && name[0] ? name : "(no name)" );
	}
	gtk_drop_down_set_model( GTK_DROP_DOWN( g_ed.unit_combo ), G_LIST_MODEL( list ) );
	g_object_unref( list );
	if( g_ed.unit_num > 0 ) gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.unit_combo ), g_ed.unit_num - 1 );
	_units_refresh();
}

static void _add_unit()
{
	if( !g_ed.loaded ) return;
	fprintf( stderr, "[+unit] num=%d -> AddNew=%d\n", g_ed.pxtn->Unit_Num(),
		g_ed.pxtn->Unit_AddNew() ? 1 : 0 );
	int idx = g_ed.pxtn->Unit_Num() - 1;
	if( idx < 0 || idx <= g_ed.unit_num - 1 ){ _set_status( "unit max reached" ); return; }
	g_ed.unit_num = idx + 1;
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

// Simple A/D/S/R envelope (fps=60): fast attack, gentle decay, 100ms release.
// Without this, raw PTV playback starts/stops abruptly and sounds like a click.
static void _apply_simple_envelope( pxtnVOICEUNIT* v )
{
	v->data_flags |= PTV_DATAFLAG_ENVELOPE;
	pxtnVOICEENVELOPE& e = v->envelope;
	e.fps      = 60;
	e.head_num = 3; e.body_num = 0; e.tail_num = 1;
	e.points   = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 4 );
	e.points[0] = {  1, 128 };  // attack  ~17ms to full
	e.points[1] = { 12,  90 };  // decay   to ~70% by 200ms
	e.points[2] = { 36,  70 };  // sustain until 600ms
	e.points[3] = {  6,   0 };  // release 100ms
}

// harmonic spectra for the timbre selector (x=harmonic no, y=level)
static void _make_harmonic_points( int timbre, pxtnPOINT* pts, int32_t* p_num )
{
	switch( timbre )
	{
	case 1: // bright
		pts[0]={1,128}; pts[1]={2,96}; pts[2]={3,64}; pts[3]={4,40}; pts[4]={5,24};
		*p_num = 5; break;
	case 2: // hollow
		pts[0]={1,128}; pts[1]={3,64}; pts[2]={5,32};
		*p_num = 3; break;
	case 3: // warm
		pts[0]={1,128}; pts[1]={2,80}; pts[2]={4,40};
		*p_num = 3; break;
	case 4: // reedy
		pts[0]={1,128}; pts[1]={2,20}; pts[2]={4,90}; pts[3]={6,30};
		*p_num = 4; break;
	default: // pure
		pts[0]={1,128};
		*p_num = 1; break;
	}
}

// Build a PTV/PTN voice into a freshly allocated woice. Returns false on error.
static bool _build_sound_woice( pxtnWoice* w, int type, int wave, int volume, int basic_row,
                                int noise_type, double nfreq, double noffset, double nvol )
{
	if( !w->Voice_Allocate( 1 ) ) return false;
	pxtnVOICEUNIT* v = w->get_voice_variable( 0 );

	if( type == 0 ) // PTV (overtone synthesis, like stock pxTone default tones)
	{
		v->type      = pxtnVOICE_Overtone;
		v->basic_key = basic_row << 8; // A4 = 0x4500 by default
		v->volume    = volume;
		v->pan       = 64;
		v->tuning    = 1.0f;
		v->voice_flags = PTV_VOICEFLAG_SMOOTH | PTV_VOICEFLAG_WAVELOOP;
		v->data_flags  = PTV_DATAFLAG_WAVE;
		v->wave.num    = 8; // max harmonics slot
		v->wave.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 8 );
		_make_harmonic_points( wave, v->wave.points, &v->wave.num );
	}
	else // PTN noise
	{
		// NOTE: enve_num == 0 makes the noise builder scale everything by
		// enve_mag_start (0) => total silence. Always provide one point.
		if( !v->p_ptn->Allocate( 1, 1 ) ) return false;
		pxNOISEDESIGN_UNIT* du = v->p_ptn->get_unit( 0 );
		du->bEnable = true;
		du->enve_num = 1; du->enves[0].x = 10; du->enves[0].y = 100; // 10ms ramp to 100%
		du->pan    = 64;
		du->main.type   = (pxWAVETYPE)( pxWAVETYPE_None + 1 + noise_type );
		if( nfreq < 50 ) nfreq = 400; // guard against inaudible low frequencies
		du->main.freq   = (float)nfreq;
		du->main.volume = (float)nvol * 100; // design values are percentages
		du->main.offset = (float)noffset;
		du->main.b_rev  = false;
		du->freq.type = pxWAVETYPE_None; du->freq.volume = 0;
		// NOTE: volu == None yields a zero volume table (silence); use a constant
		du->volu.type = pxWAVETYPE_Sine; du->volu.freq = 0;
		du->volu.volume = 100; du->volu.offset = 25; du->volu.b_rev = false; // constant full volume
		v->p_ptn->set_smp_num_44k( _SAMPLE_PER_SECOND / 4 ); // 0.25s
		v->p_ptn->Fix();

		v->type      = pxtnVOICE_Noise;
		v->basic_key = basic_row << 8;
		v->volume    = volume;
		v->pan       = 64;
	}
	_apply_simple_envelope( v );
	return true;
}

typedef struct {
	GtkWidget *type, *wave, *volume, *basic_row, *ntype, *nfreq, *noffset, *nvol, *dlgwin;
	GtkWidget *audkey, *aurlen, *wavecanvas;
} SoundDlg;

// waveform preview of the selected PTV wave
static void _wave_canvas_cb( GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer ud )
{
	SoundDlg* d = (SoundDlg*)ud;
	cairo_set_source_rgb( cr, 0.08, 0.08, 0.12 );
	cairo_paint( cr );

	int sel = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->wave ) );
	if( sel < 0 ) return;

	if( gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) ) != 0 )
	{
		// noise: draw the selected oscillator waveform
		int nsel = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->ntype ) );
		if( nsel < 0 ) return;
		pxtnPOINT pts[ 32 ]; int32_t n = 0;
		_make_wave_points( nsel, pts, &n );
		cairo_set_source_rgb( cr, 0.9, 0.7, 0.35 );
		cairo_set_line_width( cr, 2 );
		for( int i = 0; i <= n; i++ )
		{
			const pxtnPOINT& pt = pts[ i % n ];
			double x = 4 + ( w - 8 ) * pt.x / 10000.0;
			double y = h / 2 - h * 0.4 * pt.y / 128.0;
			if( i == 0 ) cairo_move_to( cr, x, y ); else cairo_line_to( cr, x, y );
		}
		cairo_stroke( cr );
		return;
	}

	// overtone harmonics as bars
	pxtnPOINT pts[ 8 ]; int32_t n = 0;
	_make_harmonic_points( sel, pts, &n );
	cairo_set_source_rgb( cr, 0.35, 0.85, 1.0 );
	for( int i = 0; i < n; i++ )
	{
		double bw = ( w - 8 ) / 6.0;
		double x  = 4 + bw * ( pts[ i ].x - 1 ) + bw * 0.25;
		double bh = ( h * 0.7 ) * pts[ i ].y / 128.0;
		cairo_rectangle( cr, x, h - 6 - bh, bw * 0.5, bh );
		cairo_fill( cr );
	}
}

static void _create_sound( int type, int wave, int volume, int basic_row,
                           int noise_type, double nfreq, double noffset, double nvol )
{
	int idx = g_ed.pxtn->Woice_AddNew();
	if( idx < 0 ){ _set_status( "woice max reached" ); return; }

	pxtnWoice* w = g_ed.pxtn->Woice_Get_variable( idx );
	if( !_build_sound_woice( w, type, wave, volume, basic_row, noise_type, nfreq, noffset, nvol ) )
		{ _set_status( "voice build failed" ); return; }

	char wname[ 32 ];
	snprintf( wname, sizeof( wname ), "%s %d", type == 0 ? "ptv" : "ptn", idx );
	w->set_name_buf( wname, strlen( wname ) );

	pxtnERR err = g_ed.pxtn->Woice_ReadyTone( idx );
	if( err != pxtnOK ){ _set_status( "tone ready: %s", pxtnError_get_string( err ) ); return; }

	// assign to the selected unit and preview it
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit >= 0 && unit < g_ed.unit_num )
		g_ed.pxtn->Unit_Get_variable( unit )->set_woice( w );

	_preview_woice( w, basic_row ); // audition the new sound itself
	_set_status( "created %s (woice %d)", wname, idx );
}

// build a temporary woice, audition it, then remove it (nothing is saved)
static void _audition_sound( int type, int wave, int volume, int basic_row,
                             int noise_type, double nfreq, double noffset, double nvol,
                             int aud_row, int dur_frames )
{
	int idx = g_ed.pxtn->Woice_AddNew();
	if( idx < 0 ){ _set_status( "woice max reached" ); return; }
	pxtnWoice* w = g_ed.pxtn->Woice_Get_variable( idx );
	if( !_build_sound_woice( w, type, wave, volume, basic_row, noise_type, nfreq, noffset, nvol ) )
		{ g_ed.pxtn->Woice_Remove( idx ); return; }
	if( g_ed.pxtn->Woice_ReadyTone( idx ) != pxtnOK ){ _set_status( "audition: tone ready failed" ); g_ed.pxtn->Woice_Remove( idx ); return; }
	int frames = dur_frames > 0 ? dur_frames : -1;
	_preview_woice( w, aud_row, frames ); // render into the FIFO synchronously
	_set_status( "audition: ok (%d frames queued)", (int)( g_ed.pv_write - g_ed.pv_read ) );
	g_ed.pxtn->Woice_Remove( idx );
}

static void _on_audition_clicked( GtkButton*, gpointer user_data )
{
	SoundDlg* d = (SoundDlg*)user_data;
	int dur_sec_pct = (int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->aurlen ) ); // % of 0.35s
	_audition_sound(
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->wave ) ),
		(int)gtk_range_get_value( GTK_RANGE( d->volume ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->basic_row ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->ntype ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->nfreq ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->noffset ) ),
		gtk_range_get_value( GTK_RANGE( d->nvol ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->audkey ) ),
		_SAMPLE_PER_SECOND * dur_sec_pct / 100 );
}

static void _on_create_clicked( GtkButton*, gpointer user_data )
{
	SoundDlg* d = (SoundDlg*)user_data;
	_create_sound(
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->wave ) ),
		(int)gtk_range_get_value( GTK_RANGE( d->volume ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->basic_row ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->ntype ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->nfreq ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->noffset ) ),
		gtk_range_get_value( GTK_RANGE( d->nvol ) ) );

	GtkWidget* win = d->dlgwin;
	delete d;
	gtk_window_destroy( GTK_WINDOW( win ) );
}

static void _sound_dialog()
{
	SoundDlg* d = new SoundDlg{};

	GtkWidget* win = gtk_window_new();
	gtk_window_set_title( GTK_WINDOW( win ), "create sound" );
	gtk_window_set_default_size( GTK_WINDOW( win ), 400, 480 );
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
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->basic_row ), 0x45 );
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

	// waveform preview canvas
	d->wavecanvas = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func( GTK_DRAWING_AREA( d->wavecanvas ), _wave_canvas_cb, d, NULL );
	gtk_widget_set_size_request( d->wavecanvas, -1, 60 );
	gtk_grid_attach( GTK_GRID( grid ), d->wavecanvas, 0, r++, 2, 1 );
	g_signal_connect_swapped( d->wave, "notify::selected",
		G_CALLBACK( +[]( gpointer ud ){ gtk_widget_queue_draw( GTK_WIDGET( ud ) ); } ), d->wavecanvas );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "audition key row:" ), 0, r, 1, 1 );
	d->audkey = gtk_spin_button_new_with_range( _ROW_MIN, _ROW_MAX, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->audkey ), 0x5d ); // ~440Hz with 0x4500 base
	gtk_grid_attach( GTK_GRID( grid ), d->audkey, 1, r++, 1, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "audition length (%):" ), 0, r, 1, 1 );
	d->aurlen = gtk_spin_button_new_with_range( 10, 600, 10 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->aurlen ), 100 );
	gtk_grid_attach( GTK_GRID( grid ), d->aurlen, 1, r++, 1, 1 );

	// live updates: redraw canvas & audition on any parameter change
	auto _live = [d](){
		gtk_widget_queue_draw( d->wavecanvas );
		_audition_sound(
			(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) ),
			(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->wave ) ),
			(int)gtk_range_get_value( GTK_RANGE( d->volume ) ),
			(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->basic_row ) ),
			(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->ntype ) ),
			gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->nfreq ) ),
			gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->noffset ) ),
			gtk_range_get_value( GTK_RANGE( d->nvol ) ),
			(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->audkey ) ), -1 );
	};
	for( GtkWidget* w : { d->type, d->wave, d->ntype } )
		g_signal_connect( w, "notify::selected", G_CALLBACK( +[]( GObject*, GParamSpec*, gpointer ud ){
			SoundDlg* dd = (SoundDlg*)ud;
			gtk_widget_queue_draw( dd->wavecanvas );
			_audition_sound(
				(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( dd->type ) ),
				(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( dd->wave ) ),
				(int)gtk_range_get_value( GTK_RANGE( dd->volume ) ),
				(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( dd->basic_row ) ),
				(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( dd->ntype ) ),
				gtk_spin_button_get_value( GTK_SPIN_BUTTON( dd->nfreq ) ),
				gtk_spin_button_get_value( GTK_SPIN_BUTTON( dd->noffset ) ),
				gtk_range_get_value( GTK_RANGE( dd->nvol ) ),
				(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( dd->audkey ) ), -1 );
		} ), d );
	for( GtkWidget* w : { d->volume, d->basic_row, d->nfreq, d->noffset, d->nvol, d->audkey } )
		g_signal_connect( w, "value-changed", G_CALLBACK( +[]( GtkWidget*, gpointer ud ){
			SoundDlg* dd = (SoundDlg*)ud;
			gtk_widget_queue_draw( dd->wavecanvas );
		} ), d );

	GtkWidget* btn_aud = gtk_button_new_with_label( "audition" );
	g_signal_connect( btn_aud, "clicked", G_CALLBACK( _on_audition_clicked ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn_aud, 0, r, 1, 1 );

	GtkWidget* btn = gtk_button_new_with_label( "create & assign to unit" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_create_clicked ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn, 1, r, 2, 1 );

	g_ed.win_sound = win;
	g_signal_connect( win, "destroy", G_CALLBACK( _win_destroyed ), g_bind_sound );
	gtk_window_present( GTK_WINDOW( win ) );
}

// ---- event editing (VELOCITY / VOLUME / PAN_VOLUME / PAN_TIME) ----------

struct EventKindInfo { uint8_t kind; const char* name; double min, max, def; bool is_float; };
static const EventKindInfo _event_kinds[] =
{
	{ EVENTKIND_VELOCITY,   "velocity",   0, 129, EVENTDEFAULT_VELOCITY,   false },
	{ EVENTKIND_VOLUME,     "volume",     0, 129, EVENTDEFAULT_VOLUME,     false },
	{ EVENTKIND_PAN_VOLUME, "pan volume", 0, 128, EVENTDEFAULT_PAN_VOLUME, false },
	{ EVENTKIND_PAN_TIME,   "pan time",   0, 128, EVENTDEFAULT_PAN_TIME,   false },
	{ EVENTKIND_PORTAMENT,  "portament",  0, 127, EVENTDEFAULT_PORTAMENT,  false },
	{ EVENTKIND_VOICENO,    "voice no",   0,  63, EVENTDEFAULT_VOICENO,    false },
	{ EVENTKIND_GROUPNO,    "group no",   0,  15, EVENTDEFAULT_GROUPNO,    false },
	{ EVENTKIND_TUNING,     "tuning",     0.1, 4.0, EVENTDEFAULT_TUNING,   true  },
};
static const int _event_kind_num = sizeof( _event_kinds ) / sizeof( _event_kinds[0] );

// Write (replace) an event of the given kind at the snapped clock for a unit.
static void _set_event_f( uint8_t kind, int32_t clock, int unit, double value )
{
	if( !g_ed.loaded || unit < 0 || unit >= g_ed.unit_num ) return;
	int32_t c = _snap_clock( clock );

	SDL_LockAudio();
	_push_undo();
	_ensure_evels_capacity();
	g_ed.pxtn->evels->Record_Delete( c, c + 1, (uint8_t)unit, kind );
	if( kind == EVENTKIND_TUNING ) g_ed.pxtn->evels->Record_Add_f( c, (uint8_t)unit, kind, (float)value );
	else                           g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, kind, (int32_t)value );
	SDL_UnlockAudio();

	for( int i = 0; i < _event_kind_num; i++ )
	{
		if( _event_kinds[ i ].kind != kind ) continue;
		if( _event_kinds[ i ].is_float ) _set_status( "%s = %.3f @ clock %d (unit %d)", _event_kinds[ i ].name, value, c, unit );
		else                             _set_status( "%s = %d @ clock %d (unit %d)", _event_kinds[ i ].name, (int)value, c, unit );
		break;
	}
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _set_event( uint8_t kind, int32_t clock, int unit, int32_t value )
{
	_set_event_f( kind, clock, unit, (double)value );
}

static void _on_event_type_changed( GtkDropDown* dd, gpointer user_data )
{
	GtkWidget* value = GTK_WIDGET( user_data );
	int t = (int)gtk_drop_down_get_selected( dd );
	if( t < 0 || t >= _event_kind_num ) return;
	const EventKindInfo& ki = _event_kinds[ t ];
	double v = gtk_range_get_value( GTK_RANGE( value ) );
	if( ki.is_float ) gtk_range_set_range( GTK_RANGE( value ), ki.min, ki.max );
	else              gtk_range_set_range( GTK_RANGE( value ), ki.min, ki.max );
	gtk_range_set_value( GTK_RANGE( value ), ki.def > 0 ? ki.def : ( v > ki.max ? ki.def : v ) );
}

static void _on_event_set_clicked( GtkButton*, gpointer user_data )
{
	struct D { GtkWidget *type, *value; }* d = (D*)user_data;
	int t = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->type ) );
	if( t < 0 || t >= _event_kind_num ) return;
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	// selected note takes priority; otherwise the snapped view start
	int32_t clock = g_ed.has_sel ? g_ed.sel_clock : (int32_t)( g_ed.h_offset / g_ed.px_per_clock );
	_set_event_f( _event_kinds[ t ].kind, clock, unit,
		gtk_range_get_value( GTK_RANGE( d->value ) ) );
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
	g_signal_connect( d->type, "notify::selected", G_CALLBACK( _on_event_type_changed ), d->value );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "at: view start (snapped), selected unit" ), 0, 2, 2, 1 );

	GtkWidget* btn = gtk_button_new_with_label( "set event" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_event_set_clicked ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn, 0, 3, 2, 1 );

	g_ed.win_event = win;
	g_signal_connect( win, "destroy", G_CALLBACK( _win_destroyed ), g_bind_event );
	gtk_window_present( GTK_WINDOW( win ) );
}

// ---- unit rename ---------------------------------------------------------

static void _refresh_unit_combo(); // fwd

static void _on_rename_ok( GtkButton*, gpointer user_data )
{
	typedef struct { GtkWidget *entry,*dlgwin; } D;
	D* d = (D*)user_data;
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;

	char text[ pxtnMAX_TUNEUNITNAME + 1 ];
	const char* src = gtk_editable_get_text( GTK_EDITABLE( d->entry ) );
	snprintf( text, sizeof( text ), "%s", src ? src : "" );
	if( !text[0] ){ gtk_window_destroy( GTK_WINDOW( d->dlgwin ) ); delete d; return; }

	SDL_LockAudio();
	_push_undo();
	g_ed.pxtn->Unit_Get_variable( unit )->set_name_buf( text, strlen( text ) );
	SDL_UnlockAudio();

	_refresh_unit_combo();
	_set_status( "renamed unit %d -> %s", unit, text );
	gtk_window_destroy( GTK_WINDOW( d->dlgwin ) );
	delete d;
}

static void _rename_dialog()
{
	typedef struct { GtkWidget *entry,*dlgwin; } D;
	D* d = new D{};

	GtkWidget* win = gtk_window_new();
	gtk_window_set_title( GTK_WINDOW( win ), "rename unit" );
	gtk_window_set_default_size( GTK_WINDOW( win ), 320, 100 );
	d->dlgwin = win;

	GtkWidget* box = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	gtk_widget_set_margin_start ( box, 10 ); gtk_widget_set_margin_end( box, 10 );
	gtk_widget_set_margin_top   ( box, 10 ); gtk_widget_set_margin_bottom( box, 10 );
	gtk_window_set_child( GTK_WINDOW( win ), box );

	d->entry = gtk_entry_new();
	gtk_widget_set_hexpand( d->entry, TRUE );
	gtk_box_append( GTK_BOX( box ), d->entry );

	GtkWidget* btn = gtk_button_new_with_label( "rename" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_rename_ok ), d );
	gtk_box_append( GTK_BOX( box ), btn );

	g_ed.win_rename = win;
	g_signal_connect( win, "destroy", G_CALLBACK( _win_destroyed ), g_bind_rename );
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

	g_ed.win_song = win;
	g_signal_connect( win, "destroy", G_CALLBACK( _win_destroyed ), g_bind_song );
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

		// loop structure markers: repeat (green) / last (red)
		if( meas_clock > 0 )
		{
			int32_t rm = g_ed.pxtn->master->get_repeat_meas();
			int32_t lm = g_ed.pxtn->master->get_last_meas();
			if( rm >= 0 )
			{
				double x = rm * meas_clock * g_ed.px_per_clock - g_ed.h_offset;
				cairo_set_source_rgb( cr, 0.2, 0.9, 0.3 );
				cairo_set_line_width( cr, 2.0 );
				cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, content_h ); cairo_stroke( cr );
			}
			if( lm >= 0 )
			{
				double x = ( lm + 1 ) * meas_clock * g_ed.px_per_clock - g_ed.h_offset;
				cairo_set_source_rgb( cr, 0.95, 0.25, 0.25 );
				cairo_set_line_width( cr, 2.0 );
				cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, content_h ); cairo_stroke( cr );
			}
			cairo_set_line_width( cr, 1.0 );
		}
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
		// dim notes of non-active units (active unit stands out)
		{
			int sel_unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
			if( sel_unit >= 0 && p->unit_no != sel_unit ){ r *= 0.35; g *= 0.35; b *= 0.35; }
		}
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

static void _sync_scrollbars()
{
	if( !g_ed.hadj || !g_ed.draw_area ) return;
	GtkAllocation alloc;
	gtk_widget_get_allocation( g_ed.draw_area, &alloc );

	double hw = alloc.width  > 1 ? alloc.width  : 100;
	double vh = alloc.height > 1 ? alloc.height : 100;

	double h_up = ( g_ed.pxtn ? ( g_ed.pxtn->evels->get_Max_Clock() + 4800 * 8 ) : 4800 * 32 ) * g_ed.px_per_clock + hw;
	double v_up = ( _ROW_MAX - _ROW_MIN + 1 ) * (double)_ROW_H;

	GtkAdjustment* adjs[2] = { g_ed.hadj, g_ed.vadj };
	double vals [2] = { g_ed.h_offset, g_ed.v_offset };
	double uppers[2] = { h_up, v_up };
	double pages [2] = { hw, MIN( vh, v_up ) }; // page never exceeds range

	for( int i = 0; i < 2; i++ )
	{
		GtkAdjustment* a = adjs[ i ];
		if( fabs( gtk_adjustment_get_upper   ( a ) - uppers[ i ] ) > 0.5 ||
			fabs( gtk_adjustment_get_page_size( a ) - pages[ i ] ) > 0.5 ||
			fabs( gtk_adjustment_get_value    ( a ) - vals[ i ]   ) > 0.5 )
		{
			gtk_adjustment_configure( a, vals[ i ], 0, uppers[ i ], 40, pages[ i ] * 0.9, pages[ i ] );
		}
	}
}

static void _on_hscroll( GtkAdjustment* adj, gpointer )
{
	g_ed.h_offset = gtk_adjustment_get_value( adj );
	if( g_ed.h_offset < 0 ) g_ed.h_offset = 0;
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _on_vscroll( GtkAdjustment* adj, gpointer )
{
	g_ed.v_offset = gtk_adjustment_get_value( adj );
	if( g_ed.v_offset < 0 ) g_ed.v_offset = 0;
	gtk_widget_queue_draw( g_ed.draw_area );
}

static guint g_tick_id = 0;

static void _on_window_destroy( GtkWidget*, gpointer )
{
	if( g_tick_id ){ g_source_remove( g_tick_id ); g_tick_id = 0; }
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
	_sync_scrollbars();
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

	// header controls (horizontally scrollable when the window is narrow)
	GtkWidget* hbox = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	gtk_widget_set_margin_start ( hbox, 8 );
	gtk_widget_set_margin_end   ( hbox, 8 );
	gtk_widget_set_margin_top   ( hbox, 6 );
	gtk_widget_set_margin_bottom( hbox, 6 );
	GtkWidget* header_sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy( GTK_SCROLLED_WINDOW( header_sw ),
		GtkPolicyType::GTK_POLICY_AUTOMATIC, GtkPolicyType::GTK_POLICY_NEVER );
	gtk_scrolled_window_set_child( GTK_SCROLLED_WINDOW( header_sw ), hbox );
	gtk_box_append( GTK_BOX( vbox ), header_sw );

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

	GtkWidget* btn_units = gtk_toggle_button_new_with_label( "units..." );
	GtkWidget* btn_sound = gtk_toggle_button_new_with_label( "sound..." );
	GtkWidget* btn_event = gtk_toggle_button_new_with_label( "event..." );
	GtkWidget* btn_song  = gtk_toggle_button_new_with_label( "song..." );
	GtkWidget* btn_new   = gtk_button_new_with_label( "new" );
	GtkWidget* btn_open  = gtk_button_new_with_label( "open..." );
	GtkWidget* btn_saveas= gtk_button_new_with_label( "save as..." );
	GtkWidget* btn_undo  = gtk_button_new_with_label( "undo" );
	GtkWidget* btn_redo  = gtk_button_new_with_label( "redo" );
	GtkWidget* btn_play  = gtk_toggle_button_new_with_label( "▶ play" );
	GtkToggleButton* tb_stop = nullptr; (void)tb_stop;
	// dialog toggles: one window per button, closed by pressing again
	g_ed.tb_sound  = GTK_TOGGLE_BUTTON( btn_sound );
	g_ed.tb_event  = GTK_TOGGLE_BUTTON( btn_event );
	g_ed.tb_units  = GTK_TOGGLE_BUTTON( btn_units );
	g_ed.tb_song   = GTK_TOGGLE_BUTTON( btn_song );
	g_bind_sound [0] = &g_ed.win_sound;  g_bind_sound [1] = btn_sound; g_bind_sound [2] = (gpointer)_sound_dialog;
	g_bind_event [0] = &g_ed.win_event;  g_bind_event [1] = btn_event; g_bind_event [2] = (gpointer)_event_dialog;
	g_bind_units [0] = &g_ed.win_units;  g_bind_units [1] = btn_units; g_bind_units [2] = (gpointer)_units_dialog;
	g_bind_song  [0] = &g_ed.win_song;   g_bind_song  [1] = btn_song;  g_bind_song  [2] = (gpointer)_song_dialog;
	g_signal_connect( btn_sound, "toggled", G_CALLBACK( _on_toggle_dialog ), g_bind_sound );
	g_signal_connect( btn_event, "toggled", G_CALLBACK( _on_toggle_dialog ), g_bind_event );
	g_signal_connect( btn_units, "toggled", G_CALLBACK( _on_toggle_dialog ), g_bind_units );
	g_signal_connect( btn_song,  "toggled", G_CALLBACK( _on_toggle_dialog ), g_bind_song );

	// combined play/stop toggle
	g_signal_connect( btn_play, "toggled", G_CALLBACK( +[]( GtkToggleButton* b, gpointer ){
		fprintf( stderr, "[play-btn] toggled active=%d playing=%d\n",
			gtk_toggle_button_get_active( b ) ? 1 : 0, g_ed.playing ? 1 : 0 );
		bool active = gtk_toggle_button_get_active( b );
		if( active && !g_ed.playing ) _start_play();
		else if( !active && g_ed.playing ) _stop_play();
	} ), NULL );
	g_signal_connect_swapped( btn_new,   "clicked", G_CALLBACK( +[]( gpointer ){ _new_tune(); } ), NULL );
	g_signal_connect_swapped( btn_open,  "clicked", G_CALLBACK( +[]( gpointer ){ _open_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_saveas,"clicked", G_CALLBACK( +[]( gpointer ){ _save_as_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_undo,  "clicked", G_CALLBACK( +[]( gpointer ){ _undo(); } ), NULL );
	g_signal_connect_swapped( btn_redo,  "clicked", G_CALLBACK( +[]( gpointer ){ _redo(); } ), NULL );
	gtk_box_append( GTK_BOX( hbox ), btn_sound );
	gtk_box_append( GTK_BOX( hbox ), btn_song );
	gtk_box_append( GTK_BOX( hbox ), btn_event );
	gtk_box_append( GTK_BOX( hbox ), btn_units );
	gtk_box_append( GTK_BOX( hbox ), btn_play );
	gtk_box_append( GTK_BOX( hbox ), btn_new );
	gtk_box_append( GTK_BOX( hbox ), btn_open );
	gtk_box_append( GTK_BOX( hbox ), btn_saveas );
	gtk_box_append( GTK_BOX( hbox ), btn_undo );
	gtk_box_append( GTK_BOX( hbox ), btn_redo );

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

	// scrollbars (kept in sync with h_offset / v_offset)
	g_ed.hadj = GTK_ADJUSTMENT( gtk_adjustment_new( 0, 0, 100000, 40, 200, 200 ) );
	g_ed.vadj = GTK_ADJUSTMENT( gtk_adjustment_new( 0, 0, 1600, 40, 200, 200 ) );
	g_signal_connect( g_ed.hadj, "value-changed", G_CALLBACK( _on_hscroll ), NULL );
	g_signal_connect( g_ed.vadj, "value-changed", G_CALLBACK( _on_vscroll ), NULL );

	GtkWidget* center = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 0 );
	gtk_widget_set_hexpand( g_ed.draw_area, TRUE ); // must expand or its width collapses to 0
	gtk_box_append( GTK_BOX( center ), g_ed.draw_area );
	gtk_box_append( GTK_BOX( center ), gtk_scrollbar_new( GTK_ORIENTATION_VERTICAL, g_ed.vadj ) );
	gtk_widget_set_vexpand( center, TRUE );
	gtk_box_append( GTK_BOX( vbox ), center );
	gtk_box_append( GTK_BOX( vbox ), gtk_scrollbar_new( GTK_ORIENTATION_HORIZONTAL, g_ed.hadj ) );

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

	g_tick_id = g_timeout_add( 33, _tick, NULL );
	g_signal_connect( g_ed.window, "destroy", G_CALLBACK( _on_window_destroy ), NULL );

	gtk_window_present( GTK_WINDOW( g_ed.window ) );
}

// ---- load ---------------------------------------------------------------

// Build a fresh default project in memory (no widgets touched).
static bool _init_new_project()
{
	delete g_ed.pxtn;
	g_ed.pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );
	if( g_ed.pxtn->init() != pxtnOK ) return false;
	if( !g_ed.pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ) return false;
	g_ed.pxtn->master->Set( 4, 120.0f, _BEAT_CLOCK );
	g_ed.pxtn->master->set_meas_num   ( 8 );
	g_ed.pxtn->master->set_last_meas  ( 7 );
	g_ed.pxtn->master->set_repeat_meas( 0 ); // loop the whole (short) song by default
	g_ed.pxtn->evels->Allocate( 8192 );
	g_ed.pxtn->Unit_AddNew();
	int w = g_ed.pxtn->Woice_AddNew();
	pxtnWoice* wv = g_ed.pxtn->Woice_Get_variable( w );
	wv->Voice_Allocate( 1 );
	pxtnVOICEUNIT* v = wv->get_voice_variable( 0 );
	v->type      = pxtnVOICE_Overtone;   // stock pxTone default tone style
	v->basic_key = 0x4500;
	v->volume    = 128;
	v->voice_flags = PTV_VOICEFLAG_SMOOTH | PTV_VOICEFLAG_WAVELOOP;
	v->data_flags  = PTV_DATAFLAG_WAVE;
	v->wave.num    = 8;
	v->wave.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 8 );
	_make_harmonic_points( 1, v->wave.points, &v->wave.num ); // bright
	_apply_simple_envelope( v );

	// the unit must have a tone-ready woice, or Moo/preview will crash
	if( g_ed.pxtn->Woice_ReadyTone( w ) != pxtnOK ) return false;
	g_ed.pxtn->Unit_Get_variable( 0 )->set_woice( wv );
	g_ed.pxtn->Unit_Get_variable( 0 )->set_name_buf( "unit 0", 6 );

	// allow Play without loading a file first
	g_ed.pxtn->tones_ready();
	g_ed.pxtn->moo_set_valid_data( true );

	// reset editor state
	g_undo.clear(); g_redo.clear(); g_clipboard.clear();
	g_ed.has_sel = false; g_ed.dragging = false; g_ed.mode = DRAG_NONE;
	g_ed.unit_num = 1; g_ed.tempo = 120.0;
	g_ed.loaded = true; // the fresh project is playable
	g_ed.path.clear();
	g_ed.h_offset = 0; g_ed.v_offset = 0;
	return true;
}

static bool _load_tune()
{
	bool need_new = g_ed.path.empty();

	if( !need_new )
	{
		pxtnService* pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );
		g_ed.pxtn = pxtn;

		pxtnERR err = pxtn->init();
		if( err == pxtnOK ) err = pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ? pxtnOK : pxtnERR_INIT;

		FILE* fp = NULL;
		if( err == pxtnOK && !( fp = fopen( g_ed.path.c_str(), "rb" ) ) )
			{ err = pxtnERR_desc_r; }
		if( err == pxtnOK ){ err = pxtn->read( fp ); fclose( fp ); }
		if( err == pxtnOK && ( err = pxtn->tones_ready() ) != pxtnOK ){}

		if( err != pxtnOK )
		{
			// missing / broken file: fall back to a fresh project
			char msg[ 512 ];
			snprintf( msg, sizeof( msg ), "cannot load '%s' (%s) - creating new tune",
				g_ed.path.c_str(), pxtnError_get_string( err ) );
			_set_status( "%s", msg );
		}
		else
		{
			_ensure_evels_capacity();
			g_ed.unit_num = pxtn->Unit_Num();
			g_ed.tempo    = pxtn->master->get_beat_tempo();
			if( g_ed.tempo <= 0 ) g_ed.tempo = EVENTDEFAULT_BEATTEMPO;
			g_ed.loaded = true;
			return true;
		}
	}

	if( !_init_new_project() ){ g_ed.err = "failed to create new project"; return false; }
	return true;
}

int main( int argc, char** argv )
{
	if( argc >= 2 ) g_ed.path = argv[1]; // no argument: start a new tune

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

	// mixer starts here and runs forever; playback is gated by g_ed.playing
	SDL_PauseAudio( 0 ); // legacy devices open paused
	g_ed.pv_buf.assign( _SAMPLE_PER_SECOND * _CHANNEL_NUM, 0 ); // 1s preview FIFO

	GtkApplication* app = gtk_application_new( "com.github.pxtone.editor", G_APPLICATION_NON_UNIQUE );
	g_signal_connect( app, "activate", G_CALLBACK( _activate ), NULL );
	int ret = g_application_run( G_APPLICATION( app ), 0, NULL );
	g_object_unref( app );

	_stop_play();
	SDL_CloseAudio();
	SDL_Quit();
	return ret;
}
