/* opcost.c — per-op cost of tiny blits on the GC520 (improvement-plan Step 3 finding).
 * Usage: opcost [/dev/dri/renderD128]   Prints us per op for N ops then one finish. */
#include "etna2d.h"
#include <stdio.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(int argc,char**argv){
	struct etna2d_device*d;struct etna2d_context*c;struct etna2d_surface*a,*b;
	if(etna2d_device_open(argc>1?argv[1]:"/dev/dri/renderD128",&d))return 1;
	if(etna2d_context_create(d,512,&c))return 1;
	etna2d_surface_create(d,1024,600,&a);etna2d_surface_create(d,1024,600,&b);
	static const int ns[]={1,8,64,256};
	static const int sz[][2]={{48,4},{48,48},{256,256},{1024,600}};
	for(int m=0;m<2;m++)for(int s=0;s<4;s++)for(int k=0;k<4;k++){
		int n=ns[k];double t0=now();
		for(int r=0;r<5;r++){
			for(int i=0;i<n;i++)
				etna2d_blit(c,a,(i*7)%64,0,b,(i*13)%64,0,sz[s][0],sz[s][1],
					m?ETNA2D_BLEND_SRC_OVER:ETNA2D_BLEND_NONE,ETNA2D_ROP_COPY);
			etna2d_finish(c);
		}
		double t=(now()-t0)/5;
		printf("%s %4dx%-4d n=%-4d total=%9.1f us  per-op=%8.2f us\n",m?"over":"copy",sz[s][0],sz[s][1],n,t,t/n);
	}
	return 0;}
