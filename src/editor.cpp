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

	// playback
	std::atomic<int64_t> played_samples {0};
	std::atomic<bool>    playing        {false};

	// widgets
	GtkWidget* window    = NULL;
	GtkWidget* draw_area = NULL;
	GtkWidget* unit_combo = NULL;
	GtkWidget* snap_combo = NULL;
	GtkWidget* status     = NULL;
};

static Editor g_ed;

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

	g_ed.dragging   = true;
	g_ed.drag_clock = c;
	g_ed.drag_unit  = unit;

	gtk_widget_queue_draw( g_ed.draw_area );
}

static void _drag_update( int x )
{
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

static void _delete_note( int32_t clock, int row )
{
	int unit = gtk_drop_down_get_selected( GTK_DROP_DOWN( g_ed.unit_combo ) );
	if( unit < 0 || unit >= g_ed.unit_num ) return;

	// find the ON event under the cursor
	const EVERECORD* hit = NULL;
	for( const EVERECORD* p = g_ed.pxtn->evels->get_Records(); p; p = p->next )
	{
		if( p->kind != EVENTKIND_ON || p->unit_no != unit ) continue;
		if( p->clock > clock ) break; // records are sorted by clock
		int32_t dur = p->value > 0 ? p->value : g_ed.snap;
		if( clock >= p->clock && clock < p->clock + dur )
		{
			// check key row matches
			int k = g_ed.pxtn->evels->get_Value( p->clock, (uint8_t)unit, EVENTKIND_KEY ) >> 8;
			if( k == row ){ hit = p; }
		}
	}
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

	if( btn == GDK_BUTTON_PRIMARY )      _add_note( clock, row );
	else if( btn == GDK_BUTTON_SECONDARY ) _delete_note( clock, row );
}

static void _on_release( GtkGestureClick*, int, double, double, gpointer )
{
	g_ed.dragging = false;
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
	const char** unit_names = (const char**)malloc( sizeof( char* ) * ( g_ed.unit_num + 1 ) );
	for( int i = 0; i < g_ed.unit_num; i++ )
	{
		int32_t size = 0;
		const char* name = g_ed.pxtn->Unit_Get( i )->get_name_buf( &size );
		char* copy = strdup( name && name[0] ? name : "(no name)" );
		unit_names[ i ] = copy;
	}
	unit_names[ g_ed.unit_num ] = NULL;
	GtkStringList* unit_list = gtk_string_list_new( unit_names );
	g_ed.unit_combo = gtk_drop_down_new( G_LIST_MODEL( unit_list ), NULL );
	gtk_box_append( GTK_BOX( hbox ), g_ed.unit_combo );

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

	GtkApplication* app = gtk_application_new( "com.github.pxtone.editor", G_APPLICATION_NON_UNIQUE );
	g_signal_connect( app, "activate", G_CALLBACK( _activate ), NULL );
	int ret = g_application_run( G_APPLICATION( app ), 0, NULL );
	g_object_unref( app );

	_stop_play();
	SDL_CloseAudio();
	SDL_Quit();
	return ret;
}
