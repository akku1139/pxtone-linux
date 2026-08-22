// Smoke test: unit rename + VOICENO / GROUPNO / TUNING / PORTAMENT events
// survive a save -> reload roundtrip (task-4 contract).

#include <cmath>
#include <cstdio>
#include <cstring>

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

	// unit rename
	const char* newname = "renamed unit";
	if( !pxtn.Unit_Get_variable( 0 )->set_name_buf( newname, strlen( newname ) ) ) return 3;

	const int32_t C = pxtn.evels->get_Max_Clock() + 4800;
	struct { uint8_t kind; int32_t value; } itests[] =
	{
		{ EVENTKIND_PORTAMENT, 60 },
		{ EVENTKIND_VOICENO,   2  },
		{ EVENTKIND_GROUPNO,   3  },
	};
	for( auto& t : itests )
		if( !pxtn.evels->Record_Add_i( C, 0, t.kind, t.value ) ) return 4;

	const float tuning = 1.5f;
	if( !pxtn.evels->Record_Add_f( C, 0, EVENTKIND_TUNING, tuning ) ) return 5;

	fp = fopen( "/tmp/unit_smoke_out.ptcop", "wb" );
	if( !fp ) return 6;
	if( pxtn.write( fp, false, 0x0500 ) != pxtnOK ){ printf("write fail\n"); return 7; }
	fclose( fp );

	pxtnService q( _r, _w, _s, _p );
	q.init();
	fp = fopen( "/tmp/unit_smoke_out.ptcop", "rb" );
	if( !fp || q.read( fp ) != pxtnOK ){ printf("reload fail\n"); return 8; }
	fclose( fp );

	int fail = 0;

	int32_t size = 0;
	const char* name = q.Unit_Get( 0 )->get_name_buf( &size );
	bool name_ok = ( name && strcmp( name, newname ) == 0 );
	printf( "unit name: '%s' %s\n", name ? name : "(null)", name_ok ? "OK" : "FAIL" );
	if( !name_ok ) fail++;

	for( auto& t : itests )
	{
		int32_t v = q.evels->get_Value( C, 0, t.kind );
		printf( "kind=%d expect=%d got=%d %s\n", t.kind, t.value, v, v == t.value ? "OK" : "FAIL" );
		if( v != t.value ) fail++;
	}
	float tun = 0;
	{
		int32_t iv = q.evels->get_Value( C, 0, EVENTKIND_TUNING );
		tun = *(float*)( &iv );
	}
	bool tun_ok = fabs( tun - tuning ) < 0.001f;
	printf( "tuning expect=%.3f got=%.3f %s\n", tuning, tun, tun_ok ? "OK" : "FAIL" );
	if( !tun_ok ) fail++;

	printf( "%s\n", fail ? "UNIT SMOKE TEST FAILED" : "UNIT SMOKE TEST OK" );
	return fail ? 1 : 0;
}
