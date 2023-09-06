#!/bin/bash
echo "Figure 9: CPU SPEC 2017 performance benefits for Rapid and Scout"

echo 
echo "Scout: Performance benefits excluding allocator’s overhead, performance benefits including allocator’s overhead and worst-case performance"
echo "508.namd_r	0.51	0.51	-0.38"
echo "510.parest_r	0.73	0.3	-0.3"
echo "538.imagick_r	0.23	-1.17	0"
echo "544.nab_r	0.27	-1.27	-0.09"
echo "500.perlbench_r	0	-0.33	-0.4"
echo "502.gcc_r	0.73	1.71	0.13"
echo "520.omnetpp_r	0.5	-7.86	-1.79"
echo "523.xalancbmk_r	1.47	-1.58	0.88"
echo "525.x264_r	0.89	3.25	0"
echo "531.deepsjeng_r	0.49	-1.15	-0.1"
echo "511.povray_r	0	-7.5	-2.11"
echo "526.blender_r	0.52	3.09	-3.37"

echo
echo "Rapid: Number of versioned loops, number of regions required and performance benefits excluding allocator’s overhead"
./rapid_improvement_default.sh

echo
echo "Rapid: Performance benefits including allocator’s overhead"
./rapid_improvement_default_allocator.sh

echo
echo "Rapid: Worst-case performance"
./rapid_worst_default.sh
