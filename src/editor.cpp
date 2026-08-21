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
	int     snap         = 240; // clocks (8th note)

	// edit drag
	bool    dragging   = false;
	int32_t drag_clock = 0;
	int32_t drag_unit  = 0;

	// note resize (drag left/right on an existing note)
	bool    resizing   = false;
	int32_t resize_clock = 0;
	int32_t resize_unit  = 0;

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
	int row = _ROW_MAX - (int)floor( y / (double)_ROW_H );
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
	_ensure_evels_capacity();

	// set key only if it differs from the effective key at that point
	if( g_ed.pxtn->evels->get_Value( c, (uint8_t)unit, EVENTKIND_KEY ) != ( row << 8 ) )
	{
		g_ed.pxtn->evels->Record_Delete( c, c, (uint8_t)unit, EVENTKIND_KEY );
		g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, EVENTKIND_KEY, row << 8 );
	}
	g_ed.pxtn->evels->Record_Add_i( c, (uint8_t)unit, EVENTKIND_ON, g_ed.snap );
	SDL_UnlockAudio();

	_preview_note( unit, row );

	g_ed.dragging   = true;
	g_ed.drag_clock = c;
	g_ed.drag_unit  = unit;

	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _drag_update( int x )
{
	if( g_ed.resizing )
	{
		// resize: dragging left/right on an existing note changes its length
		int32_t c = _snap_clock( (int32_t)( ( g_ed.h_offset + x ) / g_ed.px_per_clock ) );
		int32_t dur = c - g_ed.resize_clock;
		int32_t max = g_ed.pxtn->evels->get_Max_Clock() + g_ed.snap * 64;
		if( dur < g_ed.snap ) dur = g_ed.snap;
		if( dur > max ) dur = max;

		SDL_LockAudio();
		// Record_Value_Set matches clock1 <= c < clock2, so pass a +1 range
		g_ed.pxtn->evels->Record_Value_Set( g_ed.resize_clock, g_ed.resize_clock + 1, (uint8_t)g_ed.resize_unit, EVENTKIND_ON, dur );
		SDL_UnlockAudio();

		gtk_widget_queue_draw( g_ed.draw_area );
		return;
	}

	if( !g_ed.dragging ) return;
	int32_t c = _snap_clock( (int32_t)( ( g_ed.h_offset + x ) / g_ed.px_per_clock ) );
	int32_t dur = c - g_ed.drag_clock;
	int32_t max = g_ed.pxtn->evels->get_Max_Clock() + g_ed.snap * 64;
	if( dur < g_ed.snap ) dur = g_ed.snap;
	if( dur > max ) dur = max;

	SDL_LockAudio();
	g_ed.pxtn->evels->Record_Value_Set( g_ed.drag_clock, g_ed.drag_clock, (uint8_t)g_ed.drag_unit, EVENTKIND_ON, dur );
	SDL_UnlockAudio();

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
	g_ed.pxtn->evels->Record_Delete( hit->clock, hit->clock, (uint8_t)unit, EVENTKIND_ON );
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
	SDL_QueueAudio( g_ed.preview_dev, out.data(), out.size() * sizeof( int16_t ) );
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

// ---- drawing ------------------------------------------------------------

static void _draw_cb( GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer )
{
	// background
	cairo_set_source_rgb( cr, 0.06, 0.06, 0.10 );
	cairo_paint( cr );

	const int rows = _ROW_MAX - _ROW_MIN + 1;
	const double content_h = rows * _ROW_H;

	// key rows (black stripes) + octave lines
	for( int r = _ROW_MIN; r <= _ROW_MAX; r++ )
	{
		static const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
		double y = ( _ROW_MAX - r ) * _ROW_H;
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

	// beat / snap grid
	{
		int32_t first_clock = (int32_t)( g_ed.h_offset / g_ed.px_per_clock );
		int32_t last_clock  = (int32_t)( ( g_ed.h_offset + w ) / g_ed.px_per_clock ) + g_ed.snap;

		for( int32_t c = _snap_clock( first_clock ); c <= last_clock; c += g_ed.snap )
		{
			bool is_beat = ( c % _BEAT_CLOCK == 0 );
			double x = c * g_ed.px_per_clock - g_ed.h_offset;
			cairo_set_source_rgb( cr, is_beat ? 0.30 : 0.16, is_beat ? 0.30 : 0.16, is_beat ? 0.42 : 0.22 );
			cairo_move_to( cr, x, 0 );
			cairo_line_to( cr, x, content_h );
			cairo_stroke( cr );
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
		double y = ( _ROW_MAX - row ) * _ROW_H;

		double r, g, b;
		_unit_color( p->unit_no, &r, &g, &b );
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

static void _on_click( GtkGestureClick* gesture, int n_press, double x, double y, gpointer )
{
	guint btn = gtk_gesture_single_get_current_button( GTK_GESTURE_SINGLE( gesture ) );

	int32_t clock; int row;
	if( !_screen_to_clock_row( (int)x, (int)y, &clock, &row ) ) return;

	if( btn == GDK_BUTTON_PRIMARY )
	{
		int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
		if( unit >= 0 && unit < g_ed.unit_num )
		{
			// existing note -> resize mode; empty cell -> create new note
			const EVERECORD* hit = _find_note( clock, row, unit );
			if( hit )
			{
				g_ed.resizing     = true;
				g_ed.resize_clock = hit->clock;
				g_ed.resize_unit  = unit;
			}
			else
			{
				_add_note( clock, row );
			}
		}
	}
	else if( btn == GDK_BUTTON_SECONDARY ) _delete_note( clock, row );
}

static void _on_release( GtkGestureClick*, int, double, double, gpointer )
{
	g_ed.dragging = false;
	g_ed.resizing = false;
}

static void _on_motion( GtkEventControllerMotion*, double x, double, gpointer )
{
	if( g_ed.dragging ) _drag_update( (int)x );
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
		g_ed.h_offset += ( dx - dy ) * 40;
	}
	if( g_ed.h_offset < 0 ) g_ed.h_offset = 0;
	gtk_widget_queue_draw( g_ed.draw_area );
	return TRUE;
}

static gboolean _on_key( GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer )
{
	if( ( state & GDK_CONTROL_MASK ) && keyval == 's' ){ _save(); return TRUE; }
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
	GtkWidget* btn_play  = gtk_button_new_with_label( "▶ play" );
	GtkWidget* btn_stop  = gtk_button_new_with_label( "■ stop" );
	g_signal_connect_swapped( btn_unit,  "clicked", G_CALLBACK( +[]( gpointer ){ _add_unit(); } ), NULL );
	g_signal_connect_swapped( btn_sound, "clicked", G_CALLBACK( +[]( gpointer ){ _sound_dialog(); } ), NULL );
	g_signal_connect_swapped( btn_play,  "clicked", G_CALLBACK( +[]( gpointer ){ _start_play(); } ), NULL );
	g_signal_connect_swapped( btn_stop,  "clicked", G_CALLBACK( +[]( gpointer ){ _stop_play(); } ), NULL );
	gtk_box_append( GTK_BOX( hbox ), btn_unit );
	gtk_box_append( GTK_BOX( hbox ), btn_sound );
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

	// input controllers
	GtkGesture* click = gtk_gesture_click_new();
	gtk_gesture_single_set_button( GTK_GESTURE_SINGLE( click ), 0 ); // all buttons
	g_signal_connect( click, "pressed",  G_CALLBACK( _on_click ),   NULL );
	g_signal_connect( click, "released", G_CALLBACK( _on_release ), NULL );
	gtk_widget_add_controller( g_ed.draw_area, GTK_EVENT_CONTROLLER( click ) );

	GtkEventController* motion = gtk_event_controller_motion_new();
	g_signal_connect( motion, "motion", G_CALLBACK( _on_motion ), NULL );
	gtk_widget_add_controller( g_ed.draw_area, motion );

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
