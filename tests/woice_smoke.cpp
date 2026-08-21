// Smoke test: create PTV / PTN woices programmatically (like the editor's
// "create sound" feature), assign to a unit, add a note and render via Moo.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../pxtone/pxtnService.h"
#include "../pxtone/pxtnError.h"

static bool _r( void* u, void* p, int s, int n ){ return fread( p, s, n, (FILE*)u ) >= n; }
static bool _w( void* u, const void* p, int s, int n ){ return fwrite( p, s, n, (FILE*)u ) >= n; }
static bool _s( void* u, int m, int s ){ return !fseek( (FILE*)u, s, m ); }
static bool _p( void* u, int32_t* o ){ long i = ftell( (FILE*)u ); if( i < 0 ) return false; *o = i; return true; }



int main()
{
	const double PI = 3.141592653589793;

	pxtnService pxtn( _r, _w, _s, _p );

	if( pxtn.init() != pxtnOK ) return 1;
	if( !pxtn.set_destination_quality( 2, 44100 ) ) return 1;

	// load a real tune (Moo needs a valid loaded project)
	FILE* lfp = fopen( "../pxtone-source-code/sample data/sample.ptcop", "rb" );
	if( !lfp ) return 2;
	if( pxtn.read( lfp ) != pxtnOK ) return 3;
	fclose( lfp );
	if( pxtn.tones_ready() != pxtnOK ) return 4;

	// --- PTV woice ---
	int wi = pxtn.Woice_AddNew();
	if( wi < 0 ) return 3;
	pxtnWoice* w = pxtn.Woice_Get_variable( wi );
	if( !w->Voice_Allocate( 1 ) ) return 4;
	pxtnVOICEUNIT* v = w->get_voice_variable( 0 );
	v->type = pxtnVOICE_Coodinate;
	v->basic_key = 0x6000; v->volume = 100; v->pan = 64; v->tuning = 1.0f;
	v->voice_flags = PTV_VOICEFLAG_SMOOTH; v->data_flags = PTV_DATAFLAG_WAVE;
	v->wave.reso = 10000; v->wave.num = 4;
	v->wave.points = (pxtnPOINT*)malloc( sizeof( pxtnPOINT ) * 4 );
	v->wave.points[0] = {     0, -100 };
	v->wave.points[1] = {  4999,  100 };
	v->wave.points[2] = {  5000, -100 };
	v->wave.points[3] = {  9999, -100 };
	if( pxtn.Woice_ReadyTone( wi ) != pxtnOK ){ printf("ptv ready fail\n"); return 5; }
	printf("PTV woice created (idx %d)\n", wi);

	// --- PTN noise woice ---
	int ni = pxtn.Woice_AddNew();
	if( ni < 0 ) return 6;
	pxtnWoice* nw = pxtn.Woice_Get_variable( ni );
	if( !nw->Voice_Allocate( 1 ) ) return 7;
	pxtnVOICEUNIT* nv = nw->get_voice_variable( 0 );
	if( !nv->p_ptn->Allocate( 1, 0 ) ) return 8;
	pxNOISEDESIGN_UNIT* du = nv->p_ptn->get_unit( 0 );
	du->bEnable = true; du->enve_num = 0; du->pan = 64;
	du->main.type = pxWAVETYPE_Random; du->main.freq = 10; du->main.volume = 0.8f; du->main.offset = 0; du->main.b_rev = false;
	du->freq.type = pxWAVETYPE_None; du->freq.volume = 0;
	du->volu.type = pxWAVETYPE_None; du->volu.volume = 0;
	nv->p_ptn->set_smp_num_44k( 44100 / 4 );
	nv->p_ptn->Fix();
	nv->type = pxtnVOICE_Noise; nv->basic_key = 0x6000; nv->volume = 100; nv->pan = 64;
	if( pxtn.Woice_ReadyTone( ni ) != pxtnOK ){ printf("ptn ready fail\n"); return 9; }
	printf("PTN woice created (idx %d)\n", ni);

	// --- assign PTV to unit, add a note ---
	pxtn.Unit_Get_variable( 0 )->set_woice( w );

	int count_before = 0;
	if( !pxtn.evels->Allocate( count_before + 8192 ) ) return 10;
	if( !pxtn.evels->Record_Add_i( 480, 0, EVENTKIND_KEY, 0x6000 ) ) return 11;
	if( !pxtn.evels->Record_Add_i( 480, 0, EVENTKIND_ON, 480 ) ) return 12;

	// --- render ---
	pxtnVOMITPREPARATION prep = {0};
	prep.flags |= pxtnVOMITPREPFLAG_loop;
	prep.start_pos_float = 0;
	prep.master_volume = 1.0f;
	if( !pxtn.moo_preparation( &prep ) ){ printf("moo prep fail\n"); return 13; }

	std::vector<int16_t> buf( 44100 * 2 ); // 1s
	if( !pxtn.Moo( buf.data(), buf.size() * 2 ) ){ printf("Moo fail\n"); return 14; }

	double peak = 0;
	for( auto s : buf ){ double a = fabs( s / 32768.0 ); if( a > peak ) peak = a; }
	printf("rendered 1s, peak=%.4f\n", peak);

	bool ok = peak > 0.01;
	printf("%s\n", ok ? "WOICE SMOKE TEST OK" : "WOICE SMOKE TEST FAILED (silent)");
	return ok ? 0 : 15;
}
