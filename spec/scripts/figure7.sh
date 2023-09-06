#!/bin/bash
echo "Figure 7: CPU overhead for CPU SPEC 2017"

echo 
echo "Scout: CPU overhead"
echo "508.namd_r	0"
echo "510.parest_r	0.43"
echo "538.imagick_r	1.4"
echo "544.nab_r	1.54"
echo "500.perlbench_r	0.33"
echo "502.gcc_r	-0.98"
echo "520.omnetpp_r	8.36"
echo "523.xalancbmk_r	3.05"
echo "525.x264_r	-2.36"
echo "531.deepsjeng_r	1.64"
echo "511.povray_r	7.5"
echo "526.blender_r	-2.57"

echo
echo "Rapid: CPU overhead"
./cpu_overhead_default.sh

