/* alphaprobe.c - Step 5b: which DE alpha register combos give (A) global-opacity blit of a premultiplied
 * ARGB source and (B) coloured A8-mask blit (glyph).  Brute-force over the register fields, compare to the
 * exact formula.  Usage: alphaprobe [A|B|AB] */
#include "etna2d.h"
#include "state_2d.xml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define W 64
#define H 4
static struct etna2d_device *d; static struct etna2d_context *c;
static struct etna2d_surface *srcA, *srcM, *dst;
static uint32_t sp[W*H], dp[W*H]; static uint8_t mp[W*H];
static uint32_t rnd(void){static uint32_t x=12345;x^=x<<13;x^=x>>17;x^=x<<5;return x;}
static void upload(struct etna2d_surface*s,const void*v,int bpp){
	etna2d_surface_cpu_begin(c,s);uint8_t*m=etna2d_surface_map(s);uint32_t st=etna2d_surface_stride(s);
	for(int y=0;y<H;y++)memcpy(m+y*st,(const uint8_t*)v+y*W*bpp,W*bpp);etna2d_surface_cpu_fini(s);}
static double CA=1.0;
static int run(uint32_t ctl,uint32_t modes,uint32_t cm,uint32_t gs,int B,uint32_t Ccol,int ga,int*maxerr){
	upload(dst,dp,4);
	if(etna2d_blit_raw(c,B?srcM:srcA,0,0,dst,0,0,W,H,ctl,modes,cm,gs,0))return 0;
	etna2d_finish(c);
	etna2d_surface_cpu_begin(c,dst);uint8_t*m=etna2d_surface_map(dst);uint32_t st=etna2d_surface_stride(dst);
	int worst=0;
	for(int y=0;y<H;y++)for(int x=0;x<W;x++){
		uint32_t g=*(uint32_t*)(m+y*st+x*4),dv=dp[y*W+x];
		for(int k=0;k<3;k++){
			double dc=((dv>>(8*k))&255)/255.0,e;
			if(!B){double sa=(sp[y*W+x]>>24)/255.0,sc=((sp[y*W+x]>>(8*k))&255)/255.0,G=ga/255.0;
				e=sc*G+dc*(1-sa*G);}
			else{double mm=CA*mp[y*W+x]/255.0,cc=((Ccol>>(8*k))&255)/255.0;e=cc*mm+dc*(1-mm);}
			int err=abs((int)lround(e*255)-(int)((g>>(8*k))&255));if(err>worst)worst=err;}}
	etna2d_surface_cpu_fini(dst);*maxerr=worst;return 1;}
int main(int argc,char**argv){
	const char*mode=argc>1?argv[1]:"AB";
	if(etna2d_device_open("/dev/dri/renderD128",&d))return 1;etna2d_context_create(d,512,&c);
	etna2d_surface_create(d,W,H,&srcA);etna2d_surface_create_fmt(d,W,H,ETNA2D_FMT_A8,&srcM);etna2d_surface_create(d,W,H,&dst);
	for(int i=0;i<W*H;i++){uint32_t a=rnd()&255,r=rnd()%(a+1),g=rnd()%(a+1),b=rnd()%(a+1);
		if(i%8==0)a=255;if(i%8==1){a=0;r=g=b=0;}
		sp[i]=(a<<24)|(r<<16)|(g<<8)|b;mp[i]=(i%8==0)?255:(i%8==1)?0:rnd()&255;
		dp[i]=0xff000000u|(rnd()&0xffffff);
		if(strchr(mode,'W')){uint32_t da=rnd()&255;if(i%8==2)da=0;if(i%8==3)da=255;dp[i]=(da<<24)|((rnd()%(da+1))<<16)|((rnd()%(da+1))<<8)|(rnd()%(da+1));}}
	upload(srcA,sp,4);upload(srcM,mp,1);
	if(strchr(mode,'V')){
		int ref_ga[]={255,128,10,1,0},ref_ca[]={255,128,32,1,0};int bad=0;
		for(int i=0;i<5;i++){upload(dst,dp,4);etna2d_blit_opacity(c,srcA,0,0,dst,0,0,W,H,ref_ga[i]);etna2d_finish(c);
			etna2d_surface_cpu_begin(c,dst);uint8_t*m=etna2d_surface_map(dst);uint32_t st=etna2d_surface_stride(dst);int worst=0;
			for(int y=0;y<H;y++)for(int x=0;x<W;x++){uint32_t g=*(uint32_t*)(m+y*st+x*4);
				for(int k=0;k<4;k++){double dc=((dp[y*W+x]>>(8*k))&255)/255.0,sc=((sp[y*W+x]>>(8*k))&255)/255.0,sa=(sp[y*W+x]>>24)/255.0,G=ref_ga[i]/255.0;
					double e=sc*G+dc*(1-sa*G);int err=abs((int)lround(e*255)-(int)((g>>(8*k))&255));if(err>worst)worst=err;}}
			etna2d_surface_cpu_fini(dst);printf("V opacity ga=%3d maxerr(all 4 ch)=%d\n",ref_ga[i],worst);if(worst>3)bad++;}
		for(int i=0;i<5;i++){upload(dst,dp,4);etna2d_mask_blit(c,srcM,0,0,dst,0,0,W,H,((uint32_t)ref_ca[i]<<24)|0x336699);etna2d_finish(c);
			etna2d_surface_cpu_begin(c,dst);uint8_t*m=etna2d_surface_map(dst);uint32_t st=etna2d_surface_stride(dst);int worst=0,worsta=0;
			for(int y=0;y<H;y++)for(int x=0;x<W;x++){uint32_t g=*(uint32_t*)(m+y*st+x*4);
				double a=ref_ca[i]/255.0*mp[y*W+x]/255.0;
				for(int k=0;k<4;k++){double dc=((dp[y*W+x]>>(8*k))&255)/255.0,cc=k==3?1.0:((0x336699>>(8*k))&255)/255.0;
					double e=cc*a+dc*(1-a);int err=abs((int)lround(e*255)-(int)((g>>(8*k))&255));if(k==3){if(err>worsta)worsta=err;}else if(err>worst)worst=err;}}
			etna2d_surface_cpu_fini(dst);printf("V mask    ca=%3d maxerr(rgb)=%d alpha=%d\n",ref_ca[i],worst,worsta);if(worst>3||worsta>3)bad++;}
		printf(bad?"VFAIL\n":"VPASS\n");return bad;}
	int ga=0x80;uint32_t Ccol=0x336699;
	for(int B=0;B<2;B++){
		if(!strchr(mode,B?'B':'A')&&!(B&&strchr(mode,'C')))continue;
		if(strchr(mode,'C'))CA=0x80/255.0;
		int found=0,tried=0;
		for(int ctlv=0;ctlv<3;ctlv++)for(int gsam=0;gsam<3;gsam++)for(int pm=0;pm<2;pm++)
		for(int sb=0;sb<6;sb++)for(int db=0;db<6;db++)for(int sf=0;sf<2;sf++)for(int gp=0;gp<3;gp++)for(int sp_=0;sp_<2;sp_++)
		for(int gsc=0;gsc<2;gsc++){
			uint32_t ctl=VIVS_DE_ALPHA_CONTROL_ENABLE_ON|(ctlv==1?VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_SRC_ALPHA(ga):0)|(ctlv==2?VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_SRC_ALPHA(ga)|VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_DST_ALPHA(ga):0);
			uint32_t modes=(gsam<<8)|(pm?VIVS_DE_ALPHA_MODES_PE10_SRC_COLOR_MULTIPLY_ENABLE:0)|VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(sb)|VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(db)|(sf?VIVS_DE_ALPHA_MODES_SRC_ALPHA_FACTOR_ENABLE:0);
			uint32_t cm=(gp==1?VIVS_DE_COLOR_MULTIPLY_MODES_SRC_GLOBAL_PREMULTIPLY_ALPHA:gp==2?VIVS_DE_COLOR_MULTIPLY_MODES_SRC_GLOBAL_PREMULTIPLY_COLOR:0)|(sp_?VIVS_DE_COLOR_MULTIPLY_MODES_SRC_PREMULTIPLY_ENABLE:0);
			uint32_t gs=B?((strchr(mode,'C')?0x80000000u:0xff000000u)|Ccol):((uint32_t)ga<<24)|(gsc?0x00ffffff:0);
			int err;tried++;
			if(!run(ctl,modes,cm,gs,B,Ccol,ga,&err))continue;
			if(err<=3){found++;printf("%c MATCH err=%d ctl=%08x modes=%08x cm=%08x gs=%08x\n",B?'B':'A',err,ctl,modes,cm,gs);fflush(stdout);}
		}
		printf("%c tried=%d found=%d\n",B?'B':'A',tried,found);
	}
	return 0;}
