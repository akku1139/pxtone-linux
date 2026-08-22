// Smoke test: ALL .ptcop-representable event kinds survive a save -> reload
// roundtrip (task-5 contract).
//
// Note: in the v5 format, tempo/beat params live in the master block
// (BEATTEMPO/BEATNUM/BEATCLOCK events are not used by v5 writers), so they
// are covered via master fields instead of events.

#include <cmath>
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

	int32_t C = pxtn.evels->get_Max_Clock() + 4800;
	pxtn.evels->Allocate( pxtn.evels->get_Count() + 8192 );

	// --- place every representable event kind on unit 0 ---
	struct { uint8_t kind; int32_t value; const char* name; } evtests[] =
	{
		{ EVENTKIND_ON,        240   , "on"         },
		{ EVENTKIND_KEY,       0x6321, "key"        },
		{ EVENTKIND_VELOCITY,  77    , "velocity"   },
		{ EVENTKIND_VOLUME,    55    , "volume"     },
		{ EVENTKIND_PAN_VOLUME,110   , "pan_volume" },
		{ EVENTKIND_PAN_TIME,  20    , "pan_time"   },
		{ EVENTKIND_PORTAMENT, 64    , "portament"  },
		{ EVENTKIND_VOICENO,   1     , "voice_no"   },
		{ EVENTKIND_GROUPNO,   4     , "group_no"   },
	};
	for( auto& t : evtests )
		if( !pxtn.evels->Record_Add_i( C, 0, t.kind, t.value ) ){ printf("add %s fail\n", t.name); return 3; }

	const float tuning = 0.75f;
	if( !pxtn.evels->Record_Add_f( C, 0, EVENTKIND_TUNING, tuning ) ) return 4;

	// repeat / last markers
	if( pxtn.evels->get_Count( EVENTKIND_REPEAT, -1 ) == 0 )
		pxtn.evels->Record_Add_i( 960, 0, EVENTKIND_REPEAT, 19200 ); // value = span clocks
	if( pxtn.evels->get_Count( EVENTKIND_LAST, -1 ) == 0 )
		pxtn.evels->Record_Add_i( 3840, 0, EVENTKIND_LAST, 0 );

	// --- master params ---
	pxtn.master->Set( 7, 174.5f, 480 );
	pxtn.master->set_meas_num   ( 30 );
	pxtn.master->set_repeat_meas( 2  );
	pxtn.master->set_last_meas  ( 29 );

	fp = fopen( "/tmp/roundtrip_out.ptcop", "wb" );
	if( !fp ) return 5;
	if( pxtn.write( fp, false, 0x0500 ) != pxtnOK ){ printf("write fail\n"); return 6; }
	fclose( fp );

	pxtnService q( _r, _w, _s, _p );
	q.init();
	fp = fopen( "/tmp/roundtrip_out.ptcop", "rb" );
	if( !fp || q.read( fp ) != pxtnOK ){ printf("reload fail: %s\n", "see above"); return 7; }
	fclose( fp );

	int fail = 0;

	for( auto& t : evtests )
	{
		int32_t v = q.evels->get_Value( C, 0, t.kind );
		bool ok = ( v == t.value );
		printf( "%-10s expect=%d got=%d %s\n", t.name, t.value, v, ok ? "OK" : "FAIL" );
		if( !ok ) fail++;
	}
	{
		int32_t iv = q.evels->get_Value( C, 0, EVENTKIND_TUNING );
		float tun = *(float*)( &iv );
		bool ok = fabs( tun - tuning ) < 0.001f;
		printf( "%-10s expect=%.3f got=%.3f %s\n", "tuning", tuning, tun, ok ? "OK" : "FAIL" );
		if( !ok ) fail++;
	}

	struct { const char* name; float want; float got; } mt[] =
	{
		{ "beat_num",    7,      (float)q.master->get_beat_num()    },
		{ "tempo",       174.5f, q.master->get_beat_tempo()          },
		{ "repeat_meas", 2,      (float)q.master->get_repeat_meas() },
		{ "last_meas",   29,     (float)q.master->get_last_meas()   },
	};
	for( auto& c : mt )
	{
		bool ok = ( c.want == c.got );
		printf( "%-10s want=%.1f got=%.1f %s\n", c.name, c.want, c.got, ok ? "OK" : "FAIL" );
		if( !ok ) fail++;
	}

	// unit count preserved
	if( q.Unit_Num() != pxtn.Unit_Num() ){ printf("unit_num mismatch\n"); fail++; }

	printf( "%s (%d failures)\n", fail ? "ROUNDTRIP SMOKE TEST FAILED" : "ROUNDTRIP SMOKE TEST OK", fail );
	return fail ? 1 : 0;
}
