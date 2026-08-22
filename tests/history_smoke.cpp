// Smoke test: note move / copy-paste / undo-redo core operations
// (mirrors the editor's snapshot-based history implementation).

#include <cstdio>
#include <cstring>
#include <vector>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static bool _r( void* u, void* p, int s, int n ){ return fread( p, s, n, (FILE*)u ) >= n; }
static bool _w( void* u, const void* p, int s, int n ){ return fwrite( p, s, n, (FILE*)u ) >= n; }
static bool _s( void* u, int m, int s ){ return !fseek( (FILE*)u, s, m ); }
static bool _p( void* u, int32_t* o ){ long i = ftell( (FILE*)u ); if( i < 0 ) return false; *o = i; return true; }

typedef std::vector<EVERECORD> Snap;

static Snap snap_of( pxtnEvelist* e )
{
	Snap v;
	for( const EVERECORD* p = e->get_Records(); p; p = p->next ) v.push_back( *p );
	return v;
}
static void restore( pxtnService* pxtn, const Snap& s )
{
	pxtn->evels->Allocate( s.size() + 4096 );
	for( const EVERECORD& r : s ) pxtn->evels->Record_Add_i( r.clock, r.unit_no, r.kind, r.value );
}
static bool same( const Snap& a, const Snap& b )
{
	if( a.size() != b.size() ) return false;
	for( size_t i = 0; i < a.size(); i++ )
	{
		const EVERECORD &x = a[i], &y = b[i];
		if( x.clock != y.clock || x.unit_no != y.unit_no || x.kind != y.kind || x.value != y.value ) return false;
	}
	return true;
}
static const EVERECORD* find_on( pxtnEvelist* e, int32_t clock, int unit )
{
	for( const EVERECORD* p = e->get_Records(); p; p = p->next )
	{
		if( p->kind == EVENTKIND_ON && p->unit_no == unit && p->clock == clock ) return p;
		if( p->clock > clock ) break;
	}
	return NULL;
}

int main()
{
	int fail = 0;

	pxtnService pxtn( _r, _w, _s, _p );
	if( pxtn.init() != pxtnOK ) return 1;
	if( !pxtn.set_destination_quality( 2, 44100 ) ) return 1;
	FILE* fp = fopen( "../pxtone-source-code/sample data/sample.ptcop", "rb" );
	if( !fp || pxtn.read( fp ) != pxtnOK ) return 2;
	fclose( fp );
	pxtn.evels->Allocate( pxtn.evels->get_Count() + 8192 );

	// --- setup: one isolated note on a fresh area ---
	const int32_t C0 = pxtn.evels->get_Max_Clock() + 4800;
	pxtn.evels->Record_Add_i( C0,     0, EVENTKIND_KEY, 0x6000 );
	pxtn.evels->Record_Add_i( C0,     0, EVENTKIND_ON,  240  );

	// --- undo stack ---
	std::vector<Snap> undo, redo;

	// === MOVE: delete at old pos, add at new pos (editor logic) ===
	undo.push_back( snap_of( pxtn.evels ) ); redo.clear();
	{
		int32_t nc = C0 + 480, dur = 240, key = 0x60;
		pxtn.evels->Record_Delete( C0, C0 + 1, 0, EVENTKIND_ON );
		pxtn.evels->Record_Delete( C0, C0 + 1, 0, EVENTKIND_KEY );
		pxtn.evels->Record_Add_i( nc, 0, EVENTKIND_KEY, key << 8 );
		pxtn.evels->Record_Add_i( nc, 0, EVENTKIND_ON, dur );
	}
	const EVERECORD* moved = find_on( pxtn.evels, C0 + 480, 0 );
	printf( "move: %s\n", moved ? "OK" : "FAIL" );
	if( !moved ) fail++;

	// === UNDO restores pre-move state ===
	redo.push_back( snap_of( pxtn.evels ) );
	restore( &pxtn, undo.back() ); undo.pop_back();
	Snap base = snap_of( pxtn.evels );
	bool has_old = find_on( pxtn.evels, C0, 0 ) != NULL;
	printf( "undo after move: %s\n", has_old ? "OK" : "FAIL" );
	if( !has_old ) fail++;

	// === COPY / PASTE: duplicate the note at +960 ===
	undo.push_back( base ); redo.clear();
	{
		int32_t c = C0 + 960;
		pxtn.evels->Record_Delete( c, c + 1, 0, EVENTKIND_KEY );
		pxtn.evels->Record_Add_i( c, 0, EVENTKIND_KEY, 0x6000 );
		pxtn.evels->Record_Add_i( c, 0, EVENTKIND_ON, 240 );
	}
	bool pasted = find_on( pxtn.evels, C0 + 960, 0 ) != NULL;
	printf( "paste: %s\n", pasted ? "OK" : "FAIL" );
	if( !pasted ) fail++;

	// === UNDO restores again ===
	Snap before_undo = snap_of( pxtn.evels );
	restore( &pxtn, undo.back() ); undo.pop_back();
	redo.push_back( before_undo );
	Snap after = snap_of( pxtn.evels );
	printf( "undo after paste identical to earlier state: %s\n", same( base, after ) ? "OK" : "FAIL" );
	if( !same( base, after ) ) fail++;

	// === REDO re-applies paste ===
	restore( &pxtn, redo.back() ); redo.pop_back();
	printf( "redo: %s\n", find_on( pxtn.evels, C0 + 960, 0 ) ? "OK" : "FAIL" );
	if( !find_on( pxtn.evels, C0 + 960, 0 ) ) fail++;

	printf( "%s\n", fail ? "HISTORY SMOKE TEST FAILED" : "HISTORY SMOKE TEST OK" );
	return fail ? 1 : 0;
}
