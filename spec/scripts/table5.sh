#!/bin/bash
echo "Table 4: CPU SPEC 2017 performance benefits handling memory allocations inside the loops"

echo
echo "Rapid: Worst-case performance"
./rapid_worst_8.sh
./rapid_worst_16.sh
./rapid_worst_32.sh
./rapid_worst_128.sh
./rapid_worst_256.sh

echo
echo "Rapid: Total number of versioned loops and performance benefits (8)"
./rapid_improvement_8.sh

echo
echo "Rapid: Performance benefits (16)"
./rapid_improvement_16.sh

echo
echo "Rapid: Performance benefits (32)"
./rapid_improvement_32.sh

echo
echo "Rapid: Performance benefits (128)"
./rapid_improvement_128.sh

echo
echo "Rapid: Performance benefits (256)"
./rapid_improvement_256.sh
