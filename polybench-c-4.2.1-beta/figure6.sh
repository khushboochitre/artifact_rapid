#!/bin/bash
echo "Figure 6: Polybench performance benefits"

echo
echo "Restrict"
./polybench_restrict.sh

echo
echo "Scout: Number of versioned loops, performance benefits"
echo "gesummv	1	49.37"
echo "2mm	2	4.22"
echo "3mm	3	4.28"
echo "bicg	1	51.11"
echo "doitgen	1	10.2"
echo "jacobi-1d	2	11.8"
echo "jacobi-2d	2	2.37"

: 'echo
echo "Column 3: Scout"
./polybench_scout_stats.sh #third column

echo
echo "Column 4: Scout"
./polybench_scout_improvement.sh #Fourth column

echo
echo "Profiler"
./polybench_rapid_profiler.sh'

echo
echo "Rapid: Minimum number of regions required, performance benefits"
./polybench_rapid_improvement.sh

echo
echo "Rapid: Number of versioned loops"
./polybench_rapid_stats.sh

echo
echo "Rapid_w"
./polybench_rapid_worst.sh
