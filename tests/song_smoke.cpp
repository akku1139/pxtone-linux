// Smoke test: master params (tempo / beat num / beat clock / measures /
// repeat / last) survive a save -> reload roundtrip (task-3 contract).

#include <cstdio>

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

	pxtn.master->Set( 5, 165.0f, 480 );
	pxtn.master->set_meas_num   ( 42 );
	pxtn.master->set_repeat_meas( 3  );
	pxtn.master->set_last_meas  ( 41 ); // meas_num is derived from content on load

	fp = fopen( "/tmp/song_smoke_out.ptcop", "wb" );
	if( !fp ) return 3;
	if( pxtn.write( fp, false, 0x0500 ) != pxtnOK ){ printf("write fail\n"); return 4; }
	fclose( fp );

	pxtnService q( _r, _w, _s, _p );
	q.init();
	fp = fopen( "/tmp/song_smoke_out.ptcop", "rb" );
	if( !fp || q.read( fp ) != pxtnOK ){ printf("reload fail\n"); return 5; }
	fclose( fp );

	struct { const char* name; float want; float got; } checks[] =
	{
		{ "beat_num",    5,     (float)q.master->get_beat_num()    },
		{ "tempo",       165.0f,        q.master->get_beat_tempo() },
		{ "meas_num",    41,    (float)q.master->get_meas_num()    }, // == last_meas (derived)
		{ "repeat_meas", 3,     (float)q.master->get_repeat_meas() },
		{ "last_meas",   41,    (float)q.master->get_last_meas()   },
	};

	int fail = 0;
	for( auto& c : checks )
	{
		bool ok = ( c.want == c.got );
		printf( "%-12s want=%.1f got=%.1f %s\n", c.name, c.want, c.got, ok ? "OK" : "FAIL" );
		if( !ok ) fail++;
	}
	printf( "%s\n", fail ? "SONG SMOKE TEST FAILED" : "SONG SMOKE TEST OK" );
	return fail ? 1 : 0;
}
