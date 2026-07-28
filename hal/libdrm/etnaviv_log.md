# modprobe etnaviv
[    9.099688] cmdbuf suballoc(kernel pool): vaddr ffff8000812b0000, paddr 0x88880000
[    9.107942] Etnaviv GPU core clock rate: 250000000
[    9.112693] etnaviv etnaviv: bound 40280000.gpu (ops gpu_ops [etnaviv])
[    9.123447] Etnaviv reset deasserted
[    9.126970] etnaviv-gpu 40280000.gpu: model: GC520, revision: 5341
[    9.133221] cmdbuf init: vaddr 0xffff8000812b0000, size 4096, suballoc offset 0x0
[    9.141465] [drm] Initialized etnaviv 1.4.0 20151214 for etnaviv on minor 1

# /usr/bin/etnaviv_2d_test /dev/dri/renderD128
[   15.541513] get suballoc iova 0x1000, PA 0x88880000
Version: 1.4.0
[   15.548722] submit: stream_size=2368, nr_relocs=1, nr_bos=1, nr_pmrs=0

  Date: 2015121[   15.556002] cmdbuf init: vaddr 0xffff8000812b1000, size 2376, suballoc offset 0x1000
4
  Description[   15.565259] MMU init File: drivers/gpu/drm/etnaviv/etnaviv_iommu_v2.c, Line: 182
: etnaviv DRM
[   15.573867] Start FE: 0x88880000, debug 0x8880000, prefetch length 6
[   15.581471] iova 0x1000, offset 0x0
[   15.584935] Start FE: 0x1000, debug 0x1000, prefetch length 2
[   15.590736] etnaviv-gpu 40280000.gpu: intr 0x00000001
[   15.595712] axi status 0x0
Compare OK

# mount -t debugfs none /sys/kernel/debug/
# cat /sys/kernel/debug/dri/128/gpu
40280000.gpu Status:
        identity
         model: 0x520
         revision: 0x5341
         product_id: 0x5202
         customer_id: 0x204
         eco_id: 0x0
        features
         major_features: 0xe02c7eca
         minor_features0: 0xe9399eff
         minor_features1: 0xfe1fb2db
         minor_features2: 0xcedf0080
         minor_features3: 0x10800005
         minor_features4: 0x20000000
         minor_features5: 0x00020880
         minor_features6: 0x00000000
         minor_features7: 0x00001000
         minor_features8: 0x00000000
         minor_features9: 0x00000000
         minor_features10: 0x00000000
         minor_features11: 0x00000000
        specs
         stream_count:  1
         register_max: 64
         thread_count: 256
         vertex_cache_size: 8
         shader_core_count: 1
         nn_core_count: 0
         pixel_pipes: 1
         vertex_output_buffer_size: 512
         buffer_size: 0
         instruction_count: 256
         num_constants: 168
         varyings_count: 8
        axi: 0x00000000
        idle: 0x7fffffff
        DMA seems to be stuck
         address 0: 0x000010b0
         address 1: 0x000010b0
         state 0: 0x00000000
         state 1: 0x00000000

# cat /sys/kernel/debug/dri/128/mmu
Active Objects (40280000.gpu):
0x0000000000001000-0x0000000000081000: 524288: used
0x0000000000081000-0x0000000100000000: 4294438912: free
total: 4294963200, used 524288 free 4294438912

