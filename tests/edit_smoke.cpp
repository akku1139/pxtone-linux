// Smoke test for the editor's core edit operations (no GUI).
// 1. Load sample.ptcop
// 2. Add a note (KEY+ON events) like the editor does
// 3. Save as .ptcop
// 4. Reload and verify the note exists

#include <cstdio>
#include <cstring>
#include <vector>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static bool _r( void* u, void* p, int s, int n ){ return fread( p, s, n, (FILE*)u ) >= n; }
static bool _w( void* u, const void* p, int s, int n ){ return fwrite( p, s, n, (FILE*)u ) >= n; }
static bool _s( void* u, int m, int s ){ return !fseek( (FILE*)u, s, m ); }
static bool _p( void* u, int32_t* o ){ long i = ftell( (FILE*)u ); if( i < 0 ) return false; *o = i; return true; }

static int _count_on( pxtnService* pxtn )
{
	int n = 0;
	for( const EVERECORD* e = pxtn->evels->get_Records(); e; e = e->next ) if( e->kind == EVENTKIND_ON ) n++;
	return n;
}

int main( int argc, char** argv )
{
	if( argc < 2 ){ fprintf( stderr, "usage: %s file.ptcop\n", argv[0] ); return 1; }

	pxtnService pxtn( _r, _w, _s, _p );
	if( pxtn.init() != pxtnOK ) return 2;
	if( !pxtn.set_destination_quality( 2, 44100 ) ) return 2;

	FILE* fp = fopen( argv[1], "rb" ); if( !fp ) return 3;
	if( pxtn.read( fp ) != pxtnOK ) return 4;
	fclose( fp );
	if( pxtn.tones_ready() != pxtnOK ) return 5;

	const int unit = 0, row = 0x60;
	int before = _count_on( &pxtn );

	// ensure capacity like the editor
	int count = 0;
	for( const EVERECORD* e = pxtn.evels->get_Records(); e; e = e->next ) count++;
	if( pxtn.evels->get_Num_Max() < count + 4096 )
	{
		std::vector<EVERECORD> recs;
		for( const EVERECORD* e = pxtn.evels->get_Records(); e; e = e->next ) recs.push_back( *e );
		pxtn.evels->Allocate( count + 4096 );
		for( auto& r : recs ) pxtn.evels->Record_Add_i( r.clock, r.unit_no, r.kind, r.value );
	}

	// add note at clock 480*4, key 0x6000, len 240
	pxtn.evels->Record_Add_i( 1920, unit, EVENTKIND_KEY, row << 8 );
	if( !pxtn.evels->Record_Add_i( 1920, unit, EVENTKIND_ON, 240 ) ) { fprintf( stderr, "Record_Add failed\n" ); return 6; }

	// save as .ptcop project format (b_tune=false; tune format quantizes clocks /10)
	fp = fopen( "/tmp/smoke_out.ptcop", "wb" );
	if( !fp ) return 7;
	pxtnERR err = pxtn.write( fp, false, 0x0500 );
	fclose( fp );
	if( err != pxtnOK ){ fprintf( stderr, "write: %s\n", pxtnError_get_string( err ) ); return 8; }

	// reload
	pxtnService pxtn2( _r, _w, _s, _p );
	if( pxtn2.init() != pxtnOK ) return 9;
	fp = fopen( "/tmp/smoke_out.ptcop", "rb" ); if( !fp ) return 10;
	if( pxtn2.read( fp ) != pxtnOK ){ fprintf( stderr, "reload failed\n" ); return 11; }
	fclose( fp );

	int after = _count_on( &pxtn2 );
	int key_at = pxtn2.evels->get_Value( 1920, unit, EVENTKIND_KEY );

	// find the added note (adding an ON truncates overlapping notes, so
	// compare against the note itself, not the total count)
	int added_on_value = -1;
	for( const EVERECORD* e = pxtn2.evels->get_Records(); e; e = e->next )
		if( e->kind == EVENTKIND_ON && e->unit_no == unit && e->clock == 1920 ) added_on_value = e->value;

	printf( "events ON: %d -> %d\n", before, after );
	printf( "added note: on-value=%d key=0x%04x\n", added_on_value, key_at );

	bool ok = ( added_on_value == 240 ) && ( key_at == ( row << 8 ) );
	printf( "%s\n", ok ? "SMOKE TEST OK" : "SMOKE TEST FAILED" );
	return ok ? 0 : 1;
}
