// pxtone-editor: GTK4 piano-roll editor for .ptcop files.
// Links against the shared libpxtn.so core.
//
// Usage: pxtone-editor [file.ptcop]
//
// Left-drag on empty cell : add a note (snapped) and stretch it
// Left-drag on a note body: move the note
// Left-drag on note right edge: resize
// Right-click / right-drag: delete notes
// Wheel / Shift+Wheel / Ctrl+Wheel: scroll V / scroll H / zoom
// Space or play toggle: start/stop playback
// Ctrl+S save, Ctrl+Z/Y undo/redo, Ctrl+C/V copy/paste
// 1-4: snap length

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

static const int _ROW_MIN = 0x24;
static const int _ROW_MAX = 0x94;
static const int _ROW_H   = 14;

static const int32_t _BEAT_CLOCK = 480;

static bool _pxtn_r( void* u, void* p, int s, int n ){ return fread( p, s, n, (FILE*)u ) >= n; }
static bool _pxtn_w( void* u, const void* p, int s, int n ){ return fwrite( p, s, n, (FILE*)u ) >= n; }
static bool _pxtn_s( void* u, int m, int s ){ return !fseek( (FILE*)u, s, m ); }
static bool _pxtn_p( void* u, int32_t* o )
{
	long i = ftell( (FILE*)u );
	if( i < 0 ) return false;
	*o = (int32_t)i;
	return true;
}

enum DragMode { DRAG_NONE = 0, DRAG_STRETCH, DRAG_RESIZE, DRAG_MOVE };

struct Editor
{
	bool        loaded   = false;
	std::string path;
	std::string err;
	int         unit_num = 0;
	double      tempo    = EVENTDEFAULT_BEATTEMPO;

	// view
	double px_per_clock = 80.0 / _BEAT_CLOCK;
	double h_offset     = 0;
	double v_offset     = 0;
	int    snap         = 240;

	// drag state
	DragMode mode          = DRAG_NONE;
	bool     dragging      = false;
	int32_t  drag_clock    = 0;
	int32_t  drag_unit     = 0;
	int32_t  drag_orig_clk = 0;
	int32_t  drag_cur_clk  = 0;
	int      drag_cur_row  = 0;
	int32_t  drag_dur      = 0;
	int32_t  drag_key      = 0;

	// selection
	bool    has_sel   = false;
	int32_t sel_clock = 0;
	int     sel_unit  = 0;

	// playback
	std::atomic<int64_t> played_samples {0};
	std::atomic<bool>    playing        {false};

	// preview FIFO (frames)
	std::vector<int16_t> pv_buf;
	std::atomic<uint64_t> pv_read {0};
	std::atomic<uint64_t> pv_write{0};

	pxtnService* pxtn     = NULL;

	GtkWidget* notebook    = NULL;

	// units panel widgets
	GtkWidget* units_list  = NULL;
	GtkWidget* units_stats = NULL;
	GtkWidget* rename_entry= NULL;

	// widgets
	GtkWidget* window    = NULL;
	GtkWidget* draw_area = NULL;
	GtkWidget* unit_combo= NULL;
	GtkWidget* status    = NULL;
	GtkAdjustment *hadj  = NULL;
	GtkAdjustment *vadj  = NULL;
	GtkToggleButton* tb_play = nullptr;
	guint tick_id = 0;

	bool file_dlg_busy = false;
};

static Editor g_ed;

static const double PI = 3.141592653589793;

// ---- fwd -----------------------------------------------------------------

static void _set_status( const char* fmt, ... );
static void _refresh_unit_combo();
static void _units_refresh();
static void _add_unit();
static void _preview_note( int unit, int row, int32_t clock, int dur_frames = -1 );
static void _preview_woice( const pxtnWoice* woice, int row, int dur_frames_arg );
static void _start_play();
static void _stop_play();
static void _save();
static void _save_as_dialog();
static void _delete_unit( int idx );
static bool _screen_to_clock_row( int x, int y, int32_t* p_clock, int* p_row );
static void _drag_update( int x, int y );
static void _fix_overlaps( int32_t clock, int unit, int32_t dur );
static void Record_Value_Set_safe( pxtnEvelist* ev, int32_t clock, int unit, int32_t dur );
static void _apply_song( double tempo, int beat_num, int32_t beat_clock,
                         int meas_num, int repeat_meas, int last_meas );
static void _set_event_f( uint8_t kind, int32_t clock, int unit, double value );
static void _make_harmonic_points( int timbre, pxtnPOINT* pts, int32_t* p_num );
static void _apply_simple_envelope( pxtnVOICEUNIT* v );
static void _build_sound_page( GtkWidget* parent );
static void _on_rename_ok( GtkButton*, gpointer user_data );
static void _build_units_page( GtkWidget* parent );
static void _build_event_page( GtkWidget* parent );
static void _build_song_page( GtkWidget* parent );

// ---- undo / redo snapshots -----------------------------------------------

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

// LockAudio serializes against the audio callback; the mixer itself is never
// paused so that previews keep working.
static void _restore_safely( const SongSnap& s )
{
	SDL_LockAudio();
	_restore_snapshot( s );
	SDL_UnlockAudio();
}

static void _push_undo()
{
	g_undo.push_back( _snapshot() );
	if( g_undo.size() > 100 ) g_undo.erase( g_undo.begin() );
	g_redo.clear();
}

static void _refresh_unit_combo()
{
	// rebuild the model wholesale so the dropdown always refreshes
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
}

static void _on_rename_ok( GtkButton*, gpointer user_data )
{
	GtkWidget* entry = GTK_WIDGET( user_data );
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;
	char text[ pxtnMAX_TUNEUNITNAME + 1 ];
	const char* src = gtk_editable_get_text( GTK_EDITABLE( entry ) );
	snprintf( text, sizeof( text ), "%s", src ? src : "" );
	if( !text[0] ) return;
	SDL_LockAudio();
	_push_undo();
	g_ed.pxtn->Unit_Get_variable( unit )->set_name_buf( text, strlen( text ) );
	SDL_UnlockAudio();
	gtk_editable_set_text( GTK_EDITABLE( entry ), "" );
	_refresh_unit_combo();
	_set_status( "renamed unit %d -> %s", unit, text );
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

// ---- helpers --------------------------------------------------------------

static void _set_status( const char* fmt, ... )
{
	char buf[ 512 ];
	va_list ap; va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	fprintf( stderr, "[status] %s\n", buf );
	if( GTK_IS_LABEL( g_ed.status ) ) gtk_label_set_text( GTK_LABEL( g_ed.status ), buf );
}

static double _sec_per_clock()
{
	return 60.0 / ( g_ed.tempo * _BEAT_CLOCK );
}

static int32_t _snap_clock( int32_t clock )
{
	if( clock < 0 ) clock = 0;
	return ( clock / g_ed.snap ) * g_ed.snap;
}

static void Record_Value_Set_safe( pxtnEvelist* ev, int32_t clock, int unit, int32_t dur )
{
	if( dur < 1 ) dur = 1;
	ev->Record_Value_Set( clock, clock + 1, (uint8_t)unit, EVENTKIND_ON, dur );
}

// resolve overlaps like Record_Add_i does when inserting a tail event
static void _fix_overlaps( int32_t clock, int unit, int32_t dur )
{
	pxtnEvelist* ev = g_ed.pxtn->evels;
	std::vector<int32_t> inside;
	int32_t prev_clock = -1;

	for( const EVERECORD* p = ev->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON || p->unit_no != unit || p->clock == clock ) continue;
		if( p->clock < clock && p->clock + p->value > clock ) prev_clock = p->clock;
		if( p->clock > clock && p->clock < clock + dur ) inside.push_back( p->clock );
	}
	if( prev_clock >= 0 ) Record_Value_Set_safe( ev, prev_clock, unit, clock - prev_clock );
	for( int32_t c : inside ) ev->Record_Delete( c, c + 1, (uint8_t)unit, EVENTKIND_ON );
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

static int _key_row( int32_t key ){ return key >> 8; }

static bool _is_black_key( int row )
{
	static const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
	return black[ ((row % 12) + 12) % 12 ];
}

static void _ensure_evels_capacity()
{
	pxtnService* pxtn = g_ed.pxtn;
	int count = 0;
	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next ) count++;
	if( pxtn->evels->get_Num_Max() >= count + 4096 ) return;

	std::vector<EVERECORD> recs;
	recs.reserve( count );
	for( const EVERECORD* p = pxtn->evels->get_Records(); p; p = p->next ) recs.push_back( *p );
	pxtn->evels->Allocate( count + 4096 );
	for( const EVERECORD& r : recs )
		pxtn->evels->Record_Add_i( r.clock, r.unit_no, r.kind, r.value );
}

const EVERECORD* _find_note( int32_t clock, int row, int unit )
{
	for( const EVERECORD* p = g_ed.pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON || p->unit_no != unit ) continue;
		if( p->clock > clock ) break;
		int32_t dur = p->value > 0 ? p->value : g_ed.snap;
		if( clock >= p->clock && clock < p->clock + dur )
		{
			int k = g_ed.pxtn->evels->get_Value( p->clock, (uint8_t)unit, EVENTKIND_KEY ) >> 8;
			if( k == row ) return p;
		}
	}
	return NULL;
}

// ---- audio ----------------------------------------------------------------

static void _sdl_audio_callback( void*, Uint8* stream, int len )
{
	if( !g_ed.pxtn || !g_ed.playing ) memset( stream, 0, len );
	else if( !g_ed.pxtn->Moo( stream, len ) ) memset( stream, 0, len );
	g_ed.played_samples += len / ( _CHANNEL_NUM * sizeof(int16_t) );

	// mix preview FIFO
	int16_t* out = (int16_t*)stream;
	size_t   frames = len / ( _CHANNEL_NUM * sizeof(int16_t) );
	size_t   capf   = g_ed.pv_buf.size() / _CHANNEL_NUM;
	uint64_t rd = g_ed.pv_read;
	uint64_t wr = g_ed.pv_write;
	for( size_t f = 0; f < frames; f++ )
	{
		if( rd >= wr ) break;
		const int16_t* ps = &g_ed.pv_buf[ ( rd % capf ) * _CHANNEL_NUM ];
		for( int c = 0; c < _CHANNEL_NUM; c++ )
		{
			double v = out[ f * _CHANNEL_NUM + c ] / 32768.0 + ps[ c ] / 32768.0;
			if( v >  1 ) v =  1;
			if( v < -1 ) v = -1;
			out[ f * _CHANNEL_NUM + c ] = (int16_t)( v * 32767 );
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
		if( g_ed.tb_play && gtk_toggle_button_get_active( g_ed.tb_play ) )
			gtk_toggle_button_set_active( g_ed.tb_play, FALSE );
		return;
	}

	pxtnVOMITPREPARATION prep = {0};
	prep.flags          |= pxtnVOMITPREPFLAG_loop | pxtnVOMITPREPFLAG_unit_mute;
	prep.start_pos_float = 0;
	prep.master_volume   = 0.80f;
	if( !g_ed.pxtn->moo_preparation( &prep ) )
	{
		fprintf( stderr, "[play] FAILED valid=%d\n", g_ed.pxtn->moo_is_valid_data()?1:0 );
		_set_status( "play preparation failed" );
		return;
	}

	g_ed.played_samples = 0;
	g_ed.playing = true;
	if( g_ed.tb_play && !gtk_toggle_button_get_active( g_ed.tb_play ) )
		gtk_toggle_button_set_active( g_ed.tb_play, TRUE );
	_set_status( "playing" );
}

static void _stop_play()
{
	if( !g_ed.playing ) return;
	g_ed.playing = false;
	if( g_ed.tb_play && gtk_toggle_button_get_active( g_ed.tb_play ) )
		gtk_toggle_button_set_active( g_ed.tb_play, FALSE );
	_set_status( "stopped" );
	gtk_widget_queue_draw( g_ed.draw_area );
}

// ---- preview ---------------------------------------------------------------

static void _preview_woice( const pxtnWoice* woice, int row, int dur_frames_arg )
{
	if( !woice ) return;
	const int32_t SPSEC = _SAMPLE_PER_SECOND;
	const int dur_frames = dur_frames_arg > 0 ? dur_frames_arg : SPSEC * 35 / 100;
	std::vector<float> acc( dur_frames * 2, 0.0f );

	for( int v = 0; v < woice->get_voice_num(); v++ )
	{
		const pxtnVOICEINSTANCE* vi = woice->get_instance( v );
		const pxtnVOICEUNIT*     vc = woice->get_voice( v );
		if( !vi || !vi->p_smp_w || vi->smp_body_w <= 0 ) continue;

		double ratio = pow( 2.0, ( row - ( vc->basic_key >> 8 ) ) / 12.0 );
		double vol   = vc->volume / 128.0;
		const int16_t* p = (const int16_t*)vi->p_smp_w;

		for( int i = 0; i < dur_frames; i++ )
		{
			int64_t si = (int64_t)( i * ratio ) % vi->smp_body_w;
			double env = 1.0;
			if( i < SPSEC * 3 / 1000 ) env = i / (double)( SPSEC * 3 / 1000 );
			if( i > dur_frames - SPSEC / 10 ) env = ( dur_frames - i ) / (double)( SPSEC / 10 );
			acc[ i * 2 + 0 ] += p[ si * 2 + 0 ] / 32768.0f * vol * env;
			acc[ i * 2 + 1 ] += p[ si * 2 + 1 ] / 32768.0f * vol * env;
		}
	}

	size_t capf = g_ed.pv_buf.size() / _CHANNEL_NUM;
	if( capf == 0 ) return;
	for( int i = 0; i < dur_frames; i++ )
	{
		uint64_t wr = g_ed.pv_write;
		if( wr - g_ed.pv_read >= capf ) g_ed.pv_read = wr - capf + 1;
		double l = acc[ i * 2 + 0 ] * 0.6, r = acc[ i * 2 + 1 ] * 0.6;
		if( l >  1 ) l =  1; if( l < -1 ) l = -1;
		if( r >  1 ) r =  1; if( r < -1 ) r = -1;
		g_ed.pv_buf[ ( wr % capf ) * 2 + 0 ] = (int16_t)( l * 32767 );
		g_ed.pv_buf[ ( wr % capf ) * 2 + 1 ] = (int16_t)( r * 32767 );
		g_ed.pv_write = wr + 1;
	}
}

static void _preview_note( int unit, int row, int32_t clock, int dur_frames )
{
	if( unit < 0 || unit >= g_ed.unit_num ) return;
	int32_t vno = g_ed.pxtn->evels->get_Value( clock, (uint8_t)unit, EVENTKIND_VOICENO );
	const pxtnWoice* woice = NULL;
	if( vno >= 0 && vno < g_ed.pxtn->Woice_Num() ) woice = g_ed.pxtn->Woice_Get( vno );
	if( !woice && g_ed.pxtn->Woice_Num() > 0 ) woice = g_ed.pxtn->Woice_Get( 0 );
	_preview_woice( woice, row, dur_frames );
}

// ---- load / new / open / save ----------------------------------------------

static bool _init_new_project()
{
	delete g_ed.pxtn;
	g_ed.pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );
	if( g_ed.pxtn->init() != pxtnOK ) return false;
	if( !g_ed.pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ) return false;
	g_ed.pxtn->master->Set( 4, 120.0f, _BEAT_CLOCK );
	g_ed.pxtn->master->set_meas_num   ( 8 );
	g_ed.pxtn->master->set_last_meas  ( 7 );
	g_ed.pxtn->master->set_repeat_meas( 0 );
	g_ed.pxtn->evels->Allocate( 8192 );
	g_ed.pxtn->Unit_AddNew();
	int w = g_ed.pxtn->Woice_AddNew();
	pxtnWoice* wv = g_ed.pxtn->Woice_Get_variable( w );
	wv->Voice_Allocate( 1 );
	pxtnVOICEUNIT* v = wv->get_voice_variable( 0 );
	v->type      = pxtnVOICE_Overtone;
	v->basic_key = 0x4500;
	v->volume    = 128;
	v->voice_flags = PTV_VOICEFLAG_SMOOTH | PTV_VOICEFLAG_WAVELOOP;
	v->data_flags  = PTV_DATAFLAG_WAVE;
	v->wave.num    = 8;
	v->wave.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 8 );
	v->wave.points[0] = {1,128}; v->wave.points[1] = {2,96}; v->wave.points[2] = {3,64};
	v->wave.points[3] = {4,40};  v->wave.points[4] = {5,24};
	v->wave.num = 5;
	// simple envelope to avoid clicks
	v->data_flags |= PTV_DATAFLAG_ENVELOPE;
	v->envelope.fps = 60; v->envelope.head_num = 3; v->envelope.body_num = 0; v->envelope.tail_num = 1;
	v->envelope.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 4 );
	v->envelope.points[0] = {1,128}; v->envelope.points[1] = {12,90};
	v->envelope.points[2] = {36,70}; v->envelope.points[3] = {6,0};
	if( g_ed.pxtn->Woice_ReadyTone( w ) != pxtnOK ) return false;
	g_ed.pxtn->Unit_Get_variable( 0 )->set_woice( wv );
	g_ed.pxtn->Unit_Get_variable( 0 )->set_name_buf( "unit 0", 6 );
	g_ed.pxtn->tones_ready();
	g_ed.pxtn->moo_set_valid_data( true );

	g_undo.clear(); g_redo.clear(); g_clipboard.clear();
	g_ed.has_sel = false; g_ed.dragging = false; g_ed.mode = DRAG_NONE;
	g_ed.unit_num = 1; g_ed.tempo = 120.0;
	g_ed.loaded = true;
	g_ed.path.clear();
	g_ed.h_offset = 0; g_ed.v_offset = 0;
	return true;
}

static void _new_tune()
{
	SDL_LockAudio();
	bool ok = _init_new_project();
	SDL_UnlockAudio();
	if( !ok ) return;
	_refresh_unit_combo();
	_units_refresh();
	_set_status( "new tune created (unsaved)" );
	gtk_widget_queue_draw( g_ed.draw_area );
}

static bool _load_tune()
{
	bool need_new = g_ed.path.empty();

	if( !need_new )
	{
		pxtnService* pxtn = new pxtnService( _pxtn_r, _pxtn_w, _pxtn_s, _pxtn_p );
		g_ed.pxtn = pxtn;
		pxtnERR err = pxtn->init();
		if( err == pxtnOK && !pxtn->set_destination_quality( _CHANNEL_NUM, _SAMPLE_PER_SECOND ) ) err = pxtnERR_INIT;

		FILE* fp = NULL;
		if( err == pxtnOK && !( fp = fopen( g_ed.path.c_str(), "rb" ) ) ) err = pxtnERR_desc_r;
		if( err == pxtnOK ){ err = pxtn->read( fp ); fclose( fp ); }
		if( err == pxtnOK ) err = pxtn->tones_ready();

		if( err != pxtnOK )
		{
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

static void _save_as_path( const char* path )
{
	g_ed.path = path;
	_save();
}

static void _save()
{
	if( !g_ed.loaded ) return;
	if( g_ed.path.empty() ){ _save_as_dialog(); return; }
	FILE* fp = fopen( g_ed.path.c_str(), "wb" );
	if( !fp ){ _set_status( "ERROR: cannot write %s", g_ed.path.c_str() ); return; }
	pxtnERR err = g_ed.pxtn->write( fp, false, 0x0500 );
	fclose( fp );
	if( err != pxtnOK ){ _set_status( "ERROR: %s", pxtnError_get_string( err ) ); return; }
	_set_status( "saved: %s", g_ed.path.c_str() );
}

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
	_units_refresh();
	_set_status( "opened: %s", path );
	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _on_open_response( GObject* src, GAsyncResult* res, gpointer )
{
	GError* err = NULL;
	GFile* f = gtk_file_dialog_open_finish( GTK_FILE_DIALOG( src ), res, &err );
	g_ed.file_dlg_busy = false;
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
	if( g_ed.file_dlg_busy ) return;
	g_ed.file_dlg_busy = true;
	GtkFileDialog* dlg = gtk_file_dialog_new();
	gtk_file_dialog_set_title( dlg, "open ptcop" );
	GtkFileFilter* f = gtk_file_filter_new();
	gtk_file_filter_set_name( f, "pxtone project (*.ptcop)" );
	gtk_file_filter_add_pattern( f, "*.ptcop" );
	GListStore* store = g_list_store_new( GTK_TYPE_FILE_FILTER );
	g_list_store_append( store, f );
	gtk_file_dialog_set_filters( dlg, G_LIST_MODEL( store ) );
	g_object_unref( store ); g_object_unref( f );
	gtk_file_dialog_open( dlg, GTK_WINDOW( g_ed.window ), NULL, _on_open_response, NULL );
	g_object_unref( dlg );
}

static void _on_save_response( GObject* src, GAsyncResult* res, gpointer )
{
	GError* err = NULL;
	GFile* f = gtk_file_dialog_save_finish( GTK_FILE_DIALOG( src ), res, &err );
	g_ed.file_dlg_busy = false;
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

// ---- editing ---------------------------------------------------------------


static void _add_note( int32_t clock, int row )
{
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;
	int32_t c = _snap_clock( clock );

	SDL_LockAudio();
	_push_undo();
	_ensure_evels_capacity();
	if( g_ed.pxtn->evels->get_Value( c, (uint8_t)unit, EVENTKIND_KEY ) != ( row << 8 ) )
	{
		g_ed.pxtn->evels->Record_Delete( c, c + 1, (uint8_t)unit, EVENTKIND_KEY );
		g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, EVENTKIND_KEY, row << 8 );
	}
	g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, EVENTKIND_ON, g_ed.snap );
	SDL_UnlockAudio();

	_preview_note( unit, row, c );
	_units_refresh();

	g_ed.dragging   = true;
	g_ed.drag_clock = c;
	g_ed.drag_unit  = unit;
	g_ed.mode       = DRAG_STRETCH;
	g_ed.has_sel    = true;
	g_ed.sel_clock  = c;
	g_ed.sel_unit   = unit;
	gtk_widget_queue_draw( g_ed.draw_area );
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
	_units_refresh();
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
		int32_t dur = c - g_ed.drag_orig_clk;
		if( dur < g_ed.snap ) dur = g_ed.snap;
		if( dur > max ) dur = max;
		SDL_LockAudio();
		Record_Value_Set_safe( ev, g_ed.drag_orig_clk, g_ed.drag_unit, dur );
		_fix_overlaps( g_ed.drag_orig_clk, g_ed.drag_unit, dur );
		SDL_UnlockAudio();
		g_ed.drag_dur = dur;
	}
	else if( g_ed.mode == DRAG_MOVE )
	{
		int row = _ROW_MAX - (int)floor( ( y + g_ed.v_offset ) / (double)_ROW_H );
		if( row < _ROW_MIN ) row = _ROW_MIN;
		if( row > _ROW_MAX ) row = _ROW_MAX;
		if( c == g_ed.drag_cur_clk && row == g_ed.drag_cur_row ) return;
		SDL_LockAudio();
		ev->Record_Delete( g_ed.drag_cur_clk, g_ed.drag_cur_clk + 1, (uint8_t)g_ed.drag_unit, EVENTKIND_ON );
		ev->Record_Delete( g_ed.drag_cur_clk, g_ed.drag_cur_clk + 1, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY );
		if( ev->get_Value( c, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY ) != ( g_ed.drag_key << 8 ) )
		{
			ev->Record_Delete( c, c + 1, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY );
			ev->Record_Add_i( c, (uint8_t)g_ed.drag_unit, EVENTKIND_KEY, g_ed.drag_key << 8 );
		}
		ev->Record_Add_i( c, (uint8_t)g_ed.drag_unit, EVENTKIND_ON, g_ed.drag_dur );
		_fix_overlaps( c, g_ed.drag_unit, g_ed.drag_dur );
		SDL_UnlockAudio();
		g_ed.drag_cur_clk = c;
		g_ed.drag_cur_row = row;
	}
	else if( g_ed.mode == DRAG_STRETCH && g_ed.dragging )
	{
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

// ---- units panel -----------------------------------------------------------

static void _units_refresh()
{
	if( !g_ed.units_list || !g_ed.pxtn ) return;

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
		int events = g_ed.pxtn->evels ? g_ed.pxtn->evels->get_Count( (uint8_t)i ) : 0;
		int notes  = g_ed.pxtn->evels ? g_ed.pxtn->evels->get_Count( (uint8_t)i, EVENTKIND_ON ) : 0;
		total_events += events;

		GtkWidget* row = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
		GtkWidget* lbl = gtk_label_new( NULL );
		char txt[ 256 ];
		snprintf( txt, sizeof( txt ), "%d: %s  (%d evt / %d notes)%s",
			i, name && name[0] ? name : "(no name)", events, notes, i == sel ? "  <" : "" );
		gtk_label_set_text( GTK_LABEL( lbl ), txt );
		gtk_widget_set_hexpand( lbl, TRUE );
		gtk_widget_set_halign( lbl, GTK_ALIGN_START );
		gtk_box_append( GTK_BOX( row ), lbl );

		GtkWidget* use = gtk_check_button_new_with_label( "audible" );
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( use ), g_ed.pxtn->Unit_Get( i )->get_played() );
		g_object_set_data( G_OBJECT( use ), "uidx", GINT_TO_POINTER( i ) );
		g_signal_connect( use, "toggled", G_CALLBACK( +[]( GtkToggleButton* b, gpointer ){
			int i = GPOINTER_TO_INT( g_object_get_data( G_OBJECT( b ), "uidx" ) );
			g_ed.pxtn->Unit_Get_variable( i )->set_played( gtk_toggle_button_get_active( b ) );
			_set_status( "unit %d %s", i, gtk_toggle_button_get_active( b ) ? "audible" : "muted" );
		} ), NULL );
		gtk_box_append( GTK_BOX( row ), use );

		if( g_ed.unit_num > 1 )
		{
			GtkWidget* del = gtk_button_new_with_label( "del" );
			g_object_set_data( G_OBJECT( del ), "uidx", GINT_TO_POINTER( i ) );
			g_signal_connect_swapped( del, "clicked", G_CALLBACK( +[]( gpointer ud ){
				_delete_unit( GPOINTER_TO_INT( ud ) );
			} ), GINT_TO_POINTER( i ) );
			gtk_box_append( GTK_BOX( row ), del );
		}
		gtk_list_box_append( GTK_LIST_BOX( g_ed.units_list ), row );
	}

	if( g_ed.units_stats )
	{
		int notes = 0;
		for( const EVERECORD* p = g_ed.pxtn->evels->get_Records(); p; p = p->next )
			if( p->kind == EVENTKIND_ON ) notes++;
		char txt[ 128 ];
		snprintf( txt, sizeof( txt ), "units: %d   events: %d   notes: %d", g_ed.unit_num, total_events, notes );
		gtk_label_set_text( GTK_LABEL( g_ed.units_stats ), txt );
	}
}

static void _delete_unit( int idx )
{
	if( idx < 0 || idx >= g_ed.unit_num || g_ed.unit_num <= 1 )
		{ _set_status( "cannot delete the last unit" ); return; }
	SDL_LockAudio();
	_push_undo();
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

static void _add_unit()
{
	if( !g_ed.loaded ) return;
	if( !g_ed.pxtn->Unit_AddNew() ){ _set_status( "unit max reached" ); return; }
	int idx = g_ed.pxtn->Unit_Num() - 1;
	g_ed.unit_num = idx + 1;

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
	_units_refresh();
	_set_status( "added %s", name );
}

// ---- sound creation --------------------------------------------------------

static void _apply_simple_envelope( pxtnVOICEUNIT* v )
{
	v->data_flags |= PTV_DATAFLAG_ENVELOPE;
	pxtnVOICEENVELOPE& e = v->envelope;
	e.fps = 60; e.head_num = 3; e.body_num = 0; e.tail_num = 1;
	e.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 4 );
	e.points[0] = {1,128}; e.points[1] = {12,90};
	e.points[2] = {36,70}; e.points[3] = {6,0};
}

static void _make_harmonic_points( int timbre, pxtnPOINT* pts, int32_t* p_num )
{
	switch( timbre )
	{
	case 1: pts[0]={1,128}; pts[1]={2,96}; pts[2]={3,64}; pts[3]={4,40}; pts[4]={5,24}; *p_num=5; break;
	case 2: pts[0]={1,128}; pts[1]={3,64}; pts[2]={5,32}; *p_num=3; break;
	case 3: pts[0]={1,128}; pts[1]={2,80}; pts[2]={4,40}; *p_num=3; break;
	case 4: pts[0]={1,128}; pts[1]={2,20}; pts[2]={4,90}; pts[3]={6,30}; *p_num=4; break;
	default: pts[0]={1,128}; *p_num=1; break;
	}
}

static bool _build_sound_woice( pxtnWoice* w, int type, int wave, int volume, int basic_row,
                                int noise_type, double nfreq, double noffset, double nvol )
{
	if( !w->Voice_Allocate( 1 ) ) return false;
	pxtnVOICEUNIT* v = w->get_voice_variable( 0 );

	if( type == 0 ) // PTV overtone synthesis
	{
		v->type      = pxtnVOICE_Overtone;
		v->basic_key = basic_row << 8;
		v->volume    = volume;
		v->pan       = 64;
		v->tuning    = 1.0f;
		v->voice_flags = PTV_VOICEFLAG_SMOOTH | PTV_VOICEFLAG_WAVELOOP;
		v->data_flags  = PTV_DATAFLAG_WAVE;
		v->wave.num    = 8;
		v->wave.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 8 );
		_make_harmonic_points( wave, v->wave.points, &v->wave.num );
	}
	else // PTN noise
	{
		if( nfreq < 50 ) nfreq = 400;
		if( !v->p_ptn->Allocate( 1, 1 ) ) return false;
		pxNOISEDESIGN_UNIT* du = v->p_ptn->get_unit( 0 );
		du->bEnable = true;
		du->enve_num = 1; du->enves[0].x = 10; du->enves[0].y = 100;
		du->pan = 64;
		du->main.type   = (pxWAVETYPE)( pxWAVETYPE_None + 1 + noise_type );
		du->main.freq   = (float)nfreq;
		du->main.volume = (float)nvol * 100;
		du->main.offset = (float)noffset;
		du->main.b_rev  = false;
		du->freq.type = pxWAVETYPE_None; du->freq.volume = 0;
		du->volu.type = pxWAVETYPE_Sine; du->volu.freq = 0;
		du->volu.volume = 100; du->volu.offset = 25; du->volu.b_rev = false;
		v->p_ptn->set_smp_num_44k( _SAMPLE_PER_SECOND / 4 );
		v->p_ptn->Fix();

		v->type      = pxtnVOICE_Noise;
		v->basic_key = basic_row << 8;
		v->volume    = volume;
		v->pan       = 64;
	}
	_apply_simple_envelope( v );
	return true;
}

static void _create_sound( int type, int wave, int volume, int basic_row,
                           int noise_type, double nfreq, double noffset, double nvol )
{
	int idx = g_ed.pxtn->Woice_AddNew();
	if( idx < 0 ){ _set_status( "woice max reached" ); return; }
	pxtnWoice* w = g_ed.pxtn->Woice_Get_variable( idx );
	if( !_build_sound_woice( w, type, wave, volume, basic_row, noise_type, nfreq, noffset, nvol ) )
		{ _set_status( "voice build failed" ); return; }

	char wname[ 32 ]; snprintf( wname, sizeof( wname ), "%s %d", type == 0 ? "ptv" : "ptn", idx );
	w->set_name_buf( wname, strlen( wname ) );
	if( g_ed.pxtn->Woice_ReadyTone( idx ) != pxtnOK ){ _set_status( "tone ready failed" ); return; }

	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit >= 0 && unit < g_ed.unit_num )
		g_ed.pxtn->Unit_Get_variable( unit )->set_woice( w );
	_preview_woice( w, 0x45 + 24, -1 );
	_set_status( "created %s (woice %d)", wname, idx );
	_units_refresh();
}

static void _audition_sound( int type, int wave, int volume, int basic_row,
                             int noise_type, double nfreq, double noffset, double nvol,
                             int aud_row, int dur_frames )
{
	int idx = g_ed.pxtn->Woice_AddNew();
	if( idx < 0 ){ _set_status( "woice max reached" ); return; }
	pxtnWoice* w = g_ed.pxtn->Woice_Get_variable( idx );
	if( !_build_sound_woice( w, type, wave, volume, basic_row, noise_type, nfreq, noffset, nvol ) )
		{ g_ed.pxtn->Woice_Remove( idx ); return; }
	if( g_ed.pxtn->Woice_ReadyTone( idx ) != pxtnOK )
		{ _set_status( "audition: tone ready failed" ); g_ed.pxtn->Woice_Remove( idx ); return; }
	_preview_woice( w, aud_row, dur_frames );
	_set_status( "audition: %d frames queued", (int)( g_ed.pv_write - g_ed.pv_read ) );
	g_ed.pxtn->Woice_Remove( idx );
}

// ---- events (velocity etc.) ------------------------------------------------

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
		if( _event_kinds[ i ].is_float ) _set_status( "%s = %.2f @ clock %d (unit %d)", _event_kinds[ i ].name, value, c, unit );
		else                             _set_status( "%s = %d @ clock %d (unit %d)", _event_kinds[ i ].name, (int)value, c, unit );
		break;
	}
	gtk_widget_queue_draw( g_ed.draw_area );
}

// ---- song settings ----------------------------------------------------------

static void _apply_song( double tempo, int beat_num, int32_t beat_clock,
                         int meas_num, int repeat_meas, int last_meas )
{
	if( !g_ed.loaded ) return;
	if( last_meas < 0 ) last_meas = meas_num - 1;
	SDL_LockAudio();
	_push_undo();
	g_ed.pxtn->master->Set( beat_num, (float)tempo, beat_clock );
	g_ed.pxtn->master->set_repeat_meas( repeat_meas );
	g_ed.pxtn->master->set_last_meas  ( last_meas );
	g_ed.pxtn->master->set_meas_num   ( last_meas );
	SDL_UnlockAudio();
	g_ed.tempo = tempo;
	_set_status( "song: tempo=%.1f beats=%d clock=%d meas=%d repeat=%d last=%d",
		tempo, beat_num, beat_clock, meas_num, repeat_meas, last_meas );
	gtk_widget_queue_draw( g_ed.draw_area );
}

// ---- coordinate helpers -----------------------------------------------------

static bool _screen_to_clock_row( int x, int y, int32_t* p_clock, int* p_row )
{
	int32_t clock = (int32_t)( ( g_ed.h_offset + x ) / g_ed.px_per_clock );
	int row = _ROW_MAX - (int)floor( ( y + g_ed.v_offset ) / (double)_ROW_H );
	if( row < _ROW_MIN || row > _ROW_MAX ) return false;
	if( clock < 0 ) return false;
	*p_clock = clock; *p_row = row;
	return true;
}

// ---- drawing ----------------------------------------------------------------

static void _draw_cb( GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer )
{
	cairo_set_source_rgb( cr, 0.06, 0.06, 0.10 );
	cairo_paint( cr );

	const int rows = _ROW_MAX - _ROW_MIN + 1;
	const double content_h = rows * _ROW_H;

	for( int r = _ROW_MIN; r <= _ROW_MAX; r++ )
	{
		double y = ( _ROW_MAX - r ) * _ROW_H - g_ed.v_offset;
		if( y + _ROW_H < 0 || y > h ) continue;
		if( _is_black_key( r ) )
		{
			cairo_set_source_rgb( cr, 0.10, 0.10, 0.14 );
			cairo_rectangle( cr, 0, y, w, _ROW_H );
			cairo_fill( cr );
		}
		if( r % 12 == 0 )
		{
			cairo_set_source_rgb( cr, 0.25, 0.25, 0.35 );
			cairo_move_to( cr, 0, y + _ROW_H ); cairo_line_to( cr, w, y + _ROW_H );
			cairo_stroke( cr );
		}
	}

	int32_t beat_clock  = g_ed.pxtn->master->get_beat_clock();
	int32_t meas_clock  = beat_clock * g_ed.pxtn->master->get_beat_num();
	{
		int32_t first_c = (int32_t)( g_ed.h_offset / g_ed.px_per_clock );
		int32_t last_c  = (int32_t)( ( g_ed.h_offset + w ) / g_ed.px_per_clock ) + g_ed.snap;
		for( int32_t c = _snap_clock( first_c ); c <= last_c; c += g_ed.snap )
		{
			bool m = ( meas_clock > 0 && c % meas_clock == 0 );
			bool b = ( c % beat_clock == 0 );
			double x = c * g_ed.px_per_clock - g_ed.h_offset;
			cairo_set_source_rgb( cr, m?0.55:b?0.30:0.16, m?0.45:b?0.30:0.16, m?0.75:b?0.42:0.22 );
			cairo_set_line_width( cr, m ? 2.0 : 1.0 );
			cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, content_h ); cairo_stroke( cr );
		}
		cairo_set_line_width( cr, 1.0 );

		int32_t rm = g_ed.pxtn->master->get_repeat_meas();
		int32_t lm = g_ed.pxtn->master->get_last_meas();
		if( rm >= 0 ){ double x = rm * meas_clock * g_ed.px_per_clock - g_ed.h_offset;
			cairo_set_source_rgb( cr, 0.2, 0.9, 0.3 ); cairo_set_line_width( cr, 2 );
			cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, content_h ); cairo_stroke( cr ); }
		if( lm >= 0 ){ double x = ( lm + 1 ) * meas_clock * g_ed.px_per_clock - g_ed.h_offset;
			cairo_set_source_rgb( cr, 0.95, 0.25, 0.25 ); cairo_set_line_width( cr, 2 );
			cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, content_h ); cairo_stroke( cr ); }
		cairo_set_line_width( cr, 1.0 );
	}

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

		double r, g, b;
		_unit_color( p->unit_no, &r, &g, &b );
		{ // velocity shading
			int32_t vel = g_ed.pxtn->evels->get_Value( p->clock, p->unit_no, EVENTKIND_VELOCITY );
			double f = 0.35 + 0.65 * ( vel / 129.0 ); r *= f; g *= f; b *= f;
		}
		{ // dim non-active units
			int su = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
			if( su >= 0 && p->unit_no != su ){ r *= 0.35; g *= 0.35; b *= 0.35; }
		}
		cairo_set_source_rgb( cr, r, g, b );
		cairo_rectangle( cr, x0, y + 1, x1 - x0, _ROW_H - 2 ); cairo_fill( cr );
		cairo_set_source_rgb( cr, r * 0.5, g * 0.5, b * 0.5 );
		cairo_rectangle( cr, x0, y + 1, x1 - x0, _ROW_H - 2 ); cairo_stroke( cr );
	}

	if( g_ed.playing )
	{
		double sec = (double)g_ed.played_samples / _SAMPLE_PER_SECOND;
		double x   = sec / _sec_per_clock() * g_ed.px_per_clock - g_ed.h_offset;
		cairo_set_source_rgb( cr, 0.95, 0.95, 0.95 );
		cairo_move_to( cr, x, 0 ); cairo_line_to( cr, x, h ); cairo_stroke( cr );
	}

	cairo_set_source_rgb( cr, 0.03, 0.03, 0.05 );
	cairo_rectangle( cr, 0, content_h, w, h - content_h ); cairo_fill( cr );
}

// ---- scroll / zoom ----------------------------------------------------------

static void _sync_scrollbars()
{
	if( !g_ed.hadj || !g_ed.draw_area ) return;
	GtkAllocation alloc;
	gtk_widget_get_allocation( g_ed.draw_area, &alloc );
	double hw = alloc.width  > 1 ? alloc.width  : 100;
	double vh = alloc.height > 1 ? alloc.height : 100;
	double h_up = ( g_ed.pxtn ? ( g_ed.pxtn->evels->get_Max_Clock() + 4800 * 8 ) : 4800 * 32 ) * g_ed.px_per_clock + hw;
	double v_up = ( _ROW_MAX - _ROW_MIN + 1 ) * (double)_ROW_H;

	GtkAdjustment* adjs[2]  = { g_ed.hadj, g_ed.vadj };
	double vals [2]  = { g_ed.h_offset, g_ed.v_offset };
	double uppers[2] = { h_up, v_up };
	double pages [2] = { hw, MIN( vh, v_up ) };
	for( int i = 0; i < 2; i++ )
	{
		GtkAdjustment* a = adjs[ i ];
		if( fabs( gtk_adjustment_get_upper( a )    - uppers[ i ] ) > 0.5 ||
			fabs( gtk_adjustment_get_page_size( a ) - pages[ i ] ) > 0.5 ||
			fabs( gtk_adjustment_get_value( a )     - vals[ i ]   ) > 0.5 )
			gtk_adjustment_configure( a, vals[ i ], 0, uppers[ i ], 40, pages[ i ] * 0.9, pages[ i ] );
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

// ---- input ------------------------------------------------------------------

static void _on_drag_begin( GtkGestureDrag*, double x, double y, gpointer )
{
	int32_t clock; int row;
	if( !_screen_to_clock_row( (int)x, (int)y, &clock, &row ) ) return;
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;

	const EVERECORD* hit = _find_note( clock, row, unit );
	if( hit )
	{
		_push_undo();
		double end_x = ( hit->clock + hit->value ) * g_ed.px_per_clock - g_ed.h_offset;
		g_ed.mode          = ( x > end_x - 10.0 ) ? DRAG_RESIZE : DRAG_MOVE;
		g_ed.drag_unit     = unit;
		g_ed.drag_orig_clk = hit->clock;
		g_ed.drag_cur_clk  = hit->clock;
		g_ed.drag_cur_row  = row;
		g_ed.drag_dur      = hit->value > 0 ? hit->value : g_ed.snap;
		g_ed.drag_key      = g_ed.pxtn->evels->get_Value( hit->clock, (uint8_t)unit, EVENTKIND_KEY ) >> 8;
		g_ed.has_sel       = true;
		g_ed.sel_clock     = hit->clock;
		g_ed.sel_unit      = unit;
		_set_status( "%s: unit=%d clock=%d len=%d", g_ed.mode == DRAG_RESIZE ? "resize" : "move",
			unit, hit->clock, g_ed.drag_dur );
	}
	else _add_note( clock, row );
}

static void _on_drag_update( GtkGestureDrag* gesture, double, double, gpointer )
{
	double x = 0, y = 0;
	gtk_gesture_get_point( GTK_GESTURE( gesture ), NULL, &x, &y );
	_drag_update( (int)x, (int)y );
}

static void _on_drag_end( GtkGestureDrag*, double, double, gpointer )
{
	g_ed.dragging = false;
	g_ed.mode     = DRAG_NONE;
}

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

static gboolean _on_scroll( GtkEventControllerScroll*, double dx, double dy, gpointer state )
{
	GdkModifierType mod = (GdkModifierType)(uintptr_t)state;
	if( mod & GDK_CONTROL_MASK )
	{
		double px = g_ed.px_per_clock * ( dy < 0 ? 1.2 : 1 / 1.2 );
		if( px > 2.0 ) px = 2.0;
		if( px < 0.02 ) px = 0.02;
		g_ed.px_per_clock = px;
	}
	else if( mod & GDK_SHIFT_MASK ) g_ed.h_offset += dx * 40 + dy * 40;
	else                            g_ed.v_offset += dy * 40;

	if( g_ed.h_offset < 0 ) g_ed.h_offset = 0;
	if( g_ed.v_offset < 0 ) g_ed.v_offset = 0;
	{ double mv = MAX( 0.0, ( _ROW_MAX - _ROW_MIN + 1 ) * (double)_ROW_H - 100 );
	  if( g_ed.v_offset > mv ) g_ed.v_offset = mv; }
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
		if( !g_ed.has_sel ) return TRUE;
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
			_set_status( "copied (unit=%d key=0x%02x len=%d)", g_ed.sel_unit, k, p->value );
		}
		return TRUE;
	}
	if( ( state & GDK_CONTROL_MASK ) && keyval == 'v' )
	{
		if( g_clipboard.empty() || !g_ed.loaded ) return TRUE;
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
		static const int snaps[] = { _BEAT_CLOCK, _BEAT_CLOCK/2, _BEAT_CLOCK/4, _BEAT_CLOCK/8 };
		g_ed.snap = snaps[ keyval - '1' ];
		_set_status( "snap: %d clocks", g_ed.snap );
		return TRUE;
	}
	return FALSE;
}

// ---- tick -------------------------------------------------------------------

static guint g_tick_id = 0;

static void _on_window_destroy( GtkWidget*, gpointer )
{
	if( g_tick_id ){ g_source_remove( g_tick_id ); g_tick_id = 0; }
}

static gboolean _tick( gpointer )
{
	if( g_ed.playing )
	{
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

// ---- sound / units / event / song panels ------------------------------------

typedef struct {
	GtkWidget *type, *wave, *volume, *basic_row;
	GtkWidget *ntype, *nfreq, *noffset, *nvol;
	GtkWidget *audkey, *aurlen, *canvas;
	GtkWidget *ptv_rows[8]; int ptv_n;
	GtkWidget *ptn_rows[8]; int ptn_n;
} SoundDlg;
static SoundDlg g_snd;

static void _snd_live_audition()
{
	_audition_sound(
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.type ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.wave ) ),
		(int)gtk_range_get_value( GTK_RANGE( g_snd.volume ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.basic_row ) ),
		(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.ntype ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.nfreq ) ),
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.noffset ) ),
		gtk_range_get_value( GTK_RANGE( g_snd.nvol ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.audkey ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.aurlen ) ) * 44100 * 35 / 10000 );
}

// coordinate wave presets (also used for the noise-oscillator curve display)
static void _make_wave_points( int type, pxtnPOINT* pts, int32_t* p_num )
{
	switch( type )
	{
	case 1: pts[0]={0,-100}; pts[1]={9999,100}; *p_num=2; break;
	case 2: pts[0]={0,100}; pts[1]={4999,100}; pts[2]={5000,-100}; pts[3]={9999,-100}; *p_num=4; break;
	case 3: pts[0]={0,-100}; pts[1]={4999,100}; pts[2]={9999,-100}; *p_num=3; break;
	case 4: pts[0]={0,100}; pts[1]={2499,100}; pts[2]={2500,-100}; pts[3]={9999,-100}; *p_num=4; break;
	default: *p_num=32;
		for( int i = 0; i < 32; i++ ){ pts[i].x=(int)(10000.0*i/32); pts[i].y=(int)(sin(2*PI*i/32)*100); }
		break;
	}
}

static void _snd_canvas_cb( GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer )
{
	cairo_set_source_rgb( cr, 0.08, 0.08, 0.12 ); cairo_paint( cr );
	bool ptv = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.type ) ) == 0;
	cairo_set_source_rgb( cr, 0.35, 0.85, 1.0 );

	if( ptv )
	{
		pxtnPOINT pts[8]; int32_t n = 0;
		int sel = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.wave ) );
		if( sel < 0 ) return;
		_make_harmonic_points( sel, pts, &n );
		double bw = ( w - 8 ) / 5.0;
		for( int i = 0; i < n; i++ )
		{
			double x  = 4 + bw * i + bw * 0.2;
			double bh = h * 0.7 * pts[i].y / 128.0;
			cairo_rectangle( cr, x, h - 6 - bh, bw * 0.6, bh ); cairo_fill( cr );
		}
	}
	else
	{
		int sel = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.ntype ) );
		if( sel < 0 ) return;
		pxtnPOINT pts[32]; int32_t n = 0;
		_make_wave_points( sel, pts, &n );
		cairo_set_line_width( cr, 2 );
		for( int i = 0; i <= n; i++ )
		{
			const pxtnPOINT& pt = pts[ i % n ];
			double x = 4 + ( w - 8 ) * pt.x / 10000.0;
			double y = h / 2 - h * 0.4 * pt.y / 128.0;
			if( i == 0 ) cairo_move_to( cr, x, y ); else cairo_line_to( cr, x, y );
		}
		cairo_stroke( cr );
	}
}

static void _build_sound_page( GtkWidget* parent )
{
	SoundDlg* d = &g_snd;
	memset( d, 0, sizeof( *d ) );

	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing( GTK_GRID( grid ), 6 );
	gtk_grid_set_column_spacing( GTK_GRID( grid ), 8 );
	gtk_widget_set_margin_start( grid, 10 ); gtk_widget_set_margin_end( grid, 10 );
	gtk_widget_set_margin_top( grid, 10 ); gtk_widget_set_margin_bottom( grid, 10 );
	gtk_box_append( GTK_BOX( parent ), grid );
	int r = 0;

	auto attach_label = [&]( const char* t ){ GtkWidget* l = gtk_label_new( t );
		gtk_grid_attach( GTK_GRID( grid ), l, 0, r, 1, 1 ); return l; };

	attach_label( "type:" );
	d->type = gtk_drop_down_new_from_strings( (const char*[]){ "PTV tone", "PTN noise", NULL } );
	gtk_grid_attach( GTK_GRID( grid ), d->type, 1, r++, 1, 1 );

	// PTV rows
	d->ptv_n = 0;
	d->ptv_rows[ d->ptv_n++ ] = attach_label( "timbre:" );
	d->wave = gtk_drop_down_new_from_strings( (const char*[]){ "pure", "bright", "hollow", "warm", "reedy", NULL } );
	gtk_grid_attach( GTK_GRID( grid ), d->wave, 1, r++, 1, 1 );
	d->ptv_rows[ d->ptv_n++ ] = d->wave;
	attach_label( "volume:" );
	d->volume = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0, 128, 1 );
	gtk_range_set_value( GTK_RANGE( d->volume ), 100 );
	gtk_widget_set_hexpand( d->volume, TRUE );
	gtk_grid_attach( GTK_GRID( grid ), d->volume, 1, r++, 1, 1 );
	d->ptv_rows[ d->ptv_n++ ] = d->volume;

	// PTN rows
	d->ptn_n = 0;
	d->ptn_rows[ d->ptn_n++ ] = attach_label( "noise osc:" );
	d->ntype = gtk_drop_down_new_from_strings( (const char*[]){ "random", "sine", "saw", "rect", "saw2", "rect2", "tri", "random2", NULL } );
	gtk_grid_attach( GTK_GRID( grid ), d->ntype, 1, r++, 1, 1 );
	d->ptn_rows[ d->ptn_n++ ] = d->ntype;
	attach_label( "noise freq:" );
	d->nfreq = gtk_spin_button_new_with_range( 50, 5000, 10 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->nfreq ), 400 );
	gtk_grid_attach( GTK_GRID( grid ), d->nfreq, 1, r++, 1, 1 );
	d->ptn_rows[ d->ptn_n++ ] = d->nfreq;
	attach_label( "noise offset:" );
	d->noffset = gtk_spin_button_new_with_range( 0, 100, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->noffset ), 0 );
	gtk_grid_attach( GTK_GRID( grid ), d->noffset, 1, r++, 1, 1 );
	d->ptn_rows[ d->ptn_n++ ] = d->noffset;
	attach_label( "noise volume:" );
	d->nvol = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01 );
	gtk_range_set_value( GTK_RANGE( d->nvol ), 0.8 );
	gtk_widget_set_hexpand( d->nvol, TRUE );
	gtk_grid_attach( GTK_GRID( grid ), d->nvol, 1, r++, 1, 1 );
	d->ptn_rows[ d->ptn_n++ ] = d->nvol;

	// waveform preview
	d->canvas = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func( GTK_DRAWING_AREA( d->canvas ), NULL, NULL, NULL );
	g_signal_connect( d->canvas, "realize", G_CALLBACK( +[]( GtkWidget* w, gpointer ){
		// draw func set separately below via closure over g_snd
	} ), NULL );
	gtk_drawing_area_set_draw_func( GTK_DRAWING_AREA( d->canvas ),
		+[]( GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer ){
			_snd_canvas_cb( NULL, cr, w, h, NULL );
		}, NULL, NULL );
	gtk_widget_set_size_request( d->canvas, -1, 60 );
	gtk_grid_attach( GTK_GRID( grid ), d->canvas, 0, r++, 2, 1 );

	// shared
	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "aud key row:" ), 0, r, 1, 1 );
	d->audkey = gtk_spin_button_new_with_range( _ROW_MIN, _ROW_MAX, 1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->audkey ), 0x5d );
	gtk_grid_attach( GTK_GRID( grid ), d->audkey, 1, r++, 1, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "aud length (%):" ), 0, r, 1, 1 );
	d->aurlen = gtk_spin_button_new_with_range( 10, 600, 10 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( d->aurlen ), 100 );
	gtk_grid_attach( GTK_GRID( grid ), d->aurlen, 1, r++, 1, 1 );

	// live update: redraw + audition on any change
	auto live = +[](){ gtk_widget_queue_draw( g_snd.canvas ); _snd_live_audition(); };
	for( GtkWidget* w : { d->type, d->wave, d->ntype } )
		g_signal_connect( w, "notify::selected",
			G_CALLBACK( +[]( GObject*, GParamSpec*, gpointer fn ){ ((void(*)())fn)(); } ), (gpointer)live );
	for( GtkWidget* w : { d->volume, d->basic_row, d->nfreq, d->noffset, d->nvol, d->audkey, d->aurlen } )
		g_signal_connect( w, "value-changed", G_CALLBACK( +[]( GtkWidget*, gpointer fn ){ ((void(*)())fn)(); } ),
			(gpointer)live );

	// initial visibility
	gtk_drop_down_set_selected( GTK_DROP_DOWN( d->type ), 0 );

	// buttons
	GtkWidget* btn_aud = gtk_button_new_with_label( "audition" );
	g_signal_connect( btn_aud, "clicked", G_CALLBACK( +[]( GtkButton*, gpointer ){
		_snd_live_audition();
	} ), NULL );
	gtk_grid_attach( GTK_GRID( grid ), btn_aud, 0, r, 1, 1 );

	GtkWidget* btn_create = gtk_button_new_with_label( "create & assign" );
	g_signal_connect( btn_create, "clicked", G_CALLBACK( +[]( GtkButton*, gpointer ){
		_create_sound(
			(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.type ) ),
			(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.wave ) ),
			(int)gtk_range_get_value( GTK_RANGE( g_snd.volume ) ),
			(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.basic_row ) ),
			(int)gtk_drop_down_get_selected( GTK_DROP_DOWN( g_snd.ntype ) ),
			gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.nfreq ) ),
			gtk_spin_button_get_value( GTK_SPIN_BUTTON( g_snd.noffset ) ),
			gtk_range_get_value( GTK_RANGE( g_snd.nvol ) ) );
	} ), NULL );
	gtk_grid_attach( GTK_GRID( grid ), btn_create, 1, r, 1, 1 );
}


// ---- units page -------------------------------------------------------------

static void _on_rename_ok( GtkButton*, gpointer user_data );
static void _build_units_page( GtkWidget* parent )
{
	GtkWidget* vbox = gtk_box_new( GTK_ORIENTATION_VERTICAL, 6 );
	gtk_widget_set_margin_start( vbox, 10 ); gtk_widget_set_margin_end( vbox, 10 );
	gtk_widget_set_margin_top( vbox, 10 ); gtk_widget_set_margin_bottom( vbox, 10 );

	GtkWidget* sw = gtk_scrolled_window_new();
	gtk_widget_set_vexpand( sw, TRUE );
	gtk_scrolled_window_set_policy( GTK_SCROLLED_WINDOW( sw ), GtkPolicyType::GTK_POLICY_NEVER, GtkPolicyType::GTK_POLICY_AUTOMATIC );
	gtk_box_append( GTK_BOX( vbox ), sw );

	g_ed.units_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode( GTK_LIST_BOX( g_ed.units_list ), GTK_SELECTION_SINGLE );
	g_signal_connect( g_ed.units_list, "row-selected", G_CALLBACK( +[]( GtkListBox*, GtkListBoxRow* row, gpointer ){
		if( !row ) return;
		int i = gtk_list_box_row_get_index( row );
		if( i >= 0 && i < g_ed.unit_num )
			gtk_drop_down_set_selected( GTK_DROP_DOWN( g_ed.unit_combo ), i );
	} ), NULL );
	gtk_scrolled_window_set_child( GTK_SCROLLED_WINDOW( sw ), g_ed.units_list );

	GtkWidget* hbox = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	GtkWidget* btn_add = gtk_button_new_with_label( "+ add unit" );
	g_signal_connect_swapped( btn_add, "clicked", G_CALLBACK( +[]( gpointer ){ _add_unit(); } ), NULL );
	gtk_box_append( GTK_BOX( hbox ), btn_add );
	gtk_box_append( GTK_BOX( vbox ), hbox );

	g_ed.units_stats = gtk_label_new( "" );
	gtk_widget_set_halign( g_ed.units_stats, GTK_ALIGN_START );
	gtk_box_append( GTK_BOX( vbox ), g_ed.units_stats );

	GtkWidget* rbox = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	gtk_box_append( GTK_BOX( rbox ), gtk_label_new( "rename:" ) );
	g_ed.rename_entry = gtk_entry_new();
	gtk_entry_set_max_length( GTK_ENTRY( g_ed.rename_entry ), pxtnMAX_TUNEUNITNAME );
	gtk_widget_set_hexpand( g_ed.rename_entry, TRUE );
	gtk_box_append( GTK_BOX( rbox ), g_ed.rename_entry );
	GtkWidget* rbtn = gtk_button_new_with_label( "rename" );
	g_signal_connect( rbtn, "clicked", G_CALLBACK( _on_rename_ok ), g_ed.rename_entry );
	gtk_box_append( GTK_BOX( rbox ), rbtn );
	gtk_box_append( GTK_BOX( vbox ), rbox );

	gtk_box_append( GTK_BOX( parent ), vbox );
	_units_refresh();
}

// ---- event page ---------------------------------------------------------------

typedef struct { GtkWidget *kindcombo, *value; } EventDlg;
static EventDlg g_event_dlg;


static void _on_event_kind_changed( GtkDropDown* dd, gpointer user_data )
{
	GtkWidget* value = GTK_WIDGET( user_data );
	int t = (int)gtk_drop_down_get_selected( dd );
	if( t < 0 || t >= _event_kind_num ) return;
	gtk_range_set_range( GTK_RANGE( value ), _event_kinds[ t ].min, _event_kinds[ t ].max );
	gtk_range_set_value( GTK_RANGE( value ), _event_kinds[ t ].def );
}

static void _on_event_set_clicked( GtkButton*, gpointer user_data )
{
	EventDlg* d = (EventDlg*)user_data;
	int t = (int)gtk_drop_down_get_selected( GTK_DROP_DOWN( d->kindcombo ) );
	if( t < 0 || t >= _event_kind_num ) return;
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	int32_t clock = g_ed.has_sel ? g_ed.sel_clock : (int32_t)( g_ed.h_offset / g_ed.px_per_clock );
	_set_event_f( _event_kinds[ t ].kind, clock, unit,
		gtk_range_get_value( GTK_RANGE( d->value ) ) );
}

static void _build_event_page( GtkWidget* parent )
{
	EventDlg* d = &g_event_dlg;

	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing( GTK_GRID( grid ), 6 );
	gtk_grid_set_column_spacing( GTK_GRID( grid ), 8 );
	gtk_widget_set_margin_start( grid, 10 ); gtk_widget_set_margin_end( grid, 10 );
	gtk_widget_set_margin_top( grid, 10 ); gtk_widget_set_margin_bottom( grid, 10 );
	gtk_box_append( GTK_BOX( parent ), grid );

	const char* names[ _event_kind_num + 1 ];
	for( int i = 0; i < _event_kind_num; i++ ) names[ i ] = _event_kinds[ i ].name;
	names[ _event_kind_num ] = NULL;

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "kind:" ), 0, 0, 1, 1 );
	d->kindcombo = gtk_drop_down_new_from_strings( names );
	gtk_grid_attach( GTK_GRID( grid ), d->kindcombo, 1, 0, 1, 1 );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "value:" ), 0, 1, 1, 1 );
	d->value = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0, 129, 1 );
	gtk_range_set_value( GTK_RANGE( d->value ), EVENTDEFAULT_VELOCITY );
	gtk_widget_set_hexpand( d->value, TRUE );
	gtk_grid_attach( GTK_GRID( grid ), d->value, 1, 1, 1, 1 );
	g_signal_connect( d->kindcombo, "notify::selected", G_CALLBACK( _on_event_kind_changed ), d->value );

	gtk_grid_attach( GTK_GRID( grid ), gtk_label_new( "at: selected note / view start" ), 0, 2, 2, 1 );

	GtkWidget* btn = gtk_button_new_with_label( "set event" );
	g_signal_connect( btn, "clicked", G_CALLBACK( _on_event_set_clicked ), d );
	gtk_grid_attach( GTK_GRID( grid ), btn, 0, 3, 2, 1 );
}

// ---- song page ----------------------------------------------------------------

typedef struct { GtkWidget *tempo,*beat_num,*beat_clock,*meas,*repeat,*last; } SongDlg;
static SongDlg g_song_dlg;

static void _on_song_apply( GtkButton*, gpointer user_data )
{
	SongDlg* d = (SongDlg*)user_data;
	int rm = (int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->repeat ) );
	int lm = (int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->last ) );
	_apply_song(
		gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->tempo ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->beat_num ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->beat_clock ) ),
		(int)gtk_spin_button_get_value( GTK_SPIN_BUTTON( d->meas ) ),
		rm - 1, lm - 1 );
}

static void _build_song_page( GtkWidget* parent )
{
	SongDlg* d = &g_song_dlg;
	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing( GTK_GRID( grid ), 6 );
	gtk_grid_set_column_spacing( GTK_GRID( grid ), 8 );
	gtk_widget_set_margin_start( grid, 10 ); gtk_widget_set_margin_end( grid, 10 );
	gtk_widget_set_margin_top( grid, 10 ); gtk_widget_set_margin_bottom( grid, 10 );
	gtk_box_append( GTK_BOX( parent ), grid );
	int r = 0;
	pxtnMaster* m = g_ed.pxtn->master;

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
}

// ---- activate / main -----------------------------------------------------------

static GtkApplication* g_app = NULL;

static void _activate( GtkApplication*, gpointer )
{
	g_ed.window = gtk_application_window_new( g_app );
	gtk_window_set_title( GTK_WINDOW( g_ed.window ), "pxtone-editor" );
	gtk_window_set_default_size( GTK_WINDOW( g_ed.window ), 1280, 760 );

	GtkWidget* root = gtk_paned_new( GTK_ORIENTATION_HORIZONTAL );
	gtk_window_set_child( GTK_WINDOW( g_ed.window ), root );

	GtkWidget* vbox = gtk_box_new( GTK_ORIENTATION_VERTICAL, 0 );
	gtk_widget_set_hexpand( vbox, TRUE );
	gtk_paned_set_start_child( GTK_PANED( root ), vbox );

	// header
	GtkWidget* header = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	gtk_widget_set_margin_start( header, 8 ); gtk_widget_set_margin_end( header, 8 );
	gtk_widget_set_margin_top( header, 6 ); gtk_widget_set_margin_bottom( header, 6 );
	GtkWidget* header_sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy( GTK_SCROLLED_WINDOW( header_sw ), GtkPolicyType::GTK_POLICY_AUTOMATIC, GtkPolicyType::GTK_POLICY_NEVER );
	gtk_scrolled_window_set_child( GTK_SCROLLED_WINDOW( header_sw ), header );
	gtk_box_append( GTK_BOX( vbox ), header_sw );

	gtk_box_append( GTK_BOX( header ), gtk_label_new( "unit:" ) );
	GtkStringList* unit_list = gtk_string_list_new( NULL );
	for( int i = 0; i < g_ed.unit_num; i++ )
	{
		int32_t size = 0;
		const char* name = g_ed.pxtn->Unit_Get( i )->get_name_buf( &size );
		gtk_string_list_append( unit_list, name && name[0] ? name : "(no name)" );
	}
	g_ed.unit_combo = gtk_drop_down_new( G_LIST_MODEL( unit_list ), NULL );
	gtk_widget_set_size_request( g_ed.unit_combo, 150, -1 );
	gtk_box_append( GTK_BOX( header ), g_ed.unit_combo );

	GtkWidget* btn_new   = gtk_button_new_with_label( "new file" );
	GtkWidget* btn_open  = gtk_button_new_with_label( "open..." );
	GtkWidget* btn_saveas= gtk_button_new_with_label( "save as..." );
	GtkWidget* btn_undo  = gtk_button_new_with_label( "undo" );
	GtkWidget* btn_redo  = gtk_button_new_with_label( "redo" );
	GtkWidget* btn_play  = gtk_toggle_button_new_with_label( "PLAY" );

	g_signal_connect_swapped( btn_new,   "clicked", G_CALLBACK( +[]( gpointer ){ _new_tune(); } ), NULL );
	g_signal_connect_swapped( btn_open,  "clicked", G_CALLBACK( +[]( gpointer ){ _open_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_saveas,"clicked", G_CALLBACK( +[]( gpointer ){ _save_as_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_undo,  "clicked", G_CALLBACK( +[]( gpointer ){ _undo(); } ), NULL );
	g_signal_connect_swapped( btn_redo,  "clicked", G_CALLBACK( +[]( gpointer ){ _redo(); } ), NULL );
	for( GtkWidget* b : { btn_new, btn_open, btn_saveas, btn_undo, btn_redo } )
		gtk_box_append( GTK_BOX( header ), b );

	g_ed.tb_play = GTK_TOGGLE_BUTTON( btn_play );
	gtk_box_append( GTK_BOX( header ), btn_play );
	g_signal_connect( btn_play, "toggled", G_CALLBACK( +[]( GtkToggleButton* b, gpointer ){
		bool active = gtk_toggle_button_get_active( b );
		if( active && !g_ed.playing ) _start_play();
		else if( !active && g_ed.playing ) _stop_play();
	} ), NULL );

	// piano roll
	g_ed.draw_area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func( GTK_DRAWING_AREA( g_ed.draw_area ), _draw_cb, NULL, NULL );

	g_ed.hadj = GTK_ADJUSTMENT( gtk_adjustment_new( 0, 0, 100000, 40, 200, 200 ) );
	g_ed.vadj = GTK_ADJUSTMENT( gtk_adjustment_new( 0, 0, 1600, 40, 200, 200 ) );
	g_signal_connect( g_ed.hadj, "value-changed", G_CALLBACK( _on_hscroll ), NULL );
	g_signal_connect( g_ed.vadj, "value-changed", G_CALLBACK( _on_vscroll ), NULL );

	GtkWidget* center = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 0 );
	gtk_widget_set_hexpand( g_ed.draw_area, TRUE );
	gtk_widget_set_vexpand( g_ed.draw_area, TRUE );
	gtk_box_append( GTK_BOX( center ), g_ed.draw_area );
	gtk_box_append( GTK_BOX( center ), gtk_scrollbar_new( GTK_ORIENTATION_VERTICAL, g_ed.vadj ) );
	gtk_widget_set_vexpand( center, TRUE );
	gtk_box_append( GTK_BOX( vbox ), center );
	gtk_box_append( GTK_BOX( vbox ), gtk_scrollbar_new( GTK_ORIENTATION_HORIZONTAL, g_ed.hadj ) );

	// input controllers
	GtkGesture* rdrag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button( GTK_GESTURE_SINGLE( rdrag ), GDK_BUTTON_SECONDARY );
	g_signal_connect( rdrag, "drag-begin",  G_CALLBACK( _on_rdrag_begin ),  NULL );
	g_signal_connect( rdrag, "drag-update", G_CALLBACK( _on_rdrag_update ), NULL );
	gtk_widget_add_controller( g_ed.draw_area, GTK_EVENT_CONTROLLER( rdrag ) );

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

	// right panel notebook
	g_ed.notebook = gtk_notebook_new();
	gtk_widget_set_size_request( g_ed.notebook, 260, -1 );
	const char* tabs[] = { "sound", "units", "event", "song" };
	GtkWidget* page_box[4];
	for( int i = 0; i < 4; i++ )
	{
		page_box[ i ] = gtk_box_new( GTK_ORIENTATION_VERTICAL, 0 );
		gtk_notebook_append_page( GTK_NOTEBOOK( g_ed.notebook ), page_box[ i ], gtk_label_new( tabs[ i ] ) );
	}
	_build_sound_page( page_box[0] );
	_build_units_page( page_box[1] );
	_build_event_page( page_box[2] );
	_build_song_page ( page_box[3] );
	gtk_paned_set_end_child( GTK_PANED( root ), g_ed.notebook );
	gtk_paned_set_resize_end_child( GTK_PANED( root ), FALSE );
	gtk_paned_set_shrink_end_child( GTK_PANED( root ), FALSE );
	gtk_paned_set_position( GTK_PANED( root ), 940 ); // default split; draggable

	g_tick_id = g_timeout_add( 33, _tick, NULL );
	g_signal_connect( g_ed.window, "destroy", G_CALLBACK( _on_window_destroy ), NULL );

	gtk_window_present( GTK_WINDOW( g_ed.window ) );
}

int main( int argc, char** argv )
{
	if( argc >= 2 ) g_ed.path = argv[1];

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
	want.freq = _SAMPLE_PER_SECOND; want.format = AUDIO_S16SYS;
	want.channels = _CHANNEL_NUM; want.samples = 2048;
	want.callback = _sdl_audio_callback;
	if( SDL_OpenAudio( &want, NULL ) != 0 )
	{
		fprintf( stderr, "ERROR: SDL_OpenAudio: %s\n", SDL_GetError() );
		return 1;
	}
	SDL_PauseAudio( 0 );
	g_ed.pv_buf.assign( _SAMPLE_PER_SECOND * _CHANNEL_NUM, 0 );

	g_app = gtk_application_new( "com.github.pxtone.editor", G_APPLICATION_NON_UNIQUE );
	g_signal_connect( g_app, "activate", G_CALLBACK( _activate ), NULL );
	int ret = g_application_run( G_APPLICATION( g_app ), 0, NULL );
	g_object_unref( g_app );

	_stop_play();
	SDL_CloseAudio();
	SDL_Quit();
	return ret;
}
