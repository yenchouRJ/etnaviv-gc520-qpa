/* emptyfinish.c — cost of etna2d_finish() with nothing queued, and of fill+finish. */
#include "etna2d.h"
#include <stdio.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(void){
	struct etna2d_device*d;struct etna2d_context*c;struct etna2d_surface*a;
	if(etna2d_device_open("/dev/dri/renderD128",&d))return 1;
	etna2d_context_create(d,512,&c);etna2d_surface_create(d,256,256,&a);
	etna2d_fill(c,a,0,0,8,8,0);etna2d_finish(c);
	double t0=now();for(int i=0;i<1000;i++)etna2d_finish(c);
	printf("empty finish: %.1f us\n",(now()-t0)/1000);
	t0=now();for(int i=0;i<1000;i++){etna2d_fill(c,a,0,0,8,8,0);etna2d_finish(c);}
	printf("8x8 fill + finish: %.1f us\n",(now()-t0)/1000);
	t0=now();for(int i=0;i<1000;i++){etna2d_fill(c,a,0,0,8,8,0);etna2d_flush(c);}
	etna2d_finish(c);
	printf("8x8 fill + flush (async): %.1f us\n",(now()-t0)/1000);
	return 0;}
