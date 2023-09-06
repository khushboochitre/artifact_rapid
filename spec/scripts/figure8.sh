#!/bin/bash
echo "Figure 8: Memory overhead for CPU SPEC 2017"

echo
echo "Scout: Memory overhead"
echo "508.namd_r        -2.29"
echo "510.parest_r      11.72"
echo "538.imagick_r     -0.33"
echo "544.nab_r         17.58"
echo "500.perlbench_r   6.53"
echo "502.gcc_r         6.15"
echo "520.omnetpp_r     16.72"
echo "523.xalancbmk_r   19.97"
echo "525.x264_r        0.06"
echo "531.deepsjeng_r   0.08"
echo "511.povray_r      34.89"
echo "526.blender_r     5.2"

echo
echo "Rapid: Memory overhead"
./mem_overhead_default.sh

