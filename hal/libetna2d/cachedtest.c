/* cachedtest.c — coherency of ETNA2D_SURF_CACHED surfaces (Step 1).
 * CPU write -> GPU read, GPU write -> CPU read, CPU read-modify-write after GPU, many rounds. */
#include "etna2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
int main(int argc,char**argv){
	struct etna2d_device*d;struct etna2d_context*c;struct etna2d_surface*cs,*ws,*cs2;
	const int W=1024,H=600; int bad=0, rounds=argc>1?atoi(argv[1]):200;
	if(etna2d_device_open("/dev/dri/renderD128",&d))return 1;
	etna2d_context_create(d,512,&c);
	if(etna2d_surface_create_ex(d,W,H,ETNA2D_FMT_ARGB8888,ETNA2D_SURF_CACHED,&cs))return 1;
	etna2d_surface_create_ex(d,W,H,ETNA2D_FMT_ARGB8888,ETNA2D_SURF_CACHED,&cs2);
	etna2d_surface_create(d,W,H,&ws);
	printf("cached=%d/%d\n",etna2d_surface_is_cached(cs),etna2d_surface_is_cached(ws));
	uint32_t st=etna2d_surface_stride(cs)/4;
	for(int r=0;r<rounds;r++){
		uint32_t seed=0x1234567u*(r+1);
		/* 1. CPU write cached -> GPU blit to WC -> CPU read WC */
		etna2d_surface_cpu_begin(c,cs);
		uint32_t*p=etna2d_surface_map(cs);
		for(int y=0;y<H;y++)for(int x=0;x<W;x++)p[y*st+x]=0xff000000u|((seed+y*31+x*17)&0xffffff);
		etna2d_surface_cpu_fini(cs);
		etna2d_blit(c,cs,0,0,ws,0,0,W,H,ETNA2D_BLEND_NONE,ETNA2D_ROP_COPY);
		etna2d_surface_cpu_begin(c,ws);
		uint32_t*q=etna2d_surface_map(ws);
		for(int y=0;y<H&&bad<10;y++)for(int x=0;x<W;x++)
			if(q[y*st+x]!=(0xff000000u|((seed+y*31+x*17)&0xffffff))){printf("r%d CPU->GPU mismatch y=%d x=%d got %08x\n",r,y,x,q[y*st+x]);bad++;break;}
		etna2d_surface_cpu_fini(ws);
		/* 2. GPU fill cached (dirty CPU lines from step 1 exist) -> CPU read */
		uint32_t col=0xff000000u|(seed&0xffffff);
		etna2d_fill(c,cs,0,0,W,H,col);
		etna2d_surface_cpu_begin(c,cs);
		for(int y=0;y<H&&bad<10;y++)for(int x=0;x<W;x++)
			if(p[y*st+x]!=col){printf("r%d GPU->CPU mismatch y=%d x=%d got %08x want %08x\n",r,y,x,p[y*st+x],col);bad++;break;}
		/* 3. CPU read-modify-write then GPU blit to second cached surface */
		for(int y=0;y<H;y++)for(int x=0;x<W;x+=16)p[y*st+x]^=0x00ffffff;
		etna2d_surface_cpu_fini(cs);
		etna2d_blit(c,cs,0,0,cs2,0,0,W,H,ETNA2D_BLEND_NONE,ETNA2D_ROP_COPY);
		etna2d_surface_cpu_begin(c,cs2);
		uint32_t*q2=etna2d_surface_map(cs2);
		for(int y=0;y<H&&bad<10;y++)for(int x=0;x<W;x++){
			uint32_t w=(x%16==0)?(col^0x00ffffff):col;
			if(q2[y*st+x]!=w){printf("r%d RMW mismatch y=%d x=%d got %08x want %08x\n",r,y,x,q2[y*st+x],w);bad++;break;}}
		etna2d_surface_cpu_fini(cs2);
	}
	/* read speed of a cached vs WC surface (CPU sum of the whole frame) */
	for(int k=0;k<2;k++){
		struct etna2d_surface*s=k?cs:ws; etna2d_surface_cpu_begin(c,s);
		volatile uint32_t*m=etna2d_surface_map(s); uint32_t sum=0; double t0=now();
		for(int y=0;y<H;y++)for(int x=0;x<W;x++)sum+=m[y*st+x];
		printf("%s full-frame CPU read: %.2f ms (sum %u)\n",k?"cached":"WC    ",now()-t0,sum);
		etna2d_surface_cpu_fini(s);
	}
	printf(bad?"FAIL (%d)\n":"PASS %d rounds\n",bad?bad:rounds);
	return bad!=0;}
