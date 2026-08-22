// Smoke test: VELOCITY / VOLUME / PAN_VOLUME / PAN_TIME events survive
// a save -> reload roundtrip (task-2 contract).

#include <cstdio>
#include <vector>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static bool _r( void* u, void* p, int s, int n ){ return fread( p, s, n, (FILE*)u ) >= n; }
static bool _w( void* u, const void* p, int s, int n ){ return fwrite( p, s, n, (FILE*)u ) >= n; }
static bool _s( void* u, int m, int s ){ return !fseek( (FILE*)u, s, m ); }
static bool _p( void* u, int32_t* o ){ long i = ftell( (FILE*)u ); if( i < 0 ) return false; *o = i; return true; }

int main()
{
	pxtnService pxtn( _r, _w, _s, _p );
	if( pxtn.init() != pxtnOK ) return 1;
	if( !pxtn.set_destination_quality( 2, 44100 ) ) return 1;
	FILE* fp = fopen( "../pxtone-source-code/sample data/sample.ptcop", "rb" );
	if( !fp || pxtn.read( fp ) != pxtnOK ) return 2;
	fclose( fp );
	pxtn.evels->Allocate( pxtn.evels->get_Count() + 8192 );

	const int32_t C = pxtn.evels->get_Max_Clock() + 4800;

	struct { uint8_t kind; int32_t value; } tests[] =
	{
		{ EVENTKIND_VELOCITY,   90  },
		{ EVENTKIND_VOLUME,     40  },
		{ EVENTKIND_PAN_VOLUME, 100 },
		{ EVENTKIND_PAN_TIME,   30  },
	};

	for( auto& t : tests )
		if( !pxtn.evels->Record_Add_i( C, 0, t.kind, t.value ) ) return 3;

	fp = fopen( "/tmp/event_smoke_out.ptcop", "wb" );
	if( !fp ) return 4;
	if( pxtn.write( fp, false, 0x0500 ) != pxtnOK ){ printf("write fail\n"); return 5; }
	fclose( fp );

	pxtnService q( _r, _w, _s, _p );
	q.init();
	fp = fopen( "/tmp/event_smoke_out.ptcop", "rb" );
	if( !fp || q.read( fp ) != pxtnOK ){ printf("reload fail\n"); return 6; }
	fclose( fp );

	int fail = 0;
	for( auto& t : tests )
	{
		int32_t v = q.evels->get_Value( C, 0, t.kind );
		printf( "kind=%d expect=%d got=%d %s\n", t.kind, t.value, v, v == t.value ? "OK" : "FAIL" );
		if( v != t.value ) fail++;
	}
	printf( "%s\n", fail ? "EVENT SMOKE TEST FAILED" : "EVENT SMOKE TEST OK" );
	return fail ? 1 : 0;
}
