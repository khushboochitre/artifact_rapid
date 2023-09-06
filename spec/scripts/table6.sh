#!/bin/bash
echo "Table 6: CPU SPEC 2017 performance benefits using our loop filtering mechanism with the threshold for number of iterations as 64"

echo
echo "Rapid: Number of versioned loops and performance benefits excluding the allocator’s overhead (0)"
./rapid_loop_filter_default.sh

echo
echo "Rapid: Number of versioned loops and performance benefits excluding the allocator’s overhead (8)"
./rapid_loop_filter_8.sh

echo
echo "Rapid: Performance benefits excluding the allocator’s overhead (16)"
./rapid_loop_filter_16.sh 

echo
echo "Rapid: Performance benefits excluding the allocator’s overhead (32)"
./rapid_loop_filter_32.sh

echo
echo "Rapid: Performance benefits excluding the allocator’s overhead (128)"
./rapid_loop_filter_128.sh

echo
echo "Rapid: Performance benefits excluding the allocator’s overhead (256)"
./rapid_loop_filter_256.sh

echo
echo "Rapid: Performance benefits including the allocator’s overhead (0)"
./rapid_loop_filter_default_allocator.sh

echo
echo "Rapid: Performance benefits including the allocator’s overhead (8)"
./rapid_loop_filter_8_allocator.sh 

echo
echo "Rapid: Performance benefits including the allocator’s overhead (16)"
./rapid_loop_filter_16_allocator.sh

echo
echo "Rapid: Performance benefits including the allocator’s overhead (32)"
./rapid_loop_filter_32_allocator.sh

echo
echo "Rapid: Performance benefits including the allocator’s overhead (128)"
./rapid_loop_filter_128_allocator.sh

echo
echo "Rapid: Performance benefits including the allocator’s overhead (256)"
./rapid_loop_filter_256_allocator.sh
