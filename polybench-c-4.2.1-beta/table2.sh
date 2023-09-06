#!/bin/bash
echo "Table 2: Polybench speedups"

echo
echo "Restrict with LLVM-3.6.0"
./polybench_restrict_speedup_3.sh

echo
echo "Restrict with LLVM-10"
./polybench_restrict_speedup.sh

echo
echo "Hybrid approach"
echo "gesummv"
echo "2.5"
echo "bicg"
echo "2.7"
echo "gramschmidt"
echo "1.4"

echo
echo "Scout"
echo "gesummv"
echo "1.98"
echo "bicg"
echo "2.05"
echo "gramschmidt"
echo "1.01"

#echo
#echo "Column 5: Scout"
#./polybench_scout_improvement_speedup.sh #Fifth Column

echo
echo "Rapid"
./polybench_rapid_improvement_speedup.sh
